#include <grpcpp/grpcpp.h>
#include "absl/log/check.h"
#include "tpc.grpc.pb.h"

#include <vector>
#include <spdlog/spdlog.h>

#include "../types/types.h"

using grpc::CompletionQueue;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::Status;
using google::protobuf::Empty;

using tpc::TpcServer;
using tpc::TransferReq;
using tpc::TransferRes;
using tpc::TpcTid;

class Client {
public:
    std::shared_ptr<spdlog::logger> logger;

    Client();
    void updateDisconnected(std::vector<std::string> servers);
    void processTransactions(std::vector<types::Transaction> transactions, std::vector<std::string> leaders);
    void printBalance(int client_id);
    void printDatastore();
    void printPerformance();
    void consumeReplies();

private:
    void sendTransfer(TransferReq& request, std::string leader);
    void tpcPrepare(TransferReq& request, std::string leader);
    void tpcCommit(TpcTid& request, int sender_cluster, int receiver_cluster);
    void tpcAbort(TpcTid& request, int sender_cluster, int receiver_cluster);


    std::map<std::string, std::unique_ptr<TpcServer::Stub>> stubs;
    const static int RPC_TIMEOUT_MS = 10;
    CompletionQueue cq;

    struct ClientCall {
        ClientContext context;
        Status status;

        types::RequestTypes type;
        bool is_cross_shard;
        TransferRes reply;
        Empty empty;
        std::unique_ptr<ClientAsyncResponseReader<TransferRes>> transferReader;
        std::unique_ptr<ClientAsyncResponseReader<Empty>> emptyReader;
    };

    struct TpcPrepareRes {
        types::Transaction txn;
        int successes;
        int failures;
        long start_ns;  // wall-clock start for latency measurement
        bool is_cross_shard;
    };

    std::map<long, TpcPrepareRes> processing;
    std::vector<long> latencies;  // per-transaction latency in nanoseconds
    std::vector<long> intra_latencies;
    std::vector<long> cross_latencies;
    bool current_is_cross_shard = false;
    int transactions_processed;
    std::chrono::steady_clock::time_point wall_start;
    std::chrono::steady_clock::time_point wall_end;
    bool wall_started;
};
