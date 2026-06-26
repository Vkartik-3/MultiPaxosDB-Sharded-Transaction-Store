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
    ballot_num = 0;
    promised = false;
    accepted = false;
    in_sync = false;
    await_prepare_decision = false;
    await_accept_decision = false;
    last_inserted = -1;
    paxos_index = -1;
}

// A log entry counts toward paxos_index iff it was produced by a Paxos-agreed
// append: an intra-shard commit (INTRA/COMMITTED) or a cross-shard 2PC prepare
// (CROSS/PREPARED). 2PC decision/abort entries (CROSS/COMMITTED, *_ABORTED) do
// not count — they are appended asynchronously and independently per replica.
static bool isPaxosEntry(const types::WALEntry& e) {
    return (e.type == types::TransactionType::INTRA && e.status == types::TransactionStatus::COMMITTED)
        || (e.type == types::TransactionType::CROSS && e.status == types::TransactionStatus::PREPARED);
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
    paxos_index = -1;
    for (auto& e: log) if (isPaxosEntry(e)) paxos_index++;
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

void ServerImpl::processTpcDecision(TpcTid& request, bool is_commit) {
    if (i_am_disconnected) return;
    
    prepareTimestamps.erase(request.tid());
    auto entry = is_commit ? wal.commitTransaction(request) : wal.abortTransaction(request);
    if (entry.tid != -1) {
        log.push_back(entry);
    } else {
        return;
    }

    // Locks are released regardless; balances apply at most once per tid per server.
    bool first_apply = is_commit && balance_applied.insert(entry.tid).second;
    if (isClientInCluster(entry.txn.sender)) {
        locks.erase(entry.txn.sender);
        if (first_apply) {
            balances[entry.txn.sender] -= entry.txn.amount;
            updateBalance(entry.txn.sender, balances[entry.txn.sender]);
        }
    }
    if (isClientInCluster(entry.txn.receiver)) {
        locks.erase(entry.txn.receiver);
        if (first_apply) {
            balances[entry.txn.receiver] += entry.txn.amount;
            updateBalance(entry.txn.receiver, balances[entry.txn.receiver]);
        }
    }
}

void ServerImpl::prepareTransaction(TransferReq& request, Ballot& ballot) {
    auto entry = wal.prepareTransaction(request, ballot);
    log.push_back(entry);

    last_inserted = log.size() - 1;
    last_inserted_ballot = ballot;
    paxos_index++;
    prepareTimestamps[request.tid()] = std::chrono::steady_clock::now();
}

void ServerImpl::commitTransaction(TransferReq& request, Ballot& ballot) {
    if (!balance_applied.insert(request.tid()).second) return;  // already applied on this server
    auto entry = wal.commitTransaction(request, ballot);
    log.push_back(entry);
    balances[entry.txn.sender] -= entry.txn.amount;
    balances[entry.txn.receiver] += entry.txn.amount;
    updateBalance(entry.txn.sender, balances[entry.txn.sender]);
    updateBalance(entry.txn.receiver, balances[entry.txn.receiver]);

    last_inserted = log.size() - 1;
    last_inserted_ballot = ballot;
    paxos_index++;
}

void ServerImpl::enqueueClientTxn(InCall* call, const TransferReq& req, bool is_cross_shard) {
    if (i_am_disconnected) {
        call->completeTransfer(false, req.tid());
        return;
    }
    pending_.push_back({req, call, is_cross_shard});
    maybeStartRound();
}

// Promote the accumulated pending_ batch into a single in-flight round, unless a
// round is already running or we're mid-sync.
void ServerImpl::maybeStartRound() {
    if (is_paxos_running || in_sync || pending_.empty()) return;
    startRound();
}

void ServerImpl::startRound() {
    current_.swap(pending_);
    pending_.clear();
    is_paxos_running = true;
    reissuePrepareForCurrent();
}

// (Re)run the prepare phase for current_ with a fresh ballot. Used both to start
// a round and to retry after a sync or a stale-ballot rejection.
void ServerImpl::reissuePrepareForCurrent() {
    Ballot ballot;
    ballot.set_num(++ballot_num);
    ballot.set_server_id(server_id);
    persistBallotNum();

    promised = true;
    promised_num = ballot;
    await_prepare_decision = true;
    await_accept_decision = false;
    prepare_successes = 1;
    prepare_failures = 0;

    PrepareReq prepare;
    prepare.mutable_ballot()->CopyFrom(ballot);
    prepare.set_paxos_index(paxos_index);

    logger->debug("Sending prepare for batch of {} to replicas", current_.size());
    for (auto& pair: stubs) {
        OutCall* call = new OutCall(this, response_cq.get(), types::PREPARE, ServerImpl::RPC_TIMEOUT_MS);
        call->sendPrepare(prepare, pair.second);
    }
}

// Prepare quorum reached: validate each entry independently. Entries that fail
// their preconditions (locked account or insufficient balance, including a
// conflict with an earlier entry in the same batch) are aborted immediately; the
// survivors are locked and proposed together in one Accept.
void ServerImpl::onPrepareQuorum() {
    await_prepare_decision = false;

    std::vector<BatchEntry> kept;
    for (auto& e: current_) {
        int sender = e.request.txn().sender();
        int receiver = e.request.txn().receiver();
        int amount = e.request.txn().amount();
        bool sender_in_cluster = isClientInCluster(sender);
        bool receiver_in_cluster = isClientInCluster(receiver);

        if ((sender_in_cluster && locks.count(sender))
                || (receiver_in_cluster && locks.count(receiver))
                || (sender_in_cluster && balances[sender] < amount)) {
            e.call->completeTransfer(false, e.request.tid());
            continue;
        }

        if (sender_in_cluster) locks.insert(sender);
        if (receiver_in_cluster) locks.insert(receiver);
        if (e.is_cross_shard) prepareTransaction(e.request, promised_num);
        kept.push_back(e);
    }
    current_.swap(kept);

    if (current_.empty()) {
        finishRound();
        return;
    }

    accepted = true;
    accept_num = promised_num;
    accept_batch.clear();
    for (auto& e: current_) accept_batch.push_back(e.request);

    await_accept_decision = true;
    accept_successes = 1;
    accept_failures = 0;

    AcceptReq accept;
    accept.mutable_ballot()->CopyFrom(promised_num);
    for (auto& e: current_) accept.add_batch()->CopyFrom(e.request);

    logger->debug("Sending accept for batch of {} to replicas", current_.size());
    for (auto& pair: stubs) {
        OutCall* call = new OutCall(this, response_cq.get(), types::ACCEPT, ServerImpl::RPC_TIMEOUT_MS);
        call->sendAccept(accept, pair.second);
    }
}

// Accept quorum reached: commit the batch. Intra-shard entries apply and release
// their locks; cross-shard (2PC prepare) entries reply PREPARED and keep their
// locks until the 2PC decision arrives.
void ServerImpl::onAcceptQuorum() {
    await_accept_decision = false;

    CommitReq commit;
    commit.mutable_ballot()->CopyFrom(accept_num);
    for (auto& pair: stubs) {
        OutCall* call = new OutCall(this, response_cq.get(), types::COMMIT, ServerImpl::RPC_TIMEOUT_MS);
        call->sendCommit(commit, pair.second);
    }

    for (auto& e: current_) {
        if (!e.is_cross_shard) {
            locks.erase(e.request.txn().sender());
            locks.erase(e.request.txn().receiver());
            commitTransaction(e.request, accept_num);
        }
        e.call->completeTransfer(true, e.request.tid());
    }

    finishRound();
}

// Quorum could not be reached for the round. Abort every entry still in flight,
// releasing locks acquired during prepare (accept-phase failures only).
void ServerImpl::onRoundAbort(bool prepare_phase) {
    for (auto& e: current_) {
        if (!prepare_phase && !e.is_cross_shard) {
            locks.erase(e.request.txn().sender());
            locks.erase(e.request.txn().receiver());
        }
        e.call->completeTransfer(false, e.request.tid());
    }
    finishRound();
}

void ServerImpl::finishRound() {
    is_paxos_running = false;
    promised = false;
    accepted = false;
    await_prepare_decision = false;
    await_accept_decision = false;
    current_.clear();
    accept_batch.clear();
    maybeStartRound();   // drain anything that accumulated while this round ran
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
        if (request.paxos_index() == paxos_index) {
            ack = true;
            promised = true;
            promised_num = request.ballot();
            ballot_num = request.ballot().num();
            persistBallotNum();

        } else if (request.paxos_index() > paxos_index) {
            // Behind on the agreed log: catch up. The sync range still uses the
            // raw log index (last_inserted), not paxos_index.
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
        for (auto& v: accept_batch) response.add_accept_val()->CopyFrom(v);
    } else if (!ack) {
        response.set_server_id(server_id);
        if (request.ballot().num() <= ballot_num) response.set_latest_ballot_num(ballot_num);
        if (request.paxos_index() < paxos_index) response.set_paxos_index(paxos_index);
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

        accept_batch.clear();
        for (auto& r: request.batch()) {
            accept_batch.push_back(r);
            TransferReq& stored = accept_batch.back();

            int sender = stored.txn().sender();
            int receiver = stored.txn().receiver();

            int in_cluster = 0;
            if (isClientInCluster(sender)) {
                locks.insert(sender);
                ++in_cluster;
            }
            if (isClientInCluster(receiver)) {
                locks.insert(receiver);
                ++in_cluster;
            }

            // Exactly one side in this cluster => cross-shard 2PC prepare.
            if (in_cluster == 1) {
                prepareTransaction(stored, accept_num);
            }
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
        for (auto& r: accept_batch) {
            int sender = r.txn().sender();
            int receiver = r.txn().receiver();

            int in_cluster = 0;
            if (isClientInCluster(sender)) {
                locks.erase(sender);
                ++in_cluster;
            }
            if (isClientInCluster(receiver)) {
                locks.erase(receiver);
                ++in_cluster;
            }

            // Both sides in this cluster => intra-shard: apply now. Cross-shard
            // entries wait for the 2PC decision to apply balances.
            if (in_cluster == 2) {
                commitTransaction(r, accept_num);
            }
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
        ++prepare_failures;
    } else if (status.ok() && response.ballot().num() == promised_num.num() && response.ballot().server_id() == server_id) {
        response.ack() ? ++prepare_successes : ++prepare_failures;

        // Leader's ballot number is outdated: adopt the higher number and retry
        // the same batch with a fresh ballot.
        if (response.has_latest_ballot_num()) {
            ballot_num = response.latest_ballot_num();
            persistBallotNum();
            reissuePrepareForCurrent();
            return;
        }

        // Leader's log is outdated: sync first, then retry the batch.
        if (response.has_paxos_index() && response.paxos_index() > paxos_index) {
            in_sync = true;
            await_prepare_decision = false;

            SyncReq sync;
            sync.set_last_inserted(last_inserted);

            OutCall* call = new OutCall(this, response_cq.get(), types::SYNC, ServerImpl::RPC_TIMEOUT_MS);
            call->sendSync(sync, stubs[response.server_id()]);
            return;
        }
    }

    if (prepare_successes >= MAJORITY) {
        onPrepareQuorum();
    } else if (prepare_failures >= MAJORITY) {
        onRoundAbort(true);
    }
}

void ServerImpl::handleAcceptReply(Status& status, AcceptReq& request, AcceptRes& response) {
    if (!await_accept_decision) return;
    if (!status.ok() && status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED && request.ballot().num() == accept_num.num() && request.ballot().server_id() == server_id) {
        ++accept_failures;
    } else if (status.ok() && response.ballot().num() == accept_num.num() && response.ballot().server_id() == server_id) {
        response.ack() ? ++accept_successes : ++accept_failures;
    }

    if (accept_successes >= MAJORITY) {
        onAcceptQuorum();
    } else if (accept_failures >= MAJORITY) {
        onRoundAbort(false);
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
                if (isPaxosEntry(entry)) paxos_index++;

                // Only move balances for entries this server hasn't already applied.
                bool first_apply = entry.status == types::TransactionStatus::COMMITTED
                                   && balance_applied.insert(entry.tid).second;
                if (first_apply && isClientInCluster(entry.txn.sender)) {
                    balances[entry.txn.sender] -= entry.txn.amount;
                    updateBalance(entry.txn.sender, balances[entry.txn.sender]);
                }
                if (first_apply && isClientInCluster(entry.txn.receiver)) {
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
    // If a round was paused waiting for sync, retry its prepare now; otherwise
    // start a round for anything that queued up while we were syncing.
    if (!current_.empty()) {
        reissuePrepareForCurrent();
    } else {
        maybeStartRound();
    }
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
