#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "interlog.hpp"

int TCMallocDebug::level;

void TCMalloc_MESSAGE(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char buf[800];
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  write(STDERR_FILENO, buf, strlen(buf));
}

void TCMalloc_Printer::printf(const char* format, ...) {
  if (left_ > 0) {
    va_list ap;
    va_start(ap, format);
    const int r = vsnprintf(buf_, left_, format, ap);
    va_end(ap);
    if (r < 0) {
      left_ = 0;
    } else if (r > left_) {
      left_ = 0;
    } else {
      left_ -= r;
      buf_ += r;
    }
  }
}
