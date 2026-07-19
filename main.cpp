#include <iostream>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

#include "TCPServer.hpp"
#include "KVStore.hpp"

void signal_handler(int signum) {
    if (signum == SIGINT) {
        std::cout << "\n[Ctrl+C detected] Gracefully shutting down the server.\n";
        exit(0);
    }
}

int main() {
    // Register signal handlers
    signal(SIGINT, signal_handler);

    KVStore my_store;
    TCPServer server(8080, my_store);

    server.start();

    return 0;
}
