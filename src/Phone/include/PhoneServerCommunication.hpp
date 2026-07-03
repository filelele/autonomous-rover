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

class PhoneServerCommunication{

public:
PhoneServerCommunication(const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data);
~PhoneServerCommunication();

void initialize(int signalPort = 8888);
void startCommunication();
void stopCommunication();

private:
    const FrameBuffer& frame_buffer;
    RobotState* robot_state = nullptr;
    bool& manual_mode;
    bool& record_data;

    std::atomic<bool> is_running{false};
    

    struct Connection{
        std::shared_ptr<rtc::PeerConnection> peer_connection;
        std::shared_ptr<rtc::DataChannel> out_video_channel;
        std::shared_ptr<rtc::DataChannel> out_state_channel;
        std::shared_ptr<rtc::DataChannel> in_out_mode_channel;
        std::shared_ptr<rtc::DataChannel> in_manual_control_channel;
        int control_udp_socket = -1;
        sockaddr_in control_udp_addr{};
    };
    Connection connection;

    struct VideoStreamFrame{
        cv::Mat i420;
        cv::Mat bgr;
        cv::Mat resized;
        std::vector<uint8_t> webp;
    };
    VideoStreamFrame video_stream_frame;

    const std::vector<uint8_t>& convertToWebp(const FramePtr& frame, int quality);


    void videoStream();
    void stateStream();
    std::thread state_stream_thread;
    std::thread video_stream_thread;
};

#endif