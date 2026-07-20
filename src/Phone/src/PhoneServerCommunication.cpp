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

PhoneServerCommunication::PhoneServerCommunication(
    const FrameBuffer& frame_buffer, bool& manual_mode, bool& record_data) : 
    frame_buffer(frame_buffer), manual_mode(manual_mode), record_data(record_data)
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

                if (label == "state_channel") {

                    channel->onOpen([this, channel](){
                        this->connection.out_state_channel = channel;
                        LOGI("State channel opened");
                    });

                } else if (label == "mode_channel") {

                    auto sendState = [this, channel]() {
                        std::string state = "";
                        state += (manual_mode ? "m1" : "m0");
                        state += (record_data ? "r1" : "r0");
                        channel->send(state);
                    };

                    channel->onOpen([this, channel, sendState](){
                        LOGI("Mode channel opened");
                        this->connection.in_out_mode_channel = channel;
                        sendState();
                    });

                    channel->onMessage([this, sendState](const rtc::message_variant& message) {
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

                } else if (label == "manual_control_channel") {

                    channel->onOpen([this, channel](){
                        this->connection.in_manual_control_channel = channel;
                        LOGI("Manual control channel opened");
                    });

                    channel->onMessage([this](const rtc::message_variant& message) {
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

        FramePtr frame = frame_buffer.get_latest_frame();
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

                    uint32_t timestamp = static_cast<uint32_t>(pts_us * 90 / 1000);
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

void PhoneServerCommunication::stateStream(){
    while(is_running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void PhoneServerCommunication::startCommunication(){
    video_stream_thread = std::thread(&PhoneServerCommunication::videoStream, this);
}

void PhoneServerCommunication::stopCommunication(){
    if (video_stream_thread.joinable()) video_stream_thread.join(); 
    is_running.store(false, std::memory_order_release);
    if (encoder) {
        encoder->stop();
        encoder.reset();
    }
    if (connection.control_peer_connection) connection.control_peer_connection->close();
    if (connection.video_peer_connection) connection.video_peer_connection->close();
}
