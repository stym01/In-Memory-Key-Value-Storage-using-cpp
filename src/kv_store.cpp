#include "../include/kv_store.h" 
#include <iostream>

KVStore::KVStore(size_t cap, const std::string& node_prefix) : capacity(cap), disk_db(node_prefix) {
    // Disk DB is initialized automatically via BTree constructor
    
    // Populate Bloom Filter with existing keys from disk
    auto existing_entries = disk_db.get_all_entries();
    for (const auto& entry : existing_entries) {
        bloom_filter.add(std::get<0>(entry));
    }
}

KVStore::~KVStore() {
    // Disk DB is destroyed automatically
}

uint64_t KVStore::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    
    uint64_t version = ++version_counter; // Lamport clock increment
    
    // Always insert to disk first
    disk_db.insert(key, value, version);
    bloom_filter.add(key); // Update Bloom Filter
    
    time_t expiry = 0;
    if (ttl_seconds > 0) {
        expiry = time(NULL) + ttl_seconds; // Current time + seconds
    }

    if (store.find(key) != store.end()) {
        store[key].first = {value, expiry, version}; 
        
        lru_list.splice(lru_list.begin(), lru_list, store[key].second);
    }
    
    else {
        if (store.size() >= capacity) {
            std::string lru_key = lru_list.back();
            lru_list.pop_back(); 
            store.erase(lru_key);
            // We do NOT delete from disk_db here! This is tiered storage.
        }

        lru_list.push_front(key);
        store[key] = {{value, expiry, version}, lru_list.begin()};
    }
    
    return version;
}

void KVStore::set_versioned(const std::string& key, const std::string& value, uint64_t version, int ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    
    // Advance Lamport clock (learn from incoming version — Lamport rule)
    uint64_t current = version_counter.load();
    while (version > current && !version_counter.compare_exchange_weak(current, version)) {}
    
    // Always attempt disk insert (BTree handles version comparison internally —
    // it only overwrites if incoming version >= existing version)
    disk_db.insert(key, value, version);
    bloom_filter.add(key); // Update Bloom Filter
    
    time_t expiry = (ttl_seconds > 0) ? time(NULL) + ttl_seconds : 0;
    
    // Only update LRU cache if key is already cached and incoming version is newer.
    // If key is NOT in cache, don't add it — let GET populate the cache lazily from disk.
    // This prevents a stale incoming version from polluting the cache when the disk
    // already has a newer version that wasn't cached.
    if (store.find(key) != store.end()) {
        if (version > store[key].first.version) {
            store[key].first = {value, expiry, version};
            lru_list.splice(lru_list.begin(), lru_list, store[key].second);
        }
    }
}


std::string KVStore::get(const std::string& key) {
    uint64_t dummy;
    return get_with_version(key, dummy);
}

std::string KVStore::get_with_version(const std::string& key, uint64_t& out_version) {
    /* If we find an expired key, we have to delete it (Lazy Deletion i.e. we will not delete just after timeout...
    we will wait for someone to access it and the moment they access it we will delete it), 
    so we can't use a shared_lock. so using the unique(write) lock */

    std::unique_lock<std::shared_mutex> lock(rw_lock); 
    
    if (store.find(key) == store.end()) {
        // Cache Miss! 
        // 1. Check Bloom Filter to avoid unnecessary disk read
        if (!bloom_filter.possibly_contains(key)) {
            out_version = 0;
            return "NULL";
        }

        // 2. Bloom Filter says "possibly yes", check Disk DB
        std::string disk_val = disk_db.search(key, out_version);
        if (disk_val != "NULL") {
            // Found on disk! Bring it into RAM Cache (Evict if necessary)
            if (store.size() >= capacity) {
                std::string lru_key = lru_list.back();
                lru_list.pop_back(); 
                store.erase(lru_key);
            }
            lru_list.push_front(key);
            store[key] = {{disk_val, 0, out_version}, lru_list.begin()};
            return disk_val;
        }
        out_version = 0;
        return "NULL";
    }

    // Check Expiry (Lazy Deletion)
    Entry& entry = store[key].first;
    if (entry.expiry_time != 0 && time(NULL) > entry.expiry_time) {
        // Removing  from list and map
        lru_list.erase(store[key].second);
        store.erase(key);
        disk_db.remove(key); // Remove from disk too
        out_version = 0;
        return "NULL";
    }

    // updating the lru by moving the accessed key to the front of list
    lru_list.splice(lru_list.begin(), lru_list, store[key].second);

    out_version = entry.version;
    return entry.value;
}

bool KVStore::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    
    bool deleted_from_disk = disk_db.remove(key);
    
    if (store.find(key) != store.end()) {
        // Remove from list and map
        lru_list.erase(store[key].second);
        store.erase(key);
        return true;
    }
    return deleted_from_disk;
}

std::vector<std::tuple<std::string, std::string, uint64_t>> KVStore::get_all_entries() {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    return disk_db.get_all_entries();
}

void KVStore::advance_clock(uint64_t received_version) {
    uint64_t current = version_counter.load();
    while (received_version > current && !version_counter.compare_exchange_weak(current, received_version)) {}
}