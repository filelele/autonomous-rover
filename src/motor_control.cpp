#include "motor_control.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

DutyCycleController initialize_motors(){
	{
		std::ofstream pwm_export("/sys/class/pwm/pwmchip0/export");
		if(pwm_export.is_open()){
			pwm_export << "0" << std::flush;
			pwm_export << "1" << std::flush;
		}
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	{
		std::ofstream period_left("/sys/class/pwm/pwmchip0/pwm0/period");
		if(period_left.is_open()){
			period_left << PERIOD << std::flush; 
		}
		
	}
	{
		std::ofstream period_right("/sys/class/pwm/pwmchip0/pwm1/period");
		if(period_right.is_open()){
			period_right << PERIOD << std::flush;
		}
	}

	std::ofstream duty_cycle_left("/sys/class/pwm/pwmchip0/pwm0/duty_cycle");
	std::ofstream duty_cycle_right("/sys/class/pwm/pwmchip0/pwm1/duty_cycle");
	duty_cycle_left << "0" << std::flush;
	duty_cycle_right << "0" << std::flush;

	{ 
        std::ofstream enable_left("/sys/class/pwm/pwmchip0/pwm0/enable");
	std::ofstream enable_right("/sys/class/pwm/pwmchip0/pwm1/enable");
        if(enable_left.is_open() && enable_right.is_open()){
			enable_left << "1" << std::flush;
			enable_right << "1" << std::flush;
		} 
    }

	return DutyCycleController{std::move(duty_cycle_left), std::move(duty_cycle_right)};

}

void close_motors(DutyCycleController& controller) {
    if (controller.left.is_open())  controller.left  << "0" << std::flush;
    if (controller.right.is_open()) controller.right << "0" << std::flush;

    std::ofstream disable_left("/sys/class/pwm/pwmchip0/pwm0/enable");
    std::ofstream disable_right("/sys/class/pwm/pwmchip0/pwm1/enable");
    
    if (disable_left.is_open())  disable_left  << "0" << std::flush;
    if (disable_right.is_open()) disable_right << "0" << std::flush;
    
    std::cout << "[Hardware] Motors safely disabled and stopped." << std::endl;
}		

