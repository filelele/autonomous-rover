#ifndef ROBOT_STATE
#define ROBOT_STATE

#include "Location2D.hpp"

class RobotState{
private:
    Location pose ;
    Location future_trajectory[10] = {}; 
    Location past_trajectory[10] = {};
    

public:
    RobotState(Location initial_pose = {0.0f, 0.0f, 0.0f})
        : pose(initial_pose) {
    }

};

#endif