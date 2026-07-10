#ifndef BASE_HEAP_PROFILER_HPP__
#define BASE_HEAP_PROFILER_HPP__

#if defined HAVE_STDINT_H
#include <stdint.h>            
#include <inttypes.h>          
#else
#include <sys/types.h>         
#include <pthread.h>
#include "base/btype.hpp"
#include "ferp/heappr.hpp"
#include "ferp/perftools/hash.hpp"
#endif
template<class T> class AddressMap; 
class HeapLeakCheckecker;  

class HeapProfiler {
 public:  

  struct Bucket {
    uintptr_t hash_;      
    int     depth_;       
    void**  stack_;       
    int32   allocs_;      
    int32   frees_;      
    int64   alloc_size_;  
    int64   free_size_;   
    Bucket* next_;        
  };

  struct AllocValue {
    Bucket* bucket;  
    size_t  bytes;   
  };
  typedef AddressMap<AllocValue> AllocationMap;

  typedef HASH_NAMESPACE::hash_set<uintptr_t> IgnoredObjectSet;

 private:  

  static bool is_on_;
  static bool need_for_leaks_;
  static bool init_has_been_called_;
 
  static bool self_disable_;
  static pthread_t self_disabled_tid_;
  static IgnoredObjectSet* ignored_objects_;
  static bool dump_for_leaks_;
  static bool dumping_;
  static Bucket total_;
  static Bucket profile_;
  static Bucket self_disabled_;
  static char* filename_prefix_;
  static AllocationMap* allocation_;
  static int strip_frames_;
  static bool done_first_alloc_;
  static void* recordalloc_reference_stack_position_;
  static void Lock();
  static void Unlock();

 private: 
  static void* Malloc(size_t bytes);
  static void Free(void* p);
  static void DumpLocked(const char *reason, const char* file_name);

 private: 
  static bool HaveOnHeap(void** ptr, AllocValue* alloc_value);
  static bool HaveOnHeapLocked(void** ptr, AllocValue* alloc_value);

 private:  
  static Bucket* GetBucket(int skip_count);
  static int UnparseBucket(char* buf, int buflen, int bufsize, const Bucket* b);
  static void AdjustByIgnoredObjects(int adjust);
  static void RecordAlloc(void* ptr, size_t bytes, int skip_count);
  static void RecordFree(void* ptr);
  static void EarlyStartLocked();
  static void CleanupProfiles(const char* prefix);
  static void StartLocked(const char* prefix);
  static void StopLocked();
  static void StartForLeaks();
  static void StopForLeaks();
  static void NewHook(void* ptr, size_t size);
  static void DeleteHook(void* ptr);
  static void MmapHook(void* result,
                       void* start, size_t size,
                       int prot, int flags,
                       int fd, off_t offset);
  static void MunmapHook(void* ptr, size_t size);

 private:

  friend class HeapLeakCheckecker;
  friend void HeapProfilerStart(const char* prefix);
  friend void HeapProfilerStop();
  friend void HeapProfilerDump(const char *reason);
  friend char* GetHeapProfile();

 public:

  static void MESSAGE(int logging_level, const char* format, ...)
#ifdef _HAVE___ATTRIBUTE__
    __attribute__ ((__format__ (__printf__, 2, 3)))
#endif
;

  static const bool kMaxLogging = false;
  static void Init();
  static bool IsOn() { return is_on_; }
};

#endif 