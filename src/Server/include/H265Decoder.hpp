#ifndef SERVER_H265_DECODER_HPP
#define SERVER_H265_DECODER_HPP

#include <vector>
#include <optional>
#include <opencv2/opencv.hpp>

class H265Decoder {
public:
    H265Decoder();
    ~H265Decoder();
    bool initialize();

    // Decode an Annex-B H.265 packet; returns BGR cv::Mat if a frame is produced
    std::optional<cv::Mat> decode(const uint8_t* data, size_t size, int64_t pts_us);

    void stop();
private:
    struct Impl;
    Impl* impl = nullptr;
};

#endif
