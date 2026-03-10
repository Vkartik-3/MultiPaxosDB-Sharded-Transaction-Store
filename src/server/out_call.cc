#include "out_call.h"

OutCall::OutCall(ServerImpl* server, CompletionQueue* cq, types::RequestTypes type, int timeout_ms):
    server_(server), cq_(cq), type_(type), timeout_ms_(timeout_ms) {}

void OutCall::HandleRPCResponse() {
    switch (type_) {
        case types::RequestTypes::PREPARE:
            server_->handlePrepareReply(status, prepareReq, prepareReply);
            break;
        case types::RequestTypes::ACCEPT:
            server_->handleAcceptReply(status, acceptReq, acceptReply);
            break;
        case types::RequestTypes::SYNC:
            server_->handleSyncReply(status, syncReply);
            break;
        default:
            break;
    }

    delete this;
}

void OutCall::sendPrepare(PrepareReq& request, std::unique_ptr<TpcServer::Stub>& stub_) {    
    prepareReq.CopyFrom(request);

    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);
    context.set_deadline(deadline);
    prepareReader = stub_->PrepareAsyncPrepare(&context, request, cq_);
    prepareReader->StartCall();
    prepareReader->Finish(&prepareReply, &status, (void*)this);
}

void OutCall::sendAccept(AcceptReq& request, std::unique_ptr<TpcServer::Stub>& stub_) {  
    acceptReq.CopyFrom(request);

    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);
    context.set_deadline(deadline);
    acceptReader = stub_->PrepareAsyncAccept(&context, request, cq_);
    acceptReader->StartCall();
    acceptReader->Finish(&acceptReply, &status, (void*)this);
}

void OutCall::sendCommit(CommitReq& request, std::unique_ptr<TpcServer::Stub>& stub_) {  
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);  
    context.set_deadline(deadline);
    emptyReader = stub_->PrepareAsyncCommit(&context, request, cq_);
    emptyReader->StartCall();
    emptyReader->Finish(&emptyReply, &status, (void*)this);
}

void OutCall::sendSync(SyncReq& request, std::unique_ptr<TpcServer::Stub>& stub_) {   
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_); 
    context.set_deadline(deadline);
    syncReader = stub_->PrepareAsyncSync(&context, request, cq_);
    syncReader->StartCall();
    syncReader->Finish(&syncReply, &status, (void*)this);
}