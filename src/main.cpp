#include "File_server.hpp"
#include <cstdlib> // For std::atoi
#include <cstring>

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <port> <shared_directory>\n";
        return 1;
    }

    int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
        std::cerr << "Error: The port number must be a valid integer between 1 and 65535.\n";
        return 1;
    }

    if (std::strlen(argv[2]) == 0 || argv[2][0] != '/')
    {
        std::cerr << "Error: The shared directory path must be an absolute path starting with '/'.\n";
        return 1;
    }

    try
    {
        File_server server(port, argv[2]);
        server.start(); // Will run as daemon by default
        // Or server.start(false) to run in foreground
        return 0;
    }
    catch (const std::exception &e)
    {
        syslog(LOG_ERR, "Server error: %s", e.what());
        return 1;
    }
}