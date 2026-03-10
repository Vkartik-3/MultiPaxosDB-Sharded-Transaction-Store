#include <vector>
#include <string>
#include <unistd.h>
#include <map>
#include <csignal>

class utils {
public:
    static int getClusterIdFromClientId(int id);
    static int getClusterIdFromServerId(int id);
    static bool isClientInCluster(int client_id, int cluster_id);
    static std::vector<std::string> getServersInCluster(int cluster_id);
    static void setupApplicationState(int num_clusters, int cluster_size);
    static bool loadConfig(const std::string& path);
    static void startAllServers(int num_clusters, int cluster_size);
    static void killAllServers();

private:
    inline static std::map<std::string, pid_t> server_pids = std::map<std::string, pid_t>();
};
