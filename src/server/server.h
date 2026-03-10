#pragma once
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <leveldb/db.h>
#include "tpc.grpc.pb.h"

#include <string.h>
#include <set>
#include <map>
#include <chrono>

#include "../types/types.h"
#include "wal.h"

using grpc::Server;
using grpc::CompletionQueue;
using grpc::ServerCompletionQueue;
using grpc::Status;
using grpc::StatusCode;

using tpc::TpcServer;
using tpc::Ballot;
using tpc::Transaction;
using tpc::TransferReq;
using tpc::TransferRes;
using tpc::BalanceReq;
using tpc::BalanceRes;
using tpc::LogRes;
using tpc::PrepareReq;
using tpc::PrepareRes;
using tpc::AcceptReq;
using tpc::AcceptRes;
using tpc::CommitReq;
using tpc::SyncReq;
using tpc::SyncRes;
using tpc::DisconnectReq;
using tpc::TpcTid;
using tpc::LogEntry;

// Server Implementation
class ServerImpl final {
public:
    ServerImpl(int, std::string);
    void run(std::string);
    ~ServerImpl();
    void HandleRPCs();

    void handlePrepareReply(Status&, PrepareReq&, PrepareRes&);
    void handleAcceptReply(Status&, AcceptReq&, AcceptRes&);
    void handleSyncReply(Status&, SyncRes&);
    
    bool processTpcPrepare(TransferReq&, TransferRes&);
    void processTpcDecision(TpcTid&, bool);

    bool processTransferCall(TransferReq&, TransferRes&);
    void processPrepareCall(PrepareReq&, PrepareRes&);
    void processAcceptCall(AcceptReq&, AcceptRes&);
    void processCommitCall(CommitReq&);
    void processSyncCall(SyncReq&, SyncRes&);
    void processGetBalanceCall(BalanceReq&, BalanceRes&);
    void processGetLogsCall(LogRes&);
    void processDisconnectCall(DisconnectReq&);
    static bool shutdown;
    
private:
    bool runPaxos(TransferReq&, TransferRes&, bool write_to_wal);
    void prepareTransaction(TransferReq& request, Ballot& ballot);
    void commitTransaction(TransferReq& request, Ballot& ballot);
    void getLogEntryFromLocalLog(types::WALEntry& log, LogEntry* entry);
    bool isClientInCluster(int client_id);
    void resetDisconnectedState();
    void updateBalance(int client_id, int balance);
    void persistBallotNum();

    std::shared_ptr<spdlog::logger> logger;
    int server_id;
    std::string server_name;
    int cluster_id;

    std::unique_ptr<ServerCompletionQueue> request_cq;
    std::unique_ptr<CompletionQueue> response_cq;
    TpcServer::AsyncService service;
    std::unique_ptr<Server> server;
    std::map<int, std::unique_ptr<TpcServer::Stub>> stubs;

    bool i_am_disconnected;
    std::map<int, bool> disconnected;

    const static int CLUSTER_SIZE = 3;
    const static int MAJORITY = 2;
    const static int RETRY_TIMEOUT_MS = 10;
    const static int RPC_TIMEOUT_MS = 10;

    std::map<int, int> balances;
    std::set<int> locks;
    std::map<long, std::chrono::steady_clock::time_point> prepareTimestamps;
    static constexpr int TPC_LOCK_TIMEOUT_MS = 5000;
    leveldb::DB* db;
    WAL wal;
    std::vector<types::WALEntry> log;
    bool is_paxos_running;
    long paxos_tid;

    int ballot_num;
    bool promised;
    Ballot promised_num;

    bool accepted;
    Ballot accept_num;
    TransferReq accept_val;

    int last_inserted;
    Ballot last_inserted_ballot;

    bool in_sync;
    
    int prepare_successes;
    int prepare_failures;
    int accept_successes;
    int accept_failures;
    int commit_successes;
    int commit_failures;
    bool await_prepare_decision;
    bool await_accept_decision;
};
