#ifndef SERVER_H265_DECODER_HPP
#define SERVER_H265_DECODER_HPP

#include "FrameBuffer.hpp"

class H265Decoder {
public:
    H265Decoder();
    ~H265Decoder();
    bool initialize();

    // Decode an Annex-B H.265 packet; returns native FramePtr (YUV420 planar) if a frame is produced
    FramePtr decode(const uint8_t* data, size_t size, int64_t timestamp_ms = 0);

    void stop();
private:
    struct Impl;
    Impl* impl = nullptr;
};

#endif
