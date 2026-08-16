#ifndef DASHBOARD_PUBLISHER
#define DASHBOARD_PUBLISHER

#include <thread>
#include "FrameBuffer.hpp"
#include <rtc/rtc.hpp>
#include <memory>
#include <atomic>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <netinet/in.h>
#include <mutex>
#include <arpa/inet.h>
#include "H265Encoder.hpp"
#include "Location2D.hpp"
#include "Telemetry.hpp"

class PhoneServerCommunication{

public:
PhoneServerCommunication(const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data);
~PhoneServerCommunication();

void initialize(int controlSignalPort = 8888, int videoSignalPort = 8889);
void startCommunication();
void stopCommunication();

private:
    struct InOutData{
        const FrameBuffer& out_frame_buffer;
        Location in_slow_location;
        bool& in_manual_mode;
        bool& in_record_data;
        Telemetry out_telemetry;
    } in_out;

    std::atomic<bool> is_running{false};

    struct Connection{
        std::shared_ptr<rtc::PeerConnection> control_peer_connection;
        std::shared_ptr<rtc::PeerConnection> video_peer_connection;
        std::shared_ptr<rtc::Track> out_video_track;
        std::shared_ptr<rtc::DataChannel> out_telemetry_channel;
        std::shared_ptr<rtc::DataChannel> in_mode_channel;
        std::shared_ptr<rtc::DataChannel> in_manual_control_channel;
        std::shared_ptr<rtc::DataChannel> in_location_channel;
        int control_udp_socket = -1;
        sockaddr_in control_udp_addr{};
    };
    Connection connection;

    std::unique_ptr<H265Encoder> encoder;

    int64_t base_pts_us = 0;
    std::mutex send_mutex;

    size_t send_buffer_limit_bytes = 0 * 1024; // pause producing when buffered >= this 

    void videoStream();
    void telemetryStream();
    std::thread telemetry_stream_thread;
    std::thread video_stream_thread;

    bool controlSignalingLoop(int port);
    bool videoSignalingLoop(int port);
};

#endif