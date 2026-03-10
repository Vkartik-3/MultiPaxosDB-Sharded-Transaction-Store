#include "in_call.h"

InCall::InCall(TpcServer::AsyncService* service, ServerImpl* server, ServerCompletionQueue* cq, types::RequestTypes type, int retry_timeout_ms):
            emptyResponder(&ctx_),
            transferResponder(&ctx_), 
            balanceResponder(&ctx_), 
            logResponder(&ctx_), 
            prepareResponder(&ctx_),
            acceptResponder(&ctx_),
            syncResponder(&ctx_) {
        service_ = service;
        server_ = server;
        cq_ = cq;
        status_ = CREATE;
        type_ = type;
        retry_timeout_ms_ = retry_timeout_ms;
        Proceed();
      }

void InCall::Proceed() {
    if (status_ == CREATE) {
        status_ = PROCESS;
        switch (type_) {
            case types::RequestTypes::TPC_PREPARE:
                service_->RequestTpcPrepare(&ctx_, &transferReq, &transferResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::TPC_COMMIT:
                service_->RequestTpcCommit(&ctx_, &tpcTid, &emptyResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::TPC_ABORT:
                service_->RequestTpcAbort(&ctx_, &tpcTid, &emptyResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::TRANSFER: 
                service_->RequestTransfer(&ctx_, &transferReq, &transferResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::BALANCE:
                service_->RequestBalance(&ctx_, &balanceReq, &balanceResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::LOGS:
                service_->RequestLogs(&ctx_, &empty, &logResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::PREPARE:
                service_->RequestPrepare(&ctx_, &prepareReq, &prepareResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::ACCEPT:
                service_->RequestAccept(&ctx_, &acceptReq, &acceptResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::COMMIT:
                service_->RequestCommit(&ctx_, &commitReq, &emptyResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::SYNC:
                service_->RequestSync(&ctx_, &syncReq, &syncResponder, cq_, cq_, this);
                break;
            case types::RequestTypes::DISCONNECT:
                service_->RequestDisconnect(&ctx_, &disconnectReq, &emptyResponder, cq_, cq_, this);
                break;
        }
        
    } else if (status_ == PROCESS || status_ == RETRY) {
        if (status_ == PROCESS) new InCall(service_, server_, cq_, type_, retry_timeout_ms_);
        switch (type_) {
            case types::RequestTypes::TPC_PREPARE:
                if (!server_->processTpcPrepare(transferReq, transferRes)) {
                    status_ = RETRY;
                    Retry();
                    break;
                }
                transferResponder.Finish(transferRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::TPC_COMMIT:
                server_->processTpcDecision(tpcTid, true);
                emptyResponder.Finish(empty, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::TPC_ABORT:
                server_->processTpcDecision(tpcTid, false);
                emptyResponder.Finish(empty, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::TRANSFER:
                if (!server_->processTransferCall(transferReq, transferRes)) {
                    status_ = RETRY;
                    Retry();
                    break;
                }
                transferResponder.Finish(transferRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::PREPARE:
                server_->processPrepareCall(prepareReq, prepareRes);
                prepareResponder.Finish(prepareRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::ACCEPT:
                server_->processAcceptCall(acceptReq, acceptRes);
                acceptResponder.Finish(acceptRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::COMMIT:
                server_->processCommitCall(commitReq);
                emptyResponder.Finish(empty, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::SYNC:
                server_->processSyncCall(syncReq, syncRes);
                syncResponder.Finish(syncRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::BALANCE:
                server_->processGetBalanceCall(balanceReq, balanceRes);
                balanceResponder.Finish(balanceRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::LOGS:
                server_->processGetLogsCall(logRes);
                logResponder.Finish(logRes, Status::OK, this);
                status_ = FINISH;
                break;
            case types::RequestTypes::DISCONNECT:
                server_->processDisconnectCall(disconnectReq);
                emptyResponder.Finish(empty, Status::OK, this);
                status_ = FINISH;
                break;
        }

    } else {
        CHECK_EQ(status_, FINISH);
        delete this;
    }
}

void InCall::Retry() {
    alarm_ = std::make_unique<grpc::Alarm>();
    alarm_->Set(cq_, gpr_time_from_millis(retry_timeout_ms_, GPR_TIMESPAN), this);
}