#pragma once

#include <string>
#include <fstream>
#include <spdlog/spdlog.h>
#include "../types/types.h"

class CSVReader {
public:
    CSVReader(const std::string& filename);
    int readNextSet(types::TransactionSet& set);
    ~CSVReader();

private:
    std::shared_ptr<spdlog::logger> logger;
    std::string filename;
    std::ifstream file;
    int currentSetNo;
    types::TransactionSet currentTransactionSet;
    types::TransactionSet nextTransactionSet;
    
    void stripLineEndings(std::string& line);
    types::Transaction parseTransaction(const std::string& column);
    std::vector<std::string> parseServers(const std::string& column);
    types::CSVLine parseCSVLine(const std::string& line);
};