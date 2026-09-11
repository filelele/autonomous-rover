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

LingbotMapLocalizer::LingbotMapLocalizer(const FrameBuffer& frame_buffer, Location& location, ServerPhoneCommunication& communication)
    : frame_buffer(frame_buffer), location(location), communication(communication) {
    // 1) LINGBOT_IMAGES_DIR env var (preferred on a headless machine)
    // 2) zenity GTK dialog, only if a display is available (here I use ssh -X to forward the display to my laptop)
    std::cout << "[LingbotMapLocalizer] Starting localization thread..." << std::endl;
    std::cout << "[LingbotMapLocalizer] Pick fixed images folder..." << std::endl;
    std::string images_folder;
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

void LingbotMapLocalizer::localizationLoop() {
    int64_t last_localized_timestamp_ms = -1;
    while (is_running.load(std::memory_order_acquire)) {
        auto frame = frame_buffer.get_latest_frame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        cv::Mat bgr = frame->to_bgr();
        if (bgr.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (frame->timestamp_ms == last_localized_timestamp_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
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
            loc.y = static_cast<float>(tz);
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