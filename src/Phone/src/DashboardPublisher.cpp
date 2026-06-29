#include "DashboardPublisher.hpp"
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <poll.h>
#include <fcntl.h>

#define TAG "WebRTC_Publisher"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

DashboardPublisher::DashboardPublisher(FrameBuffer* frame_buffer) : frame_buffer(frame_buffer){
    peer_connection = nullptr;
};

DashboardPublisher::~DashboardPublisher(){
    this->stopPublishing();
}

static bool readExact(int fd, void* buffer, size_t n) {
    size_t totalRead = 0;
    char* ptr = static_cast<char*>(buffer);
    while (totalRead < n) {
        ssize_t bytesRead = read(fd, ptr + totalRead, n - totalRead);
        if (bytesRead <= 0) return false;
        totalRead += bytesRead;
    }
    return true;
}

static std::string readPrefixed(int fd) {
    uint32_t len;
    if (!readExact(fd, &len, sizeof(len))) return "";
    len = ntohl(len);
    if (len > 1024 * 1024) return "";
    std::string msg(len, '\0');
    if (!readExact(fd, &msg[0], len)) return "";
    return msg;
}

static bool sendPrefixed(int fd, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    if (write(fd, &len, sizeof(len)) != sizeof(len)) return false;
    if (write(fd, msg.c_str(), msg.size()) != static_cast<ssize_t>(msg.size())) return false;
    return true;
}

void DashboardPublisher::initialize(int signalPort){
    is_running = true;
    
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; 
    address.sin_port = htons(signalPort);

    if (bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOGE("Bind failed on port %d", signalPort);
        close(serverFd);
        return;
    }
    listen(serverFd, 5);
    
    LOGI("Signaling server listening on port %d...", signalPort);

    bool is_initialized = false;
    while (is_running && !is_initialized) {
        struct pollfd pfd;
        pfd.fd = serverFd;
        pfd.events = POLLIN;
        int res = poll(&pfd, 1, 500); // 500ms timeout

        if (res == 0) continue;
        if (res < 0) {
            if (errno == EINTR) continue;
            LOGE("Poll error: %s", strerror(errno));
            break;
        }

        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        LOGI("PC connected. Receiving SDP Offer...");
        std::string pc_offer = readPrefixed(clientFd);
        if (pc_offer.empty()) {
            LOGE("Failed to read SDP Offer from PC.");
            close(clientFd);
            continue;
        }

        try {
            rtc::Configuration config;
            auto pc = std::make_shared<rtc::PeerConnection>(config);
            
            pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
                LOGI("PeerConnection State: %d", (int)state);
                if (state == rtc::PeerConnection::State::Connected) {
                    this->peer_connection = pc;
                }
            });

            rtc::DataChannelInit video_config;
            video_config.reliability.unordered = true;
            video_config.reliability.maxRetransmits = 0;
            auto vc = pc->createDataChannel("video_stream", video_config);

            vc->onOpen([this, vc]() {
                LOGI("WebRTC JPEG Video Channel opened.");
                this->video_channel = vc;
            });

            rtc::DataChannelInit state_config;
            auto sc = pc->createDataChannel("state_stream", state_config);
            sc->onOpen([this, sc]() {
                LOGI("State Channel opened.");
                this->state_channel = sc;
            });

            pc->setRemoteDescription(rtc::Description(pc_offer, "offer"));
            LOGI("Set Remote Description success.");

            // libdatachannel generates Answer automatically on setRemoteDescription above
            int timeout_ms = 5000;
            while (!pc->localDescription().has_value() && timeout_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                timeout_ms -= 10;
            }

            if(pc->localDescription().has_value()) {
                std::string phone_answer = std::string(pc->localDescription().value());
                if (sendPrefixed(clientFd, phone_answer)) {
                    LOGI("Sent Phone SDP Answer back to PC.");
                    is_initialized = true;
                } else {
                    LOGE("Failed to send SDP Answer.");
                }
            } else {
                LOGE("SDP Answer generation timeout.");
            }
        } catch (const std::exception& e) {
            LOGE("WebRTC Handshake error: %s", e.what());
        }

        close(clientFd);
    }
    close(serverFd);
    LOGI("Signaling initialize() finished.");
}


const std::vector<uint8_t>& DashboardPublisher::convertToJpeg(const FramePtr& frame, int quality = 75) {
    int w = frame->width;
    int h = frame->height;
    
    // YUV420p: Chroma U and V half Y
    int chromaW = w / 2;
    int chromaH = h / 2;

    // zero-copy
    cv::Mat y(h,        w,       CV_8UC1, const_cast<uint8_t*>(frame->planes[0].data.data()));
    cv::Mat u(chromaH,  chromaW, CV_8UC1, const_cast<uint8_t*>(frame->planes[1].data.data()));
    cv::Mat v(chromaH,  chromaW, CV_8UC1, const_cast<uint8_t*>(frame->planes[2].data.data()));

    cv::Mat& i420 = video_stream_frame.i420;
    i420.create(h * 3 / 2, w, CV_8UC1);
    memcpy(i420.ptr(0), y.data, w * h);
    memcpy(i420.ptr(h), u.data, w * h / 4);
    memcpy(i420.ptr(h) + w * h / 4, v.data, w * h / 4);
    
    cv::Mat& bgr = video_stream_frame.bgr;
    bgr.create(h, w, CV_8UC3);
    cv::cvtColor(i420, bgr, cv::COLOR_YUV2BGR_I420);

    cv::Mat& resized = video_stream_frame.resized;
    resized.create(270, 480, CV_8UC3);
    cv::resize(bgr, resized, cv::Size(480,270), 0, 0, cv::INTER_AREA);

    std::vector<uint8_t>& jpeg = video_stream_frame.jpeg;
    cv::imencode(".jpg", resized, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality});

    return jpeg;
}

void DashboardPublisher::videoStream(){
    int64_t lastTimestamp = 0;
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); //max 40fps
        if(!video_channel || !video_channel->isOpen()){
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Avoid buffer bloat: skip frame if more than 256KB in queue
        if (video_channel->bufferedAmount() > 256 * 1024) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        FramePtr frame = frame_buffer->get_latest_frame();
        if(!frame){
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if(frame->timestamp_ns == lastTimestamp){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }else{
            lastTimestamp = frame->timestamp_ns;
        }

        const auto& jpeg = convertToJpeg(frame, 65);
        try {
            video_channel->send(reinterpret_cast<const std::byte*>(jpeg.data()), jpeg.size());
        } catch (const std::exception& e) {
            LOGE("Failed to send video frame: %s", e.what());
        }
    }
}

void DashboardPublisher::stateStream(){
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DashboardPublisher::startPublishing(){
    video_stream_thread = std::thread(&DashboardPublisher::videoStream, this);
    //state_stream_thread = std::thread(&DashboardPublisher::stateStream, this);
}

void DashboardPublisher::stopPublishing(){
    is_running.store(false, std::memory_order_release);
    if (video_stream_thread.joinable()) video_stream_thread.join(); 
    //if (state_stream_thread.joinable()) state_stream_thread.join();
}
