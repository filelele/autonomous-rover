#include "DashboardUI.hpp"
#include "ServerPhoneCommunication.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <vector>

DashboardUI::DashboardUI(const FrameBuffer& frame_buffer, const Telemetry& telemetry,
                         const OccupancyGridMap& map,
                         const std::string& title, int width, int height)
    : m_frame_buffer(frame_buffer), m_telemetry(telemetry), m_map(map), m_width(width), m_height(height) {

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

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED /*| SDL_RENDERER_PRESENTVSYNC*/);
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
            bool ctrl_pressed = (SDL_GetModState() & KMOD_CTRL) != 0;
            if (ctrl_pressed) {
                // Ctrl + / Ctrl = / Ctrl Keypad+ -> Zoom In (smaller map crop)
                if (e.key.keysym.sym == SDLK_PLUS || e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_KP_PLUS) {
                    map_zoom_factor = std::max(0.1f, map_zoom_factor - 0.05f);
                }
                // Ctrl - / Ctrl _ / Ctrl Keypad- -> Zoom Out (larger map crop)
                else if (e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_UNDERSCORE || e.key.keysym.sym == SDLK_KP_MINUS) {
                    map_zoom_factor = std::min(1.0f, map_zoom_factor + 0.05f);
                }
            } else if (e.key.repeat == 0) { // Only first press for toggles
                if (e.key.keysym.sym == SDLK_m) {
                    comm.toggleManualMode();
                } else if (e.key.keysym.sym == SDLK_r) {
                    comm.toggleRecordData();
                } else if (e.key.keysym.sym == SDLK_c) {
                    comm.toggleCaptureMode();
                } else if (e.key.keysym.sym == SDLK_SPACE) {
                    if (comm.getCaptureModeState()) {
                        comm.sendCaptureSignal();
                        m_last_capture_trigger_time = std::chrono::steady_clock::now();
                    }
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
        displayFrame = bgr.clone();
    }
    double fps = comm.getIncomingFps();
    std::string fpsText = "StreamFPS: ";
    if (fps > 0.0) {
        fpsText += std::to_string(static_cast<int>(fps));
    }else {
        fpsText += "Not available";
    }

    bool manual_mode = m_telemetry.manual_mode_state;
    bool record_data = m_telemetry.record_data_state;
    bool capture_mode = m_telemetry.capture_mode_state;
    std::string manual_modeText = "Manual Mode: ";
    std::string record_dataText = "Record Data: ";
    std::string capture_modeText = "Capture Mode: ";
    manual_modeText += manual_mode ? "On" : "Off";
    record_dataText += record_data ? "On" : "Off";
    capture_modeText += capture_mode ? "On" : "Off";

    if (capture_mode) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_last_capture_trigger_time).count();
        if (elapsed < 1500) {
            capture_modeText += " [CAPTURED!]";
        }
    }

    float x = m_telemetry.location.x;
    float z = m_telemetry.location.z;
    float heading = m_telemetry.location.heading;
    std::string locationText = "Location: (" + std::to_string(x) + ", " + std::to_string(z) + "), Heading: " + std::to_string(heading);

    std::string finalText = fpsText + " | " + manual_modeText + " | " + record_dataText + " | " + capture_modeText + " | " + locationText;
    cv::putText(displayFrame, finalText, cv::Point(30, 50),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

    // Render 2D circular heading-up minimap
    renderMinimap(displayFrame);

    // Create or recreate texture if size changed
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

    SDL_UpdateTexture(m_texture, NULL, displayFrame.data, displayFrame.step);

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, NULL, NULL);
    SDL_RenderPresent(m_renderer);
}

void DashboardUI::renderMinimap(cv::Mat& displayFrame) {
    if (!m_map.loaded || m_map.image.empty()) return;

    // 1. Fixed size: exactly 1/4 of the width of the display frame
    int D = std::max(64, displayFrame.cols / 4);
    int R = D / 2;

    // Position at bottom-right corner with 20px padding
    int margin = 20;
    int roi_x = displayFrame.cols - D - margin;
    int roi_y = displayFrame.rows - D - margin;
    if (roi_x < 0 || roi_y < 0) return;

    // 2. Rover coordinates on the map
    float rover_x = m_telemetry.location.x;
    float rover_z = m_telemetry.location.z;
    float heading = m_telemetry.location.heading;

    float map_x = (rover_x - m_map.origin_x) / m_map.resolution;
    float map_y = static_cast<float>(m_map.image.rows - 1) - (rover_z - m_map.origin_z) / m_map.resolution;

    // 3. Crop window size: default 0.5x smaller map dimension, zoomable from 0.1x to 1.0x
    int min_dim = std::min(m_map.image.cols, m_map.image.rows);
    float crop_cells = std::max(3.0f, map_zoom_factor * static_cast<float>(min_dim));
    float scale = static_cast<float>(D) / crop_cells;

    // 4. Heading-Up rotation: rover always points North (straight UP / 12 o'clock)
    float rot_deg = -heading * (180.0f / static_cast<float>(M_PI));

    // Construct affine transformation matrix: rotate around rover position, scale, and shift to minimap center (R, R)
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(map_x, map_y), rot_deg, scale);
    M.at<double>(0, 2) += (static_cast<double>(R) - map_x);
    M.at<double>(1, 2) += (static_cast<double>(R) - map_y);

    // Warp and automatically pad out-of-bounds with Black (0 / Occupied / Unknown Void)
    cv::Mat minimap_patch;
    cv::warpAffine(m_map.image, minimap_patch, M, cv::Size(D, D),
                   cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));

    // Convert grayscale map to BGR
    cv::Mat minimap_bgr;
    cv::cvtColor(minimap_patch, minimap_bgr, cv::COLOR_GRAY2BGR);

    // 5. Semi-transparent blending: 70% minimap, 30% background video
    cv::Rect roi_rect(roi_x, roi_y, D, D);
    cv::Mat roi = displayFrame(roi_rect);

    cv::Mat blended;
    cv::addWeighted(minimap_bgr, 0.7, roi, 0.3, 0.0, blended);

    // Create circular mask (radius R - 2 to leave room for outer bezel)
    cv::Mat circle_mask = cv::Mat::zeros(D, D, CV_8UC1);
    cv::circle(circle_mask, cv::Point(R, R), R - 2, cv::Scalar(255), -1, cv::LINE_AA);

    // Copy blended circular area onto ROI
    blended.copyTo(roi, circle_mask);

    // 6. HUD Elements
    // Outer border ring (Cyan-gold)
    cv::circle(roi, cv::Point(R, R), R - 2, cv::Scalar(0, 220, 255), 2, cv::LINE_AA);

    // Subtle crosshairs
    cv::line(roi, cv::Point(R - 12, R), cv::Point(R + 12, R), cv::Scalar(120, 120, 120), 1, cv::LINE_AA);
    cv::line(roi, cv::Point(R, R - 12), cv::Point(R, R + 12), cv::Scalar(120, 120, 120), 1, cv::LINE_AA);

    // North compass needle indicator on outer ring:
    // In heading-up mode, North on the world map is located at angle (rot_deg - 90 deg) from center
    float north_rad = (rot_deg - 90.0f) * (static_cast<float>(M_PI) / 180.0f);
    int nx = static_cast<int>(R + (R - 14) * std::cos(north_rad));
    int ny = static_cast<int>(R + (R - 14) * std::sin(north_rad));
    cv::putText(roi, "N", cv::Point(nx - 4, ny + 4), cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

    // Rover icon: sleek arrow at exact center (R, R) pointing straight UP (12 o'clock)
    std::vector<cv::Point> rover_pts = {
        cv::Point(R, R - 10),     // Tip forward
        cv::Point(R - 7, R + 7),  // Bottom left
        cv::Point(R, R + 3),      // Inward notch
        cv::Point(R + 7, R + 7)   // Bottom right
    };
    cv::fillPoly(roi, std::vector<std::vector<cv::Point>>{rover_pts}, cv::Scalar(0, 0, 255), cv::LINE_AA); // Red
    cv::polylines(roi, std::vector<std::vector<cv::Point>>{rover_pts}, true, cv::Scalar(255, 255, 255), 1, cv::LINE_AA); // White outline

    // Zoom factor text badge at bottom of minimap
    char zoom_buf[32];
    snprintf(zoom_buf, sizeof(zoom_buf), "Zoom: %.2fx", map_zoom_factor);
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(zoom_buf, cv::FONT_HERSHEY_SIMPLEX, 0.38, 1, &baseline);
    cv::putText(roi, zoom_buf, cv::Point(R - text_size.width / 2, D - 8),
                cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
}
