#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <atomic>
#include <vector>
#include <memory>
#include <iostream>
#include <sstream>

#include "orderbook/types.h"
#include "utils/spsc_queue.h"

// Note: Ensure that -lws2_32 is passed to the linker when compiling on Windows/MinGW

class TcpServer {
public:
    TcpServer(uint16_t port, SPSCQueue<std::shared_ptr<Order>>* order_queue)
        : port_(port), q_orders_(order_queue), running_(false), listener_thread_(NULL) {}

    ~TcpServer() { stop(); }

    void start() {
        if (running_) return;
        running_ = true;
        listener_thread_ = CreateThread(NULL, 0, ListenThreadWrapper, this, 0, NULL);
    }

    void stop() {
        running_ = false;
        if (listener_thread_ != NULL) {
            WaitForSingleObject(listener_thread_, INFINITE);
            CloseHandle(listener_thread_);
            listener_thread_ = NULL;
        }
    }

private:
    static DWORD WINAPI ListenThreadWrapper(LPVOID lpParam) {
        TcpServer* server = static_cast<TcpServer*>(lpParam);
        server->listen_loop();
        return 0;
    }
    
    struct ClientArgs {
        TcpServer* server;
        SOCKET client_socket;
    };
    
    static DWORD WINAPI ClientThreadWrapper(LPVOID lpParam) {
        ClientArgs* args = static_cast<ClientArgs*>(lpParam);
        args->server->handle_client(args->client_socket);
        delete args;
        return 0;
    }

    void listen_loop() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[TCP Server] WSAStartup failed.\n";
            return;
        }

        SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == INVALID_SOCKET) {
            std::cerr << "[TCP Server] Socket creation failed.\n";
            WSACleanup();
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
            std::cerr << "[TCP Server] Bind failed.\n";
            closesocket(server_fd);
            WSACleanup();
            return;
        }

        if (listen(server_fd, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "[TCP Server] Listen failed.\n";
            closesocket(server_fd);
            WSACleanup();
            return;
        }

        std::cout << "[TCP Server] Listening for external clients on port " << port_ << "...\n";

        // Non-blocking mode so we can periodically check the running_ flag
        u_long mode = 1;
        ioctlsocket(server_fd, FIONBIO, &mode);

        while (running_) {
            SOCKET client_socket = accept(server_fd, nullptr, nullptr);
            if (client_socket == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    Sleep(50);
                    continue;
                }
                break;
            }
            
            std::cout << "[TCP Server] External Client connected.\n";
            ClientArgs* args = new ClientArgs{this, client_socket};
            HANDLE hClient = CreateThread(NULL, 0, ClientThreadWrapper, args, 0, NULL);
            if (hClient) CloseHandle(hClient); // Detach
        }

        closesocket(server_fd);
        WSACleanup();
    }

    void handle_client(SOCKET client_socket) {
        char buffer[1024];
        uint64_t order_id_counter = 5000000; // External client order IDs start at 5M
        
        while (running_) {
            int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read <= 0) break; // Client disconnected or error

            buffer[bytes_read] = '\0';
            std::string msg(buffer);
            
            // Extremely basic protocol: "BUY AAPL 100 150.0"
            std::stringstream ss(msg);
            std::string side_str, sym;
            uint32_t qty;
            double price;

            if (ss >> side_str >> sym >> qty >> price) {
                auto o = global_order_pool.acquire();
                o->order_id = ++order_id_counter;
                o->trader_id = 999; // External Client ID
                o->timestamp_ns = 0; // To be assigned by Exchange
                o->side = (side_str == "BUY" || side_str == "buy") ? Side::BUY : Side::SELL;
                o->type = OrderType::LIMIT;
                o->price = price;
                o->quantity = qty;
                o->filled_qty = 0;
                o->status = OrderStatus::NEW;
                o->tif = TimeInForce::GTC;
                o->set_symbol(sym.c_str());

                // Push to the lock-free Ring Buffer to inject into the Matching Engine
                while (!q_orders_->push(o) && running_) {
                    Sleep(0); // Yield to other threads
                }
                
                std::string ack = "ACK OrderID " + std::to_string(o->order_id) + "\n";
                send(client_socket, ack.c_str(), ack.length(), 0);
            }
        }
        closesocket(client_socket);
        std::cout << "[TCP Server] External Client disconnected.\n";
    }

    uint16_t port_;
    SPSCQueue<std::shared_ptr<Order>>* q_orders_;
    std::atomic<bool> running_;
    HANDLE listener_thread_;
};
