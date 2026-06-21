#include <iostream>
#include <vector>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <sys/epoll.h> // epoll
#include <fcntl.h>     // For non-blocking sockets

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <random>
#include <csignal>
#include <atomic>

#include "../include/kv_store.h"
#include "../include/cluster.h"
#include "../include/gossip.h"

#define MAX_EVENTS 100

// Globals
KVStore* db;
ConsistentHashRing ring;
std::string my_ip = "127.0.0.1";
int my_port = 8081;
std::string my_node_id;

// Gossip State
GossipManager gossip_mgr;
int my_heartbeat = 0;
std::mutex hb_mutex;

// Replication Background Task
struct ReplicationTask {
    std::string command;
    std::string target_ip;
    int target_port;
};

std::queue<ReplicationTask> replication_queue;
std::mutex rep_mutex;
std::condition_variable rep_cv;

// Read Repair Background Task
struct ReadRepairTask {
    std::string key;
    std::string local_value;
    uint64_t local_version;
};

std::queue<ReadRepairTask> read_repair_queue;
std::mutex rr_mutex;
std::condition_variable rr_cv;

// ============================================================================
//  Background Worker: Replication
//  Sends INTERNAL_SET / INTERNAL_DEL / MIGRATE commands to other nodes.
// ============================================================================
void replication_worker() {
    while (true) {
        ReplicationTask task;
        {
            std::unique_lock<std::mutex> lock(rep_mutex);
            rep_cv.wait(lock, []{ return !replication_queue.empty(); });
            task = replication_queue.front();
            replication_queue.pop();
        }

        // Open blocking socket for replication to keep it simple in background thread
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        sockaddr_in target_addr;
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(task.target_port);
        inet_pton(AF_INET, task.target_ip.c_str(), &target_addr.sin_addr);

        // Set timeouts so replication doesn't hang forever if a node is down
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        if (connect(sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
            send(sock, task.command.c_str(), task.command.size(), 0);
            char buffer[1024];
            read(sock, buffer, sizeof(buffer));
        }
        close(sock);
    }
}

// ============================================================================
//  Background Worker: Key Migration
//  Watches for hash ring changes (node join/leave) and pushes keys that
//  this node no longer owns to their new owner via MIGRATE commands.
// ============================================================================
void migration_worker() {
    uint64_t last_ring_version = ring.get_ring_version();
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        uint64_t current_ring_version = ring.get_ring_version();
        if (current_ring_version == last_ring_version) continue;
        
        last_ring_version = current_ring_version;
        
        std::cout << "[MIGRATE] Ring changed (v" << current_ring_version 
                  << "). Scanning keys for migration..." << std::endl;
        
        auto entries = db->get_all_entries();
        int migrated = 0;
        
        for (const auto& [key, value, version] : entries) {
            ClusterNode owner = ring.get_node_for_key(key);
            if (owner.id == my_node_id || owner.id.empty()) continue;
            
            // This key no longer belongs to us — push it to the new owner
            std::string migrate_cmd = "MIGRATE " + key + " " + value + " " 
                                    + std::to_string(version) + "\n";
            {
                std::lock_guard<std::mutex> lock(rep_mutex);
                replication_queue.push({migrate_cmd, owner.ip, owner.port});
                rep_cv.notify_one();
            }
            migrated++;
        }
        
        if (migrated > 0) {
            std::cout << "[MIGRATE] Migration complete. Pushed " << migrated 
                      << " keys to new owners." << std::endl;
        }
    }
}

// ============================================================================
//  Background Worker: Read Repair
//  After every GET, queries all replicas in the background and fixes any
//  stale copies (or updates local if a replica has a newer version).
// ============================================================================
void read_repair_worker() {
    while (true) {
        ReadRepairTask task;
        {
            std::unique_lock<std::mutex> lock(rr_mutex);
            rr_cv.wait(lock, []{ return !read_repair_queue.empty(); });
            task = read_repair_queue.front();
            read_repair_queue.pop();
        }
        
        std::vector<ClusterNode> replicas = ring.get_replica_nodes(task.key, 2);
        
        for (const auto& replica : replicas) {
            if (replica.id == my_node_id) continue; // Don't query ourselves
            
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;
            
            sockaddr_in target_addr;
            target_addr.sin_family = AF_INET;
            target_addr.sin_port = htons(replica.port);
            inet_pton(AF_INET, replica.ip.c_str(), &target_addr.sin_addr);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
            
            if (connect(sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
                std::string cmd = "INTERNAL_GET " + task.key + "\n";
                send(sock, cmd.c_str(), cmd.size(), 0);
                
                char buffer[4096] = {0};
                int valread = read(sock, buffer, sizeof(buffer));
                if (valread > 0) {
                    std::string response(buffer, valread);
                    std::stringstream rss(response);
                    std::string replica_value;
                    uint64_t replica_version = 0;
                    rss >> replica_value >> replica_version;
                    
                    if (replica_value != "NULL") {
                        if (replica_version > task.local_version) {
                            // Replica has NEWER data — update our local copy
                            db->set_versioned(task.key, replica_value, replica_version);
                            task.local_value = replica_value;
                            task.local_version = replica_version;
                            std::cout << "[READ_REPAIR] Updated local key \"" << task.key 
                                      << "\" from " << replica.id 
                                      << " (v" << replica_version << ")" << std::endl;
                        } else if (replica_version < task.local_version) {
                            // Replica is STALE — push our version to fix it
                            std::string fix_cmd = "INTERNAL_SET " + task.key + " " 
                                                + task.local_value + " " 
                                                + std::to_string(task.local_version) + "\n";
                            {
                                std::lock_guard<std::mutex> lock(rep_mutex);
                                replication_queue.push({fix_cmd, replica.ip, replica.port});
                                rep_cv.notify_one();
                            }
                            std::cout << "[READ_REPAIR] Fixed stale key \"" << task.key 
                                      << "\" on " << replica.id 
                                      << " (v" << replica_version 
                                      << " -> v" << task.local_version << ")" << std::endl;
                        }
                    }
                }
            }
            close(sock);
        }
    }
}

// ============================================================================
//  Background Worker: Anti-Entropy
//  Every 30 seconds, scans ALL keys that this node is the primary owner of
//  and ensures all replicas have the latest version. Catches drift for keys
//  that are never read (and thus never trigger read repair).
// ============================================================================
void anti_entropy_worker() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        auto entries = db->get_all_entries();
        int repaired = 0;
        
        for (const auto& [key, value, version] : entries) {
            ClusterNode owner = ring.get_node_for_key(key);
            if (owner.id != my_node_id) continue; // Only repair keys we are primary owner of
            
            std::vector<ClusterNode> replicas = ring.get_replica_nodes(key, 2);
            
            for (const auto& replica : replicas) {
                if (replica.id == my_node_id) continue;
                
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) continue;
                
                sockaddr_in target_addr;
                target_addr.sin_family = AF_INET;
                target_addr.sin_port = htons(replica.port);
                inet_pton(AF_INET, replica.ip.c_str(), &target_addr.sin_addr);
                
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
                
                if (connect(sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
                    std::string cmd = "INTERNAL_GET " + key + "\n";
                    send(sock, cmd.c_str(), cmd.size(), 0);
                    
                    char buffer[4096] = {0};
                    int valread = read(sock, buffer, sizeof(buffer));
                    if (valread > 0) {
                        std::string response(buffer, valread);
                        std::stringstream rss(response);
                        std::string replica_value;
                        uint64_t replica_version = 0;
                        rss >> replica_value >> replica_version;
                        
                        if (replica_version < version) {
                            // Replica is stale — push our version
                            std::string fix_cmd = "INTERNAL_SET " + key + " " + value 
                                                + " " + std::to_string(version) + "\n";
                            {
                                std::lock_guard<std::mutex> lock(rep_mutex);
                                replication_queue.push({fix_cmd, replica.ip, replica.port});
                                rep_cv.notify_one();
                            }
                            repaired++;
                        } else if (replica_version > version) {
                            // We're somehow stale — update local
                            if (replica_value != "NULL") {
                                db->set_versioned(key, replica_value, replica_version);
                                repaired++;
                            }
                        }
                    }
                }
                close(sock);
            }
        }
        
        if (repaired > 0) {
            std::cout << "[ANTI-ENTROPY] Scanned " << entries.size() 
                      << " keys, repaired " << repaired << " stale replicas." << std::endl;
        }
    }
}

// ============================================================================
//  Background Worker: Gossip Protocol
//  Heartbeat + failure detection + auto-discovery.
// ============================================================================
void gossip_worker() {
    std::random_device rd;
    std::mt19937 gen(rd());

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        int current_hb;
        {
            std::lock_guard<std::mutex> lock(hb_mutex);
            my_heartbeat++;
            current_hb = my_heartbeat;
        }

        std::vector<GossipNode> dead_nodes = gossip_mgr.get_dead_nodes(10);
        for (const auto& node : dead_nodes) {
            std::cout << "[GOSSIP] Node " << node.ip << ":" << node.port << " is DEAD. Removing from Ring." << std::endl;
            ring.remove_node(node.ip, node.port);
            // ring_version increments automatically, which triggers migration_worker
        }

        std::vector<GossipNode> alive_nodes = gossip_mgr.get_alive_nodes();
        if (alive_nodes.empty()) continue;

        std::uniform_int_distribution<> dis(0, alive_nodes.size() - 1);
        GossipNode target = alive_nodes[dis(gen)];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        sockaddr_in target_addr;
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target.port);
        inet_pton(AF_INET, target.ip.c_str(), &target_addr.sin_addr);

        // Set a small timeout for connect so gossip doesn't hang forever
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        if (connect(sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
            std::string gossip_msg = "GOSSIP " + my_ip + " " + std::to_string(my_port) + " " + std::to_string(current_hb) + "\n";
            send(sock, gossip_msg.c_str(), gossip_msg.size(), 0);
            
            char buffer[1024];
            read(sock, buffer, sizeof(buffer));
        } else {
            // Immediate connect failure might imply dead node, but we rely on the 10s timeout to be safe.
        }
        close(sock);
    }
}


void set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
}

bool parse_node_str(const std::string& node_str, std::string& ip, int& port) {
    size_t colon_pos = node_str.find(':');
    if (colon_pos == std::string::npos) return false;
    ip = node_str.substr(0, colon_pos);
    port = std::stoi(node_str.substr(colon_pos + 1));
    return true;
}

void process_client_request(int client_socket, int epoll_fd, const std::string& request) {
    std::stringstream ss(request);
    std::string command, key, value;
    ss >> command >> key;

    std::string result;
    bool needs_redirect = false;
    ClusterNode owner;

    // --- GOSSIP Handling ---
    if (command == "GOSSIP" || command == "GOSSIP_ACK") {
        std::string target_ip = key; // for gossip, the 2nd token is IP
        int target_port = 0, target_hb = 0;
        ss >> target_port >> target_hb;

        gossip_mgr.update_node(target_ip, target_port, target_hb);
        ring.add_node(target_ip, target_port);
        // ring_version increments automatically, which triggers migration_worker

        if (command == "GOSSIP") {
            int current_hb;
            {
                std::lock_guard<std::mutex> lock(hb_mutex);
                current_hb = my_heartbeat;
            }
            result = "GOSSIP_ACK " + my_ip + " " + std::to_string(my_port) + " " + std::to_string(current_hb) + "\n";
            send(client_socket, result.c_str(), result.size(), 0);
        }
        return;
    }


    // --- Hash Ring & Redirects ---
    // Internal commands (INTERNAL_SET, INTERNAL_DEL, MIGRATE, INTERNAL_GET)
    // bypass the redirect — they are already targeted at the correct node.
    if (!key.empty() && !ring.is_empty()) {
        owner = ring.get_node_for_key(key);
        if (owner.id != my_node_id 
            && command != "INTERNAL_SET" && command != "INTERNAL_DEL" 
            && command != "MIGRATE" && command != "INTERNAL_GET") {
            needs_redirect = true;
        }
    }

    if (needs_redirect) {
        int proxy_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (proxy_sock < 0) {
            result = "ERROR: Failed to connect to owner node\n";
            send(client_socket, result.c_str(), result.size(), 0);
            return;
        }

        sockaddr_in target_addr;
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(owner.port);
        inet_pton(AF_INET, owner.ip.c_str(), &target_addr.sin_addr);
        
        // Set a small timeout so the epoll loop doesn't freeze forever if the node is down
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(proxy_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(proxy_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        if (connect(proxy_sock, (struct sockaddr*)&target_addr, sizeof(target_addr)) >= 0) {
            send(proxy_sock, request.c_str(), request.size(), 0);
            
            char buffer[1024] = {0};
            int valread = read(proxy_sock, buffer, sizeof(buffer));
            if (valread > 0) {
                send(client_socket, buffer, valread, 0);
            } else {
                result = "ERROR: Owner node did not respond\n";
                send(client_socket, result.c_str(), result.size(), 0);
            }
        } else {
            result = "ERROR: Could not connect to owner node\n";
            send(client_socket, result.c_str(), result.size(), 0);
        }
        
        close(proxy_sock);
        return;
    }

    // ======================================================================
    //  Local DB Processing
    // ======================================================================
    
    if (command == "SET") {
        // Format: SET key value [ttl]
        std::getline(ss, value);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1); 

        int ttl = 0;
        std::stringstream value_ss(value);
        std::string actual_value;
        value_ss >> actual_value; 
        if (!(value_ss >> ttl)) {
            ttl = 0; 
        }

        uint64_t version = db->set(key, actual_value, ttl);
        result = "OK\n";

        // Replicate to backup nodes (include version for Lamport clock consistency)
        std::vector<ClusterNode> replicas = ring.get_replica_nodes(key, 2);
        std::string internal_cmd = "INTERNAL_SET " + key + " " + actual_value 
                                 + " " + std::to_string(version) + "\n";
        for (const auto& rep : replicas) {
            std::lock_guard<std::mutex> lock(rep_mutex);
            replication_queue.push({internal_cmd, rep.ip, rep.port});
            rep_cv.notify_one();
        }
    } 
    else if (command == "INTERNAL_SET" || command == "MIGRATE") {
        // Format: INTERNAL_SET key value version
        // Format: MIGRATE key value version
        // These are sent by other nodes — accept only if version is newer.
        std::getline(ss, value);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);

        std::stringstream value_ss(value);
        std::string actual_value;
        uint64_t version = 0;
        value_ss >> actual_value;
        if (!(value_ss >> version)) version = 0;

        db->set_versioned(key, actual_value, version);
        result = "OK\n";
    }
    else if (command == "GET") {
        // Return value immediately, then queue async read repair
        uint64_t version = 0;
        std::string val = db->get_with_version(key, version);
        result = val + "\n";
        
        // Queue async read repair (only for keys that actually exist)
        if (val != "NULL") {
            std::lock_guard<std::mutex> lock(rr_mutex);
            read_repair_queue.push({key, val, version});
            rr_cv.notify_one();
        }
    }
    else if (command == "INTERNAL_GET") {
        // Format: INTERNAL_GET key
        // Returns: value version (used by read repair and anti-entropy)
        uint64_t version = 0;
        std::string val = db->get_with_version(key, version);
        result = val + " " + std::to_string(version) + "\n";
    }
    else if (command == "DEL" || command == "INTERNAL_DEL") {
        bool deleted = db->del(key);
        result = deleted ? "DELETED\n" : "NOT FOUND\n";
        
        if (command == "DEL" && deleted) {
            std::vector<ClusterNode> replicas = ring.get_replica_nodes(key, 2);
            std::string internal_cmd = "INTERNAL_DEL " + key + "\n";
            for (const auto& rep : replicas) {
                std::lock_guard<std::mutex> lock(rep_mutex);
                replication_queue.push({internal_cmd, rep.ip, rep.port});
                rep_cv.notify_one();
            }
        }
    } 
    else {
        result = "ERROR: Unknown Command\n";
    }
    
    send(client_socket, result.c_str(), result.size(), 0);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port> [node1_ip:port] [node2_ip:port] ..." << std::endl;
        return 1;
    }
    
    // Ignore broken pipe signals globally so client disconnects don't crash server
    signal(SIGPIPE, SIG_IGN);

    my_port = std::stoi(argv[1]);
    my_node_id = my_ip + ":" + std::to_string(my_port);

    db = new KVStore(5, std::to_string(my_port));

    // Parse provided seed nodes and add to gossip state
    for (int i = 2; i < argc; ++i) {
        std::string ip;
        int p;
        if (parse_node_str(argv[i], ip, p)) {
            ring.add_node(ip, p);
            gossip_mgr.update_node(ip, p, 0); // Seed nodes start with HB 0
        }
    }
    
    if (ring.is_empty()) {
        ring.add_node(my_ip, my_port);
    }

    // Start background threads
    std::thread rep_thread(replication_worker);
    rep_thread.detach();

    std::thread gos_thread(gossip_worker);
    gos_thread.detach();
    
    std::thread mig_thread(migration_worker);
    mig_thread.detach();
    
    std::thread rr_thread(read_repair_worker);
    rr_thread.detach();
    
    std::thread ae_thread(anti_entropy_worker);
    ae_thread.detach();


    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(my_port); 
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    set_nonblocking(server_fd);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return 1;
    }
    listen(server_fd, 10);

    int epoll_fd = epoll_create1(0);
    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::cout << "Server listening on port " << my_port << " using epoll..." << std::endl;
    std::cout << "Node ID: " << my_node_id << std::endl;
    std::cout << "Background threads: replication, gossip, migration, read-repair, anti-entropy" << std::endl;

    while (true) {
        int num_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < num_ready; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // New client
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_socket != -1) {
                    set_nonblocking(client_socket);
                    event.events = EPOLLIN; 
                    event.data.fd = client_socket;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
                }
            } 
            else {
                // Client data
                char buffer[1024] = {0};
                int valread = read(fd, buffer, sizeof(buffer));
                
                if (valread <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    std::string request(buffer, valread);
                    process_client_request(fd, epoll_fd, request);
                }
            }
        }
    }
    
    close(server_fd);
    delete db;
    return 0;
}