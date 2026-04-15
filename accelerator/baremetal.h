#pragma once

#include <stdint.h>

uint64_t get_current_time_ns()
{
    return clock() * 1000000;
}

void nanosleep(const struct timespec *req, struct timespec *rem) {
    uint64_t start_time = get_current_time_ns();
    uint64_t requested_ns = req->tv_sec * 1000000000ULL + req->tv_nsec;

    while ((get_current_time_ns() - start_time) < requested_ns) {
        // Busy-waiting
    }

    // If a remaining time structure is provided, set it to zero
    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
}


