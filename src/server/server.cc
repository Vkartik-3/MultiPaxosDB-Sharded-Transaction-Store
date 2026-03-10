#include <grpcpp/grpcpp.h>
#include "tpc.grpc.pb.h"
#include <leveldb/write_batch.h>

#include <map>
#include <csignal>

#include "server.h"
#include "in_call.h"
#include "out_call.h"
#include "../constants.h"
#include "../utils/utils.h"

using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::CompletionQueue;
using grpc::Server;

bool ServerImpl::shutdown = false;

ServerImpl::ServerImpl(int id, std::string name): wal("wal_" + std::to_string(id) + ".log") {
    logger = spdlog::get("console");

    server_id = id;
    server_name = name;
    cluster_id = utils::getClusterIdFromServerId(id);
    
    resetDisconnectedState();
    balances = std::map<int, int>();
    
    is_paxos_running = false;
    paxos_tid = -1;
    ballot_num = 0;
    promised = false;
    accepted = false;
    in_sync = false;
    await_prepare_decision = false;
    await_accept_decision = false;
    last_inserted = -1;
}

ServerImpl::~ServerImpl() {
    server->Shutdown();
    request_cq->Shutdown();
    response_cq->Shutdown();
    delete db;
}

void ServerImpl::run(std::string address) {
    ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    request_cq = builder.AddCompletionQueue();
    response_cq = std::make_unique<CompletionQueue>();
    server = builder.BuildAndStart();

    for (auto& pair: constants::server_ids) {
        std::string name = pair.first;
        int id = pair.second;
        if (id != server_id && utils::getClusterIdFromServerId(id) == cluster_id) {
            stubs[id] = TpcServer::NewStub(grpc::CreateChannel(constants::server_addresses[name], grpc::InsecureChannelCredentials()));    
        }
    }

    leveldb::Options options;
    options.create_if_missing = true;
    std::string db_path = server_name + "_db";
    leveldb::Status status = leveldb::DB::Open(options, db_path, &db);
    if (!status.ok()) {
        logger->error("Unable to open/create database: {}", db_path);
        logger->error(status.ToString());
        return;
    }

    std::string ballot_val;
    leveldb::Status ballot_status = db->Get(leveldb::ReadOptions(), "__ballot_num__", &ballot_val);
    if (ballot_status.ok()) {
        ballot_num = std::stoi(ballot_val);
    }

    // Load balances from LevelDB. Initialize to 10 only on first run.
    for (int i = 1; i <= constants::total_clients; i++) {
        if (utils::isClientInCluster(i, cluster_id)) {
            std::string balance_val;
            leveldb::Status balance_status = db->Get(leveldb::ReadOptions(), std::to_string(i), &balance_val);
            if (balance_status.ok()) {
                balances[i] = std::stoi(balance_val);
            } else if (balance_status.IsNotFound()) {
                balances[i] = 10;
                leveldb::Status put_status = db->Put(leveldb::WriteOptions(), std::to_string(i), "10");
                if (!put_status.ok()) {
                    logger->error("Failed to initialize client {} balance", i);
                    logger->error(put_status.ToString());
                    return;
                }
            } else {
                logger->error("Failed to load client {} balance", i);
                logger->error(balance_status.ToString());
                return;
            }
        }
    }

    wal.recover(balances, cluster_id, last_inserted, ballot_num, log);
    if (last_inserted >= 0) {
        last_inserted_ballot.set_num(log[last_inserted].ballot_num);
        last_inserted_ballot.set_server_id(log[last_inserted].ballot_server_id);
    }
    persistBallotNum();

    logger->info("Server running on {}. Stubs size {}", address, stubs.size());
    HandleRPCs();
}

void ServerImpl::HandleRPCs() {
    new InCall(&service, this, request_cq.get(), types::RequestTypes::TPC_PREPARE, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::TPC_COMMIT, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::TPC_ABORT, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::TRANSFER, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::BALANCE, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::LOGS, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::PREPARE, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::ACCEPT, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::COMMIT, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::SYNC, ServerImpl::RETRY_TIMEOUT_MS);
    new InCall(&service, this, request_cq.get(), types::RequestTypes::DISCONNECT, ServerImpl::RETRY_TIMEOUT_MS);

    void* request_tag;
    bool request_ok;
    void* response_tag;
    bool response_ok;

    while (!shutdown) {
        // Poll the request queue
        grpc::CompletionQueue::NextStatus request_status = request_cq->AsyncNext(&request_tag, &request_ok, gpr_time_0(GPR_CLOCK_REALTIME));

        // Poll the response queue
        grpc::CompletionQueue::NextStatus response_status = response_cq->AsyncNext(&response_tag, &response_ok, gpr_time_0(GPR_CLOCK_REALTIME));

        // Handle request events
        if (request_status == grpc::CompletionQueue::NextStatus::GOT_EVENT && request_ok) {
            static_cast<InCall*>(request_tag)->Proceed();  // Process request
        }

        // Handle response events
        if (response_status == grpc::CompletionQueue::NextStatus::GOT_EVENT && response_ok) {
            static_cast<OutCall*>(response_tag)->HandleRPCResponse();  // Process response
        }

        auto now = std::chrono::steady_clock::now();
        for (auto it = prepareTimestamps.begin(); it != prepareTimestamps.end();) {
            long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed_ms > TPC_LOCK_TIMEOUT_MS) {
                long timed_out_tid = it->first;
                TpcTid fake;
                fake.set_tid(timed_out_tid);
                auto entry = wal.abortTransaction(fake);
                if (entry.tid != -1) {
                    if (isClientInCluster(entry.txn.sender)) {
                        locks.erase(entry.txn.sender);
                    }
                    if (isClientInCluster(entry.txn.receiver)) {
                        locks.erase(entry.txn.receiver);
                    }
                    log.push_back(entry);
                }
                it = prepareTimestamps.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool ServerImpl::isClientInCluster(int client_id) {
    return utils::getClusterIdFromClientId(client_id) == cluster_id;
}

bool ServerImpl::processTpcPrepare(TransferReq& request, TransferRes& response) {
    logger->debug("2PC Prepare received {}. Current tid {}", request.DebugString(), paxos_tid);
    return runPaxos(request, response, true);
}

void ServerImpl::processTpcDecision(TpcTid& request, bool is_commit) {
    if (i_am_disconnected) return;
    
    logger->debug("TPC decision received {}. is_commit {}", request.DebugString(), is_commit);
    prepareTimestamps.erase(request.tid());
    auto entry = is_commit ? wal.commitTransaction(request) : wal.abortTransaction(request);
    if (entry.tid != -1) {
        log.push_back(entry);
    } else {
        return;
    }
    
    if (isClientInCluster(entry.txn.sender)) {
        locks.erase(entry.txn.sender);
        if (is_commit) {
            balances[entry.txn.sender] -= entry.txn.amount;
            updateBalance(entry.txn.sender, balances[entry.txn.sender]);
        }
    }
    if (isClientInCluster(entry.txn.receiver)) {
        locks.erase(entry.txn.receiver);
        if (is_commit) {
            balances[entry.txn.receiver] += entry.txn.amount;
            updateBalance(entry.txn.receiver, balances[entry.txn.receiver]);
        }
    }
}

bool ServerImpl::processTransferCall(TransferReq& request, TransferRes& response) {
    logger->debug("Transfer received {}. Current tid {}", request.DebugString(), paxos_tid);    
    return runPaxos(request, response, false);
}

void ServerImpl::prepareTransaction(TransferReq& request, Ballot& ballot) {
    auto entry = wal.prepareTransaction(request, ballot);
    log.push_back(entry);

    last_inserted = log.size() - 1;
    last_inserted_ballot = ballot;
    prepareTimestamps[request.tid()] = std::chrono::steady_clock::now();
}

void ServerImpl::commitTransaction(TransferReq& request, Ballot& ballot) {
    auto entry = wal.commitTransaction(request, ballot);
    log.push_back(entry);
    balances[entry.txn.sender] -= entry.txn.amount;
    balances[entry.txn.receiver] += entry.txn.amount;
    updateBalance(entry.txn.sender, balances[entry.txn.sender]);
    updateBalance(entry.txn.receiver, balances[entry.txn.receiver]);

    last_inserted = log.size() - 1;
    last_inserted_ballot = ballot;    
}

bool ServerImpl::runPaxos(TransferReq& request, TransferRes& response, bool is_cross_shard) {
    if (in_sync) {
        logger->debug("Syncing...");
        return false;
    }

    int sender = request.txn().sender();
    int receiver = request.txn().receiver();
    int amount = request.txn().amount();
    long tid = request.tid();

    if (is_paxos_running && paxos_tid != tid) return false;
    response.set_tid(tid);

    if (!await_prepare_decision && !await_accept_decision) {
        is_paxos_running = true;
        paxos_tid = tid;

        Ballot ballot;
        ballot.set_num(++ballot_num);
        persistBallotNum();
        ballot.set_server_id(server_id);

        PrepareReq prepare;
        prepare.mutable_ballot()->CopyFrom(ballot);
        prepare.set_last_inserted(last_inserted);

        // Send prepare request to all
        promised = true;
        promised_num = ballot;
        await_prepare_decision = true;
        prepare_successes = 1;
        prepare_failures = 0;
        logger->debug("Sending prepare to replicas {}", prepare.DebugString());
        for (auto& pair: stubs) {
            OutCall* call = new OutCall(this, response_cq.get(), types::PREPARE, ServerImpl::RPC_TIMEOUT_MS);
            call->sendPrepare(prepare, pair.second);
        }
        return false;       // await prepare decision

    } else if (await_prepare_decision) {
        if (prepare_successes >= ServerImpl::MAJORITY) {
            await_prepare_decision = false;

            // Check transaction conditions. Abort if not satisfied.
            bool sender_in_cluster = isClientInCluster(sender);
            bool receiver_in_cluster = isClientInCluster(receiver);

            if ((sender_in_cluster && locks.find(sender) != locks.end())
                    || (receiver_in_cluster && locks.find(receiver) != locks.end())
                    || (sender_in_cluster && balances[sender] < amount)) {

                is_paxos_running = false;
                paxos_tid = -1;
                logger->debug("Transaction conditions not met");
                response.set_ack(false);
                return true;
            }
            
            if (sender_in_cluster) locks.insert(sender);
            if (receiver_in_cluster) locks.insert(receiver);

            logger->debug("Locking {} and/or {}", sender, receiver);
            locks.insert(receiver);

            AcceptReq accept;
            accept.mutable_ballot()->CopyFrom(promised_num);
            accept.mutable_r()->CopyFrom(request);

            // Send accept request to all
            accepted = true;
            accept_num = promised_num;
            accept_val = request;
            await_accept_decision = true;
            accept_successes = 1;
            accept_failures = 0;

            if (is_cross_shard) prepareTransaction(request, accept_num);

            logger->debug("Sending accept to replicas {}", accept.DebugString());
            for (auto& pair: stubs) {
                OutCall* call = new OutCall(this, response_cq.get(), types::ACCEPT, ServerImpl::RPC_TIMEOUT_MS);
                call->sendAccept(accept, pair.second);
            }
            return false;       // await accept decision

        } else if (prepare_failures >= ServerImpl::MAJORITY) {
            await_prepare_decision = false;
            is_paxos_running = false;
            paxos_tid = -1;
            
            response.set_ack(false);
            return true;        // abort transaction
        }

        return false;           // await for a prepare decision

    } else if (await_accept_decision) {
        if (accept_successes >= ServerImpl::MAJORITY) {
            await_accept_decision = false;

            CommitReq commit;
            commit.mutable_ballot()->CopyFrom(accept_num);

            logger->debug("Sending commit to replicas {}", commit.DebugString());
            // Send commit request to all
            for (auto& pair: stubs) {
                OutCall* call = new OutCall(this, response_cq.get(), types::COMMIT, ServerImpl::RPC_TIMEOUT_MS);
                call->sendCommit(commit, pair.second);
            }

            // Update server state
            if (!is_cross_shard) {
                locks.erase(sender);
                locks.erase(receiver);
                commitTransaction(request, accept_num);
            }
            
            promised = false;
            accepted = false;
            await_prepare_decision = false;
            await_accept_decision = false;
            
            response.set_ack(true);
            is_paxos_running = false;
            paxos_tid = -1;
            return true;
            
        } else if (accept_failures >= ServerImpl::MAJORITY) {
            await_accept_decision = false;
            is_paxos_running = false;
            paxos_tid = -1;
            
            if (!is_cross_shard) {
                locks.erase(sender);
                locks.erase(receiver);
            }
            
            response.set_ack(false);
            return true;        // abort transaction
        }

        return false;           // wait for an accept decision
    }

    return true;
}


void ServerImpl::processPrepareCall(PrepareReq& request, PrepareRes& response) {
    if (i_am_disconnected) {
        response.set_ack(false);
        response.mutable_ballot()->CopyFrom(request.ballot());
        return;
    }
    
    bool ack = false;
    logger->debug("Received prepare {}", request.DebugString());
    if (request.ballot().num() > ballot_num) {
        if (request.last_inserted() == last_inserted) {
            ack = true;
            promised = true;
            promised_num = request.ballot();
            ballot_num = request.ballot().num();
            persistBallotNum();

        } else if (request.last_inserted() > last_inserted) {
            logger->debug("Requesting sync due to prepare");
            // Synchronize
            in_sync = true;
            SyncReq sync;
            sync.set_last_inserted(last_inserted);

            OutCall* call = new OutCall(this, response_cq.get(), types::SYNC, ServerImpl::RPC_TIMEOUT_MS);
            call->sendSync(sync, stubs[request.ballot().server_id()]);
        }
    }

    response.set_ack(ack);
    response.mutable_ballot()->CopyFrom(request.ballot());
    if (ack && accepted) {
        response.mutable_accept_num()->CopyFrom(accept_num);
        response.mutable_accept_val()->CopyFrom(accept_val);
    } else if (!ack) {
        response.set_server_id(server_id);
        if (request.ballot().num() <= ballot_num) response.set_latest_ballot_num(ballot_num);
        if (request.last_inserted() < last_inserted) response.set_last_inserted(last_inserted);
    }
    logger->debug("Prepare response {}", response.DebugString());
}

void ServerImpl::processAcceptCall(AcceptReq& request, AcceptRes& response) {
    if (i_am_disconnected) {
        response.set_ack(false);
        response.mutable_ballot()->CopyFrom(request.ballot());
        return;
    }
    
    bool ack = false;
    logger->debug("Received accept {}", request.DebugString());
    if (promised && request.ballot().num() == promised_num.num()) {
        ack = true;
        accepted = true;
        accept_num = request.ballot();
        accept_val = request.r();

        int sender = accept_val.txn().sender();
        int receiver = accept_val.txn().receiver();
        int amount = accept_val.txn().amount();

        int in_cluster = 0;
        if (isClientInCluster(sender)) {
            logger->debug("Locking {}", sender);
            locks.insert(sender);
            ++in_cluster;
        }
        if (isClientInCluster(receiver)) {
            logger->debug("Locking {}", receiver);
            locks.insert(receiver);
            ++in_cluster;
        }

        if (in_cluster == 1) {
            logger->debug("Preparing transaction having tid {}", accept_val.tid());
            prepareTransaction(accept_val, accept_num);
        }
    }

    response.set_ack(ack);
    response.mutable_ballot()->CopyFrom(request.ballot());
    logger->debug("Accept response {}", response.DebugString());
}

void ServerImpl::processCommitCall(CommitReq& request) {
    if (i_am_disconnected) {
        return;
    }
    
    logger->debug("Received commit {}", request.DebugString());
    int commit_ballot_num = request.ballot().num();
    if (promised_num.num() == commit_ballot_num && accept_num.num() == commit_ballot_num) {
        last_inserted = log.size() - 1;
        last_inserted_ballot = accept_num;
        
        int sender = accept_val.txn().sender();
        int receiver = accept_val.txn().receiver();
        int amount = accept_val.txn().amount();

        int in_cluster = 0;
        if (isClientInCluster(sender)) {
            logger->debug("Unlocking {}", sender);
            locks.erase(sender);
            ++in_cluster;
        }
        if (isClientInCluster(receiver)) {
            logger->debug("Unlocking {}", receiver);
            locks.erase(receiver);
            ++in_cluster;
        }

        if (in_cluster == 2) {
            commitTransaction(accept_val, accept_num);
        }
        promised = false;
        accepted = false;
    }
}

void ServerImpl::processSyncCall(SyncReq& request, SyncRes& response) {
    logger->debug("Received sync {}", request.DebugString());
    int commit_idx = request.last_inserted();
    if (last_inserted > commit_idx) {
        for (int i = commit_idx + 1; i < log.size(); i++) {
            LogEntry* e = response.add_logs();
            getLogEntryFromLocalLog(log[i], e);
        }
        response.set_ack(true);
        response.mutable_last_inserted_ballot()->CopyFrom(last_inserted_ballot);
    } else {
        response.set_ack(false);
    }
    logger->debug("Sync response {}", response.DebugString());
}

void ServerImpl::processGetBalanceCall(BalanceReq& request, BalanceRes& response) {
    int client = request.client();
    response.set_amount(balances[client]);
}

void ServerImpl::getLogEntryFromLocalLog(types::WALEntry& log, LogEntry* entry) {
    Transaction* txn = entry->mutable_txn();
    txn->set_sender(log.txn.sender);
    txn->set_receiver(log.txn.receiver);
    txn->set_amount(log.txn.amount);

    entry->set_tid(log.tid);
    entry->set_ballot_num(log.ballot_num);
    entry->set_ballot_server_id(log.ballot_server_id);
    entry->set_type(static_cast<int>(log.type));
    entry->set_status(static_cast<int>(log.status));
}

void ServerImpl::processGetLogsCall(LogRes& response) {
    for (auto &l: log) {
        LogEntry* e = response.add_logs();
        getLogEntryFromLocalLog(l, e);
    }
}

void ServerImpl::resetDisconnectedState() {
    i_am_disconnected = false;
    for (auto& pair: constants::server_ids) {
        std::string name = pair.first;
        int id = pair.second;
        if (utils::getClusterIdFromServerId(id) == cluster_id) disconnected[id] = false;
    }
}

void ServerImpl::processDisconnectCall(DisconnectReq& request) {
    logger->debug("Received disconnect request {}", request.DebugString());
    resetDisconnectedState();

    for (int i = 0; i < request.servers_size(); i++) {
        std::string name = request.servers(i);
        int id = constants::server_ids[name];

        if (utils::getClusterIdFromServerId(id) == cluster_id) {
            logger->debug("Marking {} as disconnected", name);
            disconnected[id] = true;
        }
        if (id == server_id) i_am_disconnected = true;
    }
}

void ServerImpl::handlePrepareReply(Status& status, PrepareReq& request, PrepareRes& response) {
    if (!await_prepare_decision) return;
    if (!status.ok() && status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED && request.ballot().num() == promised_num.num() && request.ballot().server_id() == server_id) {
        // logger->debug("Status not OK. Error code {}. Response {}", std::to_string(status.error_code()), response.DebugString());
        ++prepare_failures;
    } else if (status.ok() && response.ballot().num() == promised_num.num() && response.ballot().server_id() == server_id) {
        response.ack() ? ++prepare_successes : ++prepare_failures;
        logger->debug("response last inserted: {}, my last inserted {}", response.last_inserted(), last_inserted);
        
        // Leader's ballot number is outdated
        if (response.has_latest_ballot_num()) {
            logger->debug("Updating ballot number");
            ballot_num = response.latest_ballot_num();
            persistBallotNum();
            await_prepare_decision = false;
        }

        // Leader's log is outdated
        if (response.has_last_inserted() && response.last_inserted() > last_inserted) {
            // Synchronize
            logger->debug("Requesting sync due to prepare reject");
            in_sync = true;
            await_prepare_decision = false;
            
            SyncReq sync;
            sync.set_last_inserted(last_inserted);

            OutCall* call = new OutCall(this, response_cq.get(), types::SYNC, ServerImpl::RPC_TIMEOUT_MS);
            call->sendSync(sync, stubs[response.server_id()]);
        }
    }

    logger->debug("Received prepare response {}", response.DebugString());
    logger->debug("Successes {}. Failures {}", prepare_successes, prepare_failures);
}

void ServerImpl::handleAcceptReply(Status& status, AcceptReq& request, AcceptRes& response) {
    if (!await_accept_decision) return;
    if (!status.ok() && status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED && request.ballot().num() == accept_num.num() && request.ballot().server_id() == server_id) {
        ++accept_failures;
    }
    else if (status.ok() && response.ballot().num() == accept_num.num() && response.ballot().server_id() == server_id) {
        response.ack() ? ++accept_successes : ++accept_failures;
    }
}

void ServerImpl::handleSyncReply(Status& status, SyncRes& response) {
    if (status.ok()) {
        logger->debug("Received sync response {}", response.DebugString());
        if (response.ack()) {
            for (int i = 0; i < response.logs_size(); i++) {
                LogEntry e = response.logs(i);
                types::WALEntry entry = {
                    e.tid(),
                    e.ballot_num(),
                    e.ballot_server_id(),
                    { e.txn().sender(), e.txn().receiver(), e.txn().amount() },
                    static_cast<types::TransactionType>(e.type()),
                    static_cast<types::TransactionStatus>(e.status())
                };
                log.push_back(entry);
                wal.insertEntry(entry);
                
                if (entry.status == types::TransactionStatus::COMMITTED && isClientInCluster(entry.txn.sender)) {
                    balances[entry.txn.sender] -= entry.txn.amount;
                    updateBalance(entry.txn.sender, balances[entry.txn.sender]);
                }
                if (entry.status == types::TransactionStatus::COMMITTED && isClientInCluster(entry.txn.receiver)) {
                    balances[entry.txn.receiver] += entry.txn.amount;
                    updateBalance(entry.txn.receiver, balances[entry.txn.receiver]);
                }
            }

            ballot_num = response.last_inserted_ballot().num();
            persistBallotNum();
            last_inserted += response.logs_size();
            last_inserted_ballot = response.last_inserted_ballot();
        }
    }

    in_sync = false;
}

void ServerImpl::updateBalance(int client_id, int balance) {
    auto status = db->Put(leveldb::WriteOptions(), std::to_string(client_id), std::to_string(balance));
    if (!status.ok()) {
        logger->debug("Failed to update client {} balance", client_id);
        logger->debug(status.ToString());
    }
}

void ServerImpl::persistBallotNum() {
    auto status = db->Put(leveldb::WriteOptions(), "__ballot_num__", std::to_string(ballot_num));
    if (!status.ok()) {
        logger->debug("Failed to persist ballot number {}", ballot_num);
        logger->debug(status.ToString());
    }
}

void RunServer(std::string server_name) {
    int id = constants::server_ids[server_name];
    std::string address = constants::server_addresses[server_name];
    ServerImpl server(id, server_name);
    server.run(address);
}

void handler(int signal) {
    ServerImpl::shutdown = true;
}

int main(int argc, char** argv) {
    auto logger = spdlog::stdout_color_mt("console");
    logger->set_level(spdlog::level::info);
    
    if (argc != 4 && argc != 5) {
        logger->error("Usage: server <name> <num_clusters> <cluster_size> [config_filepath]\n");
        exit(1);
    }

    try {
        int num_clusters = std::stoi(argv[2]);
        int cluster_size = std::stoi(argv[3]);
        utils::setupApplicationState(num_clusters, cluster_size);
        if (argc == 5) utils::loadConfig(argv[4]);
        RunServer(std::string(argv[1]));
        
        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
    } catch (std::exception& e) {
        logger->error("Exception: %s", e.what());
    }
}
