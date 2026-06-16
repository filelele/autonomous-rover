#ifndef MOTOR_CONTROL_HPP
#define MOTOR_CONTROL_HPP

#include <fstream>                                                               
#include <iostream>                                                              
#include <thread>                                                                
#include <chrono>                                                                                                                                                 
const int PERIOD = 50000;
struct DutyCycleController {                                                     
    std::ofstream left;                                                          
    std::ofstream right;                                                         
};                                                                               
                                                                                  
DutyCycleController initialize_motors();
void close_motors(DutyCycleController& controller);
#endif
