#include "PhoneServerCommunication.hpp"
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

#define TAG "WebRTC_Publisher"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

PhoneServerCommunication::PhoneServerCommunication(
    const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data) : 
    frame_buffer(frame_buffer), manual_mode(manual_mode), record_data(record_data)
{
    connection.control_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    connection.control_udp_addr.sin_family = AF_INET;
    connection.control_udp_addr.sin_port = htons(12345);

    // PI_IP_BAKED is passed via CMake from the build environment
    inet_pton(AF_INET, PI_IP_BAKED, &connection.control_udp_addr.sin_addr);
    LOGI("Communication initialized with baked PI_IP: %s", PI_IP_BAKED);
};

PhoneServerCommunication::~PhoneServerCommunication(){
    this->stopCommunication();
    if (connection.control_udp_socket != -1) {
        close(connection.control_udp_socket);
    }
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

void PhoneServerCommunication::initialize(int signalPort){
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
                    this->connection.peer_connection = pc;
                }
            });

            rtc::DataChannelInit video_config;
            video_config.reliability.unordered = true;
            video_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(20);
            auto vc = pc->createDataChannel("video_channel", video_config);

            vc->onOpen([this, vc]() {
                LOGI("WebRTC Webp Video Channel opened.");
                this->connection.out_video_channel = vc;
            });

            rtc::DataChannelInit state_config;
            state_config.reliability.unordered = true;
            state_config.reliability.maxRetransmits = 0;
            auto sc = pc->createDataChannel("state_channel", state_config);
            sc->onOpen([this, sc]() {
                LOGI("State Channel opened.");
                this->connection.out_state_channel = sc;
            });

            rtc::DataChannelInit mode_config;
            mode_config.reliability.maxRetransmits = 10;
            mode_config.reliability.unordered = false;
            auto mc = pc->createDataChannel("mode_channel", mode_config);

            auto sendState = [this, mc]() {
                std::string state = "";
                state += (manual_mode ? "m1" : "m0");
                state += (record_data ? "r1" : "r0");
                mc->send(state);
            };

            mc->onOpen([this, mc, sendState]() {
                LOGI("Mode Channel opened");
                this->connection.in_out_mode_channel = mc;
                sendState();
            });

            mc->onMessage([this, sendState](const rtc::message_variant& message) {
                std::string msg_string;
                if (std::holds_alternative<std::string>(message)) {
                    msg_string = std::get<std::string>(message);
                } else {
                    const rtc::binary& packet = std::get<rtc::binary>(message);
                    msg_string = std::string(reinterpret_cast<const char*>(packet.data()), packet.size());
                }

                if (msg_string.length() == 1) {
                    if (msg_string[0] == 'm') manual_mode = !manual_mode;
                    else if (msg_string[0] == 'r') record_data = !record_data;
                    else return;
                    sendState();
                }
            });

            rtc::DataChannelInit manual_control_config;
            manual_control_config.reliability.unordered = true;
            manual_control_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(30);
            auto mmc = pc->createDataChannel("manual_control_channel", manual_control_config);
            mmc->onOpen([this, mmc]() {
                LOGI("Manual Control Channel opened");
                this->connection.in_manual_control_channel = mmc;
            });

            mmc->onMessage([this](const rtc::message_variant& message) {
                if (!manual_mode) return;
                if (std::holds_alternative<std::string>(message)) {
                    std::string msg = std::get<std::string>(message);
                    sendto(connection.control_udp_socket, msg.c_str(), msg.size(), 0,
                           (struct sockaddr*)&connection.control_udp_addr, sizeof(connection.control_udp_addr));
                } else {
                    const rtc::binary& packet = std::get<rtc::binary>(message);
                    sendto(connection.control_udp_socket, packet.data(), packet.size(), 0,
                           (struct sockaddr*)&connection.control_udp_addr, sizeof(connection.control_udp_addr));
                }
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


const std::vector<uint8_t>& PhoneServerCommunication::convertToWebp(const FramePtr& frame, int quality = 75) {
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
    resized.create(180, 320, CV_8UC3);
    cv::resize(bgr, resized, cv::Size(320,180), 0, 0, cv::INTER_AREA);

    std::vector<uint8_t>& webp = video_stream_frame.webp;
    // Switch to WebP encoding
    // WebP typically provides better compression than JPEG at similar quality
    // Quality 65-75 is generally good for WebP lossy
    cv::imencode(".webp", resized, webp, {cv::IMWRITE_WEBP_QUALITY, quality});

    return webp;
}

void PhoneServerCommunication::videoStream(){
    int64_t lastTimestamp = 0;

    // Convert Monotonic clock (used by ACamera) to Realtime clock Unix Epoch
    struct timespec ts_mono, ts_real;
    clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    clock_gettime(CLOCK_REALTIME, &ts_real);
    int64_t mono_ns = static_cast<int64_t>(ts_mono.tv_sec) * 1000000000LL + ts_mono.tv_nsec;
    int64_t real_ns = static_cast<int64_t>(ts_real.tv_sec) * 1000000000LL + ts_real.tv_nsec;
    int64_t clock_offset_ns = real_ns - mono_ns;

    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); //max 40fps
        auto video_channel = connection.out_video_channel;
        if(!video_channel || !video_channel->isOpen()){
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (video_channel->bufferedAmount() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        FramePtr frame = frame_buffer.get_latest_frame();
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

        const auto& webp = convertToWebp(frame, 40);

        // Prepare packet: [int64_t timestamp_ns (Unix Epoch)][webp]
        std::vector<std::byte> packet(sizeof(int64_t) + webp.size());
        int64_t epoch_ts_ns = frame->timestamp_ns + clock_offset_ns;
        std::memcpy(packet.data(), &epoch_ts_ns, sizeof(int64_t));
        std::memcpy(packet.data() + sizeof(int64_t), webp.data(), webp.size());

        try {
            video_channel->send(packet.data(), packet.size());
        } catch (const std::exception& e) {
            LOGE("Failed to send video frame: %s", e.what());
        }
    }
}

void PhoneServerCommunication::stateStream(){
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void PhoneServerCommunication::startCommunication(){
    video_stream_thread = std::thread(&PhoneServerCommunication::videoStream, this);
    //state_stream_thread = std::thread(&PhoneServerCommunication::stateStream, this);
}

void PhoneServerCommunication::stopCommunication(){
    is_running.store(false, std::memory_order_release);
    if (video_stream_thread.joinable()) video_stream_thread.join(); 
    //if (state_stream_thread.joinable()) state_stream_thread.join();
}
