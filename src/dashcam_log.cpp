#include "dashcam_log.hpp"

#include <iostream>
#include <mutex>

void DashcamLogLine(const std::string& line) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::cout << line << std::endl;
}
