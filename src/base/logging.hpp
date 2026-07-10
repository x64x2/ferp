#ifndef _LOGGING_HPP
#define _LOGGING_HPP

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)  \
  do { \
    if (!(condition)) { \
      fprintf(stderr, "Check failed: %s\n", #condition); \
      exit(1); \
    } \
  } while (0) \

#define CHECK_OP(op, val1, val2)  \
  do { \
    if (!((val1) op (val2))) { \
      fprintf(stderr, "Check failed: %s %s %s\n", #val1, #op, #val2); \
      exit(1); \
    } \
  } while (0)

#define CHECK_EQ(val1, val2) CHECK_OP(==, val1, val2)
#define CHECK_NE(val1, val2) CHECK_OP(!=, val1, val2)
#define CHECK_LE(val1, val2) CHECK_OP(<=, val1, val2)
#define CHECK_LT(val1, val2) CHECK_OP(< , val1, val2)
#define CHECK_GE(val1, val2) CHECK_OP(>=, val1, val2)
#define CHECK_GT(val1, val2) CHECK_OP(> , val1, val2)

enum {INFO, WARNING, ERROR, FATAL, NUM_SEVERITIES};

static inline void LogPrintf(int severity, const char* pat, ...) {
  va_list ap;
  va_start(ap, pat);
  vfprintf(stderr, pat, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  if ((severity) == FATAL)
    exit(1);
}

#endif 
