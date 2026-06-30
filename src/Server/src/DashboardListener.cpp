#include "DashboardListener.hpp"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>


DashboardListener::DashboardListener(){}

DashboardListener::~DashboardListener() {
    stopListening();
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

void DashboardListener::initialize(const std::string& publisher_ip, int signalPort) {
    if (is_initialized) return;

    while (is_running && !is_initialized) {
        rtc::Configuration config;
        peer_connection = std::make_shared<rtc::PeerConnection>(config);

        peer_connection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            std::string label = channel->label();
            std::cout << "[WebRTC] New DataChannel: " << label << std::endl;

            if (label == "video_stream") {
                video_channel = channel;
                video_channel->onMessage([this](const rtc::message_variant& message) {
                    if (std::holds_alternative<std::string>(message)) return;

                    const rtc::binary& packet = std::get<rtc::binary>(message);
                    if (packet.size() < sizeof(int64_t)) return;

                    int64_t frame_ts_ns;
                    std::memcpy(&frame_ts_ns, packet.data(), sizeof(int64_t));

                    // Both sides synced to Unix Epoch Time
                    auto now = std::chrono::system_clock::now();
                    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

                    int64_t latency_ms = (now_ns - frame_ts_ns) / 1000000;
                    
                    if (latency_ms > 200) {
                        std::cout << "[Video] Dropping stale frame. Latency: " << latency_ms << "ms" << std::endl;
                        return;
                    }

                    packet_counter.packet_accumulator++;

                    auto now_steady = std::chrono::steady_clock::now();
                    std::chrono::duration<double> elapsed = now_steady - packet_counter.window_start;

                    // every 1 sec
                    if (elapsed.count() >= 1.0) {
                        packet_counter.incoming_fps = packet_counter.packet_accumulator / elapsed.count();
                        packet_counter.packet_accumulator = 0;
                        packet_counter.window_start = now_steady;
                    }

                    const std::byte* webp_data = packet.data() + sizeof(int64_t);
                    size_t webp_size = packet.size() - sizeof(int64_t);

                    cv::Mat encoded(1, static_cast<int>(webp_size), CV_8UC1, const_cast<std::byte*>(webp_data));
                    cv::Mat bgr = cv::imdecode(encoded, cv::IMREAD_COLOR);

                    if (bgr.empty()) {
                        std::cerr << "[Video] Failed to decode WebP." << std::endl;
                        return;
                    }

                    // Gamma Correction (gamma = 1.4)
                    // Precompute LUT for faster performance
                    static unsigned char lut[256];
                    static bool lut_ready = false;
                    if (!lut_ready) {
                        double gamma = 1.4;
                        for (int i = 0; i < 256; i++) {
                            lut[i] = cv::saturate_cast<uchar>(pow(i / 255.0, 1.0 / gamma) * 255.0);
                        }
                        lut_ready = true;
                    }
                    cv::Mat gamma_corrected;
                    cv::LUT(bgr, cv::Mat(1, 256, CV_8U, lut), gamma_corrected);

                    // Upsample from 384x216 to 1280x720
                    // INTER_LANCZOS4 > INTER_CUBIC > INTER_LINEAR in quality, reverse in latency
                    cv::Mat upsampled;
                    cv::resize(gamma_corrected, upsampled, cv::Size(1280, 720), 0, 0, cv::INTER_LANCZOS4);

                    bgr_buffer.store(std::make_shared<cv::Mat>(std::move(upsampled)), std::memory_order_release);
                });

                video_channel->onOpen([]() { std::cout << "[Video] Channel opened." << std::endl; });
                video_channel->onClosed([]() { std::cout << "[Video] Channel closed." << std::endl; });
            } else if (label == "state_stream") {
                data_channel = channel;
                data_channel->onOpen([]() { std::cout << "[State] Channel opened." << std::endl; });
            }
        });

        peer_connection->onStateChange([this](rtc::PeerConnection::State state) {
            std::cout << "[WebRTC] PeerConnection State: " << state << std::endl;
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                is_initialized = false;
            }
        });

        peer_connection->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
            std::cout << "[WebRTC] Gathering State: " << state << std::endl;
        });

        try {
            // Create a dummy data channel to ensure there's something to negotiate in the Offer
            auto dummy = peer_connection->createDataChannel("initial_handshake");

            peer_connection->setLocalDescription(rtc::Description::Type::Offer);
        } catch (const std::exception& e) {
            std::cerr << "[Error] setLocalDescription failed: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        int timeout_ms = 5000;
        while (!peer_connection->localDescription().has_value() && timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }

        if (!peer_connection->localDescription().has_value()) {
            std::cerr << "[Error] SDP Offer generation timeout." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::string sdp_offer = std::string(peer_connection->localDescription().value());

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(signalPort);
        inet_pton(AF_INET, publisher_ip.c_str(), &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "[Signaling] Phone not ready. Retrying..." << std::endl;
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (!sendPrefixed(sock, sdp_offer)) {
            std::cerr << "[Error] Failed to send Offer." << std::endl;
            close(sock);
            continue;
        }
        std::cout << "[Signaling] Sent SDP Offer" << std::endl;

        std::string sdp_answer = readPrefixed(sock);
        if (!sdp_answer.empty()) {
            try {
                peer_connection->setRemoteDescription(rtc::Description(sdp_answer, "answer"));
                std::cout << "[Signaling] Received SDP Answer. Negotiating WebRTC..." << std::endl;
                is_initialized = true;
            } catch (const std::exception& e) {
                std::cerr << "[Error] Failed to set remote description: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "[Error] Failed to receive valid Answer." << std::endl;
        }

        close(sock);
        if (!is_initialized) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

std::shared_ptr<const cv::Mat> DashboardListener::getLatestBgr(){
    return std::const_pointer_cast<const cv::Mat>(bgr_buffer.load(std::memory_order_acquire));
}

void DashboardListener::stopListening() {
    is_running.store(false, std::memory_order_release);
    if (peer_connection) peer_connection->close();
}
