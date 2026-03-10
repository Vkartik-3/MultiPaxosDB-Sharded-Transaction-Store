#include <iostream>
#include <cmath>
#include <fstream>
#include "../constants.h"
#include "utils.h"

int utils::getClusterIdFromClientId(int id) {
    return ceil((double) (id * constants::num_clusters) / constants::total_clients);
}

int utils::getClusterIdFromServerId(int id) {
    return ceil((double) id / constants::cluster_size);
}

bool utils::isClientInCluster(int client_id, int cluster_id) {
    return getClusterIdFromClientId(client_id) == cluster_id;
}

std::vector<std::string> utils::getServersInCluster(int cluster_id) {
    std::vector<std::string> servers;
    for (auto& s: constants::server_ids) {
        if (getClusterIdFromServerId(s.second) == cluster_id) servers.push_back(s.first);
    }
    return servers;
}

void utils::setupApplicationState(int num_clusters, int cluster_size) {
    constants::num_clusters = num_clusters;
    constants::cluster_size = cluster_size;
    constants::server_addresses.clear();
    constants::server_ids.clear();

    for (int i = 1; i <= num_clusters * cluster_size; i++) {
        std::string server_name = "S" + std::to_string(i);
        std::string address = "localhost:" + std::to_string(50000 + i);
        constants::server_addresses[server_name] = address;
        constants::server_ids[server_name] = i;

    }
}

bool utils::loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    constants::server_addresses.clear();
    constants::server_ids.clear();

    std::string name;
    std::string address;
    int id = 1;
    while (f >> name >> address) {
        constants::server_addresses[name] = address;
        constants::server_ids[name] = id++;
    }
    return true;
}

void utils::startAllServers(int num_clusters, int cluster_size) {
    for (auto& s: constants::server_addresses) {
        pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error("Failed to start server: " + s.first);
        } else if (pid > 0) {
            utils::server_pids[s.first] = pid;
        } else {
            execl("./server", "server", s.first.c_str(), std::to_string(num_clusters).c_str(), std::to_string(cluster_size).c_str(), nullptr);
            // if execl fails
            throw std::runtime_error("Failed to start server: " + s.first);
        }
    }
}

void utils::killAllServers() {
    for (auto& p: utils::server_pids) {
        pid_t pid = utils::server_pids[p.first];
        if (kill(p.second, SIGTERM) == -1) {
            throw std::runtime_error("Failed to kill server: " + p.first);
        } else {
            utils::server_pids.erase(p.first);
        }

        int status;
        if (waitpid(pid, &status, 0) == -1) {
            throw std::runtime_error("waitpid failed for server: " + p.first);
        }
    }
}
