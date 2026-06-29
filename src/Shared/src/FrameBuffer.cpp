#include "FrameBuffer.hpp"
#include <memory>
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
        
        if (src.data != nullptr && src.size > 0) {
            // Full width height for Y, Half for U and V
            int plane_width = (i == 0) ? width : width / 2;
            int plane_height = (i == 0) ? height : height / 2;

            dst.data.resize(static_cast<size_t>(plane_width * plane_height));
            uint8_t* dst_ptr = dst.data.data();

            for (int row = 0; row < plane_height; ++row) {
                const uint8_t* src_row_ptr = src.data + (row * src.row_stride);
                //row_stride = real width + row padding if exists
                if (src.pixel_stride == 1) {
                    std::memcpy(dst_ptr, src_row_ptr, plane_width);
                    //safe from row padding, row padding never at row start, so just take plane_width bytes
                    dst_ptr += plane_width;
                } else {
                    //Interleaved chroma (pixel_stride == 2, common on Android NV12/NV21)
                    for (int col = 0; col < plane_width; ++col) {
                        *dst_ptr++ = src_row_ptr[col * src.pixel_stride];
                    }
                }
            }

        } else {
            // Handle empty/null plane safety boundary
            dst.data.clear();
        }
    }

    return frame;
}

FrameBuffer::FrameBuffer() : latest_frame(nullptr) {}

void FrameBuffer::update_frame(FramePtr&& new_frame) {
    std::atomic_store(&latest_frame, new_frame);
}

FramePtr FrameBuffer::get_latest_frame() const {
    return std::atomic_load(&latest_frame);
}
