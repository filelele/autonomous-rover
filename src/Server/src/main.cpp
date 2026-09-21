#include "FrameBuffer.hpp"
#include "Location2D.hpp"
#include "Telemetry.hpp"
#include "ServerPhoneCommunication.hpp"
#include "DashboardUI.hpp"
#include "LingbotMapLocalizer.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <cstdlib>
#include <SDL2/SDL.h>

int main() {
    std::cout << "Starting..." << std::endl;

    // Shared structures
    FrameBuffer frame_buffer;
    Location location;
    Telemetry telemetry;

    // Nodes
    ServerPhoneCommunication communication(frame_buffer, telemetry);

    const char* phone_ip_char = std::getenv("TAILSCALE_PHONE_IP");
    std::string phone_ip = phone_ip_char;
    int control_port = 8888;
    int video_port = 8889;
    
    communication.initialize(phone_ip, control_port, video_port);

    LingbotMapLocalizer localizer(frame_buffer, location, communication);
    DashboardUI ui(frame_buffer, telemetry, localizer.getMap(), "Autonomous Rover Dashboard", 1280, 720);

    auto last_control_send = std::chrono::steady_clock::now();

    // Main UI Loop
    while (ui.isRunning()) {
        ui.handleEvents(communication);

        // Manual Control (30Hz)
        auto now = std::chrono::steady_clock::now();
        if (now - last_control_send >= std::chrono::milliseconds(50)) {
            last_control_send = now;

            const Uint8* state = SDL_GetKeyboardState(NULL);
            float heading = 0.0f;
            float angle = 0.0f;

            if (state[SDL_SCANCODE_W]) heading += 0.4f;
            if (state[SDL_SCANCODE_S]) heading -= 0.4f;
            if (state[SDL_SCANCODE_A]) angle -= 0.3f;
            if (state[SDL_SCANCODE_D]) angle += 0.3f;

            if (telemetry.manual_mode_state) {
                communication.sendManualControl(heading, angle);
            }
            ui.update(communication);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::cout << "Shutting down..." << std::endl;
    communication.stopCommunication();

    return 0;
}
