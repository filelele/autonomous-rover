#include "Camera.hpp"
#include "android/log.h"

#include <camera/NdkCameraMetadataTags.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>
#include <vector>

#define TAG "Camera"
#define LOGE(...)                                                              \
  ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))
#define LOGI(...)                                                              \
  ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

Camera::Camera(FrameBuffer *targetBuffer, 
               int res_width, int res_height, 
               float focus_meters,
               float shutter_time_second,
               int iso,
               float zoom_ratio,
               int post_raw_boost,
               bool distortion_correction,
               std::optional<std::vector<double>> intrinsic,
               std::optional<std::vector<double>> distortion,
               double alpha)
    : m_cameraManager(nullptr), m_cameraIdList(nullptr),
      m_cameraDevice(nullptr), m_imageReader(nullptr),
      m_imageReaderWindow(nullptr), m_captureSession(nullptr),
      m_selectedCameraId(nullptr), m_frameBuffer(targetBuffer),
      m_res_width(res_width), m_res_height(res_height),
      m_focus_meters(focus_meters),
      m_shutter_time_second(shutter_time_second),
      m_iso(iso),
      m_zoom_ratio(zoom_ratio),
      m_post_raw_boost(post_raw_boost),
      m_distortion_correction(distortion_correction),
      m_intrinsic(std::move(intrinsic)),
      m_distortion(std::move(distortion)),
      m_alpha(alpha),
      m_undistort_ready(false) {}

Camera::~Camera() { stop_stream(); }

bool Camera::init_camera() {
    m_cameraManager = ACameraManager_create();
    if (!m_cameraManager) {
    LOGE("Failed to create ACameraManager.");
    return false;
    }

    ACameraManager_getCameraIdList(m_cameraManager, &m_cameraIdList);
    if (m_cameraIdList->numCameras == 0) {
    LOGE("No cameras found on this device.");
    return false;
    }

    // Pick wide-angle camera of 0.6x zoom, back normal camera is 0, support 1.0 to 10.0
    if (m_zoom_ratio != 0.6f) {
        m_selectedCameraId = m_cameraIdList->cameraIds[0];
    } else {
        m_selectedCameraId = "2";
    }

    // Set device callbacks: What to do when camera device has problem
    m_deviceCallbacks.context = this;
    m_deviceCallbacks.onDisconnected = [](void *ctx, ACameraDevice *dev) {
    LOGI("Camera disconnected.");
    };
    m_deviceCallbacks.onError = [](void *ctx, ACameraDevice *dev, int err) {
    LOGE("Camera error: %d", err);
    };

    camera_status_t status = ACameraManager_openCamera(
        m_cameraManager, m_selectedCameraId, &m_deviceCallbacks, &m_cameraDevice);
    if (status != ACAMERA_OK) {
    LOGE("Failed to open camera hardware device.");
    return false;
    }

    ACameraMetadata *cameraCharacteristics = nullptr;
    ACameraManager_getCameraCharacteristics(m_cameraManager, m_selectedCameraId,
                                            &cameraCharacteristics);

    ACameraMetadata_const_entry entry;
    ACameraMetadata_getConstEntry(cameraCharacteristics,
                                ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                &entry);

    bool res_exist = false;
    for (size_t i = 0; i < entry.count; i += 4) {
    int32_t format = entry.data.i32[i];
    int32_t width = entry.data.i32[i + 1];
    int32_t height = entry.data.i32[i + 2];
    int32_t input = entry.data.i32[i + 3];
    if (input == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
        format == AIMAGE_FORMAT_YUV_420_888 && width == m_res_width &&
        height == m_res_height) { // YUV_420_888 is the standard for any Google
                                    // certified
                                    // device, Gemini said so =))

        res_exist = true;
        LOGI("Found supported hardware resolution: %d x %d", width, height);
    }
    }
    if (!res_exist) {
    LOGE("Not support native %dx%d YUV_420_888 capture, maybe something else.",
            m_res_width, m_res_height);
    return false;
    }

    ACameraMetadata_free(cameraCharacteristics);

    // Allocate memory buffer slots for the incoming frame stream (m_res_width x m_res_height YUV,
    // keeping max 3 frames)
    media_status_t mediaStatus =
        AImageReader_new(m_res_width, m_res_height, AIMAGE_FORMAT_YUV_420_888, 3, &m_imageReader);
    if (mediaStatus != AMEDIA_OK) {
    LOGE("Failed to allocate ImageReader.");
    return false;
    }

    m_imageListener.context = this;
    m_imageListener.onImageAvailable = Camera::onImageAvailable;

    // Register Listener callback to the Image reader container (listen if there
    // is image write to allocated mem)
    AImageReader_setImageListener(m_imageReader, &m_imageListener);

    AImageReader_getWindow(m_imageReader, &m_imageReaderWindow);

    if (m_distortion_correction) {
        init_undistort_maps();
    }

    LOGI("Camera initialization successed.");
    return true;
}

void Camera::init_undistort_maps() {
    if (!m_intrinsic.has_value() || m_intrinsic->size() != 9 ||
        !m_distortion.has_value() || m_distortion->empty()) {
        LOGE("Cannot initialize undistort maps: intrinsic or distortion not provided or invalid size");
        m_undistort_ready = false;
        return;
    }

    cv::Mat K(3, 3, CV_64F, const_cast<double*>(m_intrinsic->data()));
    cv::Mat dist(1, static_cast<int>(m_distortion->size()), CV_64F, const_cast<double*>(m_distortion->data()));

    cv::Size size_y(m_res_width, m_res_height);
    cv::Size size_uv(m_res_width / 2, m_res_height / 2);

    // Camera matrix for half-resolution chroma UV planes
    cv::Mat K_uv = K.clone();
    K_uv.at<double>(0, 0) /= 2.0; // fx / 2
    K_uv.at<double>(1, 1) /= 2.0; // fy / 2
    K_uv.at<double>(0, 2) /= 2.0; // cx / 2
    K_uv.at<double>(1, 2) /= 2.0; // cy / 2

    cv::Mat newK_y;
    cv::Mat newK_uv;

    if (m_alpha >= 0.0) {
        // alpha = 0.0: crops black borders completely
        // alpha = 1.0: preserves all original pixels with curved black borders
        newK_y = cv::getOptimalNewCameraMatrix(K, dist, size_y, m_alpha, size_y);
        newK_uv = newK_y.clone();
        newK_uv.at<double>(0, 0) /= 2.0;
        newK_uv.at<double>(1, 1) /= 2.0;
        newK_uv.at<double>(0, 2) /= 2.0;
        newK_uv.at<double>(1, 2) /= 2.0;
    } else {
        // Default (alpha < 0): keeps original intrinsic matrix K unscaled
        newK_y = K;
        newK_uv = K_uv;
    }

    cv::initUndistortRectifyMap(
        K, dist, cv::Mat(),
        newK_y, size_y, CV_16SC2, m_map1_y, m_map2_y
    );

    cv::initUndistortRectifyMap(
        K_uv, dist, cv::Mat(),
        newK_uv, size_uv, CV_16SC2, m_map1_uv, m_map2_uv
    );

    m_dst_y.create(m_res_height, m_res_width, CV_8UC1);
    m_dst_u.create(m_res_height / 2, m_res_width / 2, CV_8UC1);
    m_dst_v.create(m_res_height / 2, m_res_width / 2, CV_8UC1);

    m_undistort_ready = true;
    LOGI("Software lens undistortion initialized for %dx%d (alpha=%.2f).",
         m_res_width, m_res_height, m_alpha);
}

void Camera::start_stream(int fps) {
    m_fps = fps;
    if (!m_cameraDevice || !m_imageReaderWindow) {
        return;
    }

    struct timespec real_ts{}, boot_ts{};
    clock_gettime(CLOCK_REALTIME, &real_ts);
    clock_gettime(CLOCK_BOOTTIME, &boot_ts);
    m_base_epoch_ms = static_cast<uint64_t>(real_ts.tv_sec) * 1000ULL + real_ts.tv_nsec / 1000000ULL;
    m_base_boottime_ms = static_cast<uint64_t>(boot_ts.tv_sec) * 1000ULL + boot_ts.tv_nsec / 1000000ULL;

    // Prepare capture session configuration containers
    ACameraOutputTarget *outputTarget = nullptr;
    ACameraOutputTarget_create(m_imageReaderWindow, &outputTarget);

    ACaptureSessionOutput *sessionOutput = nullptr;
    ACaptureSessionOutput_create(m_imageReaderWindow, &sessionOutput);

    ACaptureSessionOutputContainer *container = nullptr;
    ACaptureSessionOutputContainer_create(&container);
    ACaptureSessionOutputContainer_add(container, sessionOutput);

    // Create a Capture request, a continuous video capture one
    ACameraDevice_createCaptureRequest(m_cameraDevice, TEMPLATE_PREVIEW,
                                        &m_captureRequest);
    ACaptureRequest_addTarget(m_captureRequest, outputTarget);

    // lock fps
    int32_t targetFpsRange[2] = {fps, fps};
    ACaptureRequest_setEntry_i32(m_captureRequest, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, targetFpsRange);

    // lock focus in meters (maybe ignored in wide-angle camera)
    uint8_t afMode = ACAMERA_CONTROL_AF_MODE_OFF;
    ACaptureRequest_setEntry_u8(m_captureRequest, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
    if(m_focus_meters == 0.0f) m_focus_meters = 0.1f;
    float focus_diopters = 1.0f / m_focus_meters;
    ACaptureRequest_setEntry_float(m_captureRequest, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focus_diopters);
    
    //Turn off Auto Exposure
    uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
    ACaptureRequest_setEntry_u8(m_captureRequest, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
    
    // ISO
    ACaptureRequest_setEntry_i32(m_captureRequest, ACAMERA_SENSOR_SENSITIVITY, 1, &m_iso);

    // shutter time
    int64_t shutterTimeNs = static_cast<int64_t>(m_shutter_time_second * 1e9);
    ACaptureRequest_setEntry_i64(m_captureRequest, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &shutterTimeNs);
    
    // zoom
    if(m_zoom_ratio != 0.6f){
        ACaptureRequest_setEntry_float(m_captureRequest, ACAMERA_CONTROL_ZOOM_RATIO, 1, &m_zoom_ratio);
    }

    // digital post-raw sensitivity boost
    if (m_post_raw_boost > 0) {
        ACaptureRequest_setEntry_i32(m_captureRequest, ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST, 1, &m_post_raw_boost);
    }

    // hardware noise reduction
    uint8_t noiseMode = ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY;
    ACaptureRequest_setEntry_u8(m_captureRequest, ACAMERA_NOISE_REDUCTION_MODE, 1, &noiseMode);

    // hot pixel correction (cleans up high ISO sensor pixel noise)
    uint8_t hotPixelMode = ACAMERA_HOT_PIXEL_MODE_HIGH_QUALITY;
    ACaptureRequest_setEntry_u8(m_captureRequest, ACAMERA_HOT_PIXEL_MODE, 1, &hotPixelMode);

    // hardware lens distortion correction, doesnt work with wide-angle camera 2 anw
    uint8_t distortionMode = ACAMERA_DISTORTION_CORRECTION_MODE_HIGH_QUALITY;
    ACaptureRequest_setEntry_u8(m_captureRequest, ACAMERA_DISTORTION_CORRECTION_MODE, 1, &distortionMode);

    // Capture session callbacks
    m_sessionCallbacks.context = this;
    m_sessionCallbacks.onActive = [](void *ctx, ACameraCaptureSession *ses) {
    LOGI("Camera streaming session active.");
    };
    m_sessionCallbacks.onReady = [](void *ctx, ACameraCaptureSession *ses) {};
    m_sessionCallbacks.onClosed = [](void *ctx, ACameraCaptureSession *ses) {};

    ACameraDevice_createCaptureSession(m_cameraDevice, container,
                                        &m_sessionCallbacks, &m_captureSession);

    // Config session to capture frames continuously
    ACameraCaptureSession_setRepeatingRequest(m_captureSession, nullptr, 1,
                                            &m_captureRequest, nullptr);
    if (container) {
    ACaptureSessionOutputContainer_free(container);
    }
    if (sessionOutput) {
    ACaptureSessionOutput_free(sessionOutput);
    }
    if (outputTarget) {
    ACameraOutputTarget_free(outputTarget);
    }
}

void Camera::stop_stream() {
    if (m_captureSession) {
    ACameraCaptureSession_stopRepeating(m_captureSession);
    ACameraCaptureSession_close(m_captureSession);
    m_captureSession = nullptr;
    }
    if (m_captureRequest) {
    ACaptureRequest_free(m_captureRequest);
    m_captureRequest = nullptr;
    }
    if (m_cameraDevice) {
    ACameraDevice_close(m_cameraDevice);
    m_cameraDevice = nullptr;
    }
    if (m_imageReader) {
    AImageReader_setImageListener(m_imageReader, nullptr);
    AImageReader_delete(m_imageReader);
    m_imageReader = nullptr;
    }
    if (m_cameraManager) {
    if (m_cameraIdList) {
        ACameraManager_deleteCameraIdList(m_cameraIdList);
        m_cameraIdList = nullptr;
    }
    ACameraManager_delete(m_cameraManager);
    m_cameraManager = nullptr;
    }

    LOGI("Camera stopped.");
}

void Camera::onImageAvailable(void *context, AImageReader *reader) {
    /*
    Creata Frame object out of raw YUV memory buffer, make FramePtr, then call
    update_frame to update latest_frame
    */
    Camera *instance = static_cast<Camera *>(context);

    AImage *image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) == AMEDIA_OK) {
    int plane_count = 0;
    if ((AImage_getNumberOfPlanes(image, &plane_count) != AMEDIA_OK) ||
        plane_count <= 0) {
        AImage_delete(image);
        return;
    }

    std::vector<RawYUVPlaneInput> input_planes;
    input_planes.reserve(static_cast<size_t>(plane_count)); // hopefully 3

    for (int i = 0; i < plane_count; ++i) {
        uint8_t *plane_data = nullptr;
        int plane_size = 0;
        int row_stride = 0;
        int pixel_stride = 0;

        if (AImage_getPlaneData(image, i, &plane_data, &plane_size) !=
            AMEDIA_OK) {
        continue;
        }

        AImage_getPlaneRowStride(image, i, &row_stride);
        AImage_getPlanePixelStride(image, i, &pixel_stride);

        input_planes.push_back(RawYUVPlaneInput{
            plane_data,
            static_cast<size_t>(plane_size),
            row_stride,
            pixel_stride,
        });
    }

    int format = 0;
    int width = 0;
    int height = 0;
    int64_t timestamp_ns = 0;

    AImage_getFormat(image, &format);
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    AImage_getTimestamp(image, &timestamp_ns);

    uint64_t boottime_ms = static_cast<uint64_t>(timestamp_ns / 1000000ULL);
    int64_t relative_timestamp_ms = (instance->m_base_boottime_ms > 0 && boottime_ms >= instance->m_base_boottime_ms)
        ? static_cast<int64_t>(boottime_ms - instance->m_base_boottime_ms)
        : 0;

    auto new_frame = Frame::from_android_image(format, width, height, 0,
                                                relative_timestamp_ms, input_planes);

    if (instance->m_distortion_correction && instance->m_undistort_ready && new_frame) {
        auto mutable_frame = std::const_pointer_cast<Frame>(new_frame);
        if (mutable_frame->plane_count >= 3 && !mutable_frame->planes[0].data.empty()) {
            // Remap Y Plane (Full resolution)
            cv::Mat src_y(height, width, CV_8UC1, mutable_frame->planes[0].data.data());
            cv::remap(src_y, instance->m_dst_y, instance->m_map1_y, instance->m_map2_y, cv::INTER_LINEAR);
            std::memcpy(mutable_frame->planes[0].data.data(), instance->m_dst_y.data, static_cast<size_t>(width * height));

            // Remap U and V Planes (Half resolution)
            int uv_w = width / 2;
            int uv_h = height / 2;
            size_t uv_size = static_cast<size_t>(uv_w * uv_h);

            cv::Mat src_u(uv_h, uv_w, CV_8UC1, mutable_frame->planes[1].data.data());
            cv::remap(src_u, instance->m_dst_u, instance->m_map1_uv, instance->m_map2_uv, cv::INTER_LINEAR);
            std::memcpy(mutable_frame->planes[1].data.data(), instance->m_dst_u.data, uv_size);

            cv::Mat src_v(uv_h, uv_w, CV_8UC1, mutable_frame->planes[2].data.data());
            cv::remap(src_v, instance->m_dst_v, instance->m_map1_uv, instance->m_map2_uv, cv::INTER_LINEAR);
            std::memcpy(mutable_frame->planes[2].data.data(), instance->m_dst_v.data, uv_size);
        }
    }

    if (instance->m_frameBuffer != nullptr) {
        instance->m_frameBuffer->update_frame(std::move(new_frame));
    }

    AImage_delete(image);
    }
}