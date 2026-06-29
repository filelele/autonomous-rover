#include "DashboardUI.hpp"
#include <iostream>

DashboardUI::DashboardUI(const std::string& title, int width, int height)
    : m_width(width), m_height(height) {

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return;
    }

    m_window = SDL_CreateWindow(title.c_str(),
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                m_width, m_height, SDL_WINDOW_SHOWN);
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

void DashboardUI::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) {
            m_running = false;
        }
    }
}

void DashboardUI::update(std::shared_ptr<const cv::Mat> frame) {
    if (!m_running || !frame || frame->empty()) return;

    // Create or recreate texture if size changed
    if (!m_texture || frame->cols != m_width || frame->rows != m_height) {
        if (m_texture) SDL_DestroyTexture(m_texture);

        m_width = frame->cols;
        m_height = frame->rows;

        m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_BGR24,
                                      SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
        if (!m_texture) {
            std::cerr << "Unable to create texture! SDL_Error: " << SDL_GetError() << std::endl;
            return;
        }
    }

    SDL_UpdateTexture(m_texture, NULL, frame->data, frame->step);

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, NULL, NULL);
    SDL_RenderPresent(m_renderer);
}
