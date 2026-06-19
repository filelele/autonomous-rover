#include "motor_control.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <pigpio.h>
#include <stdexcept>

void MotorsController::setLeftSpeed(float forward_frac, float reverse_frac) {
    forward_frac = std::max(0.0f, std::min(1.0f, forward_frac));
    reverse_frac = std::max(0.0f, std::min(1.0f, reverse_frac));

    if (forward_frac > 0.0f && reverse_frac > 0.0f) {
        forward_frac = 0.0f;
        reverse_frac = 0.0f;
    }

    if (forward_frac > 0.0f) {
        gpioPWM(left_reverse_gpio, 0); 
        left_forward_pwm_duty_cycle << static_cast<long>(PERIOD_FORWARD * forward_frac) << std::flush;
    } 
    else if (reverse_frac > 0.0f) {
        left_forward_pwm_duty_cycle << 0 << std::flush; 
        gpioPWM(left_reverse_gpio, static_cast<int>(reverse_frac * 255));
    } 
    else {
        left_forward_pwm_duty_cycle << 0 << std::flush;
        gpioPWM(left_reverse_gpio, 0);
    }
}

void MotorsController::setRightSpeed(float forward_frac, float reverse_frac) {
    forward_frac = std::max(0.0f, std::min(1.0f, forward_frac));
    reverse_frac = std::max(0.0f, std::min(1.0f, reverse_frac));

    if (forward_frac > 0.0f && reverse_frac > 0.0f) {
        forward_frac = 0.0f;
        reverse_frac = 0.0f;
    }

    if (forward_frac > 0.0f) {
        gpioPWM(right_reverse_gpio, 0); 
        right_forward_pwm_duty_cycle << static_cast<long>(PERIOD_FORWARD * forward_frac) << std::flush;
    } 
    else if (reverse_frac > 0.0f) {
        right_forward_pwm_duty_cycle << 0 << std::flush; 
        gpioPWM(right_reverse_gpio, static_cast<int>(reverse_frac * 255));
    } 
    else {
        right_forward_pwm_duty_cycle << 0 << std::flush;
        gpioPWM(right_reverse_gpio, 0);
    }
}


void MotorsController::MotorController(){
	{
		std::ofstream pwm_export("/sys/class/pwm/pwmchip0/export");
		if(pwm_export.is_open()){
			pwm_export << "0" << std::flush;
			pwm_export << "1" << std::flush;
		}
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	{
		std::ofstream period_forward_left("/sys/class/pwm/pwmchip0/pwm0/period");
		if(period_forward_left.is_open()){
			period_forward_left << PERIOD << std::flush; 
		}
		
	}
	{
		std::ofstream period_forward_right("/sys/class/pwm/pwmchip0/pwm1/period");
		if(period_forward_right.is_open()){
			period_forward_right << PERIOD << std::flush;
		}
	}

	std::ofstream duty_cycle_left_forward("/sys/class/pwm/pwmchip0/pwm0/duty_cycle");
	std::ofstream duty_cycle_right_right_forward("/sys/class/pwm/pwmchip0/pwm1/duty_cycle");
	duty_cycle_left_forward << "0" << std::flush;
	duty_cycle_right_forward << "0" << std::flush;

	left_forward_pwm_duty_cycle = std::move(duty_cycle_left_forward);
	right_forward_pwm_duty_cycle = std::move(duty_cycle_right_forward);

	{ 
        std::ofstream enable_left_forward("/sys/class/pwm/pwmchip0/pwm0/enable");
		std::ofstream enable_right_forward("/sys/class/pwm/pwmchip0/pwm1/enable");
        if(enable_left_forward.is_open() && enable_right_forward.is_open()){
			enable_left_forward << "1" << std::flush;
			enable_right_forward << "1" << std::flush;
		}
    }

	//reverse software pwm
	if (gpioInitialise() < 0) {
        throw std::runtime_error("GPIO Initialization Failed\n");
    }

	gpioSetPWMfrequency(left_reverse_gpio, FREQ_REVERSE) //8khz
	gpioSetPWMfrequency(right_reverse_gpio, FREQ_REVERSE)
}

void MotorsController::closeMotors() {
    if (left_forward_pwm_duty_cycle.is_open())  left_forward_pwm_duty_cycle  << "0" << std::flush;
    if (right_forward_pwm_duty_cycle.right.is_open()) right_forward_pwm_duty_cycle << "0" << std::flush;

    std::ofstream disable_left_forward("/sys/class/pwm/pwmchip0/pwm0/enable");
    std::ofstream disable_right_forward("/sys/class/pwm/pwmchip0/pwm1/enable");
    
    if (disable_left_forward.is_open())  disable_left_forward  << "0" << std::flush;
    if (disable_right_forward.is_open()) disable_right_forward << "0" << std::flush;
    
	gpioPWM(left_reverse_gpio, 0);
	gpioPWM(right_reverse_gpio, 0);
    gpioTerminate();

    std::cout << "[Hardware]: Motors safely disabled and stopped." << std::endl;
}		

