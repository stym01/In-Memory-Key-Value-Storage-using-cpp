#include "../include/cluster.h"
#include <functional>
#include <mutex>
#include <set>

ConsistentHashRing::ConsistentHashRing(int vnodes) : vnodes_per_node(vnodes) {}

size_t ConsistentHashRing::hash_func(const std::string& key) {
    return std::hash<std::string>{}(key);
}

void ConsistentHashRing::add_node(const std::string& ip, int port) {
    std::unique_lock<std::shared_mutex> lock(ring_mutex);
    std::string node_id = ip + ":" + std::to_string(port);
    
    // Check if already exists
    for (const auto& pair : ring) {
        if (pair.second.id == node_id) return;
    }

    ClusterNode node = {ip, port, node_id};

    for (int i = 0; i < vnodes_per_node; ++i) {
        std::string vnode_key = node_id + "#" + std::to_string(i);
        size_t hash_val = hash_func(vnode_key);
        ring[hash_val] = node;
    }
    
    ring_version++; // Signal ring change to migration worker
}

void ConsistentHashRing::remove_node(const std::string& ip, int port) {
    std::unique_lock<std::shared_mutex> lock(ring_mutex);
    std::string node_id = ip + ":" + std::to_string(port);

    for (int i = 0; i < vnodes_per_node; ++i) {
        std::string vnode_key = node_id + "#" + std::to_string(i);
        size_t hash_val = hash_func(vnode_key);
        ring.erase(hash_val);
    }
    
    ring_version++; // Signal ring change to migration worker
}

ClusterNode ConsistentHashRing::get_node_for_key(const std::string& key) {
    std::shared_lock<std::shared_mutex> lock(ring_mutex);
    if (ring.empty()) {
        return {"", 0, ""};
    }

    size_t hash_val = hash_func(key);
    auto it = ring.lower_bound(hash_val);

    if (it == ring.end()) {
        it = ring.begin(); // Wrap around
    }

    return it->second;
}

std::vector<ClusterNode> ConsistentHashRing::get_replica_nodes(const std::string& key, int count) {
    std::shared_lock<std::shared_mutex> lock(ring_mutex);
    std::vector<ClusterNode> replicas;
    if (ring.empty()) return replicas;

    size_t hash_val = hash_func(key);
    auto it = ring.lower_bound(hash_val);

    if (it == ring.end()) {
        it = ring.begin();
    }

    std::set<std::string> seen_nodes;
    seen_nodes.insert(it->second.id); // Primary owner
    
    auto curr_it = it;
    while ((int)replicas.size() < count) {
        curr_it++;
        if (curr_it == ring.end()) {
            curr_it = ring.begin();
        }

        // Avoid infinite loop if cluster is smaller than replication factor
        if (curr_it == it) break;

        if (seen_nodes.find(curr_it->second.id) == seen_nodes.end()) {
            seen_nodes.insert(curr_it->second.id);
            replicas.push_back(curr_it->second);
        }
    }

    return replicas;
}

bool ConsistentHashRing::is_empty() const {
    std::shared_lock<std::shared_mutex> lock(ring_mutex);
    return ring.empty();
}
