#include <thread>
#include <native_app_glue/android_native_app_glue.h>
#include <android/log.h> //logd daemon to print log to, cause Android app has no terminal to print to
#include "FrameBuffer.hpp"
#include "MyCamera.hpp"
#include "DashboardPublisher.hpp"

#define TAG "Main"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__))

struct AppContext{
    Camera* camera;
    DashboardPublisher* dashboard_publisher;
};

// how this c++ thread handles lifecycle events of main app 
void handle_android_cmd(struct android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: //App get physical screen to render
            LOGI("Android gave us a screen surface! We can render here.");
            // Initialize camera stream and render to screen here.
            break;
        case APP_CMD_TERM_WINDOW: //Windows minimized
            LOGI("Screen surface destroyed or minimized. Stop processing.");
            // everything else works, but no state->windows anymore, so stop rendering camera stream to screen...
            break;
        case APP_CMD_DESTROY:
            LOGI("App is shutting down entirely.");
            //Clean up resources, threads, etc...
            AppContext* app_context = static_cast<AppContext*>(app->userData);
            
            app_context->camera->~Camera();
            app_context->dashboard_publisher->~DashboardPublisher();
            break;
    }
}



void android_main(struct android_app* state) { //struct android_app* state contains state of this app.
    LOGI("Phone app started");

    FrameBuffer frame_buffer;
    Camera camera(&frame_buffer);
    camera.init_camera();
    camera.start_stream();
    DashboardPublisher dashboard_publisher(&frame_buffer);

    AppContext app_context = {&camera, &dashboard_publisher};

    state->onAppCmd = handle_android_cmd; //set app lifecycle events callback 
    state->userData = &app_context;

    while (true) {
        int ident;
        int events;
        struct android_poll_source* source;

        //non-blocking events handlers callback
        while ((ident = ALooper_pollOnce(0, nullptr, &events, (void**)&source)) >= 0) {
            
            // Process internal Android OS events (screen flips, touch, pausing)
            if (source != nullptr) {
                source->process(state, source); //run state->onAppCmd internally for example
            }

            // state->onAppCmd(state, APP_CMD_DESTROY) only clean resources, cannot stop this c++ thread.
            // So use this for flow control, to stop this c++ thread.
            if (state->destroyRequested != 0) {
                LOGI("Exiting C++ loop.");
                return;
            }
        }

    }
}