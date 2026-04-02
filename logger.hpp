#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace espiral {
class Logger {
public:
  Logger(std::string name) : name_(name) {}
  ~Logger() {}
  void set_verbose(bool verbose) { verbose_ = verbose; }
  void log(const char *format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (verbose_) {
      // 1. Print the logger's name prefix first
      printf("[%s] ", name_.c_str());

      // 2. Behave like printf for the rest of the arguments
      va_list args;
      va_start(args, format);
      vprintf(format, args);
      va_end(args);
      printf("\n");
    }
  }

  void print(const char *format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    printf("[%s] ", name_.c_str());
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }

  void println(const char *format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    printf("[%s] ", name_.c_str());
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
  }

private:
  std::mutex mutex_;
  bool verbose_ = false;
  std::string name_;
};
} // namespace espiral