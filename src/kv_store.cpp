#include "../include/kv_store.h" 
#include <iostream>

KVStore::KVStore(size_t cap) : capacity(cap) {
    //openingg file once
    file_stream.open("database.aof", std::ios::app);
    if (!file_stream.is_open()) {
        std::cerr << "Error: Could not open AOF file!" << std::endl;
    }
}

KVStore::~KVStore() {
    //closing the file once
    if (file_stream.is_open()) {
        file_stream.close();
    }
}

void KVStore::_log_to_file(const std::string& command) {
    if (file_stream.is_open()) {
        file_stream << command << "\n";
    }
}

void KVStore::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    
    time_t expiry = 0;
    if (ttl_seconds > 0) {
        expiry = time(NULL) + ttl_seconds; // Current time + seconds
    }

    if (store.find(key) != store.end()) {
        store[key].first = {value, expiry}; 
        
        lru_list.splice(lru_list.begin(), lru_list, store[key].second);
    }
    
    else {
        if (store.size() >= capacity) {
            std::string lru_key = lru_list.back();
            lru_list.pop_back(); 
            store.erase(lru_key);
            
            _log_to_file("EVICT " + lru_key); 
        }

        lru_list.push_front(key);
        store[key] = {{value, expiry}, lru_list.begin()};
    }

    _log_to_file("SET " + key + " " + value + " " + std::to_string(ttl_seconds));
}


std::string KVStore::get(const std::string& key) {
    /* If we find an expired key, we have to delete it (Lazy Deletiion i.e. we will not delete just after timeout...we will wait for someone to access it and the moment they access it we will delete it), 
    so we can't use a shared_lock. so using the unique(write) lock */

    std::unique_lock<std::shared_mutex> lock(rw_lock); 
    
    if (store.find(key) == store.end()) {
        return "NULL";
    }

    // Check Expiry (Lazy Deletion)
    Entry& entry = store[key].first;
    if (entry.expiry_time != 0 && time(NULL) > entry.expiry_time) {
        // Removing  from list and map
        lru_list.erase(store[key].second);
        store.erase(key);
        return "NULL";
    }

    // updating the lru by moving the accessed key to the front of list
    lru_list.splice(lru_list.begin(), lru_list, store[key].second);

    return entry.value;
}

bool KVStore::del(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(rw_lock);
    
    if (store.find(key) != store.end()) {
        // Remove from list and map
        lru_list.erase(store[key].second);
        store.erase(key);
        _log_to_file("DEL " + key);
        return true;
    }
    return false;
}