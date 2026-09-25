#include "DashboardUI.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <opencv2/imgproc.hpp>

DashboardUI::DashboardUI(const FrameBuffer& frame_buffer, const Telemetry& telemetry,
                         const Location& location,
                         const OccupancyGridMap& map,
                         const std::string& title, int width, int height)
    : m_frame_buffer(frame_buffer), m_telemetry(telemetry), m_location(location), m_map(map), m_width(width), m_height(height) {

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }

    m_window = SDL_CreateWindow(title.c_str(),
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                m_width, m_height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
    if (!m_renderer) {
        std::cerr << "Renderer could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }

    m_running = true;
}

DashboardUI::~DashboardUI() {
    if (m_texture) SDL_DestroyTexture(m_texture);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void DashboardUI::handleEvents(ServerPhoneCommunication& comm) {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) {
            m_running = false;
        } else if (e.type == SDL_KEYDOWN) {
            if ((SDL_GetModState() & KMOD_CTRL) != 0) {
                // Zoom in / out with Ctrl + / Ctrl -
                if (e.key.keysym.sym == SDLK_PLUS || e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_KP_PLUS) {
                    map_zoom_factor = std::max(0.1f, map_zoom_factor - 0.05f);
                } else if (e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_UNDERSCORE || e.key.keysym.sym == SDLK_KP_MINUS) {
                    map_zoom_factor = std::min(1.0f, map_zoom_factor + 0.05f);
                }
            } else if (e.key.repeat == 0) {
                switch (e.key.keysym.sym) {
                    case SDLK_TAB: m_map_expanded = !m_map_expanded; break;
                    case SDLK_m: comm.toggleManualMode(); break;
                    case SDLK_r: comm.toggleRecordData(); break;
                    case SDLK_c: comm.toggleCaptureMode(); break;
                    case SDLK_SPACE:
                        if (comm.getCaptureModeState()) {
                            comm.sendCaptureSignal();
                            m_last_capture_trigger_time = std::chrono::steady_clock::now();
                        }
                        break;
                }
            }
        }
    }
}

void DashboardUI::update(ServerPhoneCommunication& comm) {
    auto frame = m_frame_buffer.get_latest_frame();
    if (!m_running || !frame) return;

    cv::Mat bgr = frame->to_bgr();
    if (bgr.empty()) return;

    int targetWidth = m_width;
    int targetHeight = m_height;
    if (m_window) {
        SDL_GetWindowSize(m_window, &targetWidth, &targetHeight);
    }

    cv::Mat displayFrame;
    if (bgr.cols != targetWidth || bgr.rows != targetHeight) {
        cv::resize(bgr, displayFrame, cv::Size(targetWidth, targetHeight), 0, 0, cv::INTER_LINEAR);
    } else {
        displayFrame = bgr;
    }

    // Format HUD text overlay
    double fps = comm.getIncomingFps();
    bool captured = m_telemetry.capture_mode_state &&
        (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - m_last_capture_trigger_time).count() < 1500);

    char hud_buf[256];
    snprintf(hud_buf, sizeof(hud_buf),
             "StreamFPS: %s | Manual: %s | Record: %s | Capture: %s%s | Location: (%.2f, %.2f)",
             fps > 0.0 ? std::to_string(static_cast<int>(fps)).c_str() : "N/A",
             m_telemetry.manual_mode_state ? "On" : "Off",
             m_telemetry.record_data_state ? "On" : "Off",
             m_telemetry.capture_mode_state ? "On" : "Off",
             captured ? " [CAPTURED!]" : "",
             m_location.x, m_location.z);

    cv::putText(displayFrame, hud_buf, cv::Point(30, 50),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

    // Render minimap
    renderMinimap(displayFrame);

    // Recreate streaming texture if window dimensions changed
    if (!m_texture || displayFrame.cols != m_width || displayFrame.rows != m_height) {
        if (m_texture) SDL_DestroyTexture(m_texture);

        m_width = displayFrame.cols;
        m_height = displayFrame.rows;

        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_BGR24,
                                      SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
        if (!m_texture) {
            std::cerr << "Unable to create texture! SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }
    }

    SDL_UpdateTexture(m_texture, nullptr, displayFrame.data, displayFrame.step);
    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);
}

void DashboardUI::renderMinimap(cv::Mat& displayFrame) {
    if (!m_map.loaded || m_map.image.empty()) return;

    // Rover coordinates in map grid cells
    float rover_x = std::isfinite(m_location.x) ? m_location.x : m_map.origin_x;
    float rover_z = std::isfinite(m_location.z) ? m_location.z : m_map.origin_z;
    float res = m_map.resolution > 1e-5f ? m_map.resolution : 0.05f;

    float map_x = (rover_x - m_map.origin_x) / res;
    float map_y = static_cast<float>(m_map.image.rows - 1) - (rover_z - m_map.origin_z) / res;

    const int margin = 20;

    if (m_map_expanded) {
        // Largest side = 1/2 width of whole frame, smaller side scaled to keep aspect ratio
        int max_side = std::max(64, displayFrame.cols / 2);
        int map_w = m_map.image.cols;
        int map_h = m_map.image.rows;
        if (map_w <= 0 || map_h <= 0) return;

        int render_w = 0;
        int render_h = 0;
        if (map_w >= map_h) {
            render_w = max_side;
            render_h = std::max(1, static_cast<int>(std::round(max_side * static_cast<double>(map_h) / map_w)));
        } else {
            render_h = max_side;
            render_w = std::max(1, static_cast<int>(std::round(max_side * static_cast<double>(map_w) / map_h)));
        }

        int roi_x = displayFrame.cols - render_w - margin;
        int roi_y = displayFrame.rows - render_h - margin;
        if (roi_x < 0 || roi_y < 0) return;

        cv::Mat map_resized;
        cv::resize(m_map.image, map_resized, cv::Size(render_w, render_h), 0, 0, cv::INTER_NEAREST);

        cv::Mat map_bgr;
        if (map_resized.channels() == 1) {
            cv::cvtColor(map_resized, map_bgr, cv::COLOR_GRAY2BGR);
        } else {
            map_bgr = map_resized;
        }

        cv::Mat roi = displayFrame(cv::Rect(roi_x, roi_y, render_w, render_h));
        cv::addWeighted(map_bgr, 0.7, roi, 0.3, 0.0, roi);

        // Rectangular bezel border
        cv::rectangle(roi, cv::Point(0, 0), cv::Point(render_w - 1, render_h - 1), cv::Scalar(255, 255, 255), 2);

        // Rover marker position on the resized map
        int rx = static_cast<int>(std::round(map_x * (static_cast<double>(render_w) / map_w)));
        int ry = static_cast<int>(std::round(map_y * (static_cast<double>(render_h) / map_h)));

        if (rx >= 0 && rx < render_w && ry >= 0 && ry < render_h) {
            cv::line(roi, cv::Point(std::max(0, rx - 10), ry), cv::Point(std::min(render_w - 1, rx + 10), ry), cv::Scalar(0, 220, 255), 1, cv::LINE_AA);
            cv::line(roi, cv::Point(rx, std::max(0, ry - 10)), cv::Point(rx, std::min(render_h - 1, ry + 10)), cv::Scalar(0, 220, 255), 1, cv::LINE_AA);

            cv::circle(roi, cv::Point(rx, ry), 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            cv::circle(roi, cv::Point(rx, ry), 5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
            cv::circle(roi, cv::Point(rx, ry), 1, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);
        }

        // Full map badge
        const char* badge_text = "Full Map";
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(badge_text, cv::FONT_HERSHEY_SIMPLEX, 0.38, 1, &baseline);
        cv::putText(roi, badge_text, cv::Point(render_w / 2 - text_size.width / 2, render_h - 8),
                    cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
        return;
    }

    // Circular minimap diameter: 1/4 of frame width (minimum 64px)
    int D = std::max(64, displayFrame.cols / 4);
    int R = D / 2;

    int roi_x = displayFrame.cols - D - margin;
    int roi_y = displayFrame.rows - D - margin;
    if (roi_x < 0 || roi_y < 0) return;

    // Affine transformation for centered crop
    int min_dim = std::min(m_map.image.cols, m_map.image.rows);
    float crop_cells = std::max(3.0f, map_zoom_factor * static_cast<float>(min_dim));
    double scale = static_cast<double>(D) / crop_cells;

    cv::Mat M = (cv::Mat_<double>(2, 3) <<
        scale, 0.0,   static_cast<double>(R) - scale * map_x,
        0.0,   scale, static_cast<double>(R) - scale * map_y
    );

    cv::Mat minimap_patch;
    cv::warpAffine(m_map.image, minimap_patch, M, cv::Size(D, D),
                   cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));

    cv::Mat minimap_bgr;
    cv::cvtColor(minimap_patch, minimap_bgr, cv::COLOR_GRAY2BGR);

    // Alpha blending: 70% minimap, 30% background video
    cv::Mat roi = displayFrame(cv::Rect(roi_x, roi_y, D, D));
    cv::Mat blended;
    cv::addWeighted(minimap_bgr, 0.7, roi, 0.3, 0.0, blended);

    // Circular mask
    cv::Mat circle_mask = cv::Mat::zeros(D, D, CV_8UC1);
    cv::circle(circle_mask, cv::Point(R, R), R - 2, cv::Scalar(255), -1, cv::LINE_AA);
    blended.copyTo(roi, circle_mask);

    // HUD overlays: bezel ring, crosshairs, and center marker
    cv::circle(roi, cv::Point(R, R), R - 2, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    cv::line(roi, cv::Point(R - 12, R), cv::Point(R + 12, R), cv::Scalar(0, 220, 255), 1, cv::LINE_AA);
    cv::line(roi, cv::Point(R, R - 12), cv::Point(R, R + 12), cv::Scalar(0, 220, 255), 1, cv::LINE_AA);

    cv::circle(roi, cv::Point(R, R), 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
    cv::circle(roi, cv::Point(R, R), 5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    cv::circle(roi, cv::Point(R, R), 1, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);

    // Zoom badge
    char zoom_buf[32];
    snprintf(zoom_buf, sizeof(zoom_buf), "Zoom: %.2fx", map_zoom_factor);
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(zoom_buf, cv::FONT_HERSHEY_SIMPLEX, 0.38, 1, &baseline);
    cv::putText(roi, zoom_buf, cv::Point(R - text_size.width / 2, D - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
}
