#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "base/thlist.hpp"
#include "heapprofiler.hpp"
#include "ferp/perftools/hash.hpp"
#include <algorithm>

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>

#ifdef HAVE_LINUX_PTRACE_H
#include <linux/ptrace.h>
#endif
#ifdef HAVE_SYSCALL_H
#include <syscall.h>
#endif

#include <elf.h>
#include "ferp/heapch.hpp"

#include "base/btype.hpp"
#include "base/cmdlag.hpp"
#include "base/logging.hpp"
#include "base/elfcore.hpp"              
#include "mthreads.hpp"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#define LLX    "%"SCNx64             
#define LLD    "%"SCNd64             
#else
#define LLX    "%llx"               
#define LLD    "%lld"
#endif

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         
#endif
#endif

using std::string;
using std::map;
using std::vector;
using std::swap;
using std::make_pair;
using std::min;
using std::max;
using HASH_NAMESPACE::hash_set;

DEFINE_bool(heap_check_report, true,
            "If overall heap check should report the found leaks via pprof");
DEFINE_bool(heap_check_before_constructors, true,
            "deprecated; pretty much always true now");

DEFINE_bool(heap_check_after_destructors, false,
            "If overall heap check is to end after global destructors "
            "or right after all REGISTER_HEAPCHECK_CLEANUP's");

DEFINE_bool(heap_check_strict_check, true,
            "If overall heap check is to be done "
            "via HeapLeakCheck::*SameHeap "
            "or HeapLeakCheck::*NoLeaks call");
           
DEFINE_bool(heap_check_ignore_global_live, true,
            "If overall heap check is to ignore heap objects reachable "
            "from the global data");

DEFINE_bool(heap_check_ignore_thread_live, true,
            "If set to true, objects reachable from thread stacks "
            "and registers are not reported as leaks");
static string* flags_heap_profile_pprof = NULL;

void HeapLeakCheck::set_heap_check_report(bool b) {
  FLAGS_heap_check_report = b;
}
void HeapLeakCheck::set_pprof_path(const char* s) {
  if (flags_heap_profile_pprof == NULL) {
    flags_heap_profile_pprof = new string(s);
  } else {
    flags_heap_profile_pprof->assign(s);
  }
}

void HeapLeakCheck::set_dump_directory(const char* s) {
  if (dump_directory_ == NULL)  dump_directory_ = new string;
  dump_directory_->assign(s);
}

bool HeapLeakCheck::heap_check_report() {
  return FLAGS_heap_check_report;
}
const char* HeapLeakCheck::pprof_path() {
  if (flags_heap_profile_pprof == NULL) {
    return "/bin/pprof"; 
  } else {
    return flags_heap_profile_pprof->c_str();
  }
}
const char* HeapLeakCheck::dump_directory() {
  if (dump_directory_ == NULL) {
    return "/tmp";  
  } else {
    return dump_directory_->c_str();
  }
}

DECLARE_int32(heap_profile_log); 

static pthread_mutex_t heap_checker_lock = PTHREAD_MUTEX_INITIALIZER;
static string* disabled_regexp = NULL;
static string* profile_prefix = NULL;
static HeapLeakCheck* main_heap_checker = NULL;
static bool heap_checker_on = false;
static pid_t heap_checker_pid = 0;
static bool constructor_heap_profiling = false;
enum ObjectPlacement {
  MUST_BE_ON_HEAP,                 
  IGNORED_ON_HEAP, 
  IN_GLOBAL_DATA,   
  THREAD_STACK,    
  THREAD_REGISTERS,
};

struct AllocObject {
  void* ptr;          
  uintptr_t size;        
  ObjectPlacement place;  

  AllocObject(void* p, size_t s, ObjectPlacement l)
    : ptr(p), size(s), place(l) { }
};

typedef map<uintptr_t, size_t> IgnoredObjectsMap;
static IgnoredObjectsMap* ignored_objects = NULL;
typedef vector<AllocObject> LiveObjectsStack;
static LiveObjectsStack* live_objects = NULL;
typedef map<string, LiveObjectsStack> LibraryLiveObjectsStacks;
static LibraryLiveObjectsStacks* library_live_objects = NULL;
typedef hash_set<uintptr_t> DisabledAddressSet;
static DisabledAddressSet* disabled_addresses = NULL;

struct HeapLeakCheck::RangeValue {
  uintptr_t start_address;  
  int       max_depth;     
};
typedef map<uintptr_t, HeapLeakCheck::RangeValue> DisabledRangeMap;
static DisabledRangeMap* disabled_ranges = NULL;
typedef map<uintptr_t, uintptr_t> StackRangeMap;
static StackRangeMap* stack_ranges = NULL;

static void RegisterStackRange(void* top, void* bottom) {
  char* p1 = min(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  char* p2 = max(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  if (HeapProfilerofiler::kMaxLogging) {
    HeapProfilerofiler::MESSAGE(1, "HeapChecker: Thread stack %p..%p (%d bytes)\n",
                          p1, p2, int(p2-p1));
  }
  live_objects->push_back(AllocObject(p1, uintptr_t(p2-p1), THREAD_STACK));
  stack_ranges->insert(make_pair(reinterpret_cast<uintptr_t>(p1),
                                 reinterpret_cast<uintptr_t>(p2)));
}

static void MakeDisabledLiveCallback(void* ptr, HeapProfilerofiler::AllocValue v) {
  bool stack_disable = false;
  bool range_disable = false;
  for (int depth = 0; depth < v.bucket->depth_; depth++) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(v.bucket->stack_[depth]);
    if (disabled_addresses  &&
        disabled_addresses->find(addr) != disabled_addresses->end()) {
      stack_disable = true;  
      break;
    }
    if (disabled_ranges) {
      DisabledRangeMap::const_iterator iter
        = disabled_ranges->upper_bound(addr);
      if (iter != disabled_ranges->end()) {
        assert(iter->first > addr);
        if (iter->second.start_address < addr  &&
            iter->second.max_depth > depth) {
          range_disable = true;  // in range; dropping
          break;
        }
      }
    }
  }
  if (stack_disable || range_disable) {
    uintptr_t start_address = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end_address = start_address + v.bytes;
    StackRangeMap::const_iterator iter
      = stack_ranges->lower_bound(start_address);
    if (iter != stack_ranges->end()) {
      assert(iter->first >= start_address);
      if (iter->second <= end_address) {
    
        HeapProfilerofiler::MESSAGE(2, "HeapChecker: "
                                 "Not %s-disabling %"PRIuS" bytes at %p"
                                 ": have stack inside: %p-%p\n",
                                 (stack_disable ? "stack" : "range"),
                                 v.bytes, ptr,
                                 (void*)iter->first, (void*)iter->second);
        return;
      }
    }
    if (HeapProfilerofiler::kMaxLogging) {
      HeapProfilerofiler::MESSAGE(2, "HeapChecker: "
                               "%s-disabling %"PRIuS" bytes at %p\n",
                               (stack_disable ? "stack" : "range"),
                               v.bytes, ptr);
    }
    live_objects->push_back(AllocObject(ptr, v.bytes, MUST_BE_ON_HEAP));
  }
}

static int GetStatusOutput(const char*  command, string* output) {
  char *env_heapcheck = getenv("HEAPCHECK");
  char *env_ldpreload = getenv("LD_PRELOAD");

  if (env_heapcheck) {
    assert(env_heapcheck[-1] == '=');
    env_heapcheck[-2] = '?';
  }
  if (env_ldpreload) {
    assert(env_ldpreload[-1] == '=');
    env_ldpreload[-2] = '?';
  }

  FILE* f = popen(command, "r");
  if (f == NULL) {
    fprintf(stderr, "popen returned NULL!!!\n"); 
    exit(1);
  }

  if (env_heapcheck) env_heapcheck[-2] = 'K';
  if (env_ldpreload) env_heapcheck[-2] = 'D';

  const int kMaxOutputLine = 10000;
  char line[kMaxOutputLine];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (output)
      *output += line;
  }

  return pclose(f);
}

class FileDescriptor 
{
  public:
   FileDescriptor(int fd) : fd_(fd) { ; }
   ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
   int Close() { int fd = fd_; fd_ = -1; return close(fd); }
   operator int() { return fd_; }
  private:
   int fd_;
};

// Picking out the right segment is a bit of a mess because the
// same file offset can appear in multiple segments!  For example:
//
//   LOAD  0x000000 0x08048000 0x08048000 0x231a8 0x231a8 R E 0x1000
//   LOAD  0x0231a8 0x0806c1a8 0x0806c1a8 0x01360 0x014e8 RW  0x1000
//
// File offset 0x23000 appears in both segments.  The second segment
// stars at 0x23000 because of rounding.  Fortunately, we skip the
// first segment because it is not writeable.  Most of the shared
// objects we see have one read-executable segment and one read-write
// segment so we really skate.
//
// If a shared library has no initialized data, only BSS, then the
// size of the read-write LOAD segment will be zero, the dynamic loader
// will create an anonymous memory region for the BSS but no named
// segment [I think -- gotta check ld-linux.so -- mec].  We will
// overlook that segment and never get here.  This is a bug.

static bool RecordGlobalDataLocked(uint64 start_address,
                                   uint64 end_address,
                                   const char* permissions,
                                   uint64 file_offset,
                                   int64 inode,
                                   const char* filename) {
  if (strchr(permissions, 'w') == NULL)
    return true;

  if (inode == 0)
    return true;

  if (inode != 0) {
    if (filename && strcmp(filename, "/dev/zero") == 0)
      return true;
  }

#ifdef _LP64
  typedef Elf64_Ehdr ElfFileHeader;
  typedef Elf64_Phdr ElfProgramHeader;
#else
  typedef Elf32_Ehdr ElfFileHeader;
  typedef Elf32_Phdr ElfProgramHeader;
#endif

  HeapProfilerofiler::MESSAGE(2, "HeapChecker: Looking into %s\n", filename);
  FileDescriptor fd_elf(open(filename, O_RDONLY));
  if (fd_elf < 0)
    return false;

  ElfFileHeader efh;
  if (read(fd_elf, &efh, sizeof(efh)) != sizeof(efh))
    return false;
  if (memcmp(&efh.e_ident[0], ELFMAG, SELFMAG) != 0)
    return false;
  if (efh.e_version != EV_CURRENT)
    return false;
  if (efh.e_type != ET_EXEC && efh.e_type != ET_DYN)
    return false;

  if (efh.e_phentsize != sizeof(ElfProgramHeader))
    return false;
  if (lseek(fd_elf, efh.e_phoff, SEEK_SET) != efh.e_phoff)
    return false;
  const size_t phsize = efh.e_phnum * efh.e_phentsize;
  ElfProgramHeader* eph = new ElfProgramHeader[efh.e_phnum];
  if (read(fd_elf, eph, phsize) != phsize) {
    delete[] eph;
    return false;
  }

  const int int_page_size = getpagesize();
  if (int_page_size <= 0 || int_page_size & (int_page_size-1))
    abort();
  const uint64 page_size = int_page_size;

  bool found_load_segment = false;
  for (int iph = 0; iph < efh.e_phnum; ++iph) {
    HeapProfilerofiler::MESSAGE(3, "HeapChecker: %s %d: p_type: %d p_flags: %x\n",
                          filename, iph, eph[iph].p_type, eph[iph].p_flags);
    if (eph[iph].p_type == PT_LOAD && eph[iph].p_flags & PF_W) {

      if (eph[iph].p_vaddr != eph[iph].p_paddr) {
        delete[] eph;
        return false;
      }
      if ((eph[iph].p_vaddr  & (page_size-1)) !=
          (eph[iph].p_offset & (page_size-1))) {
        delete[] eph;
        return false;
      }

      const uint64 p_start = eph[iph].p_offset &~ (page_size-1);
      const uint64 p_end = ((eph[iph].p_offset + eph[iph].p_memsz)
                         + (page_size-1)) &~ (page_size-1);
      if (p_end < p_start) {
        delete[] eph;
        return false;
      }
      if (file_offset >= p_start && file_offset < p_end) {
        if (found_load_segment) {
          delete[] eph;
          return false;
        }
        found_load_segment = true;
        if (end_address < start_address ||
            end_address - start_address > p_end - file_offset) {
          delete[] eph;
          return false;
        }

        if (file_offset - p_start > start_address) {
          delete[] eph;
          return false;
        }
        void* addr = reinterpret_cast<void*>(start_address -
                                             (file_offset - p_start));
        const uintptr_t length = p_end - p_start;

        (*library_live_objects)[filename].
          push_back(AllocObject(addr, length, IN_GLOBAL_DATA));
      }
    }
  }
  delete[] eph;

  if (!found_load_segment) {
    HeapProfilerofiler::MESSAGE(-1,
      "HeapChecker: no LOAD segment found in %s\n",
      filename);
    return false;
  }

  if (fd_elf.Close() < 0)
    return false;

  return true;
}

static bool IsLibraryNamed(const char* library, const char* library_base) {
  const char* p = strstr(library, library_base);
  size_t sz = strlen(library_base);
  return p != NULL  &&  (p[sz] == '.'  ||  p[sz] == '-');
}

void HeapLeakCheck::DisableLibraryAllocs(const char* library,
                                           void* start_address,
                                           void* end_address) {
  int depth = 0;
  if (IsLibraryNamed(library, "/libpthread")  ||
      IsLibraryNamed(library, "/libdl")  ||
      IsLibraryNamed(library, "/libcrypto")
     ) {
    depth = 1;
  } else if (IsLibraryNamed(library, "/ld")
            ) {
    depth = 2; 
  }
  if (depth) {
    HeapProfilerofiler::MESSAGE(1, "HeapChecker: "
                          "Disabling allocations from %s at depth %d:\n",
                          library, depth);
    DisableChecksFromTo(start_address, end_address,
                        depth);
  }
}

HeapLeakCheck::ProcMapsResult
HeapLeakCheck::UseProcMaps(ProcMapsTask proc_maps_task) {
  FILE* const fp = fopen("/proc/self/maps", "r");
  if (!fp) {
    int errsv = errno;
    HeapProfilerofiler::MESSAGE(-1, "HeapChecker:  "
                          "Could not open /proc/self/maps: errno=%d.  "
                          "Libraries will not be handled correctly.\n",
                          errsv);
    return CANT_OPEN_PROC_MAPS;
  }
  char proc_map_line[1024];
  bool saw_shared_lib = false;
  while (fgets(proc_map_line, sizeof(proc_map_line), fp) != NULL) {
    uint64_t start_address, end_address, file_offset, inode;
    int size;
    char permissions[5], *filename;
    if (sscanf(proc_map_line, LLX"-" LLX" %4s " LLX" %*x:%*x " LLD" %n",
               &start_address, &end_address, permissions,
               &file_offset, &inode, &size) != 5) continue;
    proc_map_line[strlen(proc_map_line) - 1] = '\0';  // zap the newline
    filename = proc_map_line + size;
    HeapProfilerofiler::MESSAGE(4, "HeapChecker: "
                          "Looking at /proc/self/maps line:\n  %s\n",
                          proc_map_line);

    if (start_address >= end_address) {
      if (inode != 0)  abort();
      continue;
    }

    if (inode != 0 && strstr(filename, "lib") && strstr(filename, ".so")) {
      saw_shared_lib = true;
    }

    if (proc_maps_task == DISABLE_LIBRARY_ALLOCS) {
      if (inode != 0 && strncmp(permissions, "r-xp", 4) == 0) {
        DisableLibraryAllocs(filename,
                             reinterpret_cast<void*>(start_address),
                             reinterpret_cast<void*>(end_address));
      }
    }

    if (proc_maps_task == RECORD_GLOBAL_DATA_LOCKED) {
      if (!RecordGlobalDataLocked(start_address, end_address, permissions,
                                  file_offset, inode, filename)) {
        HeapProfilerofiler::MESSAGE(
          -1, "HeapChecker: failed RECORD_GLOBAL_DATA_LOCKED on %s\n",
          filename);
        abort();
      }
    }
  }
  fclose(fp);

  if (!saw_shared_lib) {
    HeapProfilerofiler::MESSAGE(-1, "HeapChecker: "
                              "No shared libs detected.  "
                              "Will likely report false leak positives "
                              "for statically linked executables.\n");
    return NO_SHARED_LIBS_IN_PROC_MAPS;
  }
  return PROC_MAPS_USED;
}

static int64 live_objects_total = 0;
static int64 live_bytes_total = 0;
static pid_t self_thread_pid = 0;

static enum {
  CALLBACK_NOT_STARTED,
  CALLBACK_STARTED,
  CALLBACK_COMPLETED,
} thread_listing_status = CALLBACK_NOT_STARTED;

int HeapLeakCheck::IgnoreLiveThreads(void* parameter,
                                       int num_threads,
                                       pid_t* thread_pids,
                                       va_list ap) {
  thread_listing_status = CALLBACK_STARTED;
  if (HeapProfilerofiler::kMaxLogging) {
    HeapProfilerofiler::MESSAGE(2, "HeapChecker: Found %d threads (from pid %d)\n",
                          num_threads, getpid());
  }

  vector<void*> thread_registers;

  int failures = 0;
  for (int i = 0; i < num_threads; ++i) {
    if (thread_pids[i] == self_thread_pid) continue;
    if (HeapProfilerofiler::kMaxLogging) {
      HeapProfilerofiler::MESSAGE(2, "HeapChecker: Handling thread with pid %d\n",
                            thread_pids[i]);
    }
#if defined(HAVE_LINUX_PTRACE_H) && defined(HAVE_SYSCALL_H) && defined(DUMPER)
    i386_regs thread_regs;
#define sys_ptrace(r,p,a,d)  syscall(SYS_ptrace, (r), (p), (a), (d))
    if (sys_ptrace(PTRACE_GETREGS, thread_pids[i], NULL, &thread_regs) == 0) {
      void* stack_top;
      void* stack_bottom;
      if (GetStackExtent((void*) thread_regs.BP, &stack_top, &stack_bottom)) {
        RegisterStackRange((void*) thread_regs.SP, stack_bottom);
      } else {
        failures += 1;
      }
      for (void** p = (void**)&thread_regs;
           p < (void**)(&thread_regs + 1); ++p) {
        if (HeapProfiler::kMaxLogging) {
          HeapProfiler::MESSAGE(3, "HeapChecker: Thread register %p\n", *p);
        }
        thread_registers.push_back(*p);
      }
    } else {
      failures += 1;
    }
#else
    failures += 1;
#endif
  }
  IgnoreLiveObjectsLocked("threads stack data", "");
  if (thread_registers.size()) {
    live_objects->push_back(AllocObject(&thread_registers[0],
                                        thread_registers.size() * sizeof(void*),
                                        THREAD_REGISTERS));
    IgnoreLiveObjectsLocked("threads register data", "");
  }
  IgnoreNonThreadLiveObjectsLocked();
  ResumeAllProcessThreads(num_threads, thread_pids);
  thread_listing_status = CALLBACK_COMPLETED;
  return failures;
}

struct HeapLeakCheck::StackExtent {
  bool have;
  void* top;
  void* bottom;
};

static HeapLeakCheck::StackExtent self_thread_stack;

void HeapLeakCheck::IgnoreNonThreadLiveObjectsLocked() {
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(2, "HeapChecker: Handling self thread with pid %d\n",
                          self_thread_pid);
  }
  if (self_thread_stack.have) {
    RegisterStackRange(self_thread_stack.top, self_thread_stack.bottom);
    IgnoreLiveObjectsLocked("stack data", "");
  } else {
    HeapProfiler::MESSAGE(0, "HeapChecker: Stack not found "
                             "for this thread; may get false leak reports\n");
  }
  if (ignored_objects) {
    HeapProfiler::AllocValue alloc_value;
    for (IgnoredObjectsMap::const_iterator object = ignored_objects->begin();
         object != ignored_objects->end(); ++object) {
      void* ptr = reinterpret_cast<void*>(object->first);
      live_objects->
        push_back(AllocObject(ptr, object->second, MUST_BE_ON_HEAP));
      bool have_on_heap =
        HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value);
      if (!(have_on_heap  &&  object->second == alloc_value.bytes)) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "%p of %"PRIuS" bytes "
                              "from an IgnoreObject() disappeared\n",
                              ptr, object->second);
        abort();
      }
    }
    IgnoreLiveObjectsLocked("ignored objects", "");
  }
  
  HeapProfilerofiler::allocation_->Iterate(MakeDisabledLiveCallback);
  IgnoreLiveObjectsLocked("disabled code", "");:
  if (FLAGS_heap_check_ignore_global_live) {
    for (LibraryLiveObjectsStacks::iterator l = library_live_objects->begin();
         l != library_live_objects->end(); ++l) {
      if (live_objects->size()) abort();
      live_objects->swap(l->second);
      IgnoreLiveObjectsLocked("in globals of\n  ", l->first.c_str());
    }
    delete library_live_objects;
  }
}

void HeapLeakCheck::IgnoreAllLiveObjectsLocked(const StackExtent& self_stack) {
  if (live_objects)  abort();
  live_objects = new LiveObjectsStack;
  stack_ranges = new StackRangeMap;
  if (HeapProfiler::ignored_objects_)  abort();
  HeapProfiler::ignored_objects_ = new HeapProfiler::IgnoredObjectSet;
  
  if (FLAGS_heap_check_ignore_global_live) {
    library_live_objects = new LibraryLiveObjectsStacks;
    UseProcMaps(RECORD_GLOBAL_DATA_LOCKED);
  }
  thread_listing_status = CALLBACK_NOT_STARTED;
  bool need_to_ignore_non_thread_objects = true;
  self_thread_pid = getpid();
  self_thread_stack = self_stack;
  if (FLAGS_heap_check_ignore_thread_live) {
   
    int r = ListAllProcessThreads(NULL, IgnoreLiveThreads);
    need_to_ignore_non_thread_objects = r < 0;
    if (r < 0) {
      HeapProfiler::MESSAGE(0, "HeapChecker: thread finding failed "
                               "with %d errno=%d\n", r, errno);
      if (thread_listing_status == CALLBACK_COMPLETED) {
        HeapProfiler::MESSAGE(0, "HeapChecker: thread finding callback "
                                 "finished ok; hopefully everything is fine\n");
        need_to_ignore_non_thread_objects = false;
      } else if (thread_listing_status == CALLBACK_STARTED) {
        HeapProfiler::MESSAGE(0, "HeapChecker: thread finding callback was "
                                 "interrupted or crashed; can't fix this\n");
        abort();
      } else {  
        HeapProfiler::MESSAGE(0, "HeapChecker: Could not find thread stacks; "
                                 "may get false leak reports\n");
      }
    } else if (r != 0) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Thread stacks not found "
                               "for %d threads; may get false leak reports\n",
                            r);
    } else {
      if (HeapProfiler::kMaxLogging) {
        HeapProfiler::MESSAGE(2, "HeapChecker: Thread stacks appear"
                                 " to be found for all threads\n");
      }
    }
  } else {
    HeapProfiler::MESSAGE(0, "HeapChecker: Not looking for thread stacks; "
                             "objects reachable only from there "
                             "will be reported as leaks\n");
  }
  
  if (need_to_ignore_non_thread_objects)  IgnoreNonThreadLiveObjectsLocked();
  if (live_objects_total) {
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                          "Ignoring "LLD" reachable "
                          "objects of "LLD" bytes\n",
                          live_objects_total, live_bytes_total);
    
                      }                    
  delete live_objects;
  live_objects = NULL;
  delete stack_ranges;
  stack_ranges = NULL;
}

void HeapLeakCheck::IgnoreLiveObjectsLocked(const char* name,
                                              const char* name2) {
  int64 live_object_count = 0;
  int64 live_byte_count = 0;
  while (!live_objects->empty()) {
    void* object = live_objects->back().ptr;
    size_t size = live_objects->back().size;
    const ObjectPlacement place = live_objects->back().place;
    live_objects->pop_back();
    HeapProfiler::AllocValue alloc_value;
    if (place == MUST_BE_ON_HEAP  &&
        HeapProfiler::HaveOnHeapLocked(&object, &alloc_value)  &&
        HeapProfiler::ignored_objects_
          ->insert(reinterpret_cast<uintptr_t>(object)).second) {
      live_object_count += 1;
      live_byte_count += size;
    }
    HeapProfiler::MESSAGE(5, "HeapChecker: "
                          "Looking for heap pointers "
                          "in %p of %"PRIuS" bytes\n", object, size);

    const size_t alignment = sizeof(void*);
    const size_t remainder = reinterpret_cast<uintptr_t>(object) % alignment;
    if (remainder) {
      reinterpret_cast<char*&>(object) += alignment - remainder;
      if (size >= alignment - remainder) {
        size -= alignment - remainder;
      } else {
        size = 0;
      }
    }
    while (size >= sizeof(void*)) {
#define UNALIGNED_LOAD32(_p) (*reinterpret_cast<const uint32 *>(_p))
      void* ptr = reinterpret_cast<void*>(UNALIGNED_LOAD32(object));
      void* current_object = object;
      reinterpret_cast<char*&>(object) += alignment;
      size -= alignment;
      if (ptr == NULL)  continue;
      HeapProfiler::MESSAGE(8, "HeapChecker: "
                            "Trying pointer to %p at %p\n",
                            ptr, current_object);
     
      if (HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value)  &&
          HeapProfiler::ignored_objects_
            ->insert(reinterpret_cast<uintptr_t>(ptr)).second) {
      
        HeapProfiler::MESSAGE(5, "HeapChecker: "
                              "Found pointer to %p"
                              " of %"PRIuS" bytes at %p\n",
                              ptr, alloc_value.bytes, current_object);
        live_object_count += 1;
        live_byte_count += alloc_value.bytes;
        live_objects->push_back(AllocObject(ptr, alloc_value.bytes,
                                            IGNORED_ON_HEAP));
      }
    }
  }
  live_objects_total += live_object_count;
  live_bytes_total += live_byte_count;
  if (live_object_count) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Removed "LLD" live heap objects"
                          " of "LLD" bytes: %s%s\n",
                          live_object_count, live_byte_count, name, name2);
  }
}

void HeapLeakCheck::DisableChecksUp(int stack_frames) {
  if (!heap_checker_on) return;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames+1) != 1)  abort();
  DisableChecksAt(stack[0]);
}

void HeapLeakCheck::DisableChecksAt(void* address) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksAtLocked(address);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

bool HeapLeakCheck::HaveDisabledChecksUp(int stack_frames) {
  if (!heap_checker_on) return false;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames+1) != 1)  abort();
  return HaveDisabledChecksAt(stack[0]);
}

bool HeapLeakCheck::HaveDisabledChecksAt(void* address) {
  if (!heap_checker_on) return false;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool result = disabled_addresses != NULL  &&
                disabled_addresses->
                  find(reinterpret_cast<uintptr_t>(address)) !=
                disabled_addresses->end();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  return result;
}

void HeapLeakCheck::DisableChecksIn(const char* pattern) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksInLocked(pattern);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void* HeapLeakCheck::GetDisableChecksStart() {
  if (!heap_checker_on) return NULL;
  void* start_address;
  if (GetStackTrace(&start_address, 1, 1) != 1)  abort();
  return start_address;
}

void HeapLeakCheck::DisableChecksToHereFrom(void* start_address) {
  if (!heap_checker_on) return;
  void* end_address;
  if (GetStackTrace(&end_address, 1, 1) != 1)  abort();
  if (start_address > end_address)  swap(start_address, end_address);
  DisableChecksFromTo(start_address, end_address,
                      10000);  
}

void HeapLeakCheck::IgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  IgnoreObjectLocked(ptr);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakCheck::IgnoreObjectLocked(void* ptr) {
  HeapProfiler::AllocValue alloc_value;
  if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value)) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Going to ignore live object "
                          "at %p of %"PRIuS" bytes\n",
                          ptr, alloc_value.bytes);
    if (ignored_objects == NULL)  {
      ignored_objects = new IgnoredObjectsMap;
      IgnoreObjectLocked(ignored_objects);
    }
    if (!ignored_objects->insert(make_pair(reinterpret_cast<uintptr_t>(ptr),
                                           alloc_value.bytes)).second) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "%p is already being ignored\n", ptr);
      abort();
    }
  }
}

void HeapLeakCheck::UnIgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  HeapProfiler::AllocValue alloc_value;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool ok = HeapProfiler::HaveOnHeap(&ptr, &alloc_value);
  if (ok) {
    ok = false;
    if (ignored_objects) {
      IgnoredObjectsMap::iterator object =
        ignored_objects->find(reinterpret_cast<uintptr_t>(ptr));
      if (object != ignored_objects->end()  &&
          alloc_value.bytes == object->second) {
        ignored_objects->erase(object);
        ok = true;
        HeapProfiler::MESSAGE(1, "HeapChecker: "
                              "Now not going to ignore live object "
                              "at %p of %"PRIuS" bytes\n",
                              ptr, alloc_value.bytes);
      }
    }
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (!ok) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "%p has not been ignored\n", ptr);
    abort();
  }
}

void HeapLeakCheck::DumpProfileLocked(bool start,
                                        const StackExtent& self_stack) {
  assert(!HeapProfiler::dumping_);  
  HeapProfiler::MESSAGE(0, "HeapChecker: %s check \"%s\"\n",
                        (start ? "Starting" : "Ending"), name_);
  assert(!HeapProfiler::self_disable_);
  HeapProfiler::self_disabled_tid_ = pthread_self();
  HeapProfiler::self_disable_ = true;
  {
    IgnoreAllLiveObjectsLocked(self_stack);
    HeapProfiler::dump_for_leaks_ = true;
    string* file_name = new string(*profile_prefix + "." + name_ +
                                   (start ? "-beg.heap" : "-end.heap"));
    HeapProfiler::DumpLocked("leak check", file_name->c_str());
    delete file_name;  
    HeapProfiler::dump_for_leaks_ = false;
    delete HeapProfiler::ignored_objects_;
    HeapProfiler::ignored_objects_ = NULL;
  }
  HeapProfiler::self_disable_ = false;
  int64 self_disabled_bytes = HeapProfiler::self_disabled_.alloc_size_ -
                              HeapProfiler::self_disabled_.free_size_;
  int64 self_disabled_allocs = HeapProfiler::self_disabled_.allocs_ -
                               HeapProfiler::self_disabled_.frees_;
  if (self_disabled_bytes != 0  ||  self_disabled_allocs != 0) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "internal HeapChecker leak of "LLD" objects "
                          "and/or "LLD" bytes\n",
                          self_disabled_allocs, self_disabled_bytes);
    abort();
  }
}

void HeapLeakCheck::Create(const char *name) {
  name_ = NULL;
  has_checked_ = false;
  char* n = new char[strlen(name) + 1];   
  IgnoreObject(n); 
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  HeapProfiler::Lock();
  if (heap_checker_on) {
    assert(strchr(name, '/') == NULL);  
    assert(name_ == NULL);  
    name_ = n;
    memcpy(name_, name, strlen(name) + 1);
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(true, self_stack);
    start_inuse_bytes_ = static_cast<size_t>(HeapProfiler::profile_.alloc_size_ -
                                             HeapProfiler::profile_.free_size_);
    start_inuse_allocs_ = static_cast<size_t>(HeapProfiler::profile_.allocs_ -
                                              HeapProfiler::profile_.frees_);
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "Start check \"%s\" profile: "
                               "%"PRIuS"d bytes in %"PRIuS"d objects\n",
                               name_, start_inuse_bytes_, start_inuse_allocs_);
    }
  } else {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Heap checker is not active, "
                          "hence checker \"%s\" will do nothing!\n", name);
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "To activate set the HEAPCHECK environment "
                          "variable.\n");
  }
  HeapProfiler::Unlock();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (name_ == NULL) {
    UnIgnoreObject(n);
    delete[] n;  
  }
}

HeapLeakCheck::HeapLeakCheck(const char *name) {
  assert(strcmp(name, "_main_") != 0); 
  Create(name);
}

DECLARE_int64(heap_profile_allocation_interval);
DECLARE_int64(heap_profile_inuse_interval);

int32 HeapLeakCheck::main_thread_pid_ = getpid();
string* HeapLeakCheck::dump_directory_ = NULL;
#ifdef HAVE_PROGRAM_INVOCATION_NAME
extern char* program_invocation_name;
extern char* program_invocation_short_name;
static const char* invocation_name() { return program_invocation_short_name; }
static const char* invocation_path() { return program_invocation_name; }
#else
static const char* invocation_name() { return "heap_checker"; }
static const char* invocation_path() { return "heap_checker"; }  
#endif

HeapLeakCheck::HeapLeakCheck() {
  Create("_main_");
}

ssize_t HeapLeakCheck::BytesLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_bytes_increase_;
}

ssize_t HeapLeakCheck::ObjectsLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_allocs_increase_;
}

bool HeapLeakCheck::DoNoLeaks(bool same_heap,
                                bool do_full,
                                bool do_report) {
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  HeapProfiler::Lock();
  if (heap_checker_on) {
    if (name_ == NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "*NoLeaks|SameHeap must be called only once"
                            " and profiling must be not turned on "
                            "after construction of a HeapLeakCheck\n");
      abort();
    }
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(false, self_stack);  
    const bool use_initial_profile =
      !(FLAGS_heap_check_before_constructors  &&  this == main_heap_checker);
    if (!use_initial_profile) {
      start_inuse_bytes_ = 0;
      start_inuse_allocs_ = 0;
    }
    int64 end_inuse_bytes = HeapProfiler::profile_.alloc_size_ -
                            HeapProfiler::profile_.free_size_;
    int64 end_inuse_allocs = HeapProfiler::profile_.allocs_ -
                             HeapProfiler::profile_.frees_;
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "End check \"%s\" profile: "
                               ""LLD" bytes in "LLD" objects\n",
                               name_, end_inuse_bytes, end_inuse_allocs);
    }
    inuse_bytes_increase_ = (ssize_t)(end_inuse_bytes - start_inuse_bytes_);
    inuse_allocs_increase_ = (ssize_t)(end_inuse_allocs - start_inuse_allocs_);
    has_checked_ = true;
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    bool see_leaks =
      (same_heap ? (inuse_bytes_increase_ != 0 || inuse_allocs_increase_ != 0)
                 : (inuse_bytes_increase_ > 0 || inuse_allocs_increase_ > 0));
    if (see_leaks || do_full) {
      bool pprof_can_ignore = false;
      const char* command_tail = " --text 2>/dev/null"; 
      const char* gv_command_tail
        = " --edgefraction=1e-10 --nodefraction=1e-10 --gv 2>/dev/null";
      string ignore_re;
      if (disabled_regexp) {
        pprof_can_ignore = true;
        ignore_re += " --ignore='^";
        ignore_re += *disabled_regexp;
        ignore_re += "$'";
      }
      char base_command[6 * PATH_MAX + 200];
      char beg_profile[PATH_MAX+1], end_profile[PATH_MAX+1];
      if (use_initial_profile) {
        snprintf(beg_profile, sizeof(beg_profile), "%s.%s-beg.heap",
                 profile_prefix->c_str(), name_);
        const char* drop_negative = same_heap ? "" : " --drop_negative";
        snprintf(base_command, sizeof(base_command),
                 "%s --base=\"%s\" %s ",
                 pprof_path(), beg_profile, drop_negative);
      } else {
        beg_profile[0] = '\0';
        snprintf(base_command, sizeof(base_command), "%s",
                 pprof_path());
      }
      snprintf(end_profile, sizeof(end_profile), "%s.%s-end.heap",
               profile_prefix->c_str(), name_);
      snprintf(base_command + strlen(base_command),
               sizeof(base_command) - strlen(base_command),
               " %s \"%s\" %s --inuse_objects --lines",
               invocation_path(), end_profile, ignore_re.c_str());
                   
      char cwd[PATH_MAX+1];
      if (getcwd(cwd, sizeof(cwd)) != cwd)  abort();
      if (see_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Heap memory leaks of "LLD" bytes and/or "
                              ""LLD" allocations detected by check \"%s\".\n\n",
                              (int64)inuse_bytes_increase_,
                              (int64)inuse_allocs_increase_,
                              name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "			      
                              "To investigate leaks manually use e.g.\n"
                              "cd %s; "  
                              "%s%s\n\n",
                              cwd, base_command, gv_command_tail);
      }
      string output;
      int checked_leaks = 0;
      if ((see_leaks && do_report) || do_full) {
        if (access(pprof_path(), X_OK|R_OK) != 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "WARNING: Skipping pprof check:"
                                " could not run it at %s\n",
                                pprof_path());
        } else {
          char full_command[6 * PATH_MAX + 200];  
          snprintf(full_command, sizeof(full_command), "%s%s",
                   base_command, command_tail);
          checked_leaks = GetStatusOutput(full_command, &output);
          if (checked_leaks != 0) {
            HeapProfiler::MESSAGE(-1, "ERROR: Could not run pprof at %s\n",
                                  pprof_path());
            abort();
          }
        }
        if (see_leaks && pprof_can_ignore &&
            output.empty() && checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "These must be leaks that we disabled"
                                " (pprof succeeded)! This check WILL FAIL"
                                " if the binary is strip'ped!\n");
          see_leaks = false;
        }
        if (!see_leaks  &&  strstr(output.c_str(), "nm: ") != NULL  &&
            strstr(output.c_str(), ": no symbols") != NULL)  output.resize(0);
      }
      if (access(end_profile, R_OK) != 0  ||
          (beg_profile[0]  &&  access(beg_profile, R_OK) != 0)) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "One of the heap profiles is gone: %s %s\n",
                              beg_profile, end_profile);
        abort();
      }
      if (!(see_leaks || checked_leaks == 0)) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "pprof command failed: %s%s\n",
                              base_command, command_tail);
        abort();
      }
      if (see_leaks  &&  use_initial_profile) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "CAVEAT: Some of the reported leaks might have "
                              "occurred before check \"%s\" was started!\n",
                              name_);
      }
      bool tricky_leaks = !output.empty();
      if (!see_leaks && tricky_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Tricky heap memory leaks of"
                              " no bytes and no allocations "
                              "detected by check \"%s\".\n", name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "To investigate leaks manually uge e.g.\n"
                              "cd %s; "  
                              "%s%s\n\n",
                              name_, cwd, base_command, gv_command_tail);
        if (use_initial_profile) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "CAVEAT: Some of the reported leaks might have "
                                "occurred before check \"%s\" was started!\n",
                                name_);
        }
        see_leaks = true;
      }
      if (see_leaks && do_report) {
        if (checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "Below is this pprof's output:\n\n");
          write(STDERR_FILENO, output.data(), output.size());
          HeapProfiler::MESSAGE(-1, "\n\n");
        } else {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "pprof has failed\n\n");
        }
      }
    } else {
      HeapProfiler::MESSAGE(0, "HeapChecker: No leaks found\n");
    }
    UnIgnoreObject(name_);
    delete [] name_;
    name_ = NULL;
    return !see_leaks;
  } else {
    if (name_ != NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "Profiling must stay enabled "
                            "during leak checking\n");
      abort();
    }
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    return true;
  }
}

HeapLeakCheck::~HeapLeakCheck() {
  if (name_ != NULL) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Some *NoLeaks|SameHeap method"
                          " must be called on the checker\n");
    abort();
  }
}

bool HeapLeakCheck::IsActive() {
  return heap_checker_on;
}

friend<HeapCleaner::void>* HeapCleaner::heap_cleanups_ = NULL;
HeapCleaner::HeapCleaner(void_function f) {
  if (heap_cleanups_ == NULL)
    heap_cleanups_ = new vector<HeapCleaner::void_function>;
  heap_cleanups_->push_back(f);
}


void HeapCleaner::RunHeapCleanups() {
  if (!heap_cleanups_)
    return;
  for (int i = 0; i < heap_cleanups_->size(); i++) {
    void (*f)(void) = (*heap_cleanups_)[i];
    f();
  }
  delete heap_cleanups_;
  heap_cleanups_ = NULL;
}

void HeapLeakCheck::RunHeapCleanups() {
  if (heap_checker_pid == getpid()) {  
                                       
    HeapCleaner::RunHeapCleanups();
    if (!FLAGS_heap_check_after_destructors) {
      DoMainHeapCheck();
    }
  }
}

void HeapLeakCheck::InternalInitStart(const string& heap_check_type) {
  if (heap_check_type.empty()) {
    heap_checker_on = false;
  } else {
    if (main_heap_checker) {
      return;
    }
    if (!constructor_heap_profiling) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: Can not start so late. "
                            "You have to enable heap checking by\n"
                            "setting the environment variable HEAPCHECK.\n");
      abort();
    }
    if (heap_check_type == "minimal") {
      FLAGS_heap_check_before_constructors = false;  
      FLAGS_heap_check_after_destructors = false;  
                                                  
      FLAGS_heap_check_strict_check = false;  
      FLAGS_heap_check_ignore_thread_live = true;  
      FLAGS_heap_check_ignore_global_live = true; 
    } else if (heap_check_type == "normal") {
      FLAGS_heap_check_before_constructors = true; 
      FLAGS_heap_check_after_destructors = false;  /
                                                  
      FLAGS_heap_check_strict_check = true;  
      FLAGS_heap_check_ignore_thread_live = true;  
      FLAGS_heap_check_ignore_global_live = true; 
    } else if (heap_check_type == "strict") {
      FLAGS_heap_check_before_constructors = true;  
      FLAGS_heap_check_after_destructors = true;  
                                            
      FLAGS_heap_check_strict_check = true; 
      FLAGS_heap_check_ignore_thread_live = true;  
      FLAGS_heap_check_ignore_global_live = true; 
    } else if (heap_check_type == "draconian") {
      FLAGS_heap_check_before_constructors = true;  
      FLAGS_heap_check_after_destructors = true; 
      FLAGS_heap_check_strict_check = true; 
      FLAGS_heap_check_ignore_thread_live = false;  
      FLAGS_heap_check_ignore_global_live = false;  
    } else if (heap_check_type == "as-is") {
    } else if (heap_check_type == "local") {
    } else {
      LogPrintf(FATAL, "Unsupported heap_check flag: %s",
                heap_check_type.c_str());
    }
    assert(heap_checker_pid == getpid());
    heap_checker_on = true;
    if (!HeapProfiler::is_on_)  abort();
    ProcMapsResult pm_result = UseProcMaps(DISABLE_LIBRARY_ALLOCS);
    if (pm_result != HeapLeakCheck::PROC_MAPS_USED) {
      heap_checker_on = false;
      HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
      HeapProfiler::StopForLeaks();
      return;
    }

    profile_prefix = new string(dump_directory());
    *profile_prefix += "/";
    *profile_prefix += invocation_name();
    HeapProfiler::CleanupProfiles(profile_prefix->c_str());

    char pid_buf[15];
    if (main_thread_pid_ == 0) 
      main_thread_pid_ = getpid();
    snprintf(pid_buf, sizeof(pid_buf), ".%d", main_thread_pid_);
    *profile_prefix += pid_buf;
    assert(HeapProfiler::need_for_leaks_);

    HeapProfiler::AllocValue alloc_value;
    char* test_str = new char[5];
    void* ptr = test_str;
    if (!HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
    ptr = test_str;
    delete [] test_str;
    if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
  
    if (heap_check_type != "local") {
      atexit(RunHeapCleanups);
      assert(main_heap_checker == NULL);
      main_heap_checker = new HeapLeakCheck();
    }
  }
  if (!heap_checker_on  &&  constructor_heap_profiling) {
    HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
    HeapProfiler::StopForLeaks();
  }
}

void HeapLeakCheck::DoMainHeapCheck() {
  assert(heap_checker_pid == getpid());
  if (main_heap_checker) {
    bool same_heap = FLAGS_heap_check_strict_check;
    if (FLAGS_heap_check_before_constructors)  same_heap = true;
    bool do_full = !same_heap; 
    bool do_report = FLAGS_heap_check_report;
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                             "Checking for whole-program memory leaks\n");
    if (!main_heap_checker->DoNoLeaks(same_heap, do_full, do_report)) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: crashing because of leaks\n");
      abort();
    }
    delete main_heap_checker;
    main_heap_checker = NULL;
  }
}

static inline void PThreadSpecificHack() {
  const int kKeyBlock = 32;
  for (int i = 0; i < kKeyBlock; ++i) {
    pthread_key_t key;
    if (perftools_pthread_key_create(&key, NULL) != 0)  abort();
  }
}

void HeapLeakCheck::BeforeConstructors() {
  if (!getenv("HEAPCHECK")) {
    return;
  }

  if (getuid() != geteuid()) {
    HeapProfiler::MESSAGE(0, ("HeapChecker: ignoring HEAPCHECK because "
                              "program seems to be setuid\n"));
    return;
  }

  if (constructor_heap_profiling)  abort();
  constructor_heap_profiling = true;
  HeapProfiler::Init(); 
  HeapProfiler::StartForLeaks();
  heap_checker_on = true;
  PThreadSpecificHack();

  const char* heap_check_type = getenv("HEAPCHECK");
  assert(heap_check_type);  
  if ( heap_check_type[0] == '\0') {
  } else if ( !strcmp(heap_check_type, "minimal") ||
              !strcmp(heap_check_type, "normal") ||
              !strcmp(heap_check_type, "strict") ||
              !strcmp(heap_check_type, "draconian") ||
              !strcmp(heap_check_type, "local") ) {
    HeapLeakCheck::InternalInitStart(heap_check_type);
  } else {
    HeapLeakCheck::InternalInitStart("normal");        
  }
}

extern bool heap_leak_checker_bcad_variable;  
void HeapLeakCheck_BeforeConstructors() {
  heap_checker_pid = getpid();  
  heap_leak_checker_bcad_variable = true;
  HeapLeakCheck::BeforeConstructors();
}

void HeapLeakCheck_AfterDestructors() {
  if (heap_checker_pid == getpid()) {  
    if (FLAGS_heap_check_after_destructors && main_heap_checker) {
      HeapLeakCheck::DoMainHeapCheck();
      poll(NULL, 0, 500);
    }
  }
}

void HeapLeakCheck::DisableChecksInLocked(const char* pattern) {
  if (disabled_regexp == NULL) {
    disabled_regexp = new string;
    IgnoreObjectLocked(disabled_regexp);
  }
  HeapProfiler::MESSAGE(1, "HeapChecker: "
                        "Disabling leaks checking in stack traces "
                        "under frames maching \"%s\"\n", pattern);
  if (disabled_regexp->size())  *disabled_regexp += '|';
  *disabled_regexp += pattern;
}

void HeapLeakCheck::DisableChecksFromTo(void* start_address,
                                          void* end_address,
                                          int max_depth) {
  assert(start_address < end_address);
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  if (disabled_ranges == NULL) {
    disabled_ranges = new DisabledRangeMap;
    IgnoreObjectLocked(disabled_ranges);
  }
  RangeValue value;
  value.start_address = reinterpret_cast<uintptr_t>(start_address);
  value.max_depth = max_depth;
  if (disabled_ranges->
        insert(make_pair(reinterpret_cast<uintptr_t>(end_address),
                         value)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame addresses between %p..%p\n",
                          start_address, end_address);
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakCheck::DisableChecksAtLocked(void* address) {
  if (disabled_addresses == NULL) {
    disabled_addresses = new DisabledAddressSet;
    IgnoreObjectLocked(disabled_addresses);
  }
  if (disabled_addresses->insert(reinterpret_cast<uintptr_t>(address)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame address %p\n",
                          address);
  }
}
