#pragma once

#include <grpcpp/grpcpp.h>
#include "tpc.grpc.pb.h"
#include "absl/log/check.h"

#include "server.h"
#include "../types/types.h"

using grpc::CompletionQueue;
using grpc::ClientContext;
using grpc::ClientAsyncResponseReader;
using grpc::Status;
using google::protobuf::Empty;

using tpc::TpcServer;
using tpc::PrepareReq;
using tpc::PrepareRes;
using tpc::AcceptReq;
using tpc::AcceptRes;
using tpc::CommitReq;
using tpc::SyncReq;
using tpc::SyncRes;

// struct for keeping state and data information
class OutCall {

public:
    OutCall(ServerImpl* server, CompletionQueue* cq, types::RequestTypes type, int timeout_ms);
    void sendPrepare(PrepareReq& req, std::unique_ptr<TpcServer::Stub>& stub_);
    void sendAccept(AcceptReq& req, std::unique_ptr<TpcServer::Stub>& stub_);
    void sendCommit(CommitReq& req, std::unique_ptr<TpcServer::Stub>& stub_);
    void sendSync(SyncReq& req, std::unique_ptr<TpcServer::Stub>& stub_);
    void HandleRPCResponse();

private:
    ServerImpl* server_;
    CompletionQueue* cq_;
    types::RequestTypes type_;
    int timeout_ms_;

    // Container for the data we expect from the server.
    Empty emptyReply;
    PrepareReq prepareReq;
    PrepareRes prepareReply;
    AcceptReq acceptReq;
    AcceptRes acceptReply;
    SyncRes syncReply;

    ClientContext context;

    // Storage for the status of the RPC upon completion.
    Status status;

    std::unique_ptr<ClientAsyncResponseReader<Empty>> emptyReader;
    std::unique_ptr<ClientAsyncResponseReader<PrepareRes>> prepareReader;
    std::unique_ptr<ClientAsyncResponseReader<AcceptRes>> acceptReader;
    std::unique_ptr<ClientAsyncResponseReader<SyncRes>> syncReader;
};
