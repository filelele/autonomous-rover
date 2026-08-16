#ifndef LINGBOT_MAP_LOCALIZER_HPP
#define LINGBOT_MAP_LOCALIZER_HPP

#include <atomic>
#include <thread>
#include "ServerPhoneCommunication.hpp"

class LingbotMapLocalizer {
public:
    LingbotMapLocalizer(ServerPhoneCommunication& communication);
    ~LingbotMapLocalizer();

private:
    void localizationLoop();

    ServerPhoneCommunication& communication;
    std::atomic<bool> is_running{true};
    std::thread worker_thread;
};

#endif