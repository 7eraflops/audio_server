#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

class File_server
{
private:
    int server_fd;
    int port;
    std::string shared_directory;
    std::mutex clients_mutex;
    std::unordered_map<int, std::unique_ptr<std::thread>> client_threads;
    std::atomic<bool> running{true};

    int discovery_socket;
    std::thread discovery_thread;

    static constexpr size_t BUFFER_SIZE = 8192;

    void announce_presence()
    {
        while (running)
        {
            // Format message with server port
            std::string msg = "FileServer:" + std::to_string(port);

            // Send to multicast group
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("239.255.255.250"); // Multicast address
            addr.sin_port = htons(8888);                         // Discovery port

            sendto(discovery_socket, msg.c_str(), msg.length(), 0,
                   (struct sockaddr *)&addr, sizeof(addr));

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void handle_client(int client_socket)
    {
        char buffer[BUFFER_SIZE];

        try
        {
            while (running)
            {
                // Receive command from client
                ssize_t bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
                if (bytes_read <= 0)
                    break;

                buffer[bytes_read] = '\0';
                std::string command(buffer);

                if (command == "LIST")
                {
                    send_file_list(client_socket);
                }
                else if (command.substr(0, 4) == "GET ")
                {
                    std::string filename = command.substr(4);
                    send_file(client_socket, filename);
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Client handler error: " << e.what() << std::endl;
        }

        close(client_socket);

        // Remove this client's thread from the map
        std::lock_guard<std::mutex> lock(clients_mutex);
        auto it = client_threads.find(client_socket);
        if (it != client_threads.end())
        {
            if (it->second && it->second->joinable())
            {
                it->second->detach(); // Detach the thread before removing it
            }
            client_threads.erase(it);
        }
    }

    void send_file_list(int client_socket)
    {
        std::string file_list;

        try
        {
            for (const auto &entry : fs::directory_iterator(shared_directory))
            {
                if (fs::is_regular_file(entry))
                {
                    file_list += entry.path().filename().string() + "\n";
                }
            }
        }
        catch (const fs::filesystem_error &e)
        {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
            file_list = "Error accessing directory\n";
        }

        // Always send at least an empty string
        uint32_t size = htonl(file_list.size());
        send(client_socket, &size, sizeof(size), 0);
        if (!file_list.empty())
        {
            send(client_socket, file_list.c_str(), file_list.size(), 0);
        }
    }

    void send_file(int client_socket, const std::string &filename)
    {
        // Sanitize filename to prevent directory traversal
        std::string safe_filename = fs::path(filename).filename().string();
        std::string filepath = shared_directory + "/" + safe_filename;

        // Check if file exists and is accessible
        std::ifstream file(filepath, std::ios::binary);
        if (!file || !fs::exists(filepath))
        {
            uint32_t size = htonl(0);
            send(client_socket, &size, sizeof(size), 0);
            return;
        }

        // Get file size
        uintmax_t file_size;
        try
        {
            file_size = fs::file_size(filepath);
        }
        catch (const fs::filesystem_error &e)
        {
            uint32_t size = htonl(0);
            send(client_socket, &size, sizeof(size), 0);
            return;
        }

        // Send file size
        uint32_t size_net = htonl(static_cast<uint32_t>(file_size));
        send(client_socket, &size_net, sizeof(size_net), 0);

        // Send file content
        char buffer[BUFFER_SIZE];
        while (file.read(buffer, BUFFER_SIZE))
        {
            ssize_t sent = send(client_socket, buffer, BUFFER_SIZE, 0);
            if (sent <= 0)
                break;
        }

        if (file.gcount() > 0)
        {
            send(client_socket, buffer, file.gcount(), 0);
        }
    }

public:
    File_server(int port, const std::string &directory)
        : port(port), shared_directory(directory)
    {
        // Create directory if it doesn't exist
        try
        {
            if (!fs::exists(shared_directory))
            {
                fs::create_directories(shared_directory);
            }
        }
        catch (const fs::filesystem_error &e)
        {
            throw std::runtime_error("Failed to create directory: " + std::string(e.what()));
        }
    }

    void start()
    {
        struct sockaddr_in address;

        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0)
        {
            throw std::runtime_error("Socket creation failed");
        }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        {
            throw std::runtime_error("Setsockopt failed");
        }

        discovery_socket = socket(AF_INET, SOCK_DGRAM, 0);
        int ttl = 1;
        setsockopt(discovery_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // Start discovery thread
        discovery_thread = std::thread(&File_server::announce_presence, this);

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        {
            throw std::runtime_error("Bind failed");
        }

        if (listen(server_fd, 10) < 0)
        {
            throw std::runtime_error("Listen failed");
        }

        std::cout << "Server listening on port " << port << std::endl;
        std::cout << "Sharing directory: " << fs::absolute(shared_directory) << std::endl;

        while (running)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket < 0)
                continue;

            std::cout << "New client connected: "
                      << inet_ntoa(client_addr.sin_addr) << ":"
                      << ntohs(client_addr.sin_port) << std::endl;

            try
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                client_threads[client_socket] = std::make_unique<std::thread>(
                    &File_server::handle_client, this, client_socket);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Failed to create client thread: " << e.what() << std::endl;
                close(client_socket);
            }
        }
    }

    void stop()
    {
        running = false;
        if (discovery_thread.joinable())
        {
            discovery_thread.join();
        }
        close(discovery_socket);
        close(server_fd);

        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto &pair : client_threads)
        {
            if (pair.second && pair.second->joinable())
            {
                pair.second->join();
            }
        }
        client_threads.clear();
    }

    ~File_server()
    {
        stop();
    }
};