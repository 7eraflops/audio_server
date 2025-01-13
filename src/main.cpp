#include "File_server.hpp"

int main()
{
    try
    {
        File_server server(8080, "/home/teraflops/git/PS/audio_server/audio");
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