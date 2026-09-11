#include "H265Decoder.hpp"
#include <iostream>
#include <vector>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

struct H265Decoder::Impl {
    const AVCodec *codec = nullptr;
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    bool initialized = false;
};

H265Decoder::H265Decoder() {
    impl = new Impl();
}

H265Decoder::~H265Decoder(){ stop(); delete impl; }

bool H265Decoder::initialize(){
    if (impl->initialized) return true;
    impl->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!impl->codec) {
        std::cerr << "H265 decoder not found" << std::endl;
        return false;
    }
    impl->ctx = avcodec_alloc_context3(impl->codec);
    if (!impl->ctx) return false;

    // Low latency settings
    impl->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    impl->ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    // Tolerate truncated/corrupt NAL units (packet loss) instead of erroring out
    impl->ctx->err_recognition = 0; // default; rely on flags below
    impl->ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

    if (avcodec_open2(impl->ctx, impl->codec, nullptr) < 0) {
        std::cerr << "Failed to open H265 decoder" << std::endl;
        return false;
    }
    impl->frame = av_frame_alloc();
    impl->pkt = av_packet_alloc();
    impl->initialized = true;
    return true;
}

FramePtr H265Decoder::decode(const uint8_t* data, size_t size, int64_t timestamp_ms){
    if (!impl->initialized) return nullptr;
    // copy into packet
    av_packet_unref(impl->pkt);
    if (av_new_packet(impl->pkt, static_cast<int>(size)) < 0) return nullptr;
    memcpy(impl->pkt->data, data, size);
    impl->pkt->pts = timestamp_ms;

    int ret = avcodec_send_packet(impl->ctx, impl->pkt);
    if (ret < 0) {
        // EINVAL usually means missing VPS/SPS/PPS (joined mid-GOP) or corrupt data.
        // Flush and keep going; the next IDR frame will resync the stream.
        if (ret == AVERROR(EINVAL)) {
            avcodec_flush_buffers(impl->ctx);
            return nullptr;
        }
        std::cerr << "avcodec_send_packet error " << ret << std::endl;
        return nullptr;
    }

    ret = avcodec_receive_frame(impl->ctx, impl->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return nullptr;
    } else if (ret < 0) {
        std::cerr << "avcodec_receive_frame error " << ret << std::endl;
        return nullptr;
    }

    int width = impl->frame->width;
    int height = impl->frame->height;

    std::vector<RawYUVPlaneInput> inputs = {
        { impl->frame->data[0], static_cast<size_t>(impl->frame->linesize[0] * height), impl->frame->linesize[0], 1 },
        { impl->frame->data[1], static_cast<size_t>(impl->frame->linesize[1] * (height / 2)), impl->frame->linesize[1], 1 },
        { impl->frame->data[2], static_cast<size_t>(impl->frame->linesize[2] * (height / 2)), impl->frame->linesize[2], 1 }
    };

    auto frame = Frame::from_android_image(0, width, height, 0, timestamp_ms, inputs);
    av_frame_unref(impl->frame);
    return frame;
}

void H265Decoder::stop(){
    if (!impl) return;
    if (impl->ctx) {
        avcodec_free_context(&impl->ctx);
        impl->ctx = nullptr;
    }
    if (impl->frame) { av_frame_free(&impl->frame); impl->frame = nullptr; }
    if (impl->pkt) { av_packet_free(&impl->pkt); impl->pkt = nullptr; }
    impl->initialized = false;
}
