// KV-Store server entry point.
//
// Parses CLI args for port and persistence mode, installs a SIGINT
// handler for graceful Ctrl+C shutdown, then starts the TCP server.
//
// Usage: ./kvstore [port] [persistent(true|false)]
//   port:       TCP port to listen on (default: 8080)
//   persistent: whether writes are appended to the AOF file on disk
//               (default: true)

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "TCPServer.hpp"

// Handles Ctrl+C (SIGINT) by printing a message and exiting cleanly rather
// than letting the default handler terminate the process silently.
void signal_handler(int signum) {
    if (signum == SIGINT) {
        std::cout << "\n[Ctrl+C detected] Gracefully shutting down the server.\n";
        exit(0);
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);

    int port = 8080;
    bool persistent = true;

    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    if (argc >= 3) {
        const std::string value = argv[2];

        if (value == "true") {
            persistent = true;
        } else if (value == "false") {
            persistent = false;
        } else {
            std::cerr << "Invalid persistence value: " << value << ". Expected true or false.\n";
            return 1;
        }
    }

    TCPServer server(port, persistent);

    server.start();

    return 0;
}