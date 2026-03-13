#include "daemon/node_daemon.h"
#include "core/coordinate.h"
#include "core/level.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* level_str = std::getenv("LINQU_LEVEL");
    const char* base_str = std::getenv("LINQU_BASE");

    if (!level_str) {
        fprintf(stderr, "Error: LINQU_LEVEL not set\n");
        return 1;
    }

    int level_val = std::atoi(level_str);
    auto level = static_cast<linqu::Level>(level_val);
    auto coord = linqu::LinquCoordinate::from_env();
    std::string base = base_str ? base_str : "/tmp/linqu";

    fprintf(stderr, "linqu_daemon: level=%s coord=%s base=%s\n",
            linqu::level_name(level), coord.to_string().c_str(), base.c_str());

    linqu::NodeDaemon daemon(level, coord, base);
    if (!daemon.start()) {
        return 1;
    }

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    while (g_running && daemon.is_running()) {
        daemon.handle_one_message(1000);
    }

    daemon.stop();
    fprintf(stderr, "linqu_daemon: shutdown complete\n");
    return 0;
}
