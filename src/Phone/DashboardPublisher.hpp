#ifndef DASHBOARD_PUBLISHER
#define DASHBOARD_PUBLISHER

#include <thread>
#include "FrameBuffer.hpp"
#include "RobotState.hpp"

class DashboardPublisher{

public:
DashboardPublisher(FrameBuffer* frame_buffer);
~DashboardPublisher() = default;

void startPublishing();
void stopPublishing();

private:
    FrameBuffer* frame_buffer;
    RobotState* robot_state;
};

#endif