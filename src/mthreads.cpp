#include <assert.h>
#include <pthread.h>
#include "mthreads.hpp"

#define MAX_PERTHREAD_VALS 16
static void *perftools_pthread_specific_vals[MAX_PERTHREAD_VALS];
static pthread_key_t next_key;

int perftools_pthread_key_create(pthread_key_t *key,  
                                 void (*destr_function) (void *)) {
  if (&pthread_key_create) {
    return pthread_key_create(key, destr_function);
  } else {
    assert(next_key < MAX_PERTHREAD_VALS);
    *key = next_key++;
    return 0;
  }
}

void *perftools_pthread_getspecific(pthread_key_t key) { 
  if (&pthread_getspecific) {
    return pthread_getspecific(key);
  } else {
    return perftools_pthread_specific_vals[key];
  }
}

int perftools_pthread_setspecific(pthread_key_t key, void *val) {
  if (&pthread_setspecific) {
    return pthread_setspecific(key, val);
  } else {
    perftools_pthread_specific_vals[key] = val;
    return 0;
  }
}

int perftools_pthread_once(pthread_once_t *ctl,  
                          void  (*init_routine) (void)) {
  if (&pthread_once) {
    return pthread_once(ctl, init_routine);
  } else {
    if (*ctl == PTHREAD_ONCE_INIT) {
      init_routine();
      *ctl = 1;
    }
    return 0;
  }
}
