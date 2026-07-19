/**
 * \file TCPServer.hpp
 * \brief Defines the TCPServer class that handles client connections.
 */

#pragma once
#include <string>

#include "KVStore.hpp"

/**
 * \class TCPServer
 * \brief A multithreaded TCP server that listens for client connections and processes commands.
 *
 * Each client connection is handled in a detached std::thread.
 */
class TCPServer {
public:
    /**
     * \brief Constructs a new TCPServer.
     * \param port The port number to listen on.
     * \param store A reference to the underlying KVStore engine.
     */
    TCPServer(int port, KVStore& store);
    
    /**
     * \brief Destroys the TCPServer, closing the server socket if open.
     */
    ~TCPServer();
    
    /**
     * \brief Starts the server loop, accepting new connections indefinitely.
     */
    void start();

private:
    int port_;
    int server_socket_;
    int max_backlog_;

    KVStore& store_;

    /**
     * \brief Thread function to handle a single client connection.
     * \param client_socket The file descriptor for the client socket.
     */
    void handle_client(int client_socket);
    
    /**
     * \brief Processes a single parsed request and executes it against the KVStore.
     * \param request The raw command request string.
     * \param response A reference to a string where the response will be stored.
     */
    void process_request(std::string& request, std::string& response);
};
