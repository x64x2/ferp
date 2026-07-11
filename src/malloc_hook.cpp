#include "ferp/mallochk.hpp"

MallocHk::NewHook    MallocHk::new_hook_ = NULL;
MallocHk::DeleteHook MallocHk::delete_hook_ = NULL;
MallocHk::MmapHook   MallocHk::mmap_hook_ = NULL;
MallocHk::MunmapHook MallocHk::munmap_hook_ = NULL;

#if defined(__linux) && (defined(__i386__) || defined(__x86_64__))
#include <unistd.h>
#include <syscall.h>
#include <sys/mman.h>

# if defined(__i386__) 

extern "C" void* mmap64(void *start, size_t length,
                        int prot, int flags, 
                        int fd, __off64_t offset) __THROW {

  void *result;
  static bool have_mmap2 = true;
  if (have_mmap2) {
    static int pagesize = 0;
    if (!pagesize) pagesize = getpagesize();

    if (offset & (pagesize - 1)) {
      result = MAP_FAILED;
      errno = EINVAL;
      goto out;
    }

    result = (void *)syscall(SYS_mmap2, 
                             start, length, prot, flags, fd, offset / pagesize);
    if (result != MAP_FAILED || errno != ENOSYS)  goto out;
    have_mmap2 = false;
  }

  if (((off_t)offset) != offset) {
    result = MAP_FAILED;
    errno = EINVAL;
    goto out;
  }

  {
    int32 args[6] = { (int32) start, length, prot, flags, fd, (off_t) offset };
    result = (void *)syscall(SYS_mmap, args);
  }
 out:
  MallocHk::InvokeMmapHook(result, start, length, prot, flags, fd, offset);
  return result;

}

# elif defined(__x86_64__)

extern "C" void* mmap64(void *start, size_t length,
                        int prot, int flags, 
                        int fd, __off64_t offset) __THROW {

  void *result;
  result = (void *)syscall(SYS_mmap, start, length, prot, flags, fd, offset);
  MallocHk::InvokeMmapHook(result, start, length, prot, flags, fd, offset);
  return result;
}

# endif

extern "C" void* mmap(void *start, size_t length,
                      int prot, int flags, 
                      int fd, off_t offset) __THROW {
  return mmap64(start, length, prot, flags, fd, 
                static_cast<size_t>(offset)); 
}

extern "C" int munmap(void* start, size_t length) __THROW {
  MallocHk::InvokeMunmapHook(start, length);
  return syscall(SYS_munmap, start, length);
}

#endif
