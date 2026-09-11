#ifndef DASHBOARD_UI_HPP
#define DASHBOARD_UI_HPP

#include <SDL2/SDL.h>
#include <opencv2/opencv.hpp>
#include <string>
#include "FrameBuffer.hpp"
#include "Telemetry.hpp"
#include "ServerPhoneCommunication.hpp"

class DashboardUI {
public:
    DashboardUI(const FrameBuffer& frame_buffer, const Telemetry& telemetry,
                const std::string& title = "Autonomous Rover Dashboard", int width = 1280, int height = 720);
    ~DashboardUI();

    bool isRunning() const { return m_running; }
    void handleEvents(ServerPhoneCommunication& comm);
    void update(ServerPhoneCommunication& comm);

private:
    const FrameBuffer& m_frame_buffer;
    const Telemetry& m_telemetry;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    bool m_running = false;
    int m_width;
    int m_height;
};

#endif
