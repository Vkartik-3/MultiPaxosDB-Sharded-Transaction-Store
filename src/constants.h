#pragma once
#include <string.h>
#include <vector>
#include <map>

class constants {
public:
    inline const static int total_clients = 3000;
    inline static int num_clusters = 3;
    inline static int cluster_size = 3;

    inline static std::map<std::string, std::string> server_addresses = std::map<std::string, std::string>();
    inline static std::map<std::string, int> server_ids = std::map<std::string, int>();
};