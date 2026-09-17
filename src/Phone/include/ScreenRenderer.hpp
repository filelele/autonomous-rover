#ifndef SCREEN_RENDERER_HPP
#define SCREEN_RENDERER_HPP

#include <android/native_window.h>
#include <android/input.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <opencv2/core.hpp>
#include "FrameBuffer.hpp"

class ScreenRenderer {
public:
    ScreenRenderer(const FrameBuffer& frame_buffer, const bool& capture_mode, std::string output_dir = "");
    ~ScreenRenderer();

    void setWindow(ANativeWindow* window);
    bool handleInputEvent(const AInputEvent* event);
    void triggerCapture();
    void rotatePreview();
    int getRotationDegrees() const { return m_rotation_degrees.load(); }
    void setRotationDegrees(int degrees) { m_rotation_degrees.store((degrees % 360 + 360) % 360); }

    void start();
    void stop();

private:
    void renderLoop();
    void clearScreen(ANativeWindow* window);
    std::string getCaptureFilename();

    const FrameBuffer& m_frame_buffer;
    const bool& m_capture_mode;
    std::string m_output_dir;

    ANativeWindow* m_window = nullptr;
    std::mutex m_window_mutex;

    std::thread m_render_thread;
    std::atomic<bool> m_is_running{false};
    std::atomic<bool> m_capture_requested{false};
    std::atomic<int> m_rotation_degrees{0};

    // Shutter button layout
    cv::Point m_button_center{0, 0};
    int m_button_radius{42};

    // Rotate button layout
    cv::Point m_rotate_button_center{0, 0};
    int m_rotate_button_radius{28};
    std::mutex m_button_mutex;

    // Visual feedback for capture
    std::chrono::steady_clock::time_point m_last_capture_time{};
    std::string m_last_capture_name;
};

#endif // SCREEN_RENDERER_HPP
