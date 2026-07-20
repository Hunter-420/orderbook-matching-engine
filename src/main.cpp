// main.cpp  Phase 4
//
// Hardware Isolation and Latency Measurement:
// CPU affinity pinning, nanosecond timestamps, telemetry ring buffer,
// and percentile output on shutdown.

#include "engine.hpp"
#include "protocol.hpp"
#include "telemetry.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <csignal>
#include <chrono>
#include <atomic>

const int PORT = 9000;
const int MAX_EVENTS = 64;

std::atomic<bool> keep_running{true};
TelemetryBuffer telemetry;

void sigint_handler(int) {
    keep_running = false;
}

// Pin the current thread to a specific CPU core to prevent the Linux
// scheduler from moving it, which avoids L1/L2 cache invalidations.
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "Failed to set CPU affinity to core " << core_id << "\n";
    } else {
        std::cout << "Successfully pinned to CPU core " << core_id << "\n";
    }
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

int main() {
    // 1. Pin to core 1 (assuming it exists and isn't the primary OS core)
    pin_to_core(1);

    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);
    std::signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent crash on disconnected clients

    Engine engine;

    // 2. Setup listening socket
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

    // 3. Setup epoll
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::cerr << "Failed to create epoll\n";
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[MAX_EVENTS];

    // 4. Main Event Loop
    while (keep_running) {
        // Use a short timeout so we can check keep_running cleanly
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
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
                char buf[sizeof(OrderRequest)];
                ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

                if (bytes <= 0) {
                    std::cout << "Client disconnected: fd " << fd << "\n";
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    continue;
                }

                if (bytes == sizeof(OrderRequest)) {
                    // Start latency measurement right after bytes hit user space
                    auto t_start = std::chrono::high_resolution_clock::now();

                    OrderRequest req;
                    std::memcpy(&req, buf, sizeof(OrderRequest));

                    if (req.type == 'N') {
                        engine.new_order(req.order_id, req.side, req.price, req.quantity);
                        ExecutionReport ack{'E', req.order_id, 0, 0, 'A'};
                        send(fd, &ack, sizeof(ack), 0);
                    } else if (req.type == 'C') {
                        engine.cancel_order(req.order_id);
                        ExecutionReport ack{'E', req.order_id, 0, 0, 'C'};
                        send(fd, &ack, sizeof(ack), 0);
                    } else if (req.type == 'O') {
                        OrderbookSnapshot snap;
                        std::memset(&snap, 0, sizeof(snap));
                        engine.get_orderbook_snapshot(snap);
                        send(fd, &snap, sizeof(snap), 0);
                        continue;
                    } else if (req.type == 'M') {
                        MemoryStateSnapshot snap;
                        std::memset(&snap, 0, sizeof(snap));
                        engine.get_memory_snapshot(snap);
                        send(fd, &snap, sizeof(snap), 0);
                        continue;
                    } else if (req.type == 'D') {
                        NodeSnapshot snap;
                        std::memset(&snap, 0, sizeof(snap));
                        engine.get_node_snapshot(snap);
                        send(fd, &snap, sizeof(snap), 0);
                        continue;
                    }

                    auto fills = engine.take_fills();
                    for (const auto& fill : fills) {
                        ExecutionReport report{'E', fill.buy_order_id, fill.quantity, fill.price, 'F'};
                        send(fd, &report, sizeof(report), 0);
                        
                        if (fill.buy_order_id != fill.sell_order_id) {
                            report.order_id = fill.sell_order_id;
                            send(fd, &report, sizeof(report), 0);
                        }
                    }

                    // Stop latency measurement after processing and sending is fully complete
                    auto t_end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
                    telemetry.record(duration);
                }
            }
        }
    }

    std::cout << "\nShutting down engine...\n";
    telemetry.dump_percentiles();
    
    close(listen_fd);
    close(epfd);
    return 0;
}
