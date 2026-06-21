#ifndef KV_STORE_H
#define KV_STORE_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <ctime> // for time
#include <list> // for DLL
#include <atomic>
#include <vector>
#include <tuple>
#include "btree.h"
#include "bloom_filter.h"

//stores value, ttl, and version (Lamport clock)
struct Entry {
    std::string value;
    time_t expiry_time; 
    uint64_t version;
};

class KVStore {
private:
    size_t capacity; 
    std::list<std::string> lru_list;
    
    // map stores: Key -> Pair { Entry Data , pos of this key in dll for lru }
    std::unordered_map<std::string, std::pair<Entry, std::list<std::string>::iterator>> store; 

    mutable std::shared_mutex rw_lock;

    BTree disk_db; // The On-Disk B-Tree database
    
    std::atomic<uint64_t> version_counter{0}; // Per-node Lamport clock
    
    BloomFilter bloom_filter;

public:
    KVStore(size_t cap = 100, const std::string& node_prefix = "");
    ~KVStore(); // Destructor to close file
    
    // External SET: increments Lamport clock, returns assigned version
    uint64_t set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    
    // Internal SET: accepts explicit version (for replication/migration), only writes if newer
    void set_versioned(const std::string& key, const std::string& value, uint64_t version, int ttl_seconds = 0);
    
    std::string get(const std::string& key);
    std::string get_with_version(const std::string& key, uint64_t& out_version);
    bool del(const std::string& key);
    
    // For key migration and anti-entropy: returns all (key, value, version) from disk B-Tree
    std::vector<std::tuple<std::string, std::string, uint64_t>> get_all_entries();
    
    // Advance the Lamport clock when receiving versions from other nodes
    void advance_clock(uint64_t received_version);
};

#endif