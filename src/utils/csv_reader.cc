#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include "csv_reader.h"
#include "../constants.h"
        

void CSVReader::stripLineEndings(std::string& line) {
    line.erase(std::remove_if(line.begin(), line.end(), 
    [](char c) { return c == '\r' || c == '\n' || c == ' '; }), line.end());
}

types::Transaction CSVReader::parseTransaction(const std::string& column) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream stream(column);
    char c;

    while (stream.get(c)) {
        if (c == '(' || c == ')' || c == ' ') continue;
        else if (c == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(field);

    int sender = std::stoi(fields.at(0));
    int receiver = std::stoi(fields.at(1));
    int amount = std::stoi(fields.at(2));

    struct types::Transaction t = { sender, receiver, amount };
    return t;
}
        
std::vector<std::string> CSVReader::parseServers(const std::string& column) {
    std::vector<std::string> servers;
    std::string server;
    std::istringstream stream(column);
    char c;

    while (stream.get(c)) {
        if (c == '[' || c == ']' || c == ' ') continue;
        else if (c == ',') {
            servers.push_back(server);
            server.clear();
        } else {
            server += c;
        }
    }
    servers.push_back(server);

    return servers;
}

types::CSVLine CSVReader::parseCSVLine(const std::string& line) {
    std::vector<std::string> columns;
    std::stringstream stream(line);
    std::string column;
    bool inQuotes = false;
    char c;

    while (stream.get(c)) {
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == ',' && !inQuotes) {
            columns.push_back(column);
            column.clear();
        } else {
            column += c;
        }
    }
    columns.push_back(column);

    // Parse the set number
    int setNumber = -1;
    if (!columns.at(0).empty()) {
        setNumber = std::stoi(columns.at(0));
    }

    // Parse the transaction
    types::Transaction t = this->parseTransaction(columns.at(1));

    // Parse the set of alive servers
    std::vector<std::string> aliveServers;
    if (!columns.at(2).empty()) {
        aliveServers = this->parseServers(columns.at(2));
    }

    // Parse the leaders
    std::vector<std::string> leaders;
    if (!columns.at(3).empty()) {
        leaders = this->parseServers(columns.at(3));
    }

    return { setNumber, t, aliveServers, leaders };
}

    
CSVReader::CSVReader(const std::string& filename) {
    logger = spdlog::get("console");
    this->filename = filename;
    this->file.open(filename);

    if (!this->file) {
        throw std::runtime_error("Error: Could not open the file: " + filename);
    }

    this->currentSetNo = 0;
    this->currentTransactionSet = {
        0,
        std::vector<types::Transaction>(),
        std::vector<std::string>()
    };

    this->nextTransactionSet = {
        0,
        std::vector<types::Transaction>(),
        std::vector<std::string>()
    };
}

int CSVReader::readNextSet(types::TransactionSet& t) {
    std::cout << "reading next set" << std::endl;
    this->currentSetNo += 1;
    // swap current and next transaction sets
    std::swap(this->currentTransactionSet.servers, this->nextTransactionSet.servers);
    std::swap(this->currentTransactionSet.leaders, this->nextTransactionSet.leaders);
    std::swap(this->currentTransactionSet.transactions, this->nextTransactionSet.transactions);

    // // clear the state of next transaction set
    this->nextTransactionSet.servers.clear();
    this->nextTransactionSet.transactions.clear();
    std::string line;
    while (std::getline(this->file, line)) {
        this->stripLineEndings(line);
        types::CSVLine l = this->parseCSVLine(line);
        if (l.set_no == this->currentSetNo) {
            // 1st row of 1st set
            this->currentTransactionSet.transactions.push_back(l.transaction);
            this->currentTransactionSet.servers = l.servers;
            this->currentTransactionSet.leaders = l.leaders;
        } else if (l.set_no == this->currentSetNo + 1) {
            // 1st row of any but 1st set
            this->nextTransactionSet.transactions.push_back(l.transaction);
            this->nextTransactionSet.servers = l.servers;
            this->nextTransactionSet.leaders = l.leaders;
            break;
        } else {
            // Any but 1st row of any set
            this->currentTransactionSet.transactions.push_back(l.transaction);
        } 
    }

    if (this->currentTransactionSet.transactions.empty()) {
        return 0;
    }

    t.set_no = this->currentSetNo;
    t.servers = this->currentTransactionSet.servers;
    t.leaders = this->currentTransactionSet.leaders;
    t.transactions = this->currentTransactionSet.transactions;

    // Get a list of disconnected servers
    std::vector<std::string> disconnected;
    for (auto& s: constants::server_ids) {
        if (std::find(t.servers.begin(), t.servers.end(), s.first) == t.servers.end()) disconnected.push_back(s.first);
    }
    t.disconnected = disconnected;
    return this->currentTransactionSet.transactions.size();
} 

CSVReader::~CSVReader() {
    if (this->file.is_open()) {
        this->file.close();
        std::cout << this->filename << " closed..." << std::endl;
    }
}