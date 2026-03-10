#include "wal.h"
#include <algorithm>

WAL::WAL(const std::string& fileName) : walFile(fileName) {
    logger = spdlog::get("console");
}

void WAL::recover(std::map<int, int>& balances, int cluster_id,
                  int& last_inserted, int& ballot_num,
                  std::vector<types::WALEntry>& log) {
    (void)balances;
    (void)cluster_id;

    std::ifstream inFile(walFile);
    if (!inFile.is_open()) {
        return;
    }

    transferIndex.clear();
    std::string line;
    while (true) {
        std::streampos position = inFile.tellg();
        if (!std::getline(inFile, line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string op;
        long tid;
        int entry_ballot_num;
        int entry_ballot_server_id;
        int sender;
        int receiver;
        int amount;

        if (!(iss >> op >> tid >> entry_ballot_num >> entry_ballot_server_id >> sender >> receiver >> amount)) {
            // Ignore malformed/partial lines from interrupted writes.
            continue;
        }

        ballot_num = std::max(ballot_num, entry_ballot_num);
        types::WALEntry entry = {
            tid,
            entry_ballot_num,
            entry_ballot_server_id,
            { sender, receiver, amount },
            types::TransactionType::CROSS,
            types::TransactionStatus::NO_STATUS
        };

        if (op == "IN_COMMIT" || op == "IS_COMMIT") {
            entry.type = types::TransactionType::INTRA;
            entry.status = types::TransactionStatus::COMMITTED;
            log.push_back(entry);
            last_inserted = static_cast<int>(log.size()) - 1;
        } else if (op == "CS_PREPARE") {
            entry.type = types::TransactionType::CROSS;
            entry.status = types::TransactionStatus::PREPARED;
            transferIndex[tid] = position;
            log.push_back(entry);
            last_inserted = static_cast<int>(log.size()) - 1;
        } else if (op == "CS_COMMIT") {
            entry.type = types::TransactionType::CROSS;
            entry.status = types::TransactionStatus::COMMITTED;
            transferIndex.erase(tid);
            log.push_back(entry);
            last_inserted = static_cast<int>(log.size()) - 1;
        } else if (op == "CS_ABORT") {
            entry.type = types::TransactionType::CROSS;
            entry.status = types::TransactionStatus::ABORTED;
            transferIndex.erase(tid);
            log.push_back(entry);
            last_inserted = static_cast<int>(log.size()) - 1;
        }
    }
}

types::WALEntry WAL::getTransactionFromFile(std::streampos position) {
    std::ifstream inFile(walFile);
    if (!inFile) {
        logger->debug("Error: Unable to open WAL file for reading.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    inFile.seekg(position);
    std::string transaction;
    std::getline(inFile, transaction);
    inFile.close();

    std::istringstream iss(transaction);
    std::string command;
    long tid;
    int ballot_num;
    int ballot_server_id;
    int sender;
    int receiver;
    int amount;
    iss >> command >> tid >> ballot_num >> ballot_server_id >> sender >> receiver >> amount;

    return {
        tid,
        ballot_num,
        ballot_server_id,
        { sender, receiver, amount },
        types::TransactionType::CROSS,
        types::TransactionStatus::PREPARED
    };
}


types::WALEntry WAL::prepareTransaction(TransferReq& request, Ballot& ballot) {
    long tid = request.tid();
    int sender = request.txn().sender();
    int receiver = request.txn().receiver();
    int amount = request.txn().amount();
    int ballot_num = ballot.num();
    int ballot_server_id = ballot.server_id();

    std::ofstream outFile(walFile, std::ios::app);
    if (!outFile) {
        logger->debug("Error: Unable to open WAL file for writing.");
    }

    // Record file position and write transaction
    std::streampos position = outFile.tellp();
    transferIndex[tid] = position;

    outFile << "CS_PREPARE " << tid << " " << ballot_num << " " << ballot_server_id << " " << sender << " " << receiver << " " << amount << "\n";
    outFile.close();

    return {
        tid, 
        ballot_num,
        ballot_server_id,
        { sender, receiver, amount },
        types::TransactionType::CROSS,
        types::TransactionStatus::PREPARED
    };
}

types::WALEntry WAL::commitTransaction(TransferReq& request, Ballot& ballot) {
    long tid = request.tid();
    int sender = request.txn().sender();
    int receiver = request.txn().receiver();
    int amount = request.txn().amount();
    int ballot_num = ballot.num();
    int ballot_server_id = ballot.server_id();

    std::ofstream outFile(walFile, std::ios::app);
    if (!outFile) {
        logger->debug("Error: Unable to open WAL file for writing.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    outFile << "IN_COMMIT " << tid << " " << ballot_num << " " << ballot_server_id << " " << sender << " " << receiver << " " << amount << "\n";
    outFile.close();

    return {
        tid,
        ballot_num,
        ballot_server_id,
        { sender, receiver, amount },
        types::TransactionType::INTRA,
        types::TransactionStatus::COMMITTED
    };
}

types::WALEntry WAL::commitTransaction(TpcTid& request) {
    long tid = request.tid();
    auto it = transferIndex.find(tid);
    if (it == transferIndex.end()) {
        logger->debug("Error: Transaction ID " + std::to_string(tid) + " not found.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    // Retrieve the corresponding transaction
    types::WALEntry txn = getTransactionFromFile(it->second);

    std::ofstream outFile(walFile, std::ios::app);
    if (!outFile) {
        logger->debug("Error: Unable to open WAL file for writing.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    outFile << "CS_COMMIT " << tid << " " << txn.ballot_num << " " << txn.ballot_server_id << " " << txn.txn.sender << " " << txn.txn.receiver << " " << txn.txn.amount << "\n";
    outFile.close();

    // Remove the transaction from the transferIndex
    transferIndex.erase(it);

    txn.status = types::TransactionStatus::COMMITTED;
    return txn;
}

types::WALEntry WAL::abortTransaction(TpcTid& request) {
    long tid = request.tid();
    auto it = transferIndex.find(tid);
    if (it == transferIndex.end()) {
        logger->debug("Error: Transaction ID " + std::to_string(tid) + " not found.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    // Retrieve the corresponding transaction
    types::WALEntry txn = getTransactionFromFile(it->second);

    // Append ABORT message to WAL
    std::ofstream outFile(walFile, std::ios::app);
    if (!outFile) {
        logger->debug("Error: Unable to open WAL file for writing.");
        return { -1, -1, -1, { -1, -1, -1 }, types::TransactionType::CROSS, types::TransactionStatus::NO_STATUS };
    }

    outFile << "CS_ABORT " << tid << " " << txn.ballot_num << " " << txn.ballot_server_id << " " << txn.txn.sender << " " << txn.txn.receiver << " " << txn.txn.amount << "\n";
    outFile.close();

    // Remove the transaction from the transferIndex
    transferIndex.erase(it);

    txn.status = types::TransactionStatus::ABORTED;
    return txn;
}

void WAL::insertEntry(types::WALEntry entry) {
    std::ofstream outFile(walFile, std::ios::app);
    if (!outFile) {
        logger->debug("Error: Unable to open WAL file for writing.");
        return;
    }

    std::streampos position;
    std::string message = entry.type == types::TransactionType::INTRA ? "IS_" : "CS_"; 
    switch (entry.status) {
        case types::TransactionStatus::PREPARED:
            position = outFile.tellp();
            transferIndex[entry.tid] = position;
            message += "PREPARE";
            break;
        case types::TransactionStatus::COMMITTED:
            message += "COMMIT";
            transferIndex.erase(entry.tid);
            break;
        case types::TransactionStatus::ABORTED:
            message += "ABORT";
            transferIndex.erase(entry.tid);
            break;
        default: break;
    }

    outFile << message << " " << entry.tid << " " << entry.ballot_num << " " << entry.ballot_server_id << " " << entry.txn.sender << " " << entry.txn.receiver << " " << entry.txn.amount << "\n";
    outFile.close();
}
