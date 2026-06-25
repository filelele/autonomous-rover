#include "FrameBuffer.hpp"

#include <algorithm>

std::shared_ptr<const Frame> Frame::from_android_image(
    int format,
    int width,
    int height,
    int rotation_degrees,
    int64_t timestamp_ns,
    const std::vector<RawYUVPlaneInput>& input_planes) {

    std::shared_ptr<Frame> frame = std::make_shared<Frame>();
    frame->format = format;
    frame->width = width;
    frame->height = height;
    frame->rotation_degrees = rotation_degrees;
    frame->timestamp_ns = timestamp_ns;
    frame->plane_count = static_cast<int>(std::min<size_t>(3, input_planes.size()));

    for (int i = 0; i < frame->plane_count; ++i) {
        const RawYUVPlaneInput& src = input_planes[i];
        Frame::Plane& dst = frame->planes[i];
        dst.row_stride = src.row_stride;
        dst.pixel_stride = src.pixel_stride;
        if (src.data != nullptr && src.size > 0) {
            dst.data.assign(src.data, src.data + src.size);
        }
    }

    return frame;
}

FrameBuffer::FrameBuffer() : latest_frame(nullptr) {}

void FrameBuffer::update_frame(FramePtr&& new_frame) {
    latest_frame.store(new_frame, std::memory_order_release);
}

FramePtr FrameBuffer::get_latest_frame() const {
    return latest_frame.load(std::memory_order_acquire);
}
