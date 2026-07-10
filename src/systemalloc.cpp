#include "base/btype.hpp"
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "system-alloc.h"
#include "internl.hpp"
#include "interlog.hpp"
#include "base/cmdlag.hpp"

union MemoryAligner {
  void*  p;
  double d;
  size_t s;
};

static SpinLock spinlock = SPINLOCK_INITIALIZER;
static size_t pagesize = 0;
static bool use_devmem = true;
static bool use_sbrk = true;
static bool use_mmap = true;
static bool devmem_failure = false;
static bool sbrk_failure = false;
static bool mmap_failure = false;

DEFINE_int32(malloc_devmem_start, 0,
             "Physical memory starting location in MB for /dev/mem allocation."
             "  Setting this to 0 disables /dev/mem allocation");
DEFINE_int32(malloc_devmem_limit, 0,
             "Physical memory limit location in MB for /dev/mem allocation."
             "  Setting this to 0 means no limit.");

#ifdef HAVE_SBRK

static void* TrySbrk(size_t size, size_t alignment) {
  if (static_cast<ptrdiff_t>(size + alignment) < 0) return NULL;

  size = ((size + alignment - 1) / alignment) * alignment;
  void* result = sbrk(size);
  if (result == reinterpret_cast<void*>(-1)) {
    sbrk_failure = true;
    return NULL;
  }
  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  if ((ptr & (alignment-1)) == 0)  return result;

  size_t extra = alignment - (ptr & (alignment-1));
  void* r2 = sbrk(extra);
  if (reinterpret_cast<uintptr_t>(r2) == (ptr + size)) {
    return reinterpret_cast<void*>(ptr + extra);
  }

  result = sbrk(size + alignment - 1);
  if (result == reinterpret_cast<void*>(-1)) {
    sbrk_failure = true;
    return NULL;
  }
  ptr = reinterpret_cast<uintptr_t>(result);
  if ((ptr & (alignment-1)) != 0) {
    ptr += alignment - (ptr & (alignment-1));
  }
  return reinterpret_cast<void*>(ptr);
}

#endif 

#ifdef HAVE_MMAP

static void* TryMmap(size_t size, size_t alignment) {
  if (pagesize == 0) pagesize = getpagesize();
  if (alignment < pagesize) alignment = pagesize;
  size = ((size + alignment - 1) / alignment) * alignment;

  // Ask for extra memory if alignment > pagesize
  size_t extra = 0;
  if (alignment > pagesize) {
    extra = alignment - pagesize;
  }

  void* result = mmap(NULL, size + extra,
                      PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS,
                      -1, 0);
  if (result == reinterpret_cast<void*>(MAP_FAILED)) {
    mmap_failure = true;
    return NULL;
  }

  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }

  if (adjust > 0) {
    munmap(reinterpret_cast<void*>(ptr), adjust);
  }
  if (adjust < extra) {
    munmap(reinterpret_cast<void*>(ptr + adjust + size), extra - adjust);
  }

  ptr += adjust;
  return reinterpret_cast<void*>(ptr);
}

#endif 

static void* TryDevMem(size_t size, size_t alignment) {
  static bool initialized = false;
  static off_t physmem_base;  
  static off_t physmem_limit; 
  static int physmem_fd;      
  
  if (FLAGS_malloc_devmem_start == 0) {
    return NULL;
  }
  
  if (!initialized) {
    physmem_fd = open("/dev/mem", O_RDWR);
    if (physmem_fd < 0) {
      devmem_failure = true;
      return NULL;
    }
    physmem_base = FLAGS_malloc_devmem_start*1024LL*1024LL;
    physmem_limit = FLAGS_malloc_devmem_limit*1024LL*1024LL;
    initialized = true;
  }
  
  if (pagesize == 0) pagesize = getpagesize();
  if (alignment < pagesize) alignment = pagesize;
  size = ((size + alignment - 1) / alignment) * alignment;

  size_t extra = 0;
  if (alignment > pagesize) {
    extra = alignment - pagesize;
  }
  
  if (physmem_limit != 0 &&
      ((size + extra) > (physmem_limit - physmem_base))) {
    devmem_failure = true;
    return NULL;
  }

  void *result = mmap(0, size + extra, PROT_WRITE|PROT_READ,
                      MAP_SHARED, physmem_fd, physmem_base);
  if (result == reinterpret_cast<void*>(MAP_FAILED)) {
    devmem_failure = true;
    return NULL;
  }
  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }
  
  if (adjust > 0) {
    munmap(reinterpret_cast<void*>(ptr), adjust);
  }
  if (adjust < extra) {
    munmap(reinterpret_cast<void*>(ptr + adjust + size), extra - adjust);
  }
  
  ptr += adjust;
  physmem_base += adjust + size;
  
  return reinterpret_cast<void*>(ptr);
}

void* TCMalloc_SystemAlloc(size_t size, size_t alignment) {
  if (size + alignment < size) return NULL;

  if (TCMallocDebug::level >= TCMallocDebug::kVerbose) {
    MESSAGE("TCMalloc_SystemAlloc(%"", %" ")\n", 
            size, alignment);
  }
  SpinLockHolder lock_holder(&spinlock);
  if (alignment < sizeof(MemoryAligner)) alignment = sizeof(MemoryAligner);

  for (int i = 0; i < 2; i++) {
    if (use_devmem && !devmem_failure) {
      void* result = TryDevMem(size, alignment);
      if (result != NULL) return result;
    }
    
#ifdef HAVE_SBRK
    if (use_sbrk && !sbrk_failure) {
      void* result = TrySbrk(size, alignment);
      if (result != NULL) return result;
    }
#endif

#ifdef HAVE_MMAP    
    if (use_mmap && !mmap_failure) {
      void* result = TryMmap(size, alignment);
      if (result != NULL) return result;
    }
#endif
    devmem_failure = false;
    sbrk_failure = false;
    mmap_failure = false;
  }
  return NULL;
}
