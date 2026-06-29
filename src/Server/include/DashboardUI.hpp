#ifndef DASHBOARD_UI_HPP
#define DASHBOARD_UI_HPP

#include <SDL2/SDL.h>
#include <opencv2/opencv.hpp>
#include <memory>
#include <string>

class DashboardUI {
public:
    DashboardUI(const std::string& title = "Autonomous Rover Dashboard", int width = 1280, int height = 720);
    ~DashboardUI();

    bool isRunning() const { return m_running; }
    void handleEvents();
    void update(std::shared_ptr<const cv::Mat> frame);

private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    bool m_running = false;
    int m_width;
    int m_height;
};

#endif
