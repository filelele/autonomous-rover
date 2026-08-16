#include "DashboardUI.hpp"
#include "ServerPhoneCommunication.hpp"
#include <iostream>

DashboardUI::DashboardUI(const std::string& title, int width, int height)
    : m_width(width), m_height(height) {

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

    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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
            if (e.key.repeat == 0) { // Only first press
                if (e.key.keysym.sym == SDLK_m) {
                    comm.toggleManualMode();
                } else if (e.key.keysym.sym == SDLK_r) {
                    comm.toggleRecordData();
                }
            }
        }
    }
}

void DashboardUI::update(ServerPhoneCommunication& comm) {
    auto frame = comm.getLatestFrame();
    if (!m_running || !frame || frame->bgr.empty()) return;

    int targetWidth = m_width;
    int targetHeight = m_height;
    if (m_window) {
        SDL_GetWindowSize(m_window, &targetWidth, &targetHeight);
    }

    cv::Mat displayFrame;
    if (frame->bgr.cols != targetWidth || frame->bgr.rows != targetHeight) {
        cv::resize(frame->bgr, displayFrame, cv::Size(targetWidth, targetHeight), 0, 0, cv::INTER_CUBIC);
    } else {
        displayFrame = frame->bgr.clone();
    }
    double fps = comm.getIncomingFps();
    std::string fpsText = "StreamFPS: ";
    if (fps > 0.0) {
        fpsText += std::to_string(static_cast<int>(fps));
    }else {
        fpsText += "Not available";
    }

    bool manual_mode = comm.getManualModeState();
    bool record_data = comm.getRecordDataState();
    std::string manual_modeText = "Manual Mode: ";
    std::string record_dataText = "Record Data: ";
    manual_modeText += manual_mode ? "On" : "Off";
    record_dataText += record_data ? "On" : "Off";

    float x = comm.getLocation().x;
    float y = comm.getLocation().y;
    float heading = comm.getLocation().heading;
    std::string locationText = "Location: (" + std::to_string(x) + ", " + std::to_string(y) + "), Heading: " + std::to_string(heading);

    std::string finalText = fpsText + " | " + manual_modeText + " | " + record_dataText + " | " + locationText;
    cv::putText(displayFrame, finalText, cv::Point(30, 50),
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

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
