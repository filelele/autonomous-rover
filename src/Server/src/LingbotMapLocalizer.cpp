#include "LingbotMapLocalizer.hpp"
#include <chrono>
#include <thread>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#include <vector>

LingbotMapLocalizer::LingbotMapLocalizer(const FrameBuffer& frame_buffer, Location& location, ServerPhoneCommunication& communication)
    : frame_buffer(frame_buffer), location(location), communication(communication) {
    // 1) LINGBOT_IMAGES_DIR env var (preferred on a headless machine)
    // 2) zenity GTK dialog, only if a display is available (here I use ssh -X to forward the display to my laptop)
    std::cout << "[LingbotMapLocalizer] Starting localization thread..." << std::endl;
    std::cout << "[LingbotMapLocalizer] Pick fixed images folder..." << std::endl;
    const char* env_folder = std::getenv("LINGBOT_IMAGES_DIR");
    if (env_folder && *env_folder) {
        images_folder = env_folder;
        std::cout << "[LingbotMapLocalizer] Using images folder from LINGBOT_IMAGES_DIR: "
                  << images_folder << std::endl;
    } else {
        images_folder = pickImagesFolderWithDialog();
    }
    if (images_folder.empty()) {
        std::cerr << "[LingbotMapLocalizer] No fixed images folder selected." << std::endl;
        return;
    }

    loadOccupancyGrid(images_folder);

    if (!launchPython(images_folder)) {
        std::cerr << "[LingbotMapLocalizer] Python wrapper startup/KV-build failed "
                     "(check python errors above)." << std::endl;
        killPython();
        return;
    }

    worker_thread = std::thread(&LingbotMapLocalizer::localizationLoop, this);
}

LingbotMapLocalizer::~LingbotMapLocalizer() {
    is_running.store(false, std::memory_order_release);
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    if (py_stdin) {
        fputs("QUIT\n", py_stdin);
        fflush(py_stdin);
        fclose(py_stdin);
    }
    if (py_stdout) fclose(py_stdout);
    if (py_pid > 0) waitpid(py_pid, nullptr, 0);
}

std::string LingbotMapLocalizer::pickImagesFolderWithDialog() {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::cerr << "[LingbotMapLocalizer] No display available and LINGBOT_IMAGES_DIR not set."
                  << std::endl;
        return {};
    }
    FILE* pipe = popen(
        "zenity --file-selection --directory --title='Select images folder for KV cache' 2>/dev/null",
        "r");
    if (!pipe) return {};

    char buffer[4096] = {};
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);

    // Strip trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

std::string LingbotMapLocalizer::pickModelFileWithDialog() {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::cerr << "[LingbotMapLocalizer] No display available and LINGBOT_MODEL_PATH not set."
                  << std::endl;
        return {};
    }
    FILE* pipe = popen(
        "zenity --file-selection --title='Select model checkpoint (.pth/.ckpt)' "
        "--file-filter='Checkpoints | *.pth *.ckpt *.pt' --file-filter='All files | *' 2>/dev/null",
        "r");
    if (!pipe) return {};

    char buffer[4096] = {};
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

bool LingbotMapLocalizer::launchPython(const std::string& images_folder) {
    const char* py_path_char = std::getenv("LINGBOT_PYTHON");
    std::string python = py_path_char ? py_path_char : "python3";
    std::string script = __FILE__; // .../src/LingbotMapLocalizer.cpp
    script = script.substr(0, script.find_last_of('/')) + "/lingbot-map_localizer.py";

    // 1) LINGBOT_MODEL_PATH env var  2) zenity file-chooser dialog
    const char* model_char = std::getenv("LINGBOT_MODEL_PATH");
    std::string model_path = model_char ? model_char : "";
    if (model_path.empty()) {
        model_path = pickModelFileWithDialog();
        if (model_path.empty()) {
            std::cerr << "[LingbotMapLocalizer] No model checkpoint selected." << std::endl;
            return false;
        }
    }

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execlp(python.c_str(), python.c_str(), script.c_str(), model_path.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    py_stdin = fdopen(in_pipe[1], "w");
    py_stdout = fdopen(out_pipe[0], "r");
    py_pid = pid;

    return buildKVCache(images_folder);
}

bool LingbotMapLocalizer::buildKVCache(const std::string& images_folder) {
    fprintf(py_stdin, "BUILD %s\n", images_folder.c_str());
    fflush(py_stdin);

    char response[512] = {};
    if (!fgets(response, sizeof(response), py_stdout)) {
        std::cerr << "[LingbotMapLocalizer] Python process died during KV build " << std::endl;
        return false;
    }
    if (std::strncmp(response, "OK", 2) != 0) {
        std::cerr << "[LingbotMapLocalizer] KV cache build failed: " << response << std::endl;
        return false;
    }
    std::cout << "[LingbotMapLocalizer] Fixed KV cache built from " << images_folder << std::endl;
    return true;
}

void LingbotMapLocalizer::killPython() {
    if (py_pid > 0) {
        kill(py_pid, SIGTERM);
        waitpid(py_pid, nullptr, 0);
        py_pid = -1;
    }
    if (py_stdin) { fclose(py_stdin); py_stdin = nullptr; }
    if (py_stdout) { fclose(py_stdout); py_stdout = nullptr; }
}

bool LingbotMapLocalizer::sendFrame(const cv::Mat& bgr) {
    cv::Mat continuous = bgr.isContinuous() ? bgr : bgr.clone();
    fprintf(py_stdin, "FRAME %d %d\n", continuous.cols, continuous.rows);
    fwrite(continuous.data, 1, static_cast<size_t>(continuous.total()) * continuous.elemSize(), py_stdin);
    return fflush(py_stdin) == 0;
}

bool LingbotMapLocalizer::readResult(std::array<double, 16>& c2w_out) {
    int32_t status = -1;
    if (fread(&status, sizeof(status), 1, py_stdout) != 1 || status != 0) return false;
    return fread(c2w_out.data(), sizeof(double), 16, py_stdout) == 16;
}

std::array<double, 16> LingbotMapLocalizer::getLatestC2W() {
    std::lock_guard<std::mutex> lock(result_mutex);
    return latest_c2w;
}

bool LingbotMapLocalizer::validateFrame(const cv::Mat& bgr, const FramePtr& frame, std::string& out_reason) {
    // 1. Right shape check: phone camera always provides 640x480 BGR (CV_8UC3)
    if (bgr.empty() || !bgr.data) {
        out_reason = "empty BGR frame";
        return false;
    }
    if (bgr.type() != CV_8UC3) {
        out_reason = "wrong type (expected CV_8UC3, got " + std::to_string(bgr.type()) + ")";
        return false;
    }
    if (bgr.cols != 640 || bgr.rows != 480) {
        out_reason = "wrong shape: expected 640x480, got " + std::to_string(bgr.cols) + "x" + std::to_string(bgr.rows);
        return false;
    }

    // 2. Fast 10x downsample (64x48) to inspect for missing packet gray artifacts (~0.02ms)
    constexpr int SUB_W = 64;
    constexpr int SUB_H = 48;
    constexpr int TOTAL_PIXELS = SUB_W * SUB_H; // 3072
    constexpr int NUM_BANDS = 8;
    constexpr int BAND_WIDTH = SUB_W / NUM_BANDS; // 8 columns per band
    constexpr int BAND_PIXELS = BAND_WIDTH * SUB_H; // 384 pixels per band

    cv::Mat small;
    cv::resize(bgr, small, cv::Size(SUB_W, SUB_H), 0, 0, cv::INTER_NEAREST);

    // 3. Blackout check (camera disconnected or lens covered)
    cv::Scalar mean_val = cv::mean(small);
    double avg_brightness = (mean_val[0] + mean_val[1] + mean_val[2]) / 3.0;
    if (avg_brightness < 5.0) {
        out_reason = "blackout frame (brightness < 5.0)";
        return false;
    }

    // 4. Blocky grey frame detection caused by H.265 missing packets / intra-refresh
    // Decoder conceals missing packets/macroblocks with Y=128, U=128, V=128 => BGR ~(128, 128, 128) neutral gray.
    // Periodic intra-refresh packet loss manifests as vertical gray bands or gray block patches.
    int total_gray = 0;
    std::array<int, NUM_BANDS> band_gray{};

    for (int r = 0; r < SUB_H; ++r) {
        const uint8_t* row_ptr = small.ptr<uint8_t>(r);
        for (int c = 0; c < SUB_W; ++c) {
            int b = row_ptr[c * 3 + 0];
            int g = row_ptr[c * 3 + 1];
            int red = row_ptr[c * 3 + 2];

            // Neutral gray concealment: values near 128 with near-zero chroma/saturation
            if (b >= 115 && b <= 140 && g >= 115 && g <= 140 && red >= 115 && red <= 140 &&
                std::abs(b - g) <= 5 && std::abs(g - red) <= 5) {
                total_gray++;
                band_gray[c / BAND_WIDTH]++;
            }
        }
    }

    // Reject if overall gray ratio > 10%
    if (total_gray > static_cast<int>(TOTAL_PIXELS * 0.10f)) {
        out_reason = "blocky gray frame from packet loss (" +
                     std::to_string(total_gray * 100 / TOTAL_PIXELS) + "% neutral gray)";
        return false;
    }

    // Reject if any intra-refresh vertical column band has > 35% gray (column packet loss)
    for (int b = 0; b < NUM_BANDS; ++b) {
        if (band_gray[b] > static_cast<int>(BAND_PIXELS * 0.35f)) {
            out_reason = "intra-refresh gray column drop in band " + std::to_string(b) +
                         " (" + std::to_string(band_gray[b] * 100 / BAND_PIXELS) + "% gray)";
            return false;
        }
    }

    return true;
}

void LingbotMapLocalizer::localizationLoop() {
    int64_t last_localized_timestamp_ms = -1;
    int consecutive_rejected_frames = 0;
    auto last_reject_log_time = std::chrono::steady_clock::now();

    while (is_running.load(std::memory_order_acquire)) {
        auto frame = frame_buffer.get_latest_frame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (frame->timestamp_ms == last_localized_timestamp_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        cv::Mat bgr = frame->to_bgr();
        std::string reject_reason;
        if (!validateFrame(bgr, frame, reject_reason)) {
            consecutive_rejected_frames++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reject_log_time).count();
            if (elapsed_ms >= 500) {
                std::cerr << "[LingbotMapLocalizer] Dropping corrupted/invalid frame: " << reject_reason
                          << " (consecutive drops: " << consecutive_rejected_frames << ")" << std::endl;
                last_reject_log_time = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (consecutive_rejected_frames > 0) {
            std::cout << "[LingbotMapLocalizer] Frame stream recovered (" << bgr.cols << "x" << bgr.rows
                      << ") after " << consecutive_rejected_frames << " dropped frame(s)." << std::endl;
            consecutive_rejected_frames = 0;
        }

        last_localized_timestamp_ms = frame->timestamp_ms;

        std::cout << "[LingbotMapLocalizer] Sending frame " << bgr.cols << "x"
                  << bgr.rows << " to localizer..." << std::endl;
        if (!sendFrame(bgr)) break;

        std::array<double, 16> c2w{};
        if (readResult(c2w)) {
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                latest_c2w = c2w;
                has_result = true;
            }

            // Convert 4x4 c2w (OpenCV convention) to 2D location.
            // Ground plane assumed to be world X-Z; heading from camera forward axis (+Z_cam).
            // c2w row-major: translation = elements [3], [7], [11]; rotation columns = camera axes in world.
            double tx = c2w[3], ty = c2w[7], tz = c2w[11];
            Location loc;
            loc.x = static_cast<float>(tx);
            loc.z = static_cast<float>(tz);
            // Camera forward direction (+Z of camera) expressed in world frame = third column of R
            loc.heading = static_cast<float>(std::atan2(c2w[2], c2w[10]));
            loc.timestamp = frame->timestamp_ms;
            location = loc;
            communication.sendLocation(loc/*,timeframe for debug latency here*/);
        } else {
            std::cerr << "[LingbotMapLocalizer] Localization failed on frame." << std::endl;
        }
    }
}

void LingbotMapLocalizer::loadOccupancyGrid(const std::string& folder) {
    std::string png_path = folder + "/occupancy_grid/occupancy_grid.png";
    std::string yaml_path = folder + "/occupancy_grid/occupancy_grid.yaml";

    map.image = cv::imread(png_path, cv::IMREAD_GRAYSCALE);
    if (map.image.empty()) {
        std::cerr << "[LingbotMapLocalizer] Occupancy grid map not found at: " << png_path << std::endl;
        map.loaded = false;
        return;
    }

    std::ifstream yaml_file(yaml_path);
    if (yaml_file.is_open()) {
        std::string line;
        bool in_origin = false;
        std::vector<float> origin_vals;

        while (std::getline(yaml_file, line)) {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            std::string s = line.substr(start);

            if (s.rfind("resolution:", 0) == 0) {
                try {
                    map.resolution = std::stof(s.substr(11));
                } catch (...) {}
            } else if (s.rfind("origin:", 0) == 0) {
                in_origin = true;
                origin_vals.clear();
            } else if (in_origin) {
                if (s[0] == '-') {
                    try {
                        origin_vals.push_back(std::stof(s.substr(1)));
                        if (origin_vals.size() >= 2) {
                            map.origin_x = origin_vals[0];
                            map.origin_z = origin_vals[1];
                            in_origin = false;
                        }
                    } catch (...) {}
                } else {
                    in_origin = false;
                }
            }
        }
        yaml_file.close();
    } else {
        std::cerr << "[LingbotMapLocalizer] Occupancy grid yaml not found at: " << yaml_path
                  << ", using default resolution=" << map.resolution << std::endl;
    }

    map.loaded = true;
    std::cout << "[LingbotMapLocalizer] Loaded 2D Occupancy Grid: "
              << map.image.cols << "x" << map.image.rows
              << ", res: " << map.resolution
              << ", origin: (" << map.origin_x << ", " << map.origin_z << ")" << std::endl;
}