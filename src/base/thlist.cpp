#include <sys/prctl.h>
#include "lthreads.hpp"

#ifndef THREADS


int ListAllProcessThreads(void *parameter,
                          ListAllProcessThreadsCallBack callback, ...) {
  int rc;
  va_list ap;

  int dumpable = prctl(PR_GET_DUMPABLE, 0);
  if (!dumpable)
    prctl(PR_SET_DUMPABLE, 1);
  va_start(ap, callback);
  pid_t pid = getpid();
  rc = callback(parameter, 1, &pid, ap);
  va_end(ap);
  if (!dumpable)
    prctl(PR_SET_DUMPABLE, 0);
  return rc;
}

int ResumeAllProcessThreads(int num_threads, pid_t *thread_pids) {
  return 1;
}
#endif
