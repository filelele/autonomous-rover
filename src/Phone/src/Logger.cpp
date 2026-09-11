#include "Logger.hpp"
#include <android/log.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAG_LOGGER "Logger"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG_LOGGER, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG_LOGGER, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG_LOGGER, __VA_ARGS__)

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

std::string get_default_output_dir() {
    const char* candidates[] = {
        "/sdcard/Android/data/com.filelele.autonomousrover.phone/files",
        "/data/data/com.filelele.autonomousrover.phone/files",
        "/sdcard/Download"
    };

    for (const char* dir : candidates) {
        if (make_dir_recursive(dir) && access(dir, W_OK) == 0) {
            return std::string(dir);
        }
    }
    return "/sdcard/Download";
}

std::string get_current_timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
    localtime_r(&in_time_t, &bt);

    std::ostringstream ss;
    ss << std::put_time(&bt, "%Y%m%d_%H%M%S");
    return ss.str();
}
} // namespace

Logger::Logger(const FrameBuffer& fb, const bool& rec_data, uint64_t base_epoch, std::string out_dir)
    : frame_buffer(fb), record_data(rec_data), base_epoch_ms(base_epoch), output_dir(std::move(out_dir)) {
    if (output_dir.empty()) {
        output_dir = get_default_output_dir();
    } else {
        make_dir_recursive(output_dir);
    }
    LOGI("Logger initialized with output directory: %s", output_dir.c_str());
}

Logger::~Logger() {
    stopLogging();
}

void Logger::startLogging() {
    if (is_running) return;
    is_running = true;
    log_thread = std::thread(&Logger::logLoop, this);
    LOGI("Logger thread started.");
}

void Logger::stopLogging() {
    if (!is_running) return;
    is_running = false;
    if (log_thread.joinable()) {
        log_thread.join();
    }
    LOGI("Logger thread stopped.");
}

void Logger::logLoop() {
    bool currently_recording = false;
    FILE* video_file = nullptr;
    FILE* meta_file = nullptr;
    int64_t last_recorded_timestamp_ms = -1;
    uint32_t frame_count = 0;

    // Buffer for faster file writes to avoid micro-stalls
    std::vector<char> file_buffer(1024 * 1024); // 1 MB buffer

    while (is_running) {
        // State transition: Start recording
        if (record_data && !currently_recording) {
            make_dir_recursive(output_dir);
            std::string time_str = get_current_timestamp_str();
            std::string yuv_path = output_dir + "/recording_" + time_str + ".yuv";
            std::string meta_path = output_dir + "/recording_" + time_str + ".csv";

            video_file = fopen(yuv_path.c_str(), "wb");
            meta_file = fopen(meta_path.c_str(), "w");

            if (video_file && meta_file) {
                setvbuf(video_file, file_buffer.data(), _IOFBF, file_buffer.size());
                fprintf(meta_file, "frame_index,epoch_ms,width,height,plane_count\n");
                currently_recording = true;
                frame_count = 0;
                last_recorded_timestamp_ms = -1;
                LOGI("Recording started: %s", yuv_path.c_str());
            } else {
                LOGE("Failed to open files for recording in %s", output_dir.c_str());
                if (video_file) { fclose(video_file); video_file = nullptr; }
                if (meta_file) { fclose(meta_file); meta_file = nullptr; }
            }
        }
        // State transition: Stop recording
        else if (!record_data && currently_recording) {
            if (video_file) {
                fflush(video_file);
                fclose(video_file);
                video_file = nullptr;
            }
            if (meta_file) {
                fflush(meta_file);
                fclose(meta_file);
                meta_file = nullptr;
            }
            currently_recording = false;
            LOGI("Recording stopped. Total frames recorded: %u", frame_count);
        }

        // Active recording loop
        if (currently_recording && video_file) {
            FramePtr frame = frame_buffer.get_latest_frame();

            if (!frame || frame->timestamp_ms == last_recorded_timestamp_ms) {
                // No new frame yet, sleep briefly to avoid busy-wait
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            // Write raw YUV planar data (Y, then U, then V)
            for (int i = 0; i < frame->plane_count; ++i) {
                const auto& plane = frame->planes[i];
                if (!plane.data.empty()) {
                    fwrite(plane.data.data(), sizeof(uint8_t), plane.data.size(), video_file);
                }
            }

            uint64_t epoch_ms = base_epoch_ms + static_cast<uint64_t>(frame->timestamp_ms);

            // Write metadata (epoch timestamp in ms and frame info)
            if (meta_file) {
                fprintf(meta_file, "%u,%llu,%d,%d,%d\n",
                        frame_count,
                        static_cast<unsigned long long>(epoch_ms),
                        frame->width,
                        frame->height,
                        frame->plane_count);
            }

            last_recorded_timestamp_ms = frame->timestamp_ms;
            frame_count++;

            // Yield / small sleep to match ~30fps cadence (~33ms) while checking responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            // Idle state: wait 50ms before re-checking record_data
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Cleanup upon loop exit
    if (video_file) {
        fflush(video_file);
        fclose(video_file);
    }
    if (meta_file) {
        fflush(meta_file);
        fclose(meta_file);
    }
}
