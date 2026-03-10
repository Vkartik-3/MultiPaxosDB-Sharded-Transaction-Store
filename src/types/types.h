#pragma once
#include <vector>

namespace types {
    enum RequestTypes {
        TPC_PREPARE,
        TPC_COMMIT,
        TPC_ABORT,
        TRANSFER,
        PREPARE,
        ACCEPT,
        COMMIT,
        SYNC,
        BALANCE,
        LOGS,
        DISCONNECT
    };

    struct Transaction {
        int sender;
        int receiver;
        int amount;
    };

    enum TransactionType {
        INTRA,
        CROSS
    };

    enum TransactionStatus {
        NO_STATUS,
        PREPARED,
        COMMITTED,
        ABORTED
    };

    struct WALEntry {
        long tid;
        int ballot_num;
        int ballot_server_id;
        Transaction txn;
        TransactionType type;
        TransactionStatus status;
    };

    struct TransactionSet {
        int set_no;
        std::vector<Transaction> transactions;
        std::vector<std::string> servers;
        std::vector<std::string> leaders;
        std::vector<std::string> disconnected;
    };

    struct CSVLine {
        int set_no;
        Transaction transaction;
        std::vector<std::string> servers;
        std::vector<std::string> leaders;
    };

    enum Command {
        PROCESS_NEXT_SET,
        PRINT_BALANCE,
        PRINT_DATASTORE,
        PRINT_PERFORMANCE,
        EXIT
    };

    struct AppCommand {
        Command command;
        int client_id;
    };
}