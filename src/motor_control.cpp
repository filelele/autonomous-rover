#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

#define PERIOD "50000" //20khz



struct DutyCycleController {
	std::ofstream left;
	std::ofstream right;
};

DutyCycleController initialize_motors(){
	{
		std::ofstream export_left("/sys/class/pwm/pwmchip0/export");
		if(export_left.is_open()){
			export_left << "0" << std::flush;
		}
	}

	{
		std::ofstream export_right("/sys/class/pwm/pwmchip0/export");
		if(export_right.is_open()){
			export_right << "1" << std::flush;
		}
	}

	std::this_thread::sleep_for(std::chrono::miliseconds(100));

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
