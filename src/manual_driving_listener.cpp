#include "motor_control.hpp"
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <thread>

struct DutyCycleController duty_cycle_controller = initialize_motors();

const float MAX_DUTY_CYCLE_FRACTION = 0.5f;
const float ASYMMETRIC_MOTORS_CALIBRATION = 0.9895f;

void speed_to_duty_cycle(DutyCycleController& duty_cycle_controller, 
						float heading_speed,
					    float angle_speed){
	heading_speed = std::max(0.0f, std::min(1.0f, heading_speed));
	angle_speed = std::max(-1.0f, std::min(1.0f, angle_speed));

		
	float left_mul = heading_speed + angle_speed; //-1,1
	float right_mul = heading_speed - angle_speed;

	

	if(left_mul < 0){
		right_mul = std::min(1.0f, right_mul - left_mul);
		left_mul = 0;
	}

	if(right_mul < 0){
		left_mul = std::min(1.0f, left_mul - right_mul);
		right_mul = 0;
	}

	float norm = std::max(left_mul, right_mul);
	if(norm > 1){
		left_mul /= norm;
		right_mul /= norm;
	}
	
	duty_cycle_controller.left << static_cast<long>(left_mul*MAX_DUTY_CYCLE_FRACTION*PERIOD) << std::flush;
	duty_cycle_controller.right << static_cast<long>(right_mul*MAX_DUTY_CYCLE_FRACTION*PERIOD) << std::flush;
}

class Keyboard{

private:
	struct termios orig_termios;
public:
	Keyboard(){
		tcgetattr(STDIN_FILENO, &orig_termios);
				
	}

	void init_non_blocking(){
		struct termios raw_termios = orig_termios;
		raw_termios.c_lflag &= ~(ICANON | ECHO);
		raw_termios.c_cc[VMIN] = 0;
		raw_termios.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios);
	}

	//check if there is anything in keyboard
	bool is_there_keypress(){
		struct timeval tv = {0L, 0L};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;		
	}
	
	~Keyboard(){
		restore_terminal();	
	}

	void restore_terminal(){
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
	}

	char get_latest_key() {
        char latest = 0;
        char temp;
        // Đọc liên tục cho đến khi hàm read trả về <= 0 (hết buffer)
        while (read(STDIN_FILENO, &temp, 1) > 0) {
            latest = temp;
        }
        return latest;
    }
};

int main(){
	float current_heading = 0.0f;
	float current_angle = 0.0f;

	float target_heading = 0.0f;
	float target_angle = 0.0f;

	const float TARGET_FORWARD_SPEED = 0.8f;
	const float TARGET_ANGLE_SPEED = 0.6f;

	const float ALPHA_HEADING = 0.15f;
	const float ALPHA_ANGLE = 0.25f;

	auto last_keypress_time = std::chrono::steady_clock::now();
	const std::chrono::milliseconds KEY_TIMEOUT(600);
	
	Keyboard keyboard;
	keyboard.init_non_blocking();

	while(true){
		if(keyboard.is_there_keypress()){
			char key = keyboard.get_latest_key();
			if(key != 0){
				if(key == 'q') break;
				if (key == 'w' || key == 'a' || key == 's' || key == 'd') {
					last_keypress_time = std::chrono::steady_clock::now();
					switch(key){
	                                case 'w': 
	                                        target_heading = TARGET_FORWARD_SPEED;
	                                        target_angle = 0.0f; 
	                                        break;
	                                case 'a': 
	                                        target_angle = -TARGET_ANGLE_SPEED;
	                                        target_heading = 0;
	                                        break;
	                                case 'd': 
	                                        target_angle = TARGET_ANGLE_SPEED;
	                                        target_heading = 0; 
	                                        break;
	                                case 's': //hard break
	                                        target_heading = 0.0f;
	                                        target_angle = 0.0f;
	                                        break;
	                        	}

				}
			}
		}

		if (std::chrono::steady_clock::now() - last_keypress_time > KEY_TIMEOUT) {
		            target_heading = 0.0f;
		            target_angle = 0.0f;
		}
		
		current_heading = current_heading + ALPHA_HEADING * (target_heading - current_heading);
		current_angle = current_angle + ALPHA_ANGLE * (target_angle - current_angle);

		if (std::abs(current_heading) < 0.01f) current_heading = 0.0f;
		if (std::abs(current_angle) < 0.01f) current_angle = 0.0f;
		
		speed_to_duty_cycle(duty_cycle_controller, current_heading, current_angle);
		std::this_thread::sleep_for(std::chrono::milliseconds(30)); //30ms
	}
	close_motors(duty_cycle_controller);	
	return 0;
}
