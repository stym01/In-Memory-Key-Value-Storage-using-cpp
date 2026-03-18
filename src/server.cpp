#include <iostream>
#include <vector>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <sys/epoll.h> // epoll
#include <fcntl.h>     // For non-blocking sockets
#include "../include/kv_store.h"

#define MAX_EVENTS 100

KVStore db(50);

void set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
}

void process_client_request(int client_socket, int epoll_fd) {
    char buffer[1024] = {0};

    int valread = read(client_socket, buffer, sizeof(buffer));
    
    if (valread <= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_socket, NULL);
        close(client_socket);
        return;
    }

    std::string request(buffer);
    std::stringstream ss(request);
    std::string command, key, value;
    ss >> command >> key;

    std::string result;
    if (command == "SET") {
        std::getline(ss, value);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1); 

        int ttl = 0;
        std::stringstream value_ss(value);
        std::string actual_value;
        value_ss >> actual_value; 
        if (value_ss >> ttl) {
            // Found a TTL number
        } else {
            ttl = 0; // No TTL
        }

        db.set(key, actual_value, ttl);
        result = "OK\n";
    } 
    else if (command == "GET") {
        result = db.get(key) + "\n";
    } 
    else if (command == "DEL") {
        bool deleted = db.del(key);
        result = deleted ? "DELETED\n" : "NOT FOUND\n";
    } 
    else {
        result = "ERROR: Unknown Command\n";
    }
    
    // Send the response back to the client
    send(client_socket, result.c_str(), result.size(), 0);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8081); 
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    set_nonblocking(server_fd);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return 1;
    }
    listen(server_fd, 10);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "Failed to create epoll file descriptor" << std::endl;
        return 1;
    }

    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        std::cerr << "Failed to add server_fd to epoll" << std::endl;
        return 1;
    }

    std::cout << "Server listening on port 8081 using epoll..." << std::endl;

    while (true) {
        // waiting for activity on any registered file descriptor
        int num_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < num_ready; i++) {
            
            // Case A: The server socket is ready, meaning a NEW client is connecting
            if (events[i].data.fd == server_fd) {
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_socket != -1) {
                    // Make the new client socket non-blocking
                    set_nonblocking(client_socket);
                    
                    // Add this new client to epoll watch list
                    event.events = EPOLLIN; // Level-triggered read
                    event.data.fd = client_socket;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event);
                }
            } 
            // Case B: An existing client socket is ready, meaning they sent a command
            else {
                process_client_request(events[i].data.fd, epoll_fd);
            }
        }
    }
    
    close(server_fd);
    return 0;
}