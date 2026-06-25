#include "MyCamera.hpp"
#include "android/log.h"

#include <utility>
#include <vector>

#define TAG "Camera"
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

Camera::Camera(FrameBuffer* targetBuffer) 
    : m_cameraManager(nullptr), m_cameraIdList(nullptr), m_cameraDevice(nullptr),
      m_imageReader(nullptr), m_imageReaderWindow(nullptr), m_captureSession(nullptr),
      m_selectedCameraId(nullptr), m_frameBuffer(targetBuffer) {}

Camera::~Camera() {
    stop_stream();
}

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
    
    // Pick back cam
    m_selectedCameraId = m_cameraIdList->cameraIds[0];

    // Set device callbacks: What to do when camera device has problem
    m_deviceCallbacks.context = this;
    m_deviceCallbacks.onDisconnected = [](void* ctx, ACameraDevice* dev) { LOGI("Camera disconnected."); };
    m_deviceCallbacks.onError = [](void* ctx, ACameraDevice* dev, int err) { LOGE("Camera error: %d", err); };

    camera_status_t status = ACameraManager_openCamera(m_cameraManager, m_selectedCameraId, &m_deviceCallbacks, &m_cameraDevice);
    if (status != ACAMERA_OK) {
        LOGE("Failed to open camera hardware device.");
        return false;
    }

    //characteristics container pointer
    ACameraMetadata* cameraCharacteristics = nullptr;
    ACameraManager_getCameraCharacteristics(m_cameraManager, m_selectedCameraId, &cameraCharacteristics);

    ACameraMetadata_const_entry entry;
    ACameraMetadata_getConstEntry(cameraCharacteristics, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);

    //Loop through all supported camera configs 
    for (size_t i = 0; i < entry.count; i += 4) {
        int32_t format = entry.data.i32[i];
        int32_t width  = entry.data.i32[i + 1];
        int32_t height = entry.data.i32[i + 2];
        int32_t input  = entry.data.i32[i + 3];

        // Only care about configs of 1920x1080, for now
        if (input == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT && 
            format == AIMAGE_FORMAT_YUV_420_888 &&
            width == 1920 && 
            height == 1080) { //YUV_420_888 is the standard for any Google certified device, Gemini said so =))
            
            LOGI("Found supported hardware resolution: %d x %d", width, height);
        }else return false;
    }

    ACameraMetadata_free(cameraCharacteristics);

    // Allocate memory buffer slots for the incoming frame stream (1920x1080 YUV, keeping max 3 frames)
    media_status_t mediaStatus = AImageReader_new(1920, 1080, AIMAGE_FORMAT_YUV_420_888, 3, &m_imageReader);
    if (mediaStatus != AMEDIA_OK) {
        LOGE("Failed to allocate ImageReader.");
        return false;
    }

    m_imageListener.context = this; 
    m_imageListener.onImageAvailable = Camera::onImageAvailable;

    // Register Listener callback to the Image reader container (listen if there is image write to allocated mem)
    AImageReader_setImageListener(m_imageReader, &m_imageListener);

    // Get the imageReaderWindow
    AImageReader_getWindow(m_imageReader, &m_imageReaderWindow);
    
    LOGI("Camera initialization successed.");
    return true;
}

void Camera::start_stream() {
    if (!m_cameraDevice || !m_imageReaderWindow) return;

    // Prepare capture session configuration containers
    ACameraOutputTarget* outputTarget = nullptr;
    ACameraOutputTarget_create(m_imageReaderWindow, &outputTarget);

    ACaptureSessionOutput* sessionOutput = nullptr;
    ACaptureSessionOutput_create(m_imageReaderWindow, &sessionOutput);

    ACaptureSessionOutputContainer* container = nullptr;
    ACaptureSessionOutputContainer_create(&container);
    ACaptureSessionOutputContainer_add(container, sessionOutput);

    // Create a Capture request, a continuous video capture one
    ACameraDevice_createCaptureRequest(m_cameraDevice, TEMPLATE_PREVIEW, &m_captureRequest);
    ACaptureRequest_addTarget(m_captureRequest, outputTarget);

    // Capture session callbacks
    m_sessionCallbacks.context = this;
    m_sessionCallbacks.onActive = [](void* ctx, ACameraCaptureSession* ses) { LOGI("Camera streaming session active."); };
    m_sessionCallbacks.onReady = [](void* ctx, ACameraCaptureSession* ses) {};
    m_sessionCallbacks.onClosed = [](void* ctx, ACameraCaptureSession* ses) {};

    ACameraDevice_createCaptureSession(m_cameraDevice, container, &m_sessionCallbacks, &m_captureSession);
    
    // Config session to capture frames continuously forever
    ACameraCaptureSession_setRepeatingRequest(m_captureSession, nullptr, 1, &m_captureRequest, nullptr);
}

void Camera::stop_stream() {
    if (m_captureSession) {
        ACameraCaptureSession_stopRepeating(m_captureSession);
        ACameraCaptureSession_close(m_captureSession);
        m_captureSession = nullptr;
    }
    if (m_cameraDevice) {
        ACameraDevice_close(m_cameraDevice);
        m_cameraDevice = nullptr;
    }
    if (m_imageReader) {
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

void Camera::onImageAvailable(void* context, AImageReader* reader) {
    /*
    Creata Frame object out of raw YUV memory buffer, make FramePtr, then call update_frame to update latest_frame
    */
    Camera* instance = static_cast<Camera*>(context);
    
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) == AMEDIA_OK) {
        int plane_count = 0;
        if ( (AImage_getNumberOfPlanes(image, &plane_count) != AMEDIA_OK) || plane_count <= 0) {
            AImage_delete(image);
            return;
        }

        std::vector<RawYUVPlaneInput> input_planes;
        input_planes.reserve(static_cast<size_t>(plane_count)); //hopefully 3

        for (int i = 0; i < plane_count; ++i) {
            uint8_t* plane_data = nullptr;
            int plane_size = 0;
            int row_stride = 0;
            int pixel_stride = 0;

            if (AImage_getPlaneData(image, i, &plane_data, &plane_size) != AMEDIA_OK) {
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

        auto new_frame = Frame::from_android_image(
            format,
            width,
            height,
            0,
            timestamp_ns,
            input_planes);

        if (instance->m_frameBuffer != nullptr) {
            instance->m_frameBuffer->update_frame(std::move(new_frame));
        }

        AImage_delete(image); 
    }
}