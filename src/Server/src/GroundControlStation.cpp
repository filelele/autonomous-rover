#include "ServerPhoneCommunication.hpp"
#include "DashboardUI.hpp"
#include "LingbotMapLocalizer.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <fstream>
#include <cstdlib>
#include <SDL2/SDL.h>

int main() {
    std::cout << "Starting Ground Control Station..." << std::endl;

    ServerPhoneCommunication communication;

    const char* phone_ip_char = std::getenv("TAILSCALE_PHONE_IP");
    std::string phone_ip = phone_ip_char;
    int control_port = 8888;
    int video_port = 8889;

    std::cout << "Initializing Communication. Waiting for Phone at " << phone_ip << ":" << control_port << " and " << video_port << "..." << std::endl;
    std::thread comm_thread([&communication, phone_ip, control_port, video_port]() {
        communication.initialize(phone_ip, control_port, video_port);
    });

    LingbotMapLocalizer localizer(communication);
    DashboardUI ui("Autonomous Rover Dashboard", 1280, 720);
    
    auto last_control_send = std::chrono::steady_clock::now();

    // Main UI Loop
    while (ui.isRunning()) {
        ui.handleEvents(communication);

        // Manual Control (30Hz)
        auto now = std::chrono::steady_clock::now();
        if (now - last_control_send >= std::chrono::milliseconds(33)) {
            last_control_send = now;

            const Uint8* state = SDL_GetKeyboardState(NULL);
            float heading = 0.0f;
            float angle = 0.0f;

            if (state[SDL_SCANCODE_W]) heading += 0.4f;
            if (state[SDL_SCANCODE_S]) heading -= 0.4f;
            if (state[SDL_SCANCODE_A]) angle -= 0.3f;
            if (state[SDL_SCANCODE_D]) angle += 0.3f;

            if (communication.getManualModeState()) {
                communication.sendManualControl(heading, angle);
            }
        }

        auto frame = communication.getLatestFrame();
        if (frame && !frame->bgr.empty()) {
            ui.update(communication);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::cout << "Shutting down..." << std::endl;
    communication.stopCommunication();
    if (comm_thread.joinable()) {
        comm_thread.join();
    }

    return 0;
}
