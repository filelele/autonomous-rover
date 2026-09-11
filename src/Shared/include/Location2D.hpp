#ifndef LOCATION_2D
#define LOCATION_2D

#include <cstdint>

struct Location{
    float x = 1.0f ;
    float y = 1.0f;
    float heading = 1.0f;
    int64_t timestamp = 1LL;
};

#endif