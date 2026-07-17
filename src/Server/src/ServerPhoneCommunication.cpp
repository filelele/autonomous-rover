#include "ServerPhoneCommunication.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <rtc/h265rtpdepacketizer.hpp>

ServerPhoneCommunication::ServerPhoneCommunication() {
    is_running = true;
    decoder_thread = std::thread(&ServerPhoneCommunication::decoderWorker, this);
}

ServerPhoneCommunication::~ServerPhoneCommunication() {
    stopCommunication();
    if (decoder_thread.joinable()) decoder_thread.join();
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

static bool sendPrefixed(int fd, const std::string& msg) {
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    if (send(fd, &len, sizeof(len), 0) != sizeof(len)) return false;
    if (send(fd, msg.c_str(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) return false;
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

void ServerPhoneCommunication::initialize(const std::string& publisher_ip, int controlSignalPort, int videoSignalPort) {
    if (is_initialized) return;

    rtcSctpSettings sctpSettings;
    memset(&sctpSettings, 0, sizeof(sctpSettings));
    sctpSettings.sendBufferSize = 64 * 1024;
    sctpSettings.maxChunksOnQueue = 1024;
    rtcSetSctpSettings(&sctpSettings);

    decoder.initialize();

    while (is_running && !is_initialized) {
        // Control peer connection: data channels only
        rtc::Configuration config;
        auto control_pc = std::make_shared<rtc::PeerConnection>(config);
        control_pc->onStateChange([this](rtc::PeerConnection::State state) {
            std::cout << "[WebRTC] PeerConnection State: " << (int)state << std::endl;
            if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed) {
                is_initialized = false;
            }
        });

        //state channel
        rtc::DataChannelInit state_channel_config;
        state_channel_config.reliability.unordered = true;
        state_channel_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(20);
        connection.in_state_channel = control_pc->createDataChannel("state_channel", state_channel_config);

        //mode_channel
        rtc::DataChannelInit mode_channel_config;
        mode_channel_config.reliability.unordered = false;
        mode_channel_config.reliability.maxRetransmits = 3;
        connection.in_out_mode_channel = control_pc->createDataChannel("mode_channel", mode_channel_config);
        connection.in_out_mode_channel->onMessage([this](const rtc::message_variant& message) {
            std::string msg_string;
            if (std::holds_alternative<std::string>(message)) msg_string = std::get<std::string>(message);
            else msg_string = std::string(reinterpret_cast<const char*>(std::get<rtc::binary>(message).data()), std::get<rtc::binary>(message).size());

            for (size_t i = 0; i < msg_string.length(); ++i) {
                if (msg_string[i] == 'm' && i + 1 < msg_string.length()) {
                    in_out.in_manual_mode_state.store(msg_string[i + 1] == '1', std::memory_order_release);
                    i++;
                } else if (msg_string[i] == 'r' && i + 1 < msg_string.length()) {
                    in_out.in_record_data_state.store(msg_string[i + 1] == '1', std::memory_order_release);
                    i++;
                }
            }
        });

        //manual_control_channel
        rtc::DataChannelInit manual_control_channel_config;
        manual_control_channel_config.reliability.unordered = true;
        manual_control_channel_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(20); 
        //smaller than 25ms, the rate of new control arrival
        connection.out_manual_control_channel = control_pc->createDataChannel("manual_control_channel", manual_control_channel_config);

        control_pc->setLocalDescription(rtc::Description::Type::Offer);

        int timeout_ms = 5000;
        while (!control_pc->localDescription().has_value() && timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (!control_pc->localDescription().has_value()) continue;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(controlSignalPort);
        inet_pton(AF_INET, publisher_ip.c_str(), &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sendPrefixed(sock, std::string(control_pc->localDescription().value()));
        std::string sdp_answer = readPrefixed(sock);
        if (!sdp_answer.empty()) {
            control_pc->setRemoteDescription(rtc::Description(sdp_answer, "answer"));
            connection.control_peer_connection = control_pc;
            std::cout << "[Control Signaling] Handshake complete." << std::endl;
        }
        close(sock);

        // Video peer connection: video track only
        rtc::Configuration video_config;
        auto video_pc = std::make_shared<rtc::PeerConnection>(video_config);
        video_pc->onStateChange([this](rtc::PeerConnection::State state) {
            std::cout << "[WebRTC-Video] PeerConnection State: " << (int)state << std::endl;
            if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed) {
                is_initialized = false;
            }
        });

        rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
        video.addH265Codec(96);
        auto vt = video_pc->addTrack(video);
        connection.in_video_track = vt;
        auto h265Depacketizer = std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        vt->setMediaHandler(h265Depacketizer);
        vt->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            {
                std::lock_guard<std::mutex> qlk(queue_mutex);
                decode_queue.push({std::move(data), static_cast<uint64_t>(info.timestamp)});
                while (decode_queue.size() > 2) decode_queue.pop();
            }
            queue_cv.notify_one();
            fps_counter.frame_accumulator++;
        });

        video_pc->setLocalDescription(rtc::Description::Type::Offer);

        timeout_ms = 5000;
        while (!video_pc->localDescription().has_value() && timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (!video_pc->localDescription().has_value()) continue;

        int video_sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in video_serv_addr;
        video_serv_addr.sin_family = AF_INET;
        video_serv_addr.sin_port = htons(videoSignalPort);
        inet_pton(AF_INET, publisher_ip.c_str(), &video_serv_addr.sin_addr);

        if (connect(video_sock, (struct sockaddr *)&video_serv_addr, sizeof(video_serv_addr)) < 0) {
            close(video_sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sendPrefixed(video_sock, std::string(video_pc->localDescription().value()));
        std::string video_sdp_answer = readPrefixed(video_sock);
        if (!video_sdp_answer.empty()) {
            video_pc->setRemoteDescription(rtc::Description(video_sdp_answer, "answer"));
            connection.video_peer_connection = video_pc;
            is_initialized = true;
            std::cout << "[Video Signaling] Handshake complete." << std::endl;
        }
        close(video_sock);
    }
}

void ServerPhoneCommunication::decoderWorker() {
    while (is_running) {
        EncodedFrame frame;
        {
            std::unique_lock<std::mutex> lk(queue_mutex);
            queue_cv.wait(lk, [this] { return !is_running || !decode_queue.empty(); });
            if (!is_running) break;
            frame = std::move(decode_queue.front());
            decode_queue.pop();
        }

        auto opt = decoder.decode(reinterpret_cast<const uint8_t*>(frame.data.data()), frame.data.size(), frame.timestamp_us);
        if (opt.has_value()) {
            cv::Mat bgr = opt.value();
            cv::Mat upsampled;
            cv::resize(bgr, upsampled, cv::Size(1280, 720), 0, 0, cv::INTER_LINEAR);
            in_out.in_bgr_buffer.store(std::make_shared<cv::Mat>(std::move(upsampled)), std::memory_order_release);
        }

        auto now_steady = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now_steady - fps_counter.window_start;
        if (elapsed.count() >= 1.0) {
            fps_counter.incoming_fps = fps_counter.frame_accumulator / elapsed.count();
            fps_counter.frame_accumulator = 0;
            fps_counter.window_start = now_steady;
        }
    }
}

std::shared_ptr<const cv::Mat> ServerPhoneCommunication::getLatestBgr(){
    return std::const_pointer_cast<const cv::Mat>(in_out.in_bgr_buffer.load(std::memory_order_acquire));
}

void ServerPhoneCommunication::toggleManualMode() {
    if (connection.in_out_mode_channel && connection.in_out_mode_channel->isOpen()) {
        connection.in_out_mode_channel->send("m");
    }
}

void ServerPhoneCommunication::toggleRecordData() {
    if (connection.in_out_mode_channel && connection.in_out_mode_channel->isOpen()) {
        connection.in_out_mode_channel->send("r");
    }
}

void ServerPhoneCommunication::sendManualControl(float heading, float angle) {
    if (connection.out_manual_control_channel && connection.out_manual_control_channel->isOpen()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f,%.2f", heading, angle);
        connection.out_manual_control_channel->send(buf);
    }
}

void ServerPhoneCommunication::stopCommunication() {
    is_running.store(false, std::memory_order_release);
    queue_cv.notify_all();
    if (connection.control_peer_connection) connection.control_peer_connection->close();
    if (connection.video_peer_connection) connection.video_peer_connection->close();
}
