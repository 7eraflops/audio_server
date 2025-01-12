#include "File_server.hpp"

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <port> <shared_directory>" << std::endl;
        return 1;
    }

    try
    {
        File_server server(std::stoi(argv[1]), argv[2]);
        server.start();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}