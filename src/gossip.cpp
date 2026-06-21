#include "../include/gossip.h"

void GossipManager::update_node(const std::string& ip, int port, int heartbeat) {
    std::lock_guard<std::mutex> lock(gossip_mutex);
    std::string node_id = ip + ":" + std::to_string(port);
    
    if (cluster_state.find(node_id) == cluster_state.end()) {
        cluster_state[node_id] = {ip, port, heartbeat, time(NULL), ALIVE};
    } else {
        if (heartbeat > cluster_state[node_id].heartbeat) {
            cluster_state[node_id].heartbeat = heartbeat;
            cluster_state[node_id].last_updated = time(NULL);
            cluster_state[node_id].status = ALIVE;
        }
    }
}

std::vector<GossipNode> GossipManager::get_dead_nodes(int timeout_seconds) {
    std::lock_guard<std::mutex> lock(gossip_mutex);
    std::vector<GossipNode> dead_nodes;
    time_t now = time(NULL);

    for (auto& pair : cluster_state) {
        if (pair.second.status == ALIVE && (now - pair.second.last_updated) > timeout_seconds) {
            pair.second.status = DEAD;
            dead_nodes.push_back(pair.second);
        }
    }
    return dead_nodes;
}

std::vector<GossipNode> GossipManager::get_alive_nodes() {
    std::lock_guard<std::mutex> lock(gossip_mutex);
    std::vector<GossipNode> alive_nodes;
    for (const auto& pair : cluster_state) {
        if (pair.second.status == ALIVE) {
            alive_nodes.push_back(pair.second);
        }
    }
    return alive_nodes;
}

void GossipManager::mark_dead(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(gossip_mutex);
    if (cluster_state.find(node_id) != cluster_state.end()) {
        cluster_state[node_id].status = DEAD;
    }
}
