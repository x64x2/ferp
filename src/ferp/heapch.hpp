#ifndef BASE_HEAPCH_HPP
#define BASE_HEAPCH_HPP

#include <string>
#include <sys/types.h>    
#include <vector>

#define REGISTER_HEAPCHECK_CLEANUP(name, body)  \
  namespace { \
  void heapcheck_cleanup_##name() { body; } \
  static HeapCleaner heapcheck_cleaner_##name(&heapcheck_cleanup_##name); \
  }

class HeapCleaner {
 public:
  typedef void (*void_function)(void);
  HeapCleaner(void_function f);
  static void RunHeapCleanups();
 private:
  static std::vector<void_function>* heap_cleanups_;
};

class HeapLeakCheck {
 public:  
  static bool IsActive();

 public:  
  explicit HeapLeakCheck(const char *name);
  bool NoLeaks() { return DoNoLeaks(false, true, true); }
  bool QuickNoLeaks() { return DoNoLeaks(false, false, true); }
  bool BriefNoLeaks() { return DoNoLeaks(false, false, false); }
  bool SameHeap() { return DoNoLeaks(true, true, true); }
  bool QuickSameHeap() { return DoNoLeaks(true, false, true); }
  bool BriefSameHeap() { return DoNoLeaks(true, false, false); }

  ssize_t BytesLeaked() const;
  ssize_t ObjectsLeaked() const;

  ~HeapLeakCheck();

  static void set_heap_check_report(bool);
  static void set_pprof_path(const char*);
  static void set_dump_directory(const char*);

  static bool heap_check_report();
  static const char* pprof_path();
  static const char* dump_directory();

 private:  
  char* name_; 
  size_t start_inuse_bytes_;  
  size_t start_inuse_allocs_; 
  bool has_checked_; 
  ssize_t inuse_bytes_increase_;  
  ssize_t inuse_allocs_increase_;  

  static pid_t main_thread_pid_;  
  static std::string* dump_directory_;

 public:  
  static void DisableChecksIn(const char* pattern);
  static void* GetDisableChecksStart();
  static void DisableChecksToHereFrom(void* start_address);
  static void DisableChecksUp(int stack_frames);
  static void DisableChecksAt(void* address);
  static bool HaveDisabledChecksUp(int stack_frames);
  static bool HaveDisabledChecksAt(void* address);

  static void IgnoreObject(void* ptr);
  static void UnIgnoreObject(void* ptr);

 public:  
  static void InternalInitStart(const std::string& heap_check_type);

  struct RangeValue;
  struct StackExtent;

 private:  
  void DumpProfileLocked(bool start, const StackExtent& self_stack);
  void Create(const char *name);
  bool DoNoLeaks(bool same_heap, bool do_full, bool do_report);
  static void IgnoreObjectLocked(void* ptr);
  static void DisableChecksAtLocked(void* address);
  static void DisableChecksInLocked(const char* pattern);
  static void DisableChecksFromTo(void* start_address,
                                  void* end_address,
                                  int max_depth);
  static void IgnoreAllLiveObjectsLocked(const StackExtent& self_stack);
  static int IgnoreLiveThreads(void* parameter,
                               int num_threads,
                               pid_t* thread_pids,
                               va_list ap);

  static void IgnoreLiveObjectsLocked(const char* name, const char* name2);
  static void RunHeapCleanups();
  static void DoMainHeapCheck();

  enum ProcMapsTask { 
    RECORD_GLOBAL_DATA_LOCKED, 
    DISABLE_LIBRARY_ALLOCS 
  };
  enum ProcMapsResult { PROC_MAPS_USED, CANT_OPEN_PROC_MAPS,
                        NO_SHARED_LIBS_IN_PROC_MAPS };
  static ProcMapsResult UseProcMaps(ProcMapsTask proc_maps_task);
  static void DisableLibraryAllocs(const char* library,
                                   void* start_address,
                                   void* end_address);

 private:
  static void BeforeConstructors();
  friend void HeapLeakCheckecker_BeforeConstructors();
  friend void HeapLeakCheckecker_AfterDestructors();

 private:
  HeapLeakCheck();
  HeapLeakCheck(const HeapLeakCheck&);
  void operator=(const HeapLeakCheck&);
};
#endif  