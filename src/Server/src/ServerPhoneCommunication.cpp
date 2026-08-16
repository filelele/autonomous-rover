#include "ServerPhoneCommunication.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <rtc/h265rtpdepacketizer.hpp>

/* //For benchmarking only
static constexpr const char* kLocalizationLatencyLogPath = "/home/filelele/personal_project/autonomous-rover/notes/capture_till_localization_LAN.txt";

static void appendLocalizationLatencyLog(double latency_ms) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);

    std::ofstream log_file(kLocalizationLatencyLogPath, std::ios::app);
    if (!log_file.is_open()) {
        std::cerr << "[ServerPhoneCommunication] Failed to open localization latency log file: "
                  << kLocalizationLatencyLogPath << std::endl;
        return;
    }

    log_file << latency_ms << '\n';
}
*/
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

    std::cout << "[ServerPhoneCommunication] initialize() started for "
              << publisher_ip << ":" << controlSignalPort << ", " << videoSignalPort << std::endl;

    rtcSctpSettings sctpSettings;
    memset(&sctpSettings, 0, sizeof(sctpSettings));
    sctpSettings.sendBufferSize = 64 * 1024;
    sctpSettings.maxChunksOnQueue = 1024;
    rtcSetSctpSettings(&sctpSettings);

    std::cout << "[ServerPhoneCommunication] RTC SCTP settings applied" << std::endl;

    std::cout << "[ServerPhoneCommunication] Initializing decoder" << std::endl;
    decoder.initialize();
    std::cout << "[ServerPhoneCommunication] Decoder initialized" << std::endl;

    while (is_running && !is_initialized) {
        std::cout << "[ServerPhoneCommunication] Starting control peer connection setup" << std::endl;
        // Control peer connection: data channels only
        rtc::Configuration config;
        auto control_pc = std::make_shared<rtc::PeerConnection>(config);
        control_pc->onStateChange([this](rtc::PeerConnection::State state) {
            std::cout << "[WebRTC] PeerConnection State: " << (int)state << std::endl;
            if (state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed) {
                is_initialized = false;
            }
        });

        //telemetry_channel
        rtc::DataChannelInit telemetry_channel_config;
        telemetry_channel_config.reliability.unordered = true;
        telemetry_channel_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(20);
        connection.in_telemetry_channel = control_pc->createDataChannel("telemetry_channel", telemetry_channel_config);
        connection.in_telemetry_channel->onMessage([this](const rtc::message_variant& message) {
            std::string msg_string;
            if (std::holds_alternative<std::string>(message)) msg_string = std::get<std::string>(message);
            else msg_string = std::string(reinterpret_cast<const char*>(std::get<rtc::binary>(message).data()), std::get<rtc::binary>(message).size());

            // Strictly accepts: "x,y,heading,m1,r0" format
            size_t first_comma = msg_string.find(',');
            size_t second_comma = msg_string.find(',', first_comma + 1);
            size_t third_comma = msg_string.find(',', second_comma + 1);
            size_t fourth_comma = msg_string.find(',', third_comma + 1);
            if (first_comma != std::string::npos && second_comma != std::string::npos &&
                third_comma != std::string::npos && fourth_comma != std::string::npos &&
                msg_string.find(',', fourth_comma + 1) == std::string::npos) {
                try {
                    float x = std::stof(msg_string.substr(0, first_comma));
                    float y = std::stof(msg_string.substr(first_comma + 1, second_comma - first_comma - 1));
                    float heading = std::stof(msg_string.substr(second_comma + 1, third_comma - second_comma - 1));
                    std::string manual_mode = msg_string.substr(third_comma + 1, fourth_comma - third_comma - 1);
                    std::string record_mode = msg_string.substr(fourth_comma + 1);

                    if (manual_mode == "m1") in_out.in_telemetry.manual_mode_state = true;
                    else if (manual_mode == "m0") in_out.in_telemetry.manual_mode_state = false;
                    if (record_mode == "r1") in_out.in_telemetry.record_data_state = true;
                    else if (record_mode == "r0") in_out.in_telemetry.record_data_state = false;
                    in_out.in_telemetry.location.x = x;
                    in_out.in_telemetry.location.y = y;
                    in_out.in_telemetry.location.heading = heading;
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing telemetry data: " << e.what() << std::endl;
                }
            }
        });

        //mode_channel
        rtc::DataChannelInit mode_channel_config;
        mode_channel_config.reliability.unordered = false;
        mode_channel_config.reliability.maxRetransmits = 3;
        connection.out_mode_channel = control_pc->createDataChannel("mode_channel", mode_channel_config);

        //manual_control_channel
        rtc::DataChannelInit manual_control_channel_config;
        manual_control_channel_config.reliability.unordered = true;
        manual_control_channel_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(20); 
        //smaller than 25ms, the rate of new control arrival
        connection.out_manual_control_channel = control_pc->createDataChannel("manual_control_channel", manual_control_channel_config);

        //location_channel
        rtc::DataChannelInit location_channel_config;
        location_channel_config.reliability.unordered = true;
        location_channel_config.reliability.maxPacketLifeTime = std::chrono::milliseconds(200);
        connection.out_location_channel = control_pc->createDataChannel("location_channel", location_channel_config);
        
        /* //For benchmarking only
        connection.out_location_channel->onMessage([](const rtc::message_variant& message) {
            std::string msg;
            if (std::holds_alternative<std::string>(message)) {
                msg = std::get<std::string>(message);
            } else {
                const rtc::binary& packet = std::get<rtc::binary>(message);
                msg.assign(reinterpret_cast<const char*>(packet.data()), packet.size());
            }

            try {
                double latency_ms = std::stod(msg);
                appendLocalizationLatencyLog(latency_ms);
            } catch (const std::exception& e) {
                std::cerr << "[ServerPhoneCommunication] Failed to parse localization latency payload: "
                          << e.what() << std::endl;
            }
        });
        */

        std::cout << "[ServerPhoneCommunication] Control data channels created, generating offer" << std::endl;
        control_pc->setLocalDescription(rtc::Description::Type::Offer);

        int timeout_ms = 5000;
        std::cout << "[ServerPhoneCommunication] Waiting for control local description" << std::endl;
        while (!control_pc->localDescription().has_value() && timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (!control_pc->localDescription().has_value()) {
            std::cout << "[ServerPhoneCommunication] Control local description timed out" << std::endl;
            continue;
        }

        std::cout << "[ServerPhoneCommunication] Control local description ready, connecting to phone" << std::endl;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(controlSignalPort);
        inet_pton(AF_INET, publisher_ip.c_str(), &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cout << "[ServerPhoneCommunication] Control connect() failed" << std::endl;
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::cout << "[ServerPhoneCommunication] Control socket connected, sending SDP offer" << std::endl;
        sendPrefixed(sock, std::string(control_pc->localDescription().value()));
        std::cout << "[ServerPhoneCommunication] Waiting for control SDP answer" << std::endl;
        std::string sdp_answer = readPrefixed(sock);
        if (!sdp_answer.empty()) {
            std::cout << "[ServerPhoneCommunication] Received control SDP answer" << std::endl;
            control_pc->setRemoteDescription(rtc::Description(sdp_answer, "answer"));
            connection.control_peer_connection = control_pc;
            std::cout << "[Control Signaling] Handshake complete." << std::endl;
        } else {
            std::cout << "[ServerPhoneCommunication] Empty control SDP answer" << std::endl;
        }
        close(sock);

        // Video peer connection: video track only
        std::cout << "[ServerPhoneCommunication] Starting video peer connection setup" << std::endl;
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
        std::cout << "[ServerPhoneCommunication] Video track configured" << std::endl;
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
        std::cout << "[ServerPhoneCommunication] Waiting for video local description" << std::endl;
        while (!video_pc->localDescription().has_value() && timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (!video_pc->localDescription().has_value()) {
            std::cout << "[ServerPhoneCommunication] Video local description timed out" << std::endl;
            continue;
        }

        std::cout << "[ServerPhoneCommunication] Video local description ready, connecting to phone" << std::endl;

        int video_sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in video_serv_addr;
        video_serv_addr.sin_family = AF_INET;
        video_serv_addr.sin_port = htons(videoSignalPort);
        inet_pton(AF_INET, publisher_ip.c_str(), &video_serv_addr.sin_addr);

        if (connect(video_sock, (struct sockaddr *)&video_serv_addr, sizeof(video_serv_addr)) < 0) {
            std::cout << "[ServerPhoneCommunication] Video connect() failed" << std::endl;
            close(video_sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::cout << "[ServerPhoneCommunication] Video socket connected, sending SDP offer" << std::endl;
        sendPrefixed(video_sock, std::string(video_pc->localDescription().value()));
        std::cout << "[ServerPhoneCommunication] Waiting for video SDP answer" << std::endl;
        std::string video_sdp_answer = readPrefixed(video_sock);
        if (!video_sdp_answer.empty()) {
            std::cout << "[ServerPhoneCommunication] Received video SDP answer" << std::endl;
            video_pc->setRemoteDescription(rtc::Description(video_sdp_answer, "answer"));
            connection.video_peer_connection = video_pc;
            is_initialized = true;
            std::cout << "[Video Signaling] Handshake complete." << std::endl;
        } else {
            std::cout << "[ServerPhoneCommunication] Empty video SDP answer" << std::endl;
        }
        close(video_sock);
    }

    std::cout << "[ServerPhoneCommunication] initialize() exiting" << std::endl;
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
            cv::Mat decoded = std::move(opt.value());
            const uint64_t frame_relative_timestamp_us = static_cast<uint64_t>(frame.timestamp_us * 1000ULL / 90ULL);
            auto decoded_frame = std::make_shared<DecodedFrame>(DecodedFrame{std::move(decoded), frame_relative_timestamp_us});
            in_out.in_decoded_frame.store(std::move(decoded_frame), std::memory_order_release);
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

std::shared_ptr<const DecodedFrame> ServerPhoneCommunication::getLatestFrame(){
    return in_out.in_decoded_frame.load(std::memory_order_acquire);
}

void ServerPhoneCommunication::toggleManualMode() {
    if (connection.out_mode_channel && connection.out_mode_channel->isOpen()) {
        connection.out_mode_channel->send("m");
    }
}

void ServerPhoneCommunication::toggleRecordData() {
    if (connection.out_mode_channel && connection.out_mode_channel->isOpen()) {
        connection.out_mode_channel->send("r");
    }
}

void ServerPhoneCommunication::sendManualControl(float heading, float angle) {
    if (connection.out_manual_control_channel && connection.out_manual_control_channel->isOpen()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f,%.2f", heading, angle);
        connection.out_manual_control_channel->send(buf);
    }
}

void ServerPhoneCommunication::sendLocation(const Location& loc/*, uint64_t timestamp_us*/) {
    if (connection.out_location_channel && connection.out_location_channel->isOpen()) {
        char buf[64];
        /* For benchmarking only
        snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f,%llu", loc.x, loc.y, loc.heading,
            static_cast<unsigned long long>(timestamp_us));
        */
        snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f", loc.x, loc.y, loc.heading);
        connection.out_location_channel->send(buf);
    }
}

void ServerPhoneCommunication::stopCommunication() {
    is_running.store(false, std::memory_order_release);
    queue_cv.notify_all();
    if (connection.control_peer_connection) connection.control_peer_connection->close();
    if (connection.video_peer_connection) connection.video_peer_connection->close();
}
