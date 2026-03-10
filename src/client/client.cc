#include "client.h"
#include <algorithm>
#include "../constants.h"
#include "../utils/utils.h"

using grpc::ClientContext;
using grpc::Status;
using google::protobuf::Empty;

using tpc::TpcServer;
using tpc::BalanceReq;
using tpc::BalanceRes;
using tpc::LogEntry;
using tpc::LogRes;
using tpc::DisconnectReq;
using tpc::TransferReq;
using tpc::Transaction;

Client::Client() {
    logger = spdlog::get("console");
    for (auto& s: constants::server_ids) {
        std::string name = s.first;
        stubs[name] = TpcServer::NewStub(grpc::CreateChannel(constants::server_addresses[name], grpc::InsecureChannelCredentials()));    
    }

    transactions_processed = 0;
    wall_started = false;
}

void Client::updateDisconnected(std::vector<std::string> disconnected_servers) {
    DisconnectReq request;
    for (auto& s: disconnected_servers) request.add_servers(s);
    for (auto& s: stubs) {
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(Client::RPC_TIMEOUT_MS));
        
        Empty reply;
        s.second->Disconnect(&context, request, &reply);
    }
}

void Client::processTransactions(std::vector<types::Transaction> transactions, std::vector<std::string> leaders) {
    for (auto& t: transactions) {
        bool cross_shard = false;
        int sender_cluster = utils::getClusterIdFromClientId(t.sender);
        int receiver_cluster = utils::getClusterIdFromClientId(t.receiver);
        
        std::string sender_leader = leaders[sender_cluster - 1];
        std::string receiver_leader = leaders[receiver_cluster - 1];

        if (sender_cluster != receiver_cluster) cross_shard = true;
        current_is_cross_shard = cross_shard;

        TransferReq request;
        auto epoch = std::chrono::system_clock::now().time_since_epoch();
        long tid = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
        request.set_tid(tid);
        
        Transaction* txn = request.mutable_txn();
        txn->set_sender(t.sender);
        txn->set_receiver(t.receiver);
        txn->set_amount(t.amount);

        if (!wall_started) {
            wall_start = std::chrono::steady_clock::now();
            wall_started = true;
        }

        if (!cross_shard) {
            logger->debug("Sending transfer to {}", sender_leader);
            sendTransfer(request, sender_leader);
        } else {
            long now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            processing[tid] = { { t.sender, t.receiver, t.amount }, 0, 0, now_ns, true };
            logger->debug("Sending 2PC prepare to {} and {}", sender_leader, receiver_leader);
            tpcPrepare(request, sender_leader);
            tpcPrepare(request, receiver_leader);
        }
    }
}

void Client::printBalance(int client_id) {
    int cluster_id = utils::getClusterIdFromClientId(client_id);
    std::vector<std::string> servers = utils::getServersInCluster(cluster_id);
    std::vector<std::string> balances;

    BalanceReq request;
    request.set_client(client_id);
    for (auto& s: servers) {
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(Client::RPC_TIMEOUT_MS));
        
        BalanceRes reply;
        Status status = stubs[s]->Balance(&context, request, &reply);
        balances.push_back(status.ok() ? std::to_string(reply.amount()) : "-");
    }

    std::cout << std::setw(10) << "Server|" << std::setw(10) << "Balance|" << std::endl;
    for (int i = 0; i < servers.size(); i++) {
        std::cout << std::setw(10) << servers[i] + "|" << std::setw(10) << balances[i] + "|" << std::endl;
    }
}

void Client::printDatastore() {
    std::map<std::string, std::vector<types::WALEntry>> datastore;
    Empty request;

    for (auto& s: constants::server_ids) {
        datastore[s.first] = std::vector<types::WALEntry>();
        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(Client::RPC_TIMEOUT_MS));
        LogRes reply;
        Status status = stubs[s.first]->Logs(&context, request, &reply);
        if (status.ok()) {
            for (int i = 0; i < reply.logs_size(); i++) {
                LogEntry e = reply.logs(i);
                types::WALEntry entry = {
                    e.tid(),
                    e.ballot_num(),
                    e.ballot_server_id(),
                    { e.txn().sender(), e.txn().receiver(), e.txn().amount() },
                    static_cast<types::TransactionType>(e.type()),
                    static_cast<types::TransactionStatus>(e.status())
                };
                datastore[s.first].push_back(entry);
            }
        }
    }

    std::cout << std::setw(10) << "Server|" << std::setw(10) << " " <<  "Datastore" << std::endl;
    for (auto& pair: datastore) {
        std::cout << std::setw(10) << pair.first + "|";
        std::cout << std::setw(10) << " ";
        
        for (int i = 0; i < pair.second.size(); i++) {
            types::WALEntry e = pair.second[i];
            std::stringstream entry;
            if (i > 0) std::cout << " -> ";
        
            entry << "[<" << e.ballot_num << "," << e.ballot_server_id << ">, ";
        
            if (e.type == types::TransactionType::CROSS) {
                std::string status;
                switch (e.status) {
                    case types::TransactionStatus::PREPARED:
                        status = "P"; break;
                    case types::TransactionStatus::COMMITTED:
                        status = "C"; break;
                    case types::TransactionStatus::ABORTED:
                        status = "A"; break;
                    default:
                        status = "NS";
                }
                entry << status << ", ";
            }
            
            entry << "(" << e.txn.sender << ", " << e.txn.receiver << ", " << e.txn.amount << ")]";
            std::cout << entry.str();
        }
        std::cout << std::endl << std::endl;
    }
}

void Client::printPerformance() {
    auto printLatencyStats = [](const std::vector<long>& samples) {
        if (samples.empty()) {
            std::cout << "Latency mean:                  N/A" << std::endl;
            std::cout << "Latency p50:                   N/A" << std::endl;
            std::cout << "Latency p99:                   N/A" << std::endl;
            return;
        }

        std::vector<long> sorted = samples;
        std::sort(sorted.begin(), sorted.end());

        long total_ns = 0;
        for (long l : sorted) total_ns += l;
        double mean_ms = (total_ns / static_cast<double>(sorted.size())) / 1e6;
        double p50_ms = sorted[sorted.size() * 50 / 100] / 1e6;
        double p99_ms = sorted[sorted.size() * 99 / 100] / 1e6;

        std::cout << "Latency mean:                  " << mean_ms << " ms" << std::endl;
        std::cout << "Latency p50:                   " << p50_ms << " ms" << std::endl;
        std::cout << "Latency p99:                   " << p99_ms << " ms" << std::endl;
    };

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== Overall ===" << std::endl;
    std::cout << "Transactions Processed: " << transactions_processed << std::endl;

    if (latencies.empty()) {
        std::cout << "No completed transactions to report." << std::endl;
        return;
    }

    double wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
    double tps = (wall_s > 0) ? transactions_processed / wall_s : 0;
    std::cout << "Wall-clock time:               " << wall_s << " s" << std::endl;
    std::cout << "Throughput:                    " << tps << " tps" << std::endl;
    printLatencyStats(latencies);

    std::cout << std::endl;
    std::cout << "=== Intra-Shard Only ===" << std::endl;
    std::cout << "Transactions: " << intra_latencies.size() << std::endl;
    printLatencyStats(intra_latencies);

    std::cout << std::endl;
    std::cout << "=== Cross-Shard Only ===" << std::endl;
    std::cout << "Transactions: " << cross_latencies.size() << std::endl;
    printLatencyStats(cross_latencies);
}

void Client::sendTransfer(TransferReq& request, std::string leader) {
    ClientCall* call = new ClientCall;
    call->type = types::RequestTypes::TRANSFER;
    call->is_cross_shard = current_is_cross_shard;
    call->transferReader = stubs[leader]->PrepareAsyncTransfer(&call->context, request, &cq);
    call->transferReader->StartCall();
    call->transferReader->Finish(&call->reply, &call->status, (void*)call);
}

void Client::tpcPrepare(TransferReq& request, std::string leader) {
    ClientCall* call = new ClientCall;
    call->type = types::RequestTypes::TPC_PREPARE;
    call->is_cross_shard = current_is_cross_shard;
    call->transferReader = stubs[leader]->PrepareAsyncTpcPrepare(&call->context, request, &cq);
    call->transferReader->StartCall();
    call->transferReader->Finish(&call->reply, &call->status, (void*)call);
}

void Client::tpcCommit(TpcTid& request, int sender_cluster, int receiver_cluster) {
    logger->debug("Sending TPC commit to clusters {} and {}", sender_cluster, receiver_cluster);
    for (auto& s: constants::server_ids) {
        int cluster_id = utils::getClusterIdFromServerId(s.second);
        if (cluster_id == sender_cluster || cluster_id == receiver_cluster) {
            ClientCall* call = new ClientCall;
            call->type = types::RequestTypes::TPC_COMMIT;
            call->emptyReader = stubs[s.first]->PrepareAsyncTpcCommit(&call->context, request, &cq);
            call->emptyReader->StartCall();
            call->emptyReader->Finish(&call->empty, &call->status, (void*)call);
        }
    }
}

void Client::tpcAbort(TpcTid& request, int sender_cluster, int receiver_cluster) {
    logger->debug("Sending TPC abort to clusters {} and {}", sender_cluster, receiver_cluster);
    for (auto& s: constants::server_ids) {
        int cluster_id = utils::getClusterIdFromServerId(s.second);
        if (cluster_id == sender_cluster || cluster_id == receiver_cluster) {
            ClientCall* call = new ClientCall;
            call->type = types::RequestTypes::TPC_ABORT;
            call->emptyReader = stubs[s.first]->PrepareAsyncTpcAbort(&call->context, request, &cq);
            call->emptyReader->StartCall();
            call->emptyReader->Finish(&call->empty, &call->status, (void*)call);
        }
    }
}

void Client::consumeReplies() {
    void* tag;
    bool ok;

    while(cq.Next(&tag, &ok)) {
        ClientCall* call = static_cast<ClientCall*>(tag);
        CHECK(ok);
        
        if (call->type == types::TRANSFER && call->status.ok()) {
            wall_end = std::chrono::steady_clock::now();
            long start_ns = call->reply.tid();  // tid is system_clock epoch-ns start time
            long now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            long latency_ns = now_ns - start_ns;
            latencies.push_back(latency_ns);
            if (call->is_cross_shard) cross_latencies.push_back(latency_ns);
            else intra_latencies.push_back(latency_ns);
            ++transactions_processed;
        } else if (call->type == types::RequestTypes::TPC_PREPARE) {
            long tid = call->reply.tid();
            // tid may be 0 if server was unreachable; find entry by iterating processing map
            if (tid == 0) {
                // can't correlate — skip latency tracking for this reply
                delete call;
                continue;
            }
            if (!call->status.ok() || !call->reply.ack()) ++processing[tid].failures;
            else ++processing[tid].successes;

            bool commit = processing[tid].successes == 2;
            bool abort = processing[tid].failures > 0 && (processing[tid].successes + processing[tid].failures == 2);
            if (commit || abort) {
                wall_end = std::chrono::steady_clock::now();
                long now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                long latency_ns = now_ns - processing[tid].start_ns;
                latencies.push_back(latency_ns);
                if (processing[tid].is_cross_shard) cross_latencies.push_back(latency_ns);
                else intra_latencies.push_back(latency_ns);
                ++transactions_processed;

                int sender_cluster = utils::getClusterIdFromClientId(processing[tid].txn.sender);
                int receiver_cluster = utils::getClusterIdFromClientId(processing[tid].txn.receiver);

                TpcTid request;
                request.set_tid(tid);
                commit ? 
                    tpcCommit(request, sender_cluster, receiver_cluster) :
                    tpcAbort(request, sender_cluster, receiver_cluster);
                
                processing.erase(tid);
            }
        }
        
        delete call;
    }
}
