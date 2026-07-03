#ifndef SERVER_PHONE_COMMUNICATION_HPP
#define SERVER_PHONE_COMMUNICATION_HPP

#include <thread>
#include <memory>
#include <string>
#include <atomic>
#include <rtc/rtc.hpp>
#include "FrameBuffer.hpp"
#include <opencv2/opencv.hpp>

class ServerPhoneCommunication{
private:
    std::atomic<bool> is_initialized{false};
    std::atomic<bool> is_running{true};

    struct InOutData{
        std::atomic<std::shared_ptr<cv::Mat>> in_bgr_buffer;
        struct Location{
            float x;
            float y;
        };
        Location in_location;
        //in_waypoints
        std::atomic<bool> in_manual_mode_state{false};
        std::atomic<bool> in_record_data_state{false};
    };
    InOutData in_out;

    struct Connection{
        std::shared_ptr<rtc::PeerConnection> peer_connection;
        std::shared_ptr<rtc::DataChannel> in_video_channel;
        std::shared_ptr<rtc::DataChannel> in_state_channel;
        std::shared_ptr<rtc::DataChannel> in_out_mode_channel;
        std::shared_ptr<rtc::DataChannel> out_manual_control_channel;
    };
    Connection connection;

    struct PacketCounter {
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        int packet_accumulator = 0;
        double incoming_fps = 0.0;
    };
    PacketCounter packet_counter;

public:
    ServerPhoneCommunication();
    ~ServerPhoneCommunication();

    /**
     * @brief Initialize WebRTC and perform handshake with Phone side.
     * Keeps retrying if the Publisher is not available.
     * @param publisher_ip The Tailscale IP of the Phone.
     * @param signalPort The port where Phone is listening for signaling.
     */
    void initialize(const std::string& publisher_ip, int signalPort);
    
    std::shared_ptr<const cv::Mat> getLatestBgr();
    InOutData::Location getLatestLocation() const { return in_out.in_location; };
    void toggleManualMode();
    void toggleRecordData();
    void sendManualControl(float heading, float angle);
    bool getManualModeState() const { return in_out.in_manual_mode_state; }
    bool getRecordDataState() const { return in_out.in_record_data_state; }
    double getIncomingFps() const { return packet_counter.incoming_fps; }

    void stopCommunication();
};

#endif
