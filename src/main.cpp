// main.cpp  Phase 3
//
// Network Layer:
// Real TCP server, epoll event loop, binary protocol, and TCP_NODELAY.
// 

#include "engine.hpp"
#include "protocol.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

const int PORT = 9000;
const int MAX_EVENTS = 64;

// Set a file descriptor to non-blocking mode.
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Set TCP_NODELAY to disable Nagle's algorithm for lower latency.
void set_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

int main() {
    Engine engine;

    // 1. Setup listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nodelay(listen_fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind to port " << PORT << "\n";
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        std::cerr << "Failed to listen\n";
        return 1;
    }
    
    set_nonblocking(listen_fd);
    std::cout << "Engine listening on port " << PORT << "\n";

    // 2. Setup epoll
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::cerr << "Failed to create epoll\n";
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN; // Level-triggered for simplicity, edge-triggered needs exhaustive read
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[MAX_EVENTS];

    // 3. Main Event Loop
    while (true) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // New connection
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                
                if (client_fd >= 0) {
                    set_nonblocking(client_fd);
                    set_nodelay(client_fd);
                    
                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
                    std::cout << "Client connected: fd " << client_fd << "\n";
                }
            } else {
                // Incoming data from client
                char buf[sizeof(OrderRequest)];
                // Read exactly one OrderRequest for simplicity. 
                // A production system handles streaming boundaries properly.
                ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

                if (bytes <= 0) {
                    // Disconnect or error
                    std::cout << "Client disconnected: fd " << fd << "\n";
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    continue;
                }

                if (bytes == sizeof(OrderRequest)) {
                    OrderRequest req;
                    std::memcpy(&req, buf, sizeof(OrderRequest));

                    if (req.type == 'N') {
                        engine.new_order(req.order_id, req.side, req.price, req.quantity);
                        
                        // Send ACK
                        ExecutionReport ack{'E', req.order_id, 0, 0, 'A'};
                        send(fd, &ack, sizeof(ack), 0);
                        
                    } else if (req.type == 'C') {
                        engine.cancel_order(req.order_id);
                        
                        ExecutionReport ack{'E', req.order_id, 0, 0, 'C'};
                        send(fd, &ack, sizeof(ack), 0);
                    }

                    // Process Fills
                    auto fills = engine.take_fills();
                    for (const auto& fill : fills) {
                        ExecutionReport report{'E', fill.buy_order_id, fill.quantity, fill.price, 'F'};
                        send(fd, &report, sizeof(report), 0);
                        
                        // Note: We'd normally route the sell-side fill to the other client as well,
                        // but for simplicity in this phase, we just send it to the active socket.
                        if (fill.buy_order_id != fill.sell_order_id) {
                            report.order_id = fill.sell_order_id;
                            send(fd, &report, sizeof(report), 0);
                        }
                    }
                }
            }
        }
    }

    return 0;
}
