/**
 * \file TCPServer.cpp
 * \brief Implementation of the TCPServer class.
 */

#include "TCPServer.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <thread>

TCPServer::TCPServer(int port, KVStore& store)
    : port_(port), server_socket_(-1), max_backlog_(10), store_(store) {}

TCPServer::~TCPServer() {
    if (server_socket_ != -1) {
        close(server_socket_);
    }
}

void TCPServer::handle_client(int client_socket) {
    char buffer[1024] = {0};
    std::string request, response;

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);

        if (bytes_read <= 0) {
            // Client disconnected or error occurred
            break;
        }

        std::string data(buffer, bytes_read);

        for (size_t i = 0; i < data.size(); i++) {
            if (data[i] != '\n') {
                request.push_back(data[i]);
            } else {
                process_request(request, response);

                send(client_socket, response.c_str(), response.length(), 0);

                request.clear();
                response.clear();
            }
        }
    }

    close(client_socket);
}

void TCPServer::start() {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ == -1) {
        std::cerr << "Failed to create server socket.\n";
        return;
    }

    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed! Error: " << strerror(errno) << "\n";
        close(server_socket_);
        return;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind port " << port_ << ".\n";
        return;
    }

    if (listen(server_socket_, max_backlog_) < 0) {
        std::cerr << "Failed to listen on server socket.\n";
        return;
    }

    std::cout << "Server is listening on port " << port_ << "...\n";

    while (true) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // ACCEPT CONNECTION (Blocks until a new client connects)
        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            std::cerr << "Failed to accept connection.\nRetrying...\n";
            continue;
        }

        std::thread client_thread([this, client_socket]() { handle_client(client_socket); });

        // Detach the thread to allow it to run independently and clean up after itself
        client_thread.detach();
    }
}

void TCPServer::process_request(std::string& request, std::string& response) {
    std::string commandType, key, value;

    if (parse_command(request, commandType, key, value)) {
        if (commandType == "SET") {
            store_.set(key, value, response);
        } else if (commandType == "GET") {
            store_.get(key, response);
        } else if (commandType == "DEL") {
            store_.del(key, response);
        }
    } else {
        response = "ERROR - Invalid or unknown command\n";
    }
} 