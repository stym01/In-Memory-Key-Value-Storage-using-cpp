#ifndef KV_STORE_H
#define KV_STORE_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <ctime> // NEW: For time functions
#include <list> // NEW: Doubly Linked List

// The container for our data
struct Entry {
    std::string value;
    time_t expiry_time; // Timestamp when it dies. 0 means "never dies"
};

class KVStore {
private:
    size_t capacity; // Max number of keys allowed
    std::list<std::string> lru_list; // Stores Keys ordered by usage (Front = Newest)
    
    // Map stores: Key -> Pair { Entry Data, Iterator to List Node }
    // This Iterator lets us find the key in the list instantly (O(1)) without searching. i,e. pos of this key in the DLL
    std::unordered_map<std::string, std::pair<Entry, std::list<std::string>::iterator>> store; //entry stores value and ttl

    mutable std::shared_mutex rw_lock;

    std::string filename = "database.aof"; // The file where data is saved
    std::ofstream file_stream;

    // Helper function to save commands to disk
    void _log_to_file(const std::string& command);

public:
    KVStore(size_t cap = 100);
    ~KVStore(); // Destructor to close file
    
    // Updated set to accept TTL (Time To Live) in seconds
    void set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    std::string get(const std::string& key);
    bool del(const std::string& key);
};

#endif