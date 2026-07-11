#ifndef TCMALLOC_INTERLOG_HPP__
#define TCMALLOC_INTERLOG_HPP__

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

struct TCMallocDebug {
  static int level;
  
  enum { kNone, kInfo, kVerbose };
};

extern void TCMalloc_MESSAGE(const char* format, ...)
#ifdef HAVE___ATTRIBUTE__
  __attribute__ ((__format__ (__printf__, 1, 2)))
#endif
;

#define MESSAGE TCMalloc_MESSAGE

#undef CHECK_CONDITION
#define CHECK_CONDITION(cond)                                            \
do {                                                                     \
  if (!(cond)) {                                                         \
    MESSAGE("%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
    abort();                                                             \
  }                                                                      \
} while (0)

#ifndef NDEBUG
#define ASSERT(cond) CHECK_CONDITION(cond)
#else
#define ASSERT(cond) ((void) 0)
#endif

class TCMalloc_Printer {
 private:
  char* buf_;         
  int   left_;          

 public:
  TCMalloc_Printer(char* buf, int length) : buf_(buf), left_(length) {
    buf[0] = '\0';
  }

  void printf(const char* format, ...)
#ifdef HAVE___ATTRIBUTE__
    __attribute__ ((__format__ (__printf__, 2, 3)))
#endif
;
};

#endif 
