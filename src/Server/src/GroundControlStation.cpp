#include "DashboardListener.hpp"
#include "DashboardUI.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <fstream>
#include <cstdlib>

int main() {
    std::cout << "Starting Ground Control Station..." << std::endl;

    DashboardListener listener;
    DashboardUI ui("Autonomous Rover Dashboard", 1280, 720);

    //export TAILSCALE_PHONE_IP = 100.x.x.x
    const char* phone_ip_char = std::getenv("TAILSCALE_PHONE_IP");
    int port = 8888;
    std::string phone_ip(phone_ip_char);

    std::cout << "Initializing Listener. Waiting for Phone at " << phone_ip << ":" << port << "..." << std::endl;
    std::thread listener_thread([&listener, phone_ip, port]() {
        listener.initialize(phone_ip, port);
    });

    // Main UI Loop
    while (ui.isRunning()) {
        ui.handleEvents();

        auto frame = listener.getLatestBgr();
        if (frame) {
            ui.update(frame, listener.getIncomingFps());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::cout << "Shutting down..." << std::endl;
    listener.stopListening();
    if (listener_thread.joinable()) {
        listener_thread.join();
    }

    return 0;
}
