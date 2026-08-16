#ifndef TELEMETRY
#define TELEMETRY

#include "Location2D.hpp"

struct Telemetry{
    bool manual_mode_state{false};
    bool record_data_state{false};
    Location location;
};

#endif