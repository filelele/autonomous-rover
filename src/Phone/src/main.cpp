#include <native_app_glue/android_native_app_glue.h> //ignore intellisense complaint
#include <android/log.h>
#include <android/window.h>
#include "FrameBuffer.hpp"
#include "Location2D.hpp"
#include "Telemetry.hpp"
#include "Camera.hpp"
#include "PhoneServerCommunication.hpp"
#include "Logger.hpp"
#include "ScreenRenderer.hpp"

#define TAG "Main"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

struct AppContext{
    Camera* camera;
    PhoneServerCommunication* phone_server_communication;
    Logger* logger;
    ScreenRenderer* renderer;
};

void handle_android_cmd(struct android_app* app, int32_t cmd) {
    AppContext* app_context = static_cast<AppContext*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("Android gave us a screen surface! We can render here.");
            // Keep screen on
            ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
            if (app_context && app_context->renderer) {
                app_context->renderer->setWindow(app->window);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("Screen surface destroyed or minimized. Stop processing.");
            if (app_context && app_context->renderer) {
                app_context->renderer->setWindow(nullptr);
            }
            break;
        case APP_CMD_DESTROY:
            LOGI("App is shutting down entirely.");
            break;
    }
}

int32_t handle_android_input(struct android_app* app, AInputEvent* event) {
    AppContext* app_context = static_cast<AppContext*>(app->userData);
    if (app_context && app_context->renderer) {
        if (app_context->renderer->handleInputEvent(event)) {
            return 1;
        }
    }
    return 0;
}

void android_main(struct android_app* state) {
    LOGI("Phone app started");

    //Shared structures
    FrameBuffer frame_buffer;
    Location location;
    bool manual_mode = false;
    bool record_data = false;
    bool capture_mode = false;
    Telemetry telemetry;

    //Nodes
    Camera camera(&frame_buffer, 640, 480, 0.4f, 1.0f/50.0f, 1600, 0.6f, 200); 
    //Shutter speed 1/100 match my light flickering cycle, avoid rolling shutter horizontal bars
    camera.init_camera();
    camera.start_stream(30);

    PhoneServerCommunication phone_server_communication(frame_buffer, location, manual_mode, record_data, capture_mode, telemetry);
    phone_server_communication.initialize(8888, 8889);
    phone_server_communication.startCommunication();

    Logger logger(frame_buffer, record_data, camera.getBaseEpochMs());
    logger.startLogging();

    ScreenRenderer screen_renderer(frame_buffer, capture_mode);
    if (state->window != nullptr) {
        screen_renderer.setWindow(state->window);
    }
    screen_renderer.start();

    phone_server_communication.setOnCaptureTrigger([&screen_renderer]() {
        screen_renderer.triggerCapture();
    });

    AppContext app_context = {&camera, &phone_server_communication, &logger, &screen_renderer};
    state->onAppCmd = handle_android_cmd;
    state->onInputEvent = handle_android_input;
    state->userData = &app_context;

    while (true) {
        int ident;
        int events;
        struct android_poll_source* source;

        while ((ident = ALooper_pollOnce(0, nullptr, &events, (void**)&source)) >= 0) {
            if (source != nullptr) {
                source->process(state, source);
            }
            if (state->destroyRequested != 0) {
                LOGI("Exiting C++ loop.");
                screen_renderer.stop();
                logger.stopLogging();
                phone_server_communication.stopCommunication();
                return;
            }
        }
    }
}
