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
#include <cstdio>
#include <cstring>

#include <rtc/h265rtppacketizer.hpp>

#define TAG "WebRTC_Publisher"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)


static inline int64_t get_boottime_us() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

PhoneServerCommunication::PhoneServerCommunication(
    const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data) : 
    in_out{frame_buffer, {1.0f, 1.0f, 1.0f}, manual_mode, record_data, {false, false, {0.0f, 0.0f, 0.0f}}}
{
    connection.control_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    connection.control_udp_addr.sin_family = AF_INET;
    connection.control_udp_addr.sin_port = htons(12345);

    // PI_IP via env var
    inet_pton(AF_INET, PI_IP, &connection.control_udp_addr.sin_addr);
    LOGI("Communication initialized with baked PI_IP: %s", PI_IP);
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

void PhoneServerCommunication::initialize(int controlSignalPort, int videoSignalPort){
    is_running = true;
    if (!controlSignalingLoop(controlSignalPort)) {
        LOGE("Control signaling setup failed.");
        return;
    }
    if (!videoSignalingLoop(videoSignalPort)) {
        LOGE("Video signaling setup failed.");
        return;
    }
}

bool PhoneServerCommunication::controlSignalingLoop(int signalPort) {
    LOGI("Control signaling thread started on port %d", signalPort);
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
        return false;
    }
    listen(serverFd, 5);
    bool connection_established = false;
    while (!connection_established) {
        struct pollfd pfd;
        pfd.fd = serverFd;
        pfd.events = POLLIN;
        int res = poll(&pfd, 1, 1000);

        if (res <= 0) continue;

        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        LOGI("Control PC connected. Receiving SDP Offer...");
        std::string pc_offer = readPrefixed(clientFd);
        if (pc_offer.empty()) {
            LOGE("Failed to read control SDP Offer from PC.");
            close(clientFd);
            continue;
        }

        try {
            rtc::Configuration config;
            auto pc = std::make_shared<rtc::PeerConnection>(config);
            
            pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
                LOGI("PeerConnection State: %d", (int)state);
                if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed) {
                    // Cleanup current connection if needed
                }
            });

            pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
                LOGI("Gathering State: %d", (int)state);
            });

            pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
                std::string label = channel->label();
                LOGI("DataChannel received: %s", label.c_str());

                if (label == "telemetry_channel") {
                    channel->onOpen([this, channel](){
                        this->connection.out_telemetry_channel = channel;
                        LOGI("Telemetry channel opened");
                    });

                } else if (label == "mode_channel") {
                    channel->onOpen([this, channel](){
                        LOGI("Mode channel opened");
                        this->connection.in_mode_channel = channel;
                    });

                    channel->onMessage([this](const rtc::message_variant& message) {
                        std::string msg_string;
                        if (std::holds_alternative<std::string>(message)) {
                            msg_string = std::get<std::string>(message);
                        } else {
                            const rtc::binary& packet = std::get<rtc::binary>(message);
                            msg_string = std::string(reinterpret_cast<const char*>(packet.data()), packet.size());
                        }

                        if (msg_string.length() == 1) {
                            if (msg_string[0] == 'm') in_out.in_manual_mode = !in_out.in_manual_mode;
                            else if (msg_string[0] == 'r') in_out.in_record_data = !in_out.in_record_data;
                            else return;
                        }
                    });

                } else if (label == "manual_control_channel") {

                    channel->onOpen([this, channel](){
                        this->connection.in_manual_control_channel = channel;
                        LOGI("Manual control channel opened");
                    });

                    channel->onMessage([this](const rtc::message_variant& message) {
                        if (!in_out.in_manual_mode) return;
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

                } else if (label == "location_channel") {
                    channel->onOpen([this, channel](){
                        this->connection.in_location_channel = channel;
                        LOGI("Location channel opened");
                    });

                    channel->onMessage([this](const rtc::message_variant& message) {
                        std::string msg;
                        if(std::holds_alternative<std::string>(message)){
                            msg = std::get<std::string>(message);
                        }else{
                            const rtc::binary& packet = std::get<rtc::binary>(message);
                            msg.assign(reinterpret_cast<const char*>(packet.data()), packet.size());
                        }

                        size_t first_comma = msg.find(',');
                        size_t second_comma = msg.find(',', first_comma + 1);
                        size_t third_comma = msg.find(',', second_comma + 1);
                        if (first_comma == std::string::npos || second_comma == std::string::npos || third_comma == std::string::npos) {
                            return;
                        }

                        in_out.in_slow_location.x = std::stof(msg.substr(0, first_comma));
                        in_out.in_slow_location.y = std::stof(msg.substr(first_comma + 1, second_comma - first_comma - 1));
                        in_out.in_slow_location.heading = std::stof(msg.substr(second_comma + 1, third_comma - second_comma - 1));
                        
                        /* For benchmarking only
                        uint64_t frame_relative_timestamp_us = static_cast<uint64_t>(std::stoull(msg.substr(third_comma + 1)));
                        uint64_t frame_timestamp_us = base_pts_us + frame_relative_timestamp_us;
                        const auto receive_timestamp_us = get_boottime_us();
                        const int64_t latency_us = static_cast<int64_t>(receive_timestamp_us - frame_timestamp_us);
                        const double latency_ms = static_cast<double>(latency_us) / 1000.0;

                        std::string response = std::to_string(latency_ms);
                        if (connection.in_location_channel && connection.in_location_channel->isOpen()) {
                            connection.in_location_channel->send(response);
                        }
                        */
                        
                    });
                }
            });

            pc->setRemoteDescription(rtc::Description(pc_offer, "offer"));
            LOGI("Set Remote Description. Gathering candidates...");

            int wait_ms = 3000;
            while (pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete && wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                wait_ms -= 20;
            }

            if(pc->localDescription().has_value()) {
                std::string phone_answer = std::string(pc->localDescription().value());
                if (sendPrefixed(clientFd, phone_answer)) {
                    LOGI("Control answer sent to GCS. Connection should establish now.");
                    this->connection.control_peer_connection = pc;
                }
            }
        } catch (const std::exception& e) {
            LOGE("Control handshake failure: %s", e.what());
        }
        close(clientFd);
        connection_established = true;
    }
    close(serverFd);
    return connection_established;
}

bool PhoneServerCommunication::videoSignalingLoop(int signalPort) {
    LOGI("Video signaling thread started on port %d", signalPort);
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
        return false;
    }
    listen(serverFd, 5);

    bool connection_established = false;
    while (!connection_established) {
        struct pollfd pfd;
        pfd.fd = serverFd;
        pfd.events = POLLIN;
        int res = poll(&pfd, 1, 1000);
        if (res <= 0) continue;

        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) continue;

        LOGI("Video PC connected. Receiving SDP Offer...");
        std::string pc_offer = readPrefixed(clientFd);
        if (pc_offer.empty()) {
            LOGE("Failed to read video SDP Offer from PC.");
            close(clientFd);
            continue;
        }

        try {
            rtc::Configuration config;
            auto pc = std::make_shared<rtc::PeerConnection>(config);

            pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
                LOGI("[Video] PeerConnection State: %d", (int)state);
                if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed) {
                }
            });

            pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
                LOGI("[Video] Gathering State: %d", (int)state);
            });

            pc->onTrack([this](std::shared_ptr<rtc::Track> track) {
                std::string type = track->description().type();
                LOGI("[Video] Track received, mid: %s, type: %s", track->mid().c_str(), type.c_str());

                if (type == "video") {
                    auto h265Config = std::make_shared<rtc::RtpPacketizationConfig>(rtc::SSRC(0), "H265", 96, 90000);
                    auto h265Packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, h265Config);

                    track->setMediaHandler(h265Packetizer);

                    track->onOpen([this, track]() {
                        this->connection.out_video_track = track;
                        LOGI("Video track ready to send frames.");
                        if (this->encoder) this->encoder->requestKeyFrame();
                    });
                }
            });

            pc->setRemoteDescription(rtc::Description(pc_offer, "offer"));
            LOGI("Set video remote description. Gathering candidates...");

            int wait_ms = 3000;
            while (pc->gatheringState() != rtc::PeerConnection::GatheringState::Complete && wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                wait_ms -= 20;
            }

            if (pc->localDescription().has_value()) {
                std::string phone_answer = std::string(pc->localDescription().value());
                if (sendPrefixed(clientFd, phone_answer)) {
                    LOGI("Video answer sent to GCS. Connection should establish now.");
                    this->connection.video_peer_connection = pc;
                    connection_established = true;
                }
            }
        } catch (const std::exception& e) {
            LOGE("Video handshake failure: %s", e.what());
        }
        close(clientFd);
    }
    close(serverFd);
    return connection_established;
}

void PhoneServerCommunication::videoStream(){
    LOGI("Video stream thread started.");
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(25)); // Max 50fps

        auto video_track = connection.out_video_track;
        if(!video_track || !video_track->isOpen()){
            continue;
        }

        FramePtr frame = in_out.out_frame_buffer.get_latest_frame();
        if(!frame) continue;

        // Initialize encoder once
        if (!encoder) {
            encoder = std::make_unique<H265Encoder>();
            if (!encoder->initialize(frame->width, frame->height, 450000, 25, 50)) {
                LOGE("Encoder init failed.");
                encoder.reset();
            } else {
                LOGI("Encoder initialized at %dx%d.", frame->width, frame->height);
                encoder->setCallback([this](const uint8_t* data, size_t size, bool isConfig, bool isKey, int64_t pts_us){
                    auto vt = this->connection.out_video_track;
                    if (!vt || !vt->isOpen()) return;

                    uint32_t timestamp = static_cast<uint32_t>((pts_us - base_pts_us) * 90 / 1000); // avoid overflow by minusing a base timestamp
                    rtc::FrameInfo info(timestamp);
                    vt->sendFrame(reinterpret_cast<const rtc::byte*>(data), size, info);
                });
            }
        }

        if (encoder) {
            if(connection.out_video_track->bufferedAmount() > send_buffer_limit_bytes) continue;
            else encoder->encodeFrame(frame);
        }
    }
    LOGI("Video stream thread exiting.");
}

void PhoneServerCommunication::telemetryStream(){
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        in_out.out_telemetry.manual_mode_state = in_out.in_manual_mode;
        in_out.out_telemetry.record_data_state = in_out.in_record_data;
        in_out.out_telemetry.location = in_out.in_slow_location;
        if(connection.out_telemetry_channel && connection.out_telemetry_channel->isOpen()){
            std::string telemetry_msg = std::to_string(
                in_out.out_telemetry.location.x) + "," 
                + std::to_string(in_out.out_telemetry.location.y) + "," 
                + std::to_string(in_out.out_telemetry.location.heading) + "," 
                + (in_out.out_telemetry.manual_mode_state ? "m1" : "m0") + "," 
                + (in_out.out_telemetry.record_data_state ? "r1" : "r0");
            connection.out_telemetry_channel->send(telemetry_msg);
        }
    }
}

void PhoneServerCommunication::startCommunication(){
    base_pts_us = get_boottime_us();
    video_stream_thread = std::thread(&PhoneServerCommunication::videoStream, this);
    telemetry_stream_thread = std::thread(&PhoneServerCommunication::telemetryStream, this);
}

void PhoneServerCommunication::stopCommunication(){
    if (video_stream_thread.joinable()) video_stream_thread.join();
    if (telemetry_stream_thread.joinable()) telemetry_stream_thread.join();
    is_running.store(false, std::memory_order_release);
    if (encoder) {
        encoder->stop();
        encoder.reset();
    }
    if (connection.control_peer_connection) connection.control_peer_connection->close();
    if (connection.video_peer_connection) connection.video_peer_connection->close();
}
