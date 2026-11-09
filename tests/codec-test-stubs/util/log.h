#pragma once
#include <cassert>
#include <limits>
#include <string>
#define LOG_WARN(...) ((void)0)
inline std::string log_hex(unsigned value) { return std::to_string(value); }
