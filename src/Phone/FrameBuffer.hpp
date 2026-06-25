#ifndef FRAME_BUFFER
#define FRAME_BUFFER

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>

struct RawYUVPlaneInput {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int row_stride = 0;
    int pixel_stride = 0;
};

struct Frame {
    struct Plane {
        std::vector<uint8_t> data;
        int row_stride = 0;
        int pixel_stride = 0;
    };

    int format = 0;
    int width = 0;
    int height = 0;
    int rotation_degrees = 0;
    int64_t timestamp_ns = 0;

    int plane_count = 0;
    std::array<Plane, 3> planes;

    static std::shared_ptr<const Frame> from_android_image
    (
        int format,
        int width,
        int height,
        int rotation_degrees,
        int64_t timestamp_ns,
        const std::vector<RawYUVPlaneInput>& input_planes
    );
};

using FramePtr = std::shared_ptr<const Frame>;

class FrameBuffer {
private:
    std::atomic<FramePtr> latest_frame; //use atomic, update shared_ptr object without getting blocked

public:
    FrameBuffer();
    ~FrameBuffer() = default;
    
    void update_frame(FramePtr&& new_frame);
    FramePtr get_latest_frame() const;
};

#endif