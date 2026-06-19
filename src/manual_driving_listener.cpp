#include "motor_control.hpp"
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

const float MAX_DUTY_CYCLE_FRACTION = 0.5f;
const float ASYMMETRIC_MOTORS_CALIBRATION = 0.9895f;

MotorsSpeed HighLevelSpeed_to_MotorSpeed(float heading_speed, float angle_speed){

	heading_speed = std::max(-1.0f, std::min(1.0f, heading_speed));
	angle_speed = std::max(-1.0f, std::min(1.0f, angle_speed));

	float left_forward = 0.0;
	float right_forward = 0.0;
	float left_reverse = 0.0;
	float right_reverse = 0.0;		

	if (heading_speed >= 0.0f) {
        // Forward
        left_mixed  = heading_speed + angle_speed;
        right_mixed = heading_speed - angle_speed;
    } else {
        // Reverse
        left_mixed  = heading_speed - angle_speed;
        right_mixed = heading_speed + angle_speed;
    }

	//Normalize to -1 -> 1
    float max_val = std::max(std::abs(left_mixed), std::abs(right_mixed));
    if (max_val > 1.0f) {
        left_mixed /= max_val;
        right_mixed /= max_val;
    }

    if (left_mixed >= 0.0f) {
        left_forward = left_mixed;
    } else {
        left_reverse = std::abs(left_mixed); // Make it a positive fraction [0.0, 1.0]
    }

    if (right_mixed >= 0.0f) {
        right_forward = right_mixed;
    } else {
        right_reverse = std::abs(right_mixed); // Make it a positive fraction [0.0, 1.0]
    }

    MotorsSpeed output;
    output.left_forward  = left_forward;
    output.left_reverse  = left_reverse;
    output.right_forward = right_forward;
    output.right_reverse = right_reverse;
}

class UDPServer {
private:
    int sockfd;
public:
    UDPServer(int port) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        
        // Cấu hình socket Non-blocking (Không làm đứng vòng lặp)
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in servaddr{};
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = INADDR_ANY;
        servaddr.sin_port = htons(port);

        bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));
    }

    bool receive_target(float &heading, float &angle) {
        char buffer[64];
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, nullptr, nullptr);
        if (n > 0) {
            buffer[n] = '\0';
            sscanf(buffer, "%f,%f", &heading, &angle);
            return true;
        }
        return false;
    }

    ~UDPServer() { close(sockfd); }
};

int main(){
	MotorsController motors_controller;
	MotorsSpeed motors_speed{0.0, 0.0, 0.0, 0.0};

	float current_heading = 0.0f;
	float current_angle = 0.0f;

	float target_heading = 0.0f;
	float target_angle = 0.0f;

	const float TARGET_FORWARD_SPEED = 0.8f;
	const float TARGET_ANGLE_SPEED = 0.6f;

	const float ALPHA_HEADING = 0.1f;
	const float ALPHA_ANGLE = 0.15f;

	auto last_packet_time = std::chrono::steady_clock::now();
	const std::chrono::milliseconds NETWORK_TIMEOUT(500);
	
	//Keyboard keyboard;
	//keyboard.init_non_blocking();
	UDPServer udp(12345);
	
	while(true){
		float recv_heading = 0, recv_angle = 0;

        if (udp.receive_target(recv_heading, recv_angle)) {
            target_heading = recv_heading;
            target_angle = recv_angle;
            last_packet_time = std::chrono::steady_clock::now();
        }

		if (std::chrono::steady_clock::now() - last_keypress_time > NETWORK_TIMEOUT){
		            target_heading = 0.0f;
		            target_angle = 0.0f;
		}
		
		current_heading = current_heading + ALPHA_HEADING * (target_heading - current_heading);
		current_angle = current_angle + ALPHA_ANGLE * (target_angle - current_angle);

		if (std::abs(current_heading) < 0.01f) current_heading = 0.0f;
		if (std::abs(current_angle) < 0.01f) current_angle = 0.0f;
		
		motors_speed = HighLevelSpeed_to_MotorSpeed(current_heading, current_angle);
		motors_controller.setLeftSpeed(motors_speed.left_forward, motors_speed.left_reverse);
		motors_controller.setRightSpeed(motors_speed.right_forward, motors_speed.right_reverse);

		std::this_thread::sleep_for(std::chrono::milliseconds(30)); //30ms
	}
	motors_controller.closeMotors();	
	return 0;
}
