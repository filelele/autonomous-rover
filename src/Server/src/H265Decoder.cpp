#include "H265Decoder.hpp"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct H265Decoder::Impl {
    const AVCodec *codec = nullptr;
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
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

    if (avcodec_open2(impl->ctx, impl->codec, nullptr) < 0) {
        std::cerr << "Failed to open H265 decoder" << std::endl;
        return false;
    }
    impl->frame = av_frame_alloc();
    impl->pkt = av_packet_alloc();
    impl->initialized = true;
    return true;
}

std::optional<cv::Mat> H265Decoder::decode(const uint8_t* data, size_t size, int64_t pts_us){
    if (!impl->initialized) return std::nullopt;
    // copy into packet
    av_packet_unref(impl->pkt);
    if (av_new_packet(impl->pkt, static_cast<int>(size)) < 0) return std::nullopt;
    memcpy(impl->pkt->data, data, size);
    impl->pkt->pts = pts_us;

    int ret = avcodec_send_packet(impl->ctx, impl->pkt);
    if (ret < 0) {
        std::cerr << "avcodec_send_packet error " << ret << std::endl;
        return std::nullopt;
    }

    ret = avcodec_receive_frame(impl->ctx, impl->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return std::nullopt;
    } else if (ret < 0) {
        std::cerr << "avcodec_receive_frame error " << ret << std::endl;
        return std::nullopt;
    }

    int width = impl->frame->width;
    int height = impl->frame->height;
    impl->sws = sws_getCachedContext(impl->sws, width, height, static_cast<AVPixelFormat>(impl->frame->format),
                                     width, height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl->sws) return std::nullopt;

    cv::Mat bgr(height, width, CV_8UC3);
    uint8_t* dst[4];
    int dst_linesize[4];
    dst[0] = bgr.data;
    dst_linesize[0] = static_cast<int>(bgr.step[0]);
    sws_scale(impl->sws, impl->frame->data, impl->frame->linesize, 0, height, dst, dst_linesize);

    av_frame_unref(impl->frame);
    return bgr;
}

void H265Decoder::stop(){
    if (!impl) return;
    if (impl->ctx) {
        avcodec_free_context(&impl->ctx);
        impl->ctx = nullptr;
    }
    if (impl->frame) { av_frame_free(&impl->frame); impl->frame = nullptr; }
    if (impl->pkt) { av_packet_free(&impl->pkt); impl->pkt = nullptr; }
    if (impl->sws) { sws_freeContext(impl->sws); impl->sws = nullptr; }
    impl->initialized = false;
}
