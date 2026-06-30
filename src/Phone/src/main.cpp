#include <thread>
#include <native_app_glue/android_native_app_glue.h>
#include <android/log.h>
#include <android/window.h>
#include "FrameBuffer.hpp"
#include "MyCamera.hpp"
#include "DashboardPublisher.hpp"

#define TAG "Main"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

struct AppContext{
    Camera* camera;
    DashboardPublisher* dashboard_publisher;
};

void handle_android_cmd(struct android_app* app, int32_t cmd) {
    AppContext* app_context = static_cast<AppContext*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("Android gave us a screen surface! We can render here.");
            // Keep screen on
            ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("Screen surface destroyed or minimized. Stop processing.");
            break;
        case APP_CMD_DESTROY:
            LOGI("App is shutting down entirely.");
            break;
    }
}

void android_main(struct android_app* state) {
    LOGI("Phone app started");

    FrameBuffer frame_buffer;
    Camera camera(&frame_buffer);
    camera.init_camera();
    camera.start_stream();
    DashboardPublisher dashboard_publisher(&frame_buffer);

    AppContext app_context = {&camera, &dashboard_publisher};
    state->onAppCmd = handle_android_cmd;
    state->userData = &app_context;

    //std::thread signaling_thread([&dashboard_publisher]() {
    dashboard_publisher.initialize(8888);
    //});

    dashboard_publisher.startPublishing();

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
                dashboard_publisher.stopPublishing();
                //if (signaling_thread.joinable()) signaling_thread.join();
                return;
            }
        }
    }
}
