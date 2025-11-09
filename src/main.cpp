#include "system_monitor.h"
#include <iostream>
#include <cstdlib>

int main(int argc, char *argv[]) {
    int refresh = 2;
    if (argc >= 2) {
        int r = std::atoi(argv[1]);
        if (r > 0) refresh = r;
    }
    SystemMonitor mon(refresh);
    mon.run();
    return 0;
}
