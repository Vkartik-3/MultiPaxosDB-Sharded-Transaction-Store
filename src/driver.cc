#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <regex>
#include <vector>
#include <thread>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "utils/commands_parser.h"
#include "utils/csv_reader.h"
#include "utils/utils.h"
#include "client/client.h"
#include "types/types.h"
#include "constants.h"


void mainloop(CSVReader& reader, Client& client) {
    CommandsParser parser;
    bool exit = false;
    std::string command;
    types::AppCommand c;
    types::TransactionSet set;

    while(!exit) {
        parser.promptUserForCommand(command);
        try {
            c = parser.parseCommand(command);
        } catch (const std::invalid_argument& e) {
            std::cout << "Invalid command. Try again. " << std::endl;
            continue;
        }

        try {
            switch (c.command) {
                case types::PROCESS_NEXT_SET:
                    if (!reader.readNextSet(set)) {
                        std::cout << "No more transaction sets to read..." << std::endl;
                    } else {
                        client.updateDisconnected(set.disconnected);
                        client.processTransactions(set.transactions, set.leaders);
                    }
                    break;

                case types::PRINT_BALANCE:
                    client.printBalance(c.client_id);
                    break;

                case types::PRINT_DATASTORE:
                    client.printDatastore();
                    break;

                case types::PRINT_PERFORMANCE:
                    client.printPerformance();
                    break;

                case types::EXIT:
                    std::cout << "Exiting..." << std::endl;
                    utils::killAllServers();
                    exit = true;
                    break;

                default:
                    std::runtime_error("Unknown command type: " + std::to_string(c.command));
                    break;
            }
        } catch (std::exception& e) {
            std::cerr << "Exception: " << e.what() << std::endl;
        }
    }
}


int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <num_clusters> <cluster_size> <csv_filepath> [config_filepath]" << std::endl;
        exit(1);
    }

    auto logger = spdlog::stdout_color_mt("console");
    logger->set_level(spdlog::level::info);
    
    int num_clusters = std::stoi(argv[1]);
    int cluster_size = std::stoi(argv[2]);
    std::string filename = argv[3];
    std::thread t;
    try {
        utils::setupApplicationState(num_clusters, cluster_size);
        if (argc == 5) utils::loadConfig(argv[4]);
        utils::startAllServers(num_clusters, cluster_size);
        
        Client client;
        CSVReader reader(filename);
        std::thread t(&Client::consumeReplies, &client);
        mainloop(reader, client);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        t.join();
        exit(1);
    } 
    
    return 0;
}
