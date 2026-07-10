#ifndef _THLIST_H
#define _THLIST_H

#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ListAllProcessThreadsCallBack)(void *parameter,
                                             int num_threads,
                                             pid_t *thread_pids,
                                             va_list ap);

int ListAllProcessThreads(void *parameter,
                          ListAllProcessThreadsCallBack callback, ...);
int ResumeAllProcessThreads(int num_threads, pid_t *thread_pids);

#ifdef __cplusplus
}
#endif
#endif  