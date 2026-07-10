#ifndef _FERP_STACKTRACE_H
#define _FERP_STACKTRACE_H

extern int GetStackTrace(void** result, int max_depth, int skip_count);
extern bool GetStackExtent(void* sp,
                           void** stack_top,
                           void** stack_bottom);
#endif 