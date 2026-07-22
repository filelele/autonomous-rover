#include "H265Encoder.hpp"
#include <android/log.h>
#include <vector>
#include <cstring>

#define TAG_ENCODER "H265Encoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG_ENCODER, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG_ENCODER, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG_ENCODER, __VA_ARGS__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define AMEDIACODEC_BUFFER_FLAG_KEY_FRAME 1

constexpr int32_t BITRATE_MODE_CQ  = 0; // Constant Quality
constexpr int32_t BITRATE_MODE_VBR = 1; // Variable Bitrate
constexpr int32_t BITRATE_MODE_CBR = 2; // Constant Bitrate

struct H265Encoder::Impl {
    AMediaCodec* codec = nullptr;
    AMediaFormat* format = nullptr;
    bool configured = false;
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;
    EncodedCallback callback;
};

H265Encoder::H265Encoder() : impl(new Impl()) {}

H265Encoder::~H265Encoder(){ stop(); }

bool H265Encoder::initialize(int width, int height, int bitrate_bps, int fps, int intraRefreshPeriod){
    impl->width = width;
    impl->height = height;
    impl->bitrate = bitrate_bps;
    impl->fps = fps;

    impl->format = AMediaFormat_new();
    AMediaFormat_setString(impl->format, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 19); // 19 is COLOR_FormatYUV420Planar (I420)
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_BITRATE_MODE, BITRATE_MODE_CBR);
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate_bps);
    AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);

    /*
    constant bit rate so intra frame column does not lose u and v while no big motion with variable bitrate.
    huge intrarefreshperiod -> thinner intra column -> take more frame to reach a column again if that column is lost or no u v error.
    lower fps and lower bitrate give some space on the device network queue for control ACK to send back.
    */

    if (intraRefreshPeriod > 0) {
        AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2000);
        AMediaFormat_setInt32(impl->format, "intra-refresh-period", intraRefreshPeriod);
    } else {
        AMediaFormat_setInt32(impl->format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    }

    impl->codec = AMediaCodec_createEncoderByType("video/hevc");
    if (!impl->codec) {
        LOGE("Failed to create AMediaCodec encoder for hevc");
        return false;
    }

    media_status_t status = AMediaCodec_configure(impl->codec, impl->format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", status);
        return false;
    }

    status = AMediaCodec_start(impl->codec);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", status);
        return false;
    }

    impl->configured = true;
    LOGI("AMediaCodec HEVC encoder initialized %dx%d @%dfps %dbps", width, height, fps, bitrate_bps);
    return true;
}

void H265Encoder::setCallback(EncodedCallback cb){ impl->callback = std::move(cb); }

bool H265Encoder::encodeFrame(const FramePtr& frame){
    if (!impl->configured || !impl->codec) return false;

    ssize_t idx = AMediaCodec_dequeueInputBuffer(impl->codec, 2000);
    if (idx < 0) {
        // input buffer not available
        return false;
    }

    size_t bufSize = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(impl->codec, idx, &bufSize);
    if (!buf) return false;

    // Convert FramePtr YUV planes into planar layout I420.
    int w = frame->width;
    int h = frame->height;
    size_t ySize = w * h;
    size_t uvSize = ySize / 4;
    size_t total = ySize + uvSize * 2;
    if (bufSize < total) {
        // buffer too small
        return false;
    }

    // Copy Y plane
    memcpy(buf, frame->planes[0].data.data(), ySize);
    // Copy U and V planes
    memcpy(buf + ySize, frame->planes[1].data.data(), uvSize);
    memcpy(buf + ySize + uvSize, frame->planes[2].data.data(), uvSize);

    int64_t presentation_us = frame->timestamp_ns / 1000;
    media_status_t status = AMediaCodec_queueInputBuffer(impl->codec, idx, 0, static_cast<size_t>(total), presentation_us, 0);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_queueInputBuffer failed: %d", status);
        return false;
    }

    AMediaCodecBufferInfo info;
    ssize_t outIndex = AMediaCodec_dequeueOutputBuffer(impl->codec, &info, 0);
    while (outIndex >= 0) {
        size_t outSize = 0;
        uint8_t* outBuf = AMediaCodec_getOutputBuffer(impl->codec, outIndex, &outSize);
            if (outBuf && outSize > 0) {
            bool isConfig = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
            bool isKey = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
            if (impl->callback) impl->callback(outBuf, outSize, isConfig, isKey, info.presentationTimeUs);
        }
        AMediaCodec_releaseOutputBuffer(impl->codec, outIndex, false);
        outIndex = AMediaCodec_dequeueOutputBuffer(impl->codec, &info, 0);
    }
    return true;
}

void H265Encoder::requestKeyFrame(){
    if (!impl->codec) return;
    // Request key frame via setParameters when available (API 31+).
    // Older NDKs don't expose AMEDIACODEC_KEY_REQUEST_SYNC_FRAME; skip with a warning.
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 31
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, AMEDIACODEC_KEY_REQUEST_SYNC_FRAME, 1);
    AMediaCodec_setParameters(impl->codec, params);
    AMediaFormat_delete(params);
#else
    LOGW("requestKeyFrame(): AMEDIACODEC_KEY_REQUEST_SYNC_FRAME not available on this NDK/API level; skipping request");
#endif
}

void H265Encoder::stop(){
    if (impl->codec) {
        AMediaCodec_stop(impl->codec);
        AMediaCodec_delete(impl->codec);
        impl->codec = nullptr;
    }
    if (impl->format) {
        AMediaFormat_delete(impl->format);
        impl->format = nullptr;
    }
    impl->configured = false;
}
