#include <iostream>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include "../include/kv_store.h"

KVStore db(3);

void handle_client(int client_socket) {
    char buffer[1024] = {0};
    while (true) {
        memset(buffer, 0, 1024);
        int valread = read(client_socket, buffer, 1024);
        if (valread <= 0) break;

        std::string request(buffer);
        std::stringstream ss(request);
        std::string command, key, value;
        ss >> command >> key;

        std::string result;
        if (command == "SET") {
            std::getline(ss, value);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1); // Remove leading space

            int ttl = 0;

            std::stringstream value_ss(value);
            std::string actual_value;
            value_ss >> actual_value; 
            if (value_ss >> ttl) {
                // We found a TTL number!
            } else {
                ttl = 0; // No TTL found
            }

            db.set(key, actual_value, ttl);
            result = "OK\n";
        } 
        else if (command == "GET") {
            result = db.get(key) + "\n";
        } 
        else if (command == "DEL") {
            db.del(key);
            result = "DELETED\n";
        } 
        else {
            result = "ERROR: Unknown Command\n";
        }
        send(client_socket, result.c_str(), result.size(), 0);
    }
    close(client_socket);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return 1;
    }
    listen(server_fd, 10);

    std::cout << "Server listening on port 8080..." << std::endl;

    while (true) {
        int new_socket = accept(server_fd, NULL, NULL);
        std::thread(handle_client, new_socket).detach();
    }
    return 0;
}