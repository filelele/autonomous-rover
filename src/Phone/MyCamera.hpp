#ifndef MY_CAMERA
#define MY_CAMERA

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <media/NdkImageReader.h>
#include <memory>
#include "FrameBuffer.hpp"

class Camera {
public:
    Camera(FrameBuffer* targetBuffer);
    ~Camera();

    bool init_camera();
    void start_stream();
    void stop_stream();

private:
    ACameraManager* m_cameraManager;
    ACameraIdList* m_cameraIdList;
    ACameraDevice* m_cameraDevice;
    ACaptureRequest* m_captureRequest;
    AImageReader* m_imageReader;
    ANativeWindow* m_imageReaderWindow;
    ACameraCaptureSession* m_captureSession;
    
    const char* m_selectedCameraId;
    FrameBuffer* m_frameBuffer;

    // Internal Callback structures, what to do on events of the camera device
    ACameraDevice_StateCallbacks m_deviceCallbacks;
    AImageReader_ImageListener m_imageListener;
    ACameraCaptureSession_stateCallbacks m_sessionCallbacks;

    // Callback function in event of frame arrival on memory.
    static void onImageAvailable(void* context, AImageReader* reader);
};

#endif