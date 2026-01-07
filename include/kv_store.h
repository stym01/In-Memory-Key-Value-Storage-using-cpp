#ifndef KV_STORE_H
#define KV_STORE_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <ctime> // for time
#include <list> // for DLL

//stores value and ttl
struct Entry {
    std::string value;
    time_t expiry_time; 
};

class KVStore {
private:
    size_t capacity; 
    std::list<std::string> lru_list;
    
    // map stores: Key -> Pair { Entry Data , pos of this key in dll for lru }
    std::unordered_map<std::string, std::pair<Entry, std::list<std::string>::iterator>> store; 

    mutable std::shared_mutex rw_lock;

    std::string filename = "database.aof"; // The file where data is saved (file storage)
    std::ofstream file_stream;

    void _log_to_file(const std::string& command);

public:
    KVStore(size_t cap = 100);
    ~KVStore(); // Destructor to close file
    
    void set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    std::string get(const std::string& key);
    bool del(const std::string& key);
};

#endif