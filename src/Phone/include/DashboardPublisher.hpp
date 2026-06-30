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

class DashboardPublisher{

public:
DashboardPublisher(FrameBuffer* frame_buffer);
~DashboardPublisher();

void initialize(int signalPort = 8888);
void startPublishing();
void stopPublishing();

private:
    FrameBuffer* frame_buffer;
    RobotState* robot_state = nullptr;

    std::atomic<bool> is_running{false};
    

    std::shared_ptr<rtc::PeerConnection> peer_connection;

    std::shared_ptr<rtc::DataChannel> video_channel;
    std::shared_ptr<rtc::DataChannel> state_channel;

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