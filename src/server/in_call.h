#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include "absl/log/check.h"

#include "server.h"
#include "../types/types.h"

using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::ServerAsyncResponseWriter;
using grpc::Status;
using grpc::Alarm;
using google::protobuf::Empty;

using tpc::TpcServer;
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
using tpc::TpcTid;
using tpc::DisconnectReq;

class InCall {
public:
    InCall(TpcServer::AsyncService* service, ServerImpl* server, ServerCompletionQueue* cq, types::RequestTypes type, int retry_timeout_ms);
    void Proceed();
    void Retry();

private:
    TpcServer::AsyncService* service_;
    ServerImpl* server_;
    ServerCompletionQueue* cq_;
    ServerContext ctx_;
    std::unique_ptr<grpc::Alarm> alarm_;

    // Different request and response types that server can expect to
    // receive and send to the client
    Empty empty;
    TpcTid tpcTid;
    TransferReq transferReq;
    TransferRes transferRes;
    BalanceReq balanceReq;
    BalanceRes balanceRes;
    LogRes logRes;
    PrepareReq prepareReq;
    PrepareRes prepareRes;
    AcceptReq acceptReq;
    AcceptRes acceptRes;
    CommitReq commitReq;
    SyncReq syncReq;
    SyncRes syncRes;
    DisconnectReq disconnectReq;

    // The means to get back to the client.
    ServerAsyncResponseWriter<google::protobuf::Empty> emptyResponder;
    ServerAsyncResponseWriter<TransferRes> transferResponder;
    ServerAsyncResponseWriter<BalanceRes> balanceResponder;
    ServerAsyncResponseWriter<LogRes> logResponder;
    ServerAsyncResponseWriter<PrepareRes> prepareResponder;
    ServerAsyncResponseWriter<AcceptRes> acceptResponder;
    ServerAsyncResponseWriter<SyncRes> syncResponder;

    enum CallStatus { CREATE, PROCESS, RETRY, FINISH };
    CallStatus status_;  // The current serving state.
    types::RequestTypes type_;
    int retry_timeout_ms_;
};
