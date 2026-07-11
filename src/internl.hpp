#ifndef TCMALLOC_INTERNL_HPP__
#define TCMALLOC_INTERNL_HPP__

#include <time.h>       
#include <sched.h>      
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <stdlib.h>	

#if (defined __i386__ || defined __x86_64__) && defined __GNUC__

static void TCMalloc_SlowLock(volatile unsigned int* lockword);
struct TCMalloc_SpinLock {
  volatile unsigned int private_lockword_;

  inline void Init() { private_lockword_ = 0; }
  inline void Finalize() { }
    
  inline void Lock() {
    int r;
    __asm__ __volatile__
      ("xchgl %0, %1"
       : "=r"(r), "=m"(private_lockword_)
       : "0"(1), "m"(private_lockword_)
       : "memory");
    if (r) TCMalloc_SlowLock(&private_lockword_);
  }

  inline void Unlock() {
    __asm__ __volatile__
      ("movl $0, %0"
       : "=m"(private_lockword_)
       : "m" (private_lockword_)
       : "memory");
  }
};

#define SPINLOCK_INITIALIZER { 0 }

static void TCMalloc_SlowLock(volatile unsigned int* lockword) {
  sched_yield();       
  while (true) {
    int r;
    __asm__ __volatile__
      ("xchgl %0, %1"
       : "=r"(r), "=m"(*lockword)
       : "0"(1), "m"(*lockword)
       : "memory");
    if (!r) {
      return;
    }

    struct timespec tm;
    tm.tv_sec = 0;
    tm.tv_nsec = 2000001;
    nanosleep(&tm, NULL);
  }
}

#else

#include <pthread.h>

struct TCMalloc_SpinLock {
  pthread_mutex_t private_lock_;

  inline void Init() {
    if (pthread_mutex_init(&private_lock_, NULL) != 0) abort();
  }
  inline void Finalize() {
    if (pthread_mutex_destroy(&private_lock_) != 0) abort();
  }
  inline void Lock() {
    if (pthread_mutex_lock(&private_lock_) != 0) abort();
  }
  inline void Unlock() {
    if (pthread_mutex_unlock(&private_lock_) != 0) abort();
  }
};

#define SPINLOCK_INITIALIZER { PTHREAD_MUTEX_INITIALIZER }

#endif

class TCMalloc_SpinLockHolder {
 private:
  TCMalloc_SpinLock* lock_;
 public:
  inline explicit TCMalloc_SpinLockHolder(TCMalloc_SpinLock* l)
    : lock_(l) { l->Lock(); }
  inline ~TCMalloc_SpinLockHolder() { lock_->Unlock(); }
};

typedef TCMalloc_SpinLock SpinLock;
typedef TCMalloc_SpinLockHolder SpinLockHolder;

#endif  