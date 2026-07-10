#include <cstring>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>

#include "ferp/perftools/hash.hpp"

#include "ferp/heappr.hpp"
#include "ferp/mallochk.hpp"

#include "ferp/stacktrace.hpp"
#include "heapprofiler.hpp"
#include "internl.hpp"

#include "base/btype.hpp"
#include "base/cmdlag.hpp"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#define LLD    PRId64             
#else
#define LLD    "lld"               
#endif

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096       
#endif
#endif

#define LOGF  STL_NAMESPACE::cout  
using HASH_NAMESPACE::hash_set;
using STL_NAMESPACE::string;
using STL_NAMESPACE::sort;

DEFINE_bool(cleanup_old_heap_profiles, true,
            "At initialization time, delete old heap profiles.");
DEFINE_int64(heap_profile_allocation_interval, 1 << 30,
             "Dump heap profiling information once every specified "
             "number of bytes allocated by the program.");
DEFINE_int64(heap_profile_inuse_interval, 100 << 20 ,
             "Dump heap profiling information whenever the high-water "
             "memory usage mark increases by the specified number of "
             "bytes.");
DEFINE_bool(mmap_log, false, "Should mmap/munmap calls be logged?");
DEFINE_bool(mmap_profile, false, "If heap-profiling on, also profile mmaps");
DEFINE_int32(heap_profile_log, 0,
             "Logging level for heap profiler/checker messages");

void HeapProfilerSetLogLevel(int level) {
  FLAGS_heap_profile_log = level;
}

void HeapProfilerSetAllocationInterval(size_t interval) {
  FLAGS_heap_profile_allocation_interval = interval;
}
B
void HeapProfilerSetInuseInterval(size_t interval) {
  FLAGS_heap_profile_inuse_interval = interval;
}

void HeapProfiler::MESSAGE(int level, const char* format, ...) {
  if (FLAGS_heap_profile_log < level) return;

  va_list ap;
  va_start(ap, format);
  char buf[600];
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  write(STDERR_FILENO, buf, strlen(buf));
}

class HeapProfilerMemory {
 private:
  static const int kBlockSize = 1 << 20;
  static const int kMaxBlocks = 1024;
  struct Block {
    void*       ptr;
    size_t      size;
  };

  union AlignUnion { double d; void* p; int64 i; size_t s; };
  static const int kAlignment = sizeof(AlignUnion);

  Block         blocks_[kMaxBlocks];    
  int           nblocks_;              
  char*         current_;              
  int           pos_;                 

  void* AllocBlock(size_t size) {
    const size_t pagesize = getpagesize();
    size = ((size + pagesize -1 ) / pagesize) * pagesize;

    HeapProfiler::MESSAGE(1, "HeapProfiler: allocating %"
                          " bytes for internal use\n", size);
    if (nblocks_ == kMaxBlocks) {
      HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: Alloc out of memory\n");
      abort();
    }

    MallocHk::MmapHook saved = MallocHk::SetMmapHook(NULL);
    void* ptr = mmap(NULL, size,
                     PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS,
                     -1, 0);
    MallocHk::SetMmapHook(saved);

    if (ptr == reinterpret_cast<void*>(MAP_FAILED)) {
      HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: mmap %"PRIuS": %s\n",
                            size, strerror(errno));
      abort();
    }
    blocks_[nblocks_].ptr = ptr;
    blocks_[nblocks_].size = size;
    return ptr;
  }

 public:
  void Init() {
    nblocks_ = 0;
    current_ = NULL;
    pos_ = kBlockSize;
  }

  void Clear() {
    MallocHk::MunmapHook saved = MallocHk::SetMunmapHook(NULL);
    for (int i = 0; i < nblocks_; ++i) {
      if (munmap(blocks_[i].ptr, blocks_[i].size) != 0) {
        HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: munmap: %s\n",
                              strerror(errno));
        abort();
      }
    }
    MallocHk::SetMunmapHook(saved);

    nblocks_ = 0;
    current_ = NULL;
    pos_ = kBlockSize;
  }

  void* Alloc(size_t bytes) {
    if (bytes >= kBlockSize / 8) {
      // Too big for piecemeal allocation
      return AllocBlock(bytes);
    } else {
      if (pos_ + bytes > kBlockSize) {
        current_ = reinterpret_cast<char*>(AllocBlock(kBlockSize));
        pos_ = 0;
      }
      void* result = current_ + pos_;
      pos_ = (pos_ + bytes + kAlignment - 1) & ~(kAlignment-1);
      return result;
    }
  }
};
static HeapProfilerMemory heap_profiler_memory;
void* HeapProfiler::Malloc(size_t bytes) {
  return heap_profiler_memory.Alloc(bytes);
}
void HeapProfiler::Free(void* p) {
}

static TCMalloc_SpinLock heap_lock;

void HeapProfiler::Lock() {
  if (kMaxLogging) {
    HeapProfiler::MESSAGE(10, "HeapProfiler: Lock from %d\n",
                          int(pthread_self()));
  }

  heap_lock.Lock();
}

void HeapProfiler::Unlock() {
  if (kMaxLogging) {
    HeapProfiler::MESSAGE(10, "HeapProfiler: Unlock from %d\n",
                          int(pthread_self()));
  }

  heap_lock.Unlock();
}

typedef HeapProfiler::Bucket Bucket;

bool HeapProfiler::is_on_ = false;
bool HeapProfiler::init_has_been_called_ = false;
bool HeapProfiler::need_for_leaks_ = false;
bool HeapProfiler::self_disable_ = false;
pthread_t HeapProfiler::self_disabled_tid_;
HeapProfiler::IgnoredObjectSet* HeapProfiler::ignored_objects_ = NULL;
bool HeapProfiler::dump_for_leaks_ = false;
bool HeapProfiler::dumping_ = false;
Bucket HeapProfiler::total_;
Bucket HeapProfiler::self_disabled_;
Bucket HeapProfiler::profile_;
char* HeapProfiler::filename_prefix_ = NULL;

static const int kHashTableSize = 179999;
static Bucket** table = NULL;
HeapProfiler::AllocationMap* HeapProfiler::allocation_ = NULL;

static int     num_buckets = 0;
static int     total_stack_depth = 0;
static int     dump_count = 0;    
static int64   last_dump = 0;       
static int64   high_water_mark = 0; 

int HeapProfiler::strip_frames_ = 0;
bool HeapProfiler::done_first_alloc_ = false;
void* HeapProfiler::recordalloc_reference_stack_position_ = NULL;

static bool ByAllocatedSpace(Bucket* a, Bucket* b) {
  return (a->alloc_size_ - a->free_size_) > (b->alloc_size_ - b->free_size_);
}

int HeapProfiler::UnparseBucket(char* buf, int buflen, int bufsize,
                                const Bucket* b) {
  profile_.allocs_ += b->allocs_;
  profile_.alloc_size_ += b->alloc_size_;
  profile_.frees_ += b->frees_;
  profile_.free_size_ += b->free_size_;
  if (dump_for_leaks_  &&
      b->allocs_ - b->frees_ == 0  &&
      b->alloc_size_ - b->free_size_ == 0) {
    // don't waste the profile space on buckets that do not matter
    return buflen;
  }
  int printed =
    snprintf(buf + buflen, bufsize - buflen, "%6d: %8" LLD" [%6d: %8" LLD"] @",
             b->allocs_ - b->frees_,
             b->alloc_size_ - b->free_size_,
             b->allocs_,
             b->alloc_size_);
  if (printed < 0 || printed >= bufsize - buflen)  return buflen;
  buflen += printed;
  for (int d = 0; d < b->depth_; d++) {
    printed = snprintf(buf + buflen, bufsize - buflen, " 0x%08lx",
                       (unsigned long)b->stack_[d]);
    if (printed < 0 || printed >= bufsize - buflen)  return buflen;
    buflen += printed;
  }
  printed = snprintf(buf + buflen, bufsize - buflen, "\n");
  if (printed < 0 || printed >= bufsize - buflen)  return buflen;
  buflen += printed;
  return buflen;
}

void HeapProfiler::AdjustByIgnoredObjects(int adjust) {
  if (ignored_objects_) {
    assert(dump_for_leaks_);
    for (IgnoredObjectSet::const_iterator i = ignored_objects_->begin();
         i != ignored_objects_->end(); ++i) {
      AllocValue v;
      if (!allocation_->Find(reinterpret_cast<void*>(*i), &v))  abort();
      v.bucket->allocs_ += adjust;
      v.bucket->alloc_size_ += adjust * int64(v.bytes);
      assert(v.bucket->allocs_ >= 0  &&  v.bucket->alloc_size_ >= 0);
      if (kMaxLogging  &&  adjust < 0) {
        HeapProfiler::MESSAGE(4, "HeapChecker: "
                              "Ignoring object of %"PRIuS" bytes\n", v.bytes);
      }
    }
  }
}

char* GetHeapProfile() {
  static const int size = 1 << 20;
  int buflen = 0;
  char* buf = reinterpret_cast<char*>(malloc(size));
  if (buf == NULL) {
    return NULL;
  }

  Bucket **list = NULL;
  while (true) {
    int nb = num_buckets + num_buckets / 16 + 8;

    if (list)
      delete[] list;

    list = new Bucket *[nb];

    if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Lock();
    if (!HeapProfiler::is_on_) {
      if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Unlock();
      break;
    }

    assert(table != NULL);
    if (num_buckets > nb) {
      if (!HeapProfiler::dump_for_leaks_) HeapProfiler::Unlock();
      continue;
    }

    int n = 0;
    for (int b = 0; b < kHashTableSize; b++) {
      for (Bucket* x = table[b]; x != 0; x = x->next_) {
        list[n++] = x;
      }
    }
    assert(n == num_buckets);
    std::sort(list, list + num_buckets, ByAllocatedSpace);

    buflen = snprintf(buf, size-1, "heap profile: ");
    buflen = HeapProfiler::UnparseBucket(buf, buflen, size-1,
                                         &HeapProfiler::total_);
    memset(&HeapProfiler::profile_, 0, sizeof(HeapProfiler::profile_));
    HeapProfiler::AdjustByIgnoredObjects(-1);  
    for (int i = 0; i < num_buckets; i++) {
      Bucket* b = list[i];
      buflen = HeapProfiler::UnparseBucket(buf, buflen, size-1, b);
    }
    HeapProfiler::AdjustByIgnoredObjects(1);  
    assert(buflen < size);
    if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Unlock();
    break;
  }

  buf[buflen] = '\0';
  delete[] list;

  return buf;
}

extern char* HeapProfile() {
  return GetHeapProfile();
}

void HeapProfiler::DumpLocked(const char *reason, const char* file_name) {
  assert(is_on_);

  if (filename_prefix_ == NULL  &&  file_name == NULL)  return;
  
  dumping_ = true;

  char fname[1000];
  if (file_name == NULL) {
    dump_count++;
    snprintf(fname, sizeof(fname), "%s.%04d.heap",
             filename_prefix_, dump_count);
    file_name = fname;
  }

  if (!dump_for_leaks_)  HeapProfiler::Unlock();
  {
    HeapProfiler::MESSAGE(dump_for_leaks_ ? 1 : 0,
                          "HeapProfiler: "
                          "Dumping heap profile to %s (%s)\n",
                          file_name, reason);
    FILE* f = fopen(file_name, "w");
    if (f != NULL) {
      const char* profile = HeapProfile();
      fputs(profile, f);
      free(const_cast<char*>(profile));

      fputs("\nMAPPED_LIBRARIES:\n", f);
      int maps = open("/proc/self/maps", O_RDONLY);
      if (maps >= 0) {
        char buf[100];
        ssize_t r;
        while ((r = read(maps, buf, sizeof(buf))) > 0) {
          fwrite(buf, 1, r, f);
        }
        close(maps);
      }

      fclose(f);
      f = NULL;
    } else {
      HeapProfiler::MESSAGE(0, "HeapProfiler: "
                            "FAILED Dumping heap profile to %s (%s)\n",
                            file_name, reason);
      if (dump_for_leaks_)  abort();  
    }
  }

  if (!dump_for_leaks_)  HeapProfiler::Lock();

  dumping_ = false;
}

void HeapProfilerDump(const char *reason) {
  if (HeapProfiler::is_on_ && (num_buckets > 0)) {

    HeapProfiler::Lock();
    if (!HeapProfiler::dumping_) {
      HeapProfiler::DumpLocked(reason, NULL);
    }
    HeapProfiler::Unlock();
  }
}

HeapProfiler::AllocationMap* self_disabled_allocation = NULL;
static const int kFirstAllocationNumBytes = 23;

void HeapProfiler::RecordAlloc(void* ptr, size_t bytes, int skip_count) {
 
  if (!done_first_alloc_) {
    done_first_alloc_ = true;
    assert(bytes == kFirstAllocationNumBytes);
    assert(strip_frames_ == 0);

    static const int kMaxStackTrace = 32;
    void* stack[kMaxStackTrace];
    int depth = GetStackTrace(stack, kMaxStackTrace, 1);

    int i;
    for (i = 0; i < depth; i++) {
      if (stack[i] == recordalloc_reference_stack_position_) {
        MESSAGE(1, "Determined strip_frames_ to be %d\n", i - 1);
        strip_frames_ = i - 1;
      }
    }
    if (strip_frames_ == 0) {
      MESSAGE(0, "Could not determine strip_frames_, aborting");
      abort();
    }
    return;
  }
  if (kMaxLogging) {
    HeapProfiler::MESSAGE(7, "HeapProfiler: Alloc: %p of %"PRIuS" from %d\n",
                          ptr, bytes, int(pthread_self()));
  }

  if (self_disable_  &&  self_disabled_tid_ == pthread_self()) {
    self_disabled_.allocs_++;
    self_disabled_.alloc_size_ += bytes;
    AllocValue v;
    v.bucket = NULL;  
    v.bytes = bytes;
    self_disabled_allocation->Insert(ptr, v);
    return;
  }

  HeapProfiler::Lock();
  if (is_on_) {
    Bucket* b = GetBucket(skip_count+1);
    b->allocs_++;
    b->alloc_size_ += bytes;
    total_.allocs_++;
    total_.alloc_size_ += bytes;

    AllocValue v;
    v.bucket = b;
    v.bytes = bytes;
    allocation_->Insert(ptr, v);

    if (kMaxLogging) {
      HeapProfiler::MESSAGE(8, "HeapProfiler: Alloc Recorded: %p of %""\n",
                            ptr, bytes);
    }

    const int64 inuse_bytes = total_.alloc_size_ - total_.free_size_;
    if (!dumping_) {
      bool need_dump = false;
      char buf[128];
      if (total_.alloc_size_ >=
          last_dump + FLAGS_heap_profile_allocation_interval) {
        snprintf(buf, sizeof(buf), "%"LLD" MB allocated",
                 total_.alloc_size_ >> 20);
        last_dump = total_.alloc_size_;
        need_dump = true;
      } else if(inuse_bytes >
                high_water_mark + FLAGS_heap_profile_inuse_interval) {
        sprintf(buf, "%" LLD" MB in use", inuse_bytes >> 20);
        high_water_mark = inuse_bytes;
        need_dump = true;
      }

      if (need_dump) {
        DumpLocked(buf, NULL);
      }
    }
  }
  HeapProfiler::Unlock();
}

void HeapProfiler::RecordFree(void* ptr) {
  if (kMaxLogging) {
    HeapProfiler::MESSAGE(7, "HeapProfiler: Free %p from %d\n",
                          ptr, int(pthread_self()));
  }

  if (self_disable_  &&  self_disabled_tid_ == pthread_self()) {
    AllocValue v;
    if (self_disabled_allocation->FindAndRemove(ptr, &v)) {
      self_disabled_.free_size_ += v.bytes;
      self_disabled_.frees_++;
    } else {
     
      self_disabled_.free_size_ += 100000000;
      self_disabled_.frees_ += 100000000;
    }
    return;
  }

  HeapProfiler::Lock();
  if (is_on_) {
    AllocValue v;
    if (allocation_->FindAndRemove(ptr, &v)) {
      Bucket* b = v.bucket;
      b->frees_++;
      b->free_size_ += v.bytes;
      total_.frees_++;
      total_.free_size_ += v.bytes;

      if (kMaxLogging) {
        HeapProfiler::MESSAGE(8, "HeapProfiler: Free Recorded: %p\n", ptr);
      }
    }
  }
  HeapProfiler::Unlock();
}

bool HeapProfiler::HaveOnHeapLocked(void** ptr, AllocValue* alloc_value) {
  assert(is_on_);
  const int kArraySizeOffset = sizeof(int);
  const int kStringOffset = sizeof(int) * 3;
  bool result = true;
  if (allocation_->Find(*ptr, alloc_value)) {
  } else if (allocation_->Find(reinterpret_cast<char*>(*ptr)
                               - kArraySizeOffset,
                               alloc_value)  &&
             alloc_value->bytes > kArraySizeOffset) {
    *ptr = reinterpret_cast<char*>(*ptr) - kArraySizeOffset;
    if (kMaxLogging) {
      HeapProfiler::MESSAGE(7, "HeapProfiler: Got poiter into %p at +%d\n",
                            ptr, kArraySizeOffset);
    }
  } else if (allocation_->Find(reinterpret_cast<char*>(*ptr)
                               - kStringOffset,
                               alloc_value)  &&
             alloc_value->bytes > kStringOffset) {
    *ptr = reinterpret_cast<char*>(*ptr) - kStringOffset;
    if (kMaxLogging) {
      HeapProfiler::MESSAGE(7, "HeapProfiler: Got poiter into %p at +%d\n",
                            ptr, kStringOffset);
    }
  } else {
    result = false;
  }
  return result;
}

bool HeapProfiler::HaveOnHeap(void** ptr, AllocValue* alloc_value) {
  HeapProfiler::Lock();
  bool result = is_on_  &&  HaveOnHeapLocked(ptr, alloc_value);
  HeapProfiler::Unlock();
  return result;
}

void HeapProfiler::NewHook(void* ptr, size_t size) {
  if (ptr != NULL) RecordAlloc(ptr, size, strip_frames_);
}

void HeapProfiler::DeleteHook(void* ptr) {
  if (ptr != NULL) RecordFree(ptr);
}

void HeapProfiler::MmapHook(void* result,
                            void* start, size_t size,
                            int prot, int flags,
                            int fd, off_t offset) {

  if (FLAGS_mmap_log) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "mmap(start=%p, len=%%"
            " , prot=0x%lx, flags=0x%x, "
             "fd=%d, offset=0x%x) = %u",
             start, size, prot, flags, fd, (unsigned int) offset,
             result);
  }

  if (result != (void*) MAP_FAILED &&
      FLAGS_mmap_profile &&
      is_on_) {

    RecordAlloc(result, size, strip_frames_);
  }
}

void HeapProfiler::MunmapHook(void* ptr, size_t size) {
  if (FLAGS_mmap_profile && is_on_) {
    RecordFree(ptr);
  }
  if (FLAGS_mmap_log) {
    char buf[200];
    snprintf(buf, sizeof(buf), "munmap(start=%p, len=%"PRIuS")", ptr, size);
    LOGF << buf;
  }
}

Bucket* HeapProfiler::GetBucket(int skip_count) {
  static const int kMaxStackTrace = 32;
  void* key[kMaxStackTrace];
  int depth = GetStackTrace(key, kMaxStackTrace, skip_count+1);

  uintptr_t h = 0;
  for (int i = 0; i < depth; i++) {
    uintptr_t pc = reinterpret_cast<uintptr_t>(key[i]);
    h = (h << 8) | (h >> (8*(sizeof(h)-1)));
    h += (pc * 31) + (pc * 7) + (pc * 3);
  }

  const size_t key_size = sizeof(key[0]) * depth;
  unsigned int buck = ((unsigned int) h) % kHashTableSize;
  for (Bucket* b = table[buck]; b != 0; b = b->next_) {
    if ((b->hash_ == h) &&
        (b->depth_ == depth) &&
        (memcmp(b->stack_, key, key_size) == 0)) {
      return b;
    }
  }

  void** kcopy = reinterpret_cast<void**>(Malloc(key_size));
  memcpy(kcopy, key, key_size);
  Bucket* b = reinterpret_cast<Bucket*>(Malloc(sizeof(Bucket)));
  memset(b, 0, sizeof(*b));
  b->hash_      = h;
  b->depth_     = depth;
  b->stack_     = kcopy;
  b->next_      = table[buck];
  table[buck] = b;
  num_buckets++;
  total_stack_depth += depth;
  return b;
}

void HeapProfiler::EarlyStartLocked() {
  assert(!is_on_);

  heap_profiler_memory.Init();

  is_on_ = true;
  if (need_for_leaks_)  abort();
  if (self_disable_)  abort();
  if (filename_prefix_ != NULL)  abort();

  const int table_bytes = kHashTableSize * sizeof(Bucket*);
  table = reinterpret_cast<Bucket**>(Malloc(table_bytes));
  memset(table, 0, table_bytes);

  void* aptr = Malloc(sizeof(AllocationMap));
  allocation_ = new (aptr) AllocationMap(Malloc, Free);

  memset(&total_, 0, sizeof(total_));
  num_buckets = 0;
  total_stack_depth = 0;
  last_dump = 0;
  MallocHk::SetNewHook(NewHook);

  void* here[2];
  GetStackTrace(here, 2, 1);
  recordalloc_reference_stack_position_ = here[0];
  done_first_alloc_ = false;
  void* first_alloc = malloc(kFirstAllocationNumBytes);
  free(first_alloc);

  MallocHk::SetDeleteHook(DeleteHook);
  HeapProfiler::MESSAGE(1, "HeapProfiler: Starting heap tracking\n");
}

void HeapProfiler::StartLocked(const char* prefix) {
  if (filename_prefix_ != NULL) return;

  if (!is_on_) {
    EarlyStartLocked();
  }

  const int prefix_length = strlen(prefix);
  filename_prefix_ = reinterpret_cast<char*>(Malloc(prefix_length + 1));
  memcpy(filename_prefix_, prefix, prefix_length);
  filename_prefix_[prefix_length] = '\0';
}

void HeapProfiler::StopLocked() {
  if (!is_on_) return;

  filename_prefix_ = NULL;

  if (need_for_leaks_)  return;

  MallocHk::SetNewHook(NULL);
  MallocHk::SetDeleteHook(NULL);
  heap_profiler_memory.Clear();

  table             = NULL;
  allocation_       = NULL;
  is_on_            = false;
}

void HeapProfiler::StartForLeaks() {
  Lock();

  if (!is_on_) {
    EarlyStartLocked();  
  }
  need_for_leaks_ = true;

  memset(&self_disabled_, 0, sizeof(self_disabled_)); 
  void* aptr = Malloc(sizeof(AllocationMap));
  self_disabled_allocation = new (aptr) AllocationMap(Malloc, Free);

  Unlock();
}

void HeapProfiler::StopForLeaks() {
  Lock();
  need_for_leaks_ = false;
  if (filename_prefix_ == NULL) StopLocked();
  Unlock();
}

void HeapProfilerStart(const char* prefix) {
  HeapProfiler::Lock();
  HeapProfiler::StartLocked(prefix);
  HeapProfiler::Unlock();
}

void HeapProfilerStop() {
  HeapProfiler::Lock();
  HeapProfiler::StopLocked();
  HeapProfiler::Unlock();
}

void HeapProfiler::Init() {
  if (init_has_been_called_)  return;
  init_has_been_called_ = true;
  MallocExt::Initialize();

  if (FLAGS_mmap_profile || FLAGS_mmap_log) {
    MallocHk::SetMmapHook(MmapHook);
    MallocHk::SetMunmapHook(MunmapHook);
  }
  char* heapprofile = getenv("HEAPPROFILE");
  if (!heapprofile || heapprofile[0] == '\0') {
    return;
  }
  if (getuid() != geteuid()) {
    HeapProfiler::MESSAGE(0, ("HeapProfiler: ignoring HEAPPROFILE because "
                              "program seems to be setuid\n"));
    return;
  }
  char fname[PATH_MAX];
  if (heapprofile[0] & 128) {                  
    snprintf(fname, sizeof(fname), "%c%s_%u",  
             heapprofile[0] & 127, heapprofile+1, (unsigned int)(getpid()));
  } else {
    snprintf(fname, sizeof(fname), "%s", heapprofile);
    heapprofile[0] |= 128;                    
  }
  CleanupProfiles(fname);
  HeapProfilerStart(fname);
}

void HeapProfiler::CleanupProfiles(const char* prefix) {
  if (!FLAGS_cleanup_old_heap_profiles)
    return;
  string pattern(prefix);
  pattern += ".*.heap";
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  if (r == 0 || r == GLOB_NOMATCH) {
    const int prefix_length = strlen(prefix);
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        HeapProfiler::MESSAGE(0, "HeapProfiler: "
                              "Removing old profile %s\n", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);
}

class HeapProfileEndWriter {
 public:
  ~HeapProfileEndWriter() {
    HeapProfilerDump("Exiting");
  }
};
static HeapProfileEndWriter heap_profile_end_writer;
