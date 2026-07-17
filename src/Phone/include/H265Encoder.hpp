#ifndef PHONE_H265_ENCODER_HPP
#define PHONE_H265_ENCODER_HPP
#include <cstdint>
#include <functional>
#include <memory>
#include "FrameBuffer.hpp"

class H265Encoder {
public:
    using EncodedCallback = std::function<void(const uint8_t* data, size_t size, bool isConfig, bool isKey, int64_t pts_us)>;

    H265Encoder();
    ~H265Encoder();

    // Initialize encoder with width/height/bitrate/fps. intraRefreshPeriod is in frames, 0 to disable.
    bool initialize(int width, int height, int bitrate_bps, int fps, int intraRefreshPeriod = 0);
    void setCallback(EncodedCallback cb);

    // Encode a single frame (from FramePtr). This may block briefly.
    bool encodeFrame(const FramePtr& frame);

    // Request a key frame
    void requestKeyFrame();

    // Stop and release
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // PHONE_H265_ENCODER_HPP
