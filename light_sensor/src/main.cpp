#include <iostream>

#include "transports/transport_wrapper.hpp"
#include "config.hpp"
#include "app_core.h"

int main() {
    std::cout << "[INIT] Light sensor node started\n";

#if defined(TRANSPORT_HAS_IP_AND_PORT)
    std::cout << "[CONFIG] Target: " << CONTROLLER_IP << ":" << CONTROLLER_PORT << "\n";
    TransportType transport(CONTROLLER_IP, CONTROLLER_PORT);
#else
    TransportType transport;
#endif

    if (!transport.init()) {
        std::cerr << "[ERROR] Transport initialization failed\n";
        return 1;
    }

    run_application(transport);
}
