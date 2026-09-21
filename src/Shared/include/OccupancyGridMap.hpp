#ifndef OCCUPANCY_GRID_MAP_HPP
#define OCCUPANCY_GRID_MAP_HPP

#include <opencv2/opencv.hpp>

struct OccupancyGridMap {
    cv::Mat image;              // 8-bit grayscale occupancy grid
    float resolution = 0.05f;   // meters/unit per pixel cell
    float origin_x = 0.0f;      // world X corresponding to col 0
    float origin_z = 0.0f;      // world Z corresponding to row (rows - 1)
    bool loaded = false;
};

#endif
