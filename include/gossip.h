#ifndef GOSSIP_H
#define GOSSIP_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <ctime>

enum NodeStatus { ALIVE, DEAD };

struct GossipNode {
    std::string ip;
    int port;
    int heartbeat;
    time_t last_updated;
    NodeStatus status;
};

class GossipManager {
private:
    std::unordered_map<std::string, GossipNode> cluster_state;
    std::mutex gossip_mutex;

public:
    void update_node(const std::string& ip, int port, int heartbeat);
    std::vector<GossipNode> get_dead_nodes(int timeout_seconds);
    std::vector<GossipNode> get_alive_nodes();
    void mark_dead(const std::string& node_id);
};

#endif
