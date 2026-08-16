#include "LingbotMapLocalizer.hpp"
#include <chrono>
#include <thread>

LingbotMapLocalizer::LingbotMapLocalizer(ServerPhoneCommunication& communication)
    : communication(communication), worker_thread(&LingbotMapLocalizer::localizationLoop, this) {}

LingbotMapLocalizer::~LingbotMapLocalizer() {
    is_running.store(false, std::memory_order_release);
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void LingbotMapLocalizer::localizationLoop() {
    while (is_running.load(std::memory_order_acquire)) {
        auto frame = communication.getLatestFrame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        if (!is_running.load(std::memory_order_acquire)) {
            break;
        }

        communication.sendLocation(communication.getLocation()/*, frame->timestamp_us*/); // For benchmarking only
    }
}