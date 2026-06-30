#ifndef DASHBOARD_LISTENER_HPP
#define DASHBOARD_LISTENER_HPP

#include <thread>
#include <memory>
#include <string>
#include <atomic>
#include <rtc/rtc.hpp>
#include "FrameBuffer.hpp"
#include <opencv2/opencv.hpp>

class DashboardListener {
private:
    std::atomic<std::shared_ptr<cv::Mat>> bgr_buffer;
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    std::shared_ptr<rtc::DataChannel> video_channel;
    std::shared_ptr<rtc::DataChannel> data_channel;

    std::atomic<bool> is_initialized{false};
    std::atomic<bool> is_running{true};

    struct PacketCounter {
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        int packet_accumulator = 0;
        double incoming_fps = 0.0;
    };
    PacketCounter packet_counter;

public:
    DashboardListener();
    ~DashboardListener();

    /**
     * @brief Initialize WebRTC and perform handshake with Publisher.
     * Keeps retrying if the Publisher is not available.
     * @param publisher_ip The Tailscale IP of the Phone.
     * @param signalPort The port where Phone is listening for signaling.
     */
    void initialize(const std::string& publisher_ip, int signalPort);
    std::shared_ptr<const cv::Mat> getLatestBgr();
    double getIncomingFps() const { return packet_counter.incoming_fps; }
    void stopListening();
};

#endif
