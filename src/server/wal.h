#include "tpc.grpc.pb.h"

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "../types/types.h"

using tpc::TransferReq;
using tpc::TpcTid;
using tpc::Ballot;

class WAL {
private:
    std::shared_ptr<spdlog::logger> logger;
    const std::string walFile;
    std::unordered_map<long, std::streampos> transferIndex; // Map for quick lookup of TRANSFER transactions

    // Helper function to retrieve transaction from the WAL file
    types::WALEntry getTransactionFromFile(std::streampos position);

public:
    WAL(const std::string& fileName);

    // Inserts a 2PC prepared transaction into the WAL
    types::WALEntry prepareTransaction(TransferReq& request, Ballot& ballot);

    // Commits an intra shard transaction
    types::WALEntry commitTransaction(TransferReq& request, Ballot& ballot);

    // Commits a cross shard transaction
    types::WALEntry commitTransaction(TpcTid& request);

    // Aborts a cross shard transaction
    types::WALEntry abortTransaction(TpcTid& request);

    // Inserts an entry into the WAL during synchronization
    void insertEntry(types::WALEntry entry);

    // Replays WAL metadata on startup without reapplying balances.
    void recover(std::map<int, int>& balances, int cluster_id,
                 int& last_inserted, int& ballot_num,
                 std::vector<types::WALEntry>& log);
};
