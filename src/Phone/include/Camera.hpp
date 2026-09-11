#ifndef MY_CAMERA
#define MY_CAMERA

#include "FrameBuffer.hpp"
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkImageReader.h>

class Camera {
public:
    Camera(FrameBuffer *targetBuffer, 
           int res_width = 640, int res_height = 480, 
           float focus_meters = 0.4,
           float shutter_time_second = 1e-2,
           int iso = 1600);
    ~Camera();

    bool init_camera();
    void start_stream(int fps = 30);
    void stop_stream();

    uint64_t getBaseEpochMs() const { return m_base_epoch_ms; }
    uint64_t getBaseBoottimeMs() const { return m_base_boottime_ms; }

private:
    uint64_t m_base_epoch_ms = 0;
    uint64_t m_base_boottime_ms = 0;

    int m_res_width;
    int m_res_height;
    float m_focus_meters;
    int m_fps;
    float m_shutter_time_second;
    int m_iso;

    ACameraManager *m_cameraManager;
    ACameraIdList *m_cameraIdList;
    ACameraDevice *m_cameraDevice;
    ACaptureRequest *m_captureRequest;
    AImageReader *m_imageReader;
    ANativeWindow *m_imageReaderWindow;
    ACameraCaptureSession *m_captureSession;

    const char *m_selectedCameraId;
    FrameBuffer *m_frameBuffer;

    // Internal Callback structures, what to do on events of the camera device
    ACameraDevice_StateCallbacks m_deviceCallbacks;
    AImageReader_ImageListener m_imageListener;
    ACameraCaptureSession_stateCallbacks m_sessionCallbacks;

    // Callback function in event of frame arrival on memory.
    static void onImageAvailable(void *context, AImageReader *reader);
};

#endif