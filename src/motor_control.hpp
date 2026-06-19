#ifndef MOTOR_CONTROL_HPP
#define MOTOR_CONTROL_HPP

#include <fstream>                                                               
#include <iostream>                                                              
#include <thread>                                                                
#include <chrono> 
#include <pigpio.h>

const int PERIOD_FORWARD = 50000; //hardware PWM, nanoseconds
const int FREQ_REVERSE = 8000; //software PWM through GPIO, Hz

struct MotorsSpeed{
    float left_forward;
    float right_forward;
    float left_reverse;
    float right_reverse;
};

class MotorsController {                                                     
private:

    std::ofstream left_forward_pwm_duty_cycle;                                                          
    std::ofstream right_forward_pwm_duty_cycle;
    int left_reverse_gpio = 5;
    int right_reverse_gpio = 6;

public:
MotorsController();

void setLeftSpeed(float forward_frac, float reverse_frac);
void setRightSpeed(float forward_frac, float reverse_frac);

void closeMotors();

};                                                                               

#endif
