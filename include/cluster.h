#ifndef CLUSTER_H
#define CLUSTER_H

#include <string>
#include <map>
#include <vector>
#include <shared_mutex>
#include <atomic>

struct ClusterNode {
    std::string ip;
    int port;
    std::string id; // e.g. "127.0.0.1:8081"
};

class ConsistentHashRing {
private:
    std::map<size_t, ClusterNode> ring;
    int vnodes_per_node;
    mutable std::shared_mutex ring_mutex; // Thread safety for concurrent access
    std::atomic<uint64_t> ring_version{0}; // Incremented on every ring change

    size_t hash_func(const std::string& key);

public:
    ConsistentHashRing(int vnodes = 100);
    void add_node(const std::string& ip, int port);
    void remove_node(const std::string& ip, int port);
    ClusterNode get_node_for_key(const std::string& key);
    std::vector<ClusterNode> get_replica_nodes(const std::string& key, int count);
    bool is_empty() const;
    uint64_t get_ring_version() const { return ring_version.load(); }
};

#endif
