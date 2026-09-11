#ifndef __LOGGER_HPP__
#define __LOGGER_HPP__

#include "FrameBuffer.hpp"
#include <thread>
#include <atomic>
#include <string>

class Logger {
public:
    Logger(const FrameBuffer& frame_buffer, const bool& record_data, uint64_t base_epoch_ms = 0, std::string output_dir = "");
    ~Logger();
    void startLogging();
    void stopLogging();

private:
    void logLoop();

    std::thread log_thread;
    std::atomic<bool> is_running{false};
    const FrameBuffer& frame_buffer;
    const bool& record_data;
    uint64_t base_epoch_ms = 0;
    std::string output_dir;
};

#endif