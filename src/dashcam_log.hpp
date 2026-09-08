#pragma once

#include <sstream>
#include <string>
#include <utility>

void DashcamLogLine(const std::string& line);

template <typename... Args>
void DashcamLog(Args&&... args) {
    std::ostringstream line;
    (line << ... << args);
    DashcamLogLine(line.str());
}
