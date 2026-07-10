#ifndef _FERP_MALLOCHK_HPP
#define _FERP_MALLOCHK_HPP

#include <stddef.h>
#include <sys/types.h>

class MallocHk {
 public:
  
  typedef void (*NewHook)(void* ptr, size_t size);
  inline static NewHook GetNewHook() { return new_hook_; }
  inline static NewHook SetNewHook(NewHook hook) {
    NewHook result = new_hook_;
    new_hook_ = hook;
    return result;
  }
  inline static void InvokeNewHook(void* p, size_t s) {
    if (new_hook_ != NULL) (*new_hook_)(p, s);
  }
  typedef void (*DeleteHook)(void* ptr);
  inline static DeleteHook GetDeleteHook() { return delete_hook_; }
  inline static DeleteHook SetDeleteHook(DeleteHook hook) {
    DeleteHook result = delete_hook_;
    delete_hook_ = hook;
    return result;
  }
  inline static void InvokeDeleteHook(void* p) {
    if (delete_hook_ != NULL) (*delete_hook_)(p);
  }

  typedef void (*MmapHook)(void* result, 
                           void* start,
                           size_t size,
                           int protection,
                           int flags,
                           int fd,
                           off_t offset);
  inline static MmapHook GetMmapHook() { return mmap_hook_; }
  inline static MmapHook SetMmapHook(MmapHook hook) {
    MmapHook result = mmap_hook_;
    mmap_hook_ = hook;
    return result;
  }
  inline static void InvokeMmapHook(void* result,
                                    void* start,
                                    size_t size,
                                    int protection,
                                    int flags,
                                    int fd,
                                    off_t offset) {
    if (mmap_hook_ != NULL) (*mmap_hook_)(result,
                                          start, size,
                                          protection, flags,
                                          fd, offset);
  }
  typedef void (*MunmapHook)(void* ptr, size_t size);
  inline static MunmapHook GetMunmapHook() { return munmap_hook_; }
  inline static MunmapHook SetMunmapHook(MunmapHook hook) {
    MunmapHook result = munmap_hook_;
    munmap_hook_ = hook;
    return result;
  }
  inline static void InvokeMunmapHook(void* p, size_t size) {
    if (munmap_hook_ != NULL) (*munmap_hook_)(p, size);
  }

 private:
  static NewHook     new_hook_;
  static DeleteHook  delete_hook_;
  static MmapHook    mmap_hook_;
  static MunmapHook  munmap_hook_;

};

#endif
