#ifndef SERVER_PHONE_COMMUNICATION_HPP
#define SERVER_PHONE_COMMUNICATION_HPP

#include <thread>
#include <memory>
#include <string>
#include <atomic>
#include <rtc/rtc.hpp>
#include "FrameBuffer.hpp"
#include <opencv2/opencv.hpp>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>
#include "H265Decoder.hpp"
#include "Location2D.hpp"
#include "Telemetry.hpp"

struct DecodedFrame {
    cv::Mat bgr;
    uint64_t timestamp_us;
};

class ServerPhoneCommunication{
private:
    std::atomic<bool> is_initialized{false};
    std::atomic<bool> is_running{true};

    struct InOutData{
        std::atomic<std::shared_ptr<const DecodedFrame>> in_decoded_frame{nullptr};
        Telemetry in_telemetry = {false, false, {0.0f, 0.0f, 0.0f}};
    };
    InOutData in_out;

    struct Connection{
        // two separate peer_connection so telemetry and video dont use the same use buffer
        // still same os buffer though.
        std::shared_ptr<rtc::PeerConnection> control_peer_connection;
        std::shared_ptr<rtc::PeerConnection> video_peer_connection;
        std::shared_ptr<rtc::Track> in_video_track;
        std::shared_ptr<rtc::DataChannel> in_telemetry_channel;
        std::shared_ptr<rtc::DataChannel> out_mode_channel;
        std::shared_ptr<rtc::DataChannel> out_manual_control_channel;
        std::shared_ptr<rtc::DataChannel> out_location_channel;
    };
    Connection connection;

    struct StreamFPSCounter {
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        int frame_accumulator = 0;
        double incoming_fps = 0.0;
    };
    StreamFPSCounter fps_counter;

    std::vector<uint8_t> cached_config_legacy;
    H265Decoder decoder;

    // Async decoding
    struct EncodedFrame {
        rtc::binary data;
        uint64_t timestamp_us;
    };
    std::queue<EncodedFrame> decode_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::thread decoder_thread;
    void decoderWorker();

public:
    ServerPhoneCommunication();
    ~ServerPhoneCommunication();

    //Phone Tailscale ip and which port Phone listening to
    void initialize(const std::string& publisher_ip, int controlSignalPort, int videoSignalPort);
    
    std::shared_ptr<const DecodedFrame> getLatestFrame();
    void toggleManualMode();
    void toggleRecordData();
    void sendManualControl(float heading, float angle);
    bool getManualModeState() const { return in_out.in_telemetry.manual_mode_state; }
    bool getRecordDataState() const { return in_out.in_telemetry.record_data_state; }
    double getIncomingFps() const { return fps_counter.incoming_fps; }
    Location getLocation() const { return in_out.in_telemetry.location; }

    void sendLocation(const Location& loc/*, uint64_t timestamp_us*/); // For benchmarking only

    void stopCommunication();
};

#endif
