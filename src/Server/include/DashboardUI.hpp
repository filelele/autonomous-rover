#ifndef DASHBOARD_UI_HPP
#define DASHBOARD_UI_HPP

#include <SDL2/SDL.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <chrono>
#include "FrameBuffer.hpp"
#include "Telemetry.hpp"
#include "Location2D.hpp"
#include "ServerPhoneCommunication.hpp"
#include "OccupancyGridMap.hpp"

class DashboardUI {
public:
    DashboardUI(const FrameBuffer& frame_buffer, const Telemetry& telemetry,
                const Location& location,
                const OccupancyGridMap& map,
                const std::string& title = "Autonomous Rover Dashboard", int width = 1280, int height = 720);
    ~DashboardUI();

    bool isRunning() const { return m_running; }
    void handleEvents(ServerPhoneCommunication& comm);
    void update(ServerPhoneCommunication& comm);

private:
    void renderMinimap(cv::Mat& displayFrame);

    const FrameBuffer& m_frame_buffer;
    const Telemetry& m_telemetry;
    const Location& m_location;
    const OccupancyGridMap& m_map;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    bool m_running = false;
    int m_width;
    int m_height;

    // Minimap state
    float map_zoom_factor = 0.5f; // Default 0.5x of min map dimension, ranges [0.1x, 1.0x]
    bool m_map_expanded = false;

    // Capture feedback
    std::chrono::steady_clock::time_point m_last_capture_trigger_time{};

    // Trajectory tracking on minimap
    cv::Mat m_trajectory_map;
    bool m_prev_record_state = false;
    bool m_has_prev_pos = false;
    cv::Point m_prev_map_pt{0, 0};
};

#endif
