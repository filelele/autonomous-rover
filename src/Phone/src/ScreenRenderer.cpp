#include "ScreenRenderer.hpp"
#include <android/log.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#define TAG_RENDERER "ScreenRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG_RENDERER, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG_RENDERER, __VA_ARGS__)

namespace {

bool make_dir_recursive(const std::string& path) {
    if (path.empty()) return false;
    char tmp[512];
    size_t len = path.length();
    if (len >= sizeof(tmp)) return false;
    std::memcpy(tmp, path.c_str(), len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (access(tmp, F_OK) != 0) {
                if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            *p = '/';
        }
    }
    if (access(tmp, F_OK) != 0) {
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

std::string get_default_captures_dir() {
    const char* candidates[] = {
        "/sdcard/Download/captures",
        "/sdcard/Android/data/com.filelele.autonomousrover.phone/files/captures",
        "/data/data/com.filelele.autonomousrover.phone/files/captures",
        "/sdcard/Download"
    };

    for (const char* dir : candidates) {
        if (make_dir_recursive(dir) && access(dir, W_OK) == 0) {
            return std::string(dir);
        }
    }
    return "/sdcard/Download";
}

std::string get_timestamp_string() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm bt{};
    localtime_r(&in_time_t, &bt);

    std::ostringstream ss;
    ss << std::put_time(&bt, "%Y%m%d_%H%M%S") << "_" << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

} // namespace

ScreenRenderer::ScreenRenderer(const FrameBuffer& frame_buffer, const bool& capture_mode, std::string output_dir)
    : m_frame_buffer(frame_buffer), m_capture_mode(capture_mode), m_output_dir(std::move(output_dir)) {
    if (m_output_dir.empty()) {
        m_output_dir = get_default_captures_dir();
    } else {
        make_dir_recursive(m_output_dir);
    }
    LOGI("ScreenRenderer initialized with capture directory: %s", m_output_dir.c_str());
}

ScreenRenderer::~ScreenRenderer() {
    stop();
}

void ScreenRenderer::setWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(m_window_mutex);
    m_window = window;
    if (!m_window) {
        LOGI("Native window cleared.");
    } else {
        LOGI("Native window set: %p (%dx%d)",
             m_window, ANativeWindow_getWidth(m_window), ANativeWindow_getHeight(m_window));
    }
}

bool ScreenRenderer::handleInputEvent(const AInputEvent* event) {
    if (!m_capture_mode) return false;

    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_UP) {
            float x = AMotionEvent_getX(event, 0);
            float y = AMotionEvent_getY(event, 0);

            cv::Point btn_c;
            int btn_r;
            cv::Point rot_c;
            int rot_r;
            {
                std::lock_guard<std::mutex> lock(m_button_mutex);
                btn_c = m_button_center;
                btn_r = m_button_radius;
                rot_c = m_rotate_button_center;
                rot_r = m_rotate_button_radius;
            }

            // Check shutter button
            float dx = x - static_cast<float>(btn_c.x);
            float dy = y - static_cast<float>(btn_c.y);
            float touch_radius = static_cast<float>(btn_r) * 1.4f; // slightly larger touch target
            if ((dx * dx + dy * dy) <= (touch_radius * touch_radius)) {
                LOGI("Shutter button touched at (%.1f, %.1f)! Triggering capture.", x, y);
                triggerCapture();
                return true;
            }

            // Check rotate button
            float rdx = x - static_cast<float>(rot_c.x);
            float rdy = y - static_cast<float>(rot_c.y);
            float rot_touch_radius = static_cast<float>(rot_r) * 1.5f;
            if ((rdx * rdx + rdy * rdy) <= (rot_touch_radius * rot_touch_radius)) {
                rotatePreview();
                return true;
            }
        }
    }
    return false;
}

void ScreenRenderer::rotatePreview() {
    int current = m_rotation_degrees.load(std::memory_order_relaxed);
    int next = (current + 90) % 360;
    m_rotation_degrees.store(next, std::memory_order_relaxed);
    LOGI("Preview rotation set to %d degrees", next);
}

void ScreenRenderer::triggerCapture() {
    m_capture_requested.store(true, std::memory_order_release);
}

void ScreenRenderer::start() {
    if (m_is_running.load(std::memory_order_acquire)) return;
    m_is_running.store(true, std::memory_order_release);
    m_render_thread = std::thread(&ScreenRenderer::renderLoop, this);
    LOGI("ScreenRenderer thread started.");
}

void ScreenRenderer::stop() {
    if (!m_is_running.load(std::memory_order_acquire)) return;
    m_is_running.store(false, std::memory_order_release);
    if (m_render_thread.joinable()) {
        m_render_thread.join();
    }
    LOGI("ScreenRenderer thread stopped.");
}

void ScreenRenderer::clearScreen(ANativeWindow* window) {
    if (!window) return;
    int win_w = ANativeWindow_getWidth(window);
    int win_h = ANativeWindow_getHeight(window);
    if (win_w <= 0 || win_h <= 0) return;

    ANativeWindow_setBuffersGeometry(window, win_w, win_h, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
        if (buffer.bits != nullptr) {
            std::memset(buffer.bits, 0, static_cast<size_t>(buffer.stride * buffer.height * 4));
        }
        ANativeWindow_unlockAndPost(window);
    }
}

std::string ScreenRenderer::getCaptureFilename() {
    make_dir_recursive(m_output_dir);
    return m_output_dir + "/capture_" + get_timestamp_string() + ".jpg";
}

void ScreenRenderer::renderLoop() {
    bool was_displaying = false;

    while (m_is_running.load(std::memory_order_relaxed)) {
        // When capture mode is OFF: display nothing
        if (!m_capture_mode) {
            if (was_displaying) {
                std::lock_guard<std::mutex> lock(m_window_mutex);
                clearScreen(m_window);
                was_displaying = false;
                LOGI("Capture mode OFF: cleared phone display.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        was_displaying = true;

        ANativeWindow* window = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_window_mutex);
            window = m_window;
        }

        if (!window) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        FramePtr frame = m_frame_buffer.get_latest_frame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        cv::Mat bgr = frame->to_bgr();
        if (bgr.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Save raw image if capture was requested (unscaled, unrotated original geometry)
        if (m_capture_requested.exchange(false, std::memory_order_acq_rel)) {
            std::string save_path = getCaptureFilename();
            // User requirement: quality 100 or anything as long as it doesn't affect geometry properties
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 100};
            if (cv::imwrite(save_path, bgr, params)) {
                LOGI("Saved JPEG (%dx%d) to: %s", bgr.cols, bgr.rows, save_path.c_str());
                m_last_capture_time = std::chrono::steady_clock::now();
                size_t slash = save_path.find_last_of('/');
                m_last_capture_name = (slash != std::string::npos) ? save_path.substr(slash + 1) : save_path;
            } else {
                LOGE("Failed to save JPEG to: %s", save_path.c_str());
            }
        }

        int win_w = ANativeWindow_getWidth(window);
        int win_h = ANativeWindow_getHeight(window);
        if (win_w <= 0 || win_h <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        ANativeWindow_setBuffersGeometry(window, win_w, win_h, WINDOW_FORMAT_RGBA_8888);

        // Rotate preview according to current user setting (0, 90, 180, 270 deg)
        int rotation = m_rotation_degrees.load(std::memory_order_relaxed);
        cv::Mat rot_bgr;
        if (rotation == 90) {
            cv::rotate(bgr, rot_bgr, cv::ROTATE_90_CLOCKWISE);
        } else if (rotation == 180) {
            cv::rotate(bgr, rot_bgr, cv::ROTATE_180);
        } else if (rotation == 270) {
            cv::rotate(bgr, rot_bgr, cv::ROTATE_90_COUNTERCLOCKWISE);
        } else {
            rot_bgr = bgr;
        }

        // Frame sizing per user instruction:
        // "if the frame is smaller than screen then dont resize rescale, just pad it with black
        // or somehow achieve the same effect to fill the screen remaining area. only resize if image size is larger than phone display res."
        cv::Mat disp_bgr;
        if (rot_bgr.cols <= win_w && rot_bgr.rows <= win_h) {
            // Keep original 1:1 pixel resolution
            disp_bgr = rot_bgr;
        } else {
            // Only downscale if image is larger than phone display resolution
            double scale = std::min(static_cast<double>(win_w) / rot_bgr.cols,
                                    static_cast<double>(win_h) / rot_bgr.rows);
            int new_w = std::max(1, static_cast<int>(rot_bgr.cols * scale));
            int new_h = std::max(1, static_cast<int>(rot_bgr.rows * scale));
            cv::resize(rot_bgr, disp_bgr, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);
        }

        // Full screen canvas padded with black
        cv::Mat canvas(win_h, win_w, CV_8UC3, cv::Scalar(0, 0, 0));
        int offset_x = (win_w - disp_bgr.cols) / 2;
        int offset_y = (win_h - disp_bgr.rows) / 2;
        disp_bgr.copyTo(canvas(cv::Rect(offset_x, offset_y, disp_bgr.cols, disp_bgr.rows)));

        // Compute shutter and rotate button locations and sizes
        int btn_r = std::clamp(std::min(win_w, win_h) / 12, 36, 52);
        cv::Point btn_c;
        int rot_r = static_cast<int>(btn_r * 0.65f);
        cv::Point rot_c;

        if (win_h >= win_w) {
            // Portrait: shutter centered near bottom
            btn_c = cv::Point(win_w / 2, win_h - std::max(win_h / 10, 80));
            // Rotate button to the right of shutter button
            int rot_x = btn_c.x + static_cast<int>(btn_r * 2.2f);
            if (rot_x + rot_r > win_w - 15) rot_x = win_w - rot_r - 15;
            rot_c = cv::Point(rot_x, btn_c.y);
        } else {
            // Landscape: shutter centered near right edge
            btn_c = cv::Point(win_w - std::max(win_w / 10, 80), win_h / 2);
            // Rotate button below shutter button
            int rot_y = btn_c.y + static_cast<int>(btn_r * 2.2f);
            if (rot_y + rot_r > win_h - 15) rot_y = win_h - rot_r - 15;
            rot_c = cv::Point(btn_c.x, rot_y);
        }

        {
            std::lock_guard<std::mutex> lock(m_button_mutex);
            m_button_center = btn_c;
            m_button_radius = btn_r;
            m_rotate_button_center = rot_c;
            m_rotate_button_radius = rot_r;
        }

        // Draw camera shutter button:
        // Outer white ring
        cv::circle(canvas, btn_c, btn_r, cv::Scalar(255, 255, 255), 4, cv::LINE_AA);

        // Inner filled circle with press feedback
        auto now = std::chrono::steady_clock::now();
        auto elapsed_capture_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_capture_time).count();
        if (elapsed_capture_ms < 180) {
            // Shutter press flash feedback
            cv::circle(canvas, btn_c, btn_r - 10, cv::Scalar(180, 180, 180), -1, cv::LINE_AA);
        } else {
            cv::circle(canvas, btn_c, btn_r - 8, cv::Scalar(245, 245, 245), -1, cv::LINE_AA);
        }

        // Draw rotate button
        cv::circle(canvas, rot_c, rot_r, cv::Scalar(220, 220, 220), 2, cv::LINE_AA);
        cv::circle(canvas, rot_c, rot_r - 3, cv::Scalar(60, 60, 60), -1, cv::LINE_AA);
        std::string rot_label = std::to_string(rotation) + "o";
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(rot_label, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
        cv::putText(canvas, rot_label,
                    cv::Point(rot_c.x - text_size.width / 2, rot_c.y + text_size.height / 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);

        // Rotation hint text
        std::string rot_hint = "Preview Rot: " + std::to_string(rotation) + " deg";
        cv::putText(canvas, rot_hint, cv::Point(20, win_h - 20),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(160, 160, 160), 1, cv::LINE_AA);

        // Toast feedback after capture
        if (elapsed_capture_ms < 2000 && !m_last_capture_name.empty()) {
            std::string toast = "Saved: " + m_last_capture_name;
            cv::putText(canvas, toast, cv::Point(30, 60),
                        cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 255, 0), 2);
        }

        // Convert canvas to RGBA for Android window
        cv::Mat rgba;
        cv::cvtColor(canvas, rgba, cv::COLOR_BGR2RGBA);

        // Blit to ANativeWindow buffer
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
            if (buffer.bits != nullptr) {
                uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
                const uint8_t* src = rgba.data;
                int copy_bytes = std::min(win_w, buffer.width) * 4;
                int rows_to_copy = std::min(win_h, buffer.height);
                for (int y = 0; y < rows_to_copy; ++y) {
                    std::memcpy(dst + y * buffer.stride * 4, src + y * rgba.step, static_cast<size_t>(copy_bytes));
                }
            }
            ANativeWindow_unlockAndPost(window);
        }

        // Sleep to yield (~30-35 fps)
        std::this_thread::sleep_for(std::chrono::milliseconds(28));
    }

    // Clear display on exit
    std::lock_guard<std::mutex> lock(m_window_mutex);
    clearScreen(m_window);
}
