#ifndef DASHBOARD_PUBLISHER
#define DASHBOARD_PUBLISHER

#include <thread>
#include "FrameBuffer.hpp"
#include "RobotState.hpp"
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

class PhoneServerCommunication{

public:
PhoneServerCommunication(const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data);
~PhoneServerCommunication();

void initialize(int controlSignalPort = 8888, int videoSignalPort = 8889);
void startCommunication();
void stopCommunication();

private:
    const FrameBuffer& frame_buffer;
    RobotState* robot_state = nullptr;
    bool& manual_mode;
    bool& record_data;

    std::atomic<bool> is_running{false};
    

    struct Connection{
        std::shared_ptr<rtc::PeerConnection> control_peer_connection;
        std::shared_ptr<rtc::PeerConnection> video_peer_connection;
        std::shared_ptr<rtc::Track> out_video_track;
        std::shared_ptr<rtc::DataChannel> out_state_channel;
        std::shared_ptr<rtc::DataChannel> in_out_mode_channel;
        std::shared_ptr<rtc::DataChannel> in_manual_control_channel;
        int control_udp_socket = -1;
        sockaddr_in control_udp_addr{};
    };
    Connection connection;

    std::unique_ptr<H265Encoder> encoder;
    std::mutex send_mutex;

    size_t send_buffer_limit_bytes = 0 * 1024; // pause producing when buffered >= this 

    void videoStream();
    void stateStream();
    std::thread state_stream_thread;
    std::thread video_stream_thread;

    bool controlSignalingLoop(int port);
    bool videoSignalingLoop(int port);
};

#endif