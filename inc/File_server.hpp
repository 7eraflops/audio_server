#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
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

    std::shared_mutex files_mutex;
    std::unordered_map<std::string, std::shared_mutex> file_locks;
    std::mutex file_locks_mutex;

    int discovery_socket;
    std::thread discovery_thread;

    static constexpr size_t BUFFER_SIZE = 8192;

    std::shared_mutex &get_file_lock(const std::string &filename)
    {
        std::lock_guard<std::mutex> lock(file_locks_mutex);
        return file_locks[filename];
    }

    void daemonize()
    {
        // First fork (detaches from parent process)
        pid_t pid = fork();
        if (pid < 0)
        {
            throw std::runtime_error("First fork failed");
        }
        if (pid > 0)
        {
            exit(0); // Parent process exits
        }

        // Create new session
        if (setsid() < 0)
        {
            throw std::runtime_error("setsid failed");
        }

        // Second fork (prevents the process from re-acquiring a terminal)
        pid = fork();
        if (pid < 0)
        {
            throw std::runtime_error("Second fork failed");
        }
        if (pid > 0)
        {
            exit(0);
        }

        // Change working directory
        if (chdir("/") < 0)
        {
            throw std::runtime_error("chdir failed");
        }

        // Reset file creation mask
        umask(0);

        // Store our important file descriptors
        std::vector<int> keep_fds = {server_fd, discovery_socket};

        // Close all open file descriptors except our sockets
        for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--)
        {
            if (std::find(keep_fds.begin(), keep_fds.end(), x) == keep_fds.end())
            {
                close(x);
            }
        }

        // Redirect standard streams to /dev/null
        int null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2)
        {
            close(null_fd);
        }

        // Initialize syslog
        openlog("fileserver", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }

    void announce_presence()
    {
        while (running)
        {
            std::string msg = "FileServer:" + std::to_string(port);

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("239.255.255.250");
            addr.sin_port = htons(8888);

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
                ssize_t bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
                if (bytes_read <= 0)
                    break;

                buffer[bytes_read] = '\0';
                std::string command(buffer);

                if (command == "LIST")
                {
                    syslog(LOG_INFO, "Client requested file list");
                    send_file_list(client_socket);
                }
                else if (command.substr(0, 4) == "GET ")
                {
                    std::string filename = command.substr(4);
                    syslog(LOG_INFO, "Client requested file: %s", filename.c_str());
                    send_file(client_socket, filename);
                }
                else if (command.substr(0, 4) == "PUT ")
                {
                    std::string filename = command.substr(4);
                    syslog(LOG_INFO, "Client uploading file: %s", filename.c_str());
                    receive_file(client_socket, filename);
                }
            }
        }
        catch (const std::exception &e)
        {
            syslog(LOG_ERR, "Client handler error: %s", e.what());
        }

        close(client_socket);

        std::lock_guard<std::mutex> lock(clients_mutex);
        auto it = client_threads.find(client_socket);
        if (it != client_threads.end())
        {
            if (it->second && it->second->joinable())
            {
                it->second->detach();
            }
            client_threads.erase(it);
        }
    }

    void receive_file(int client_socket, const std::string &filename)
    {
        std::string safe_filename = fs::path(filename).filename().string();
        std::string filepath = shared_directory + "/" + safe_filename;

        // First receive the file size
        uint32_t size_net;
        if (recv(client_socket, &size_net, sizeof(size_net), 0) <= 0)
        {
            syslog(LOG_ERR, "Failed to receive file size for upload: %s", safe_filename.c_str());
            return;
        }
        uint32_t file_size = ntohl(size_net);

        // Acquire exclusive lock for writing
        std::unique_lock<std::shared_mutex> file_lock(get_file_lock(safe_filename));

        // Create temporary file
        std::string temp_filepath = filepath + ".tmp";
        std::ofstream file(temp_filepath, std::ios::binary);
        if (!file)
        {
            syslog(LOG_ERR, "Failed to create temporary file for upload: %s", safe_filename.c_str());
            send(client_socket, "ERROR", 5, 0);
            return;
        }

        // Receive file content
        char buffer[BUFFER_SIZE];
        uint32_t remaining = file_size;
        bool success = true;

        while (remaining > 0 && success)
        {
            size_t to_read = std::min(remaining, (uint32_t)BUFFER_SIZE);
            ssize_t bytes_read = recv(client_socket, buffer, to_read, 0);

            if (bytes_read <= 0)
            {
                success = false;
                break;
            }

            file.write(buffer, bytes_read);
            if (!file.good())
            {
                success = false;
                break;
            }

            remaining -= bytes_read;
        }

        file.close();

        if (success)
        {
            // Atomically rename temporary file to final filename
            try
            {
                fs::rename(temp_filepath, filepath);
                send(client_socket, "OK", 2, 0);
                syslog(LOG_INFO, "File upload successful: %s", safe_filename.c_str());
            }
            catch (const fs::filesystem_error &e)
            {
                fs::remove(temp_filepath);
                send(client_socket, "ERROR", 5, 0);
                syslog(LOG_ERR, "Failed to rename uploaded file: %s", e.what());
            }
        }
        else
        {
            fs::remove(temp_filepath);
            send(client_socket, "ERROR", 5, 0);
            syslog(LOG_ERR, "File upload failed: %s", safe_filename.c_str());
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
            syslog(LOG_ERR, "Filesystem error: %s", e.what());
            file_list = "Error accessing directory\n";
        }

        uint32_t size = htonl(file_list.size());
        send(client_socket, &size, sizeof(size), 0);
        if (!file_list.empty())
        {
            send(client_socket, file_list.c_str(), file_list.size(), 0);
        }
    }

    void send_file(int client_socket, const std::string &filename)
    {
        std::string safe_filename = fs::path(filename).filename().string();
        std::string filepath = shared_directory + "/" + safe_filename;

        // Acquire shared lock for reading
        std::shared_lock<std::shared_mutex> file_lock(get_file_lock(safe_filename));

        std::ifstream file(filepath, std::ios::binary);
        if (!file || !fs::exists(filepath))
        {
            syslog(LOG_WARNING, "File not found: %s", safe_filename.c_str());
            uint32_t size = htonl(0);
            send(client_socket, &size, sizeof(size), 0);
            return;
        }

        uintmax_t file_size;
        try
        {
            file_size = fs::file_size(filepath);
            syslog(LOG_INFO, "Sending file: %s (size: %lu bytes)", safe_filename.c_str(), file_size);
        }
        catch (const fs::filesystem_error &e)
        {
            syslog(LOG_ERR, "Error getting file size: %s", e.what());
            uint32_t size = htonl(0);
            send(client_socket, &size, sizeof(size), 0);
            return;
        }

        uint32_t size_net = htonl(static_cast<uint32_t>(file_size));
        send(client_socket, &size_net, sizeof(size_net), 0);

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

    void start(bool as_daemon = true)
    {
        if (as_daemon)
        {
            daemonize();
        }

        syslog(LOG_INFO, "Starting file server on port %d", port);
        syslog(LOG_INFO, "Sharing directory: %s", fs::absolute(shared_directory).c_str());

        struct sockaddr_in address;

        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0)
        {
            syslog(LOG_ERR, "Socket creation failed");
            throw std::runtime_error("Socket creation failed");
        }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        {
            syslog(LOG_ERR, "Setsockopt failed");
            throw std::runtime_error("Setsockopt failed");
        }

        discovery_socket = socket(AF_INET, SOCK_DGRAM, 0);
        int ttl = 1;
        setsockopt(discovery_socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        discovery_thread = std::thread(&File_server::announce_presence, this);

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        {
            syslog(LOG_ERR, "Bind failed");
            throw std::runtime_error("Bind failed");
        }

        if (listen(server_fd, 10) < 0)
        {
            syslog(LOG_ERR, "Listen failed");
            throw std::runtime_error("Listen failed");
        }

        while (running)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket < 0)
                continue;

            syslog(LOG_INFO, "New client connected: %s:%d",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));

            try
            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                client_threads[client_socket] = std::make_unique<std::thread>(
                    &File_server::handle_client, this, client_socket);
            }
            catch (const std::exception &e)
            {
                syslog(LOG_ERR, "Failed to create client thread: %s", e.what());
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

        syslog(LOG_INFO, "File server stopped");
        closelog();
    }

    ~File_server()
    {
        stop();
    }
};