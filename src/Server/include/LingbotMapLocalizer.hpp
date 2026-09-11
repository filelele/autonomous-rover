#ifndef LINGBOT_MAP_LOCALIZER_HPP
#define LINGBOT_MAP_LOCALIZER_HPP

#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <array>
#include <cstdio>
#include <opencv2/opencv.hpp>
#include "FrameBuffer.hpp"
#include "Location2D.hpp"
#include "ServerPhoneCommunication.hpp"

class LingbotMapLocalizer {
public:
    LingbotMapLocalizer(const FrameBuffer& frame_buffer, Location& location, ServerPhoneCommunication& communication);
    ~LingbotMapLocalizer();

    // Latest camera-to-world 4x4 matrix (row-major, float64). Empty if none yet.
    std::array<double, 16> getLatestC2W();

private:
    void localizationLoop();
    bool launchPython(const std::string& images_folder);
    bool buildKVCache(const std::string& images_folder);
    bool sendFrame(const cv::Mat& bgr);
    bool readResult(std::array<double, 16>& c2w_out); // false on failed frame
    void killPython();
    static std::string pickImagesFolderWithDialog();
    static std::string pickModelFileWithDialog();

    const FrameBuffer& frame_buffer;
    Location& location;
    ServerPhoneCommunication& communication;
    std::atomic<bool> is_running{true};
    std::thread worker_thread;

    FILE* py_stdin = nullptr;   // write commands/frames to python
    FILE* py_stdout = nullptr;  // read results from python
    pid_t py_pid = -1;

    std::mutex result_mutex;
    std::array<double, 16> latest_c2w{};
    bool has_result = false;
};

#endif