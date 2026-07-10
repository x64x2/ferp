#include "base/btype.hpp"
#include <bits/types/stack_t.h>
#include <stdio.h>
#include <stddef.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <malloc.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include "base/cmdlag.hpp"
#include "ferp/mallochk.hpp"
#include "ferp/malloc_ext.hpp"
#include "ferp/stacktrace.hpp"
#include "interlog.hpp"
#include "internl.hpp"
#include "pagemap.hpp"
#include "systemalloc.hpp"
#include "mthreads.hpp"

#if defined HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#endif

static const size_t kPageShift  = 12;
static const size_t kPageSize   = 1 << kPageShift;
static const size_t kMaxSize    = 8u * kPageSize;
static const size_t kAlignShift = 3;
static const size_t kAlignment  = 1 << kAlignShift;
static const size_t kNumClasses = 170;
static const size_t kPageMapBigAllocationThreshold = 128 << 20;

static const int kMinSystemAlloc = 1 << (20 - kPageShift);
static int num_objects_to_move[kNumClasses];
static const int kMaxFreeListLength = 256;
static const size_t kMinThreadCacheSize = kMaxSize * 2;
static const size_t kMaxThreadCacheSize = 2 << 20;
static const size_t kDefaultOverallThreadCacheSize = 16 << 20;
static const size_t kMaxPages = kMinSystemAlloc;
static unsigned int primes_list[] = {
	32771, 65537, 131101, 262147, 524309, 1048583,
	2097169, 4194319, 8388617, 16777259, 33554467 };

DEFINE_int64(tcmalloc_sample_parameter, 262147,
	     "Twice the approximate gap between sampling actions."
	     " Must be a prime number. Otherwise will be rounded up to a "
	     " larger prime number");
static size_t sample_period = 262147;
static SpinLock sample_period_lock = SPINLOCK_INITIALIZER;
static const int kSizeBits = 8 * sizeof(size_t);
static unsigned char size_base[kSizeBits];
static unsigned char size_shift[kSizeBits];
static size_t class_to_size[kNumClasses];
static size_t class_to_pages[kNumClasses];

struct TCEntry {
  void *head;
  void *tail;
};
static const int kNumTransferEntries = kNumClasses;

#if (defined __i386__ || defined __x86_64__) && defined __GNUC__
static inline int LgFloor(size_t n) {
  size_t result;
  __asm__("bsr  %1, %0"
          : "=r" (result)               
          : "ro" (n)                 
          : "cc"                      
          );
  return result;
}
#else
static inline int LgFloor(size_t n) {
  int log = 0;
  for (int i = 4; i >= 0; --i) {
    int shift = (1 << i);
    size_t x = n >> shift;
    if (x != 0) {
      n = x;
      log += shift;
    }
  }
  ASSERT(n == 1);
  return log;
}
#endif

static inline void *SLL_Next(void *t) {
  return *(reinterpret_cast<void**>(t));
}

static inline void SLL_SetNext(void *t, void *n) {
  *(reinterpret_cast<void**>(t)) = n;
}

static inline void SLL_Push(void **list, void *element) {
  SLL_SetNext(element, *list);
  *list = element;
}

static inline void *SLL_Pop(void **list) {
  void *result = *list;
  *list = SLL_Next(*list);
  return result;
}

static inline void SLL_PopRange(void **head, int N, void **start, void **end) {
  if (N == 0) {
    *start = NULL;
    *end = NULL;
    return;
  }

  void *tmp = *head;
  for (int i = 1; i < N; ++i) {
    tmp = SLL_Next(tmp);
  }

  *start = *head;
  *end = tmp;
  *head = SLL_Next(tmp);
  SLL_SetNext(tmp, NULL);
}

static inline void SLL_PushRange(void **head, void *start, void *end) {
  if (!start) return;
  SLL_SetNext(end, *head);
  *head = start;
}

static inline size_t SLL_Size(void *head) {
  int count = 0;
  while (head) {
    count++;
    head = SLL_Next(head);
  }
  return count;
}

static inline int SizeClass(size_t size) {
  if (size == 0) size = 1;
  const int lg = LgFloor(size);
  const int align = size_shift[lg];
  return static_cast<int>(size_base[lg]) + ((size-1) >> align);
}

static inline size_t ByteSizeForClass(size_t cl) {
  return class_to_size[cl];
}


static int NumMoveSize(size_t size) {
  if (size == 0) return 0;
  int num = static_cast<int>(64.0 * 1024.0 / size);
  if (num < 2) num = 2;
  if (num > static_cast<int>(0.8 * kMaxFreeListLength))
    num = static_cast<int>(0.8 * kMaxFreeListLength);
  // TODO:make thread cache free list sizes dynamic so that we do not ave to equally divide a fixed resource amongst lots of threads.
  if (num > 32) num = 32;

  return num;
}

static void InitSizeClasses() {
  for (int lg = 0; lg < kAlignShift; lg++) {
    size_base[lg] = 1;
    size_shift[lg] = kAlignShift;
  }

  int next_class = 1;
  int alignshift = kAlignShift;
  int last_lg = -1;
  for (size_t size = kAlignment; size <= kMaxSize; size += (1 << alignshift)) {
    int lg = LgFloor(size);
    if (lg > last_lg) {
      if ((lg >= 7) && (alignshift < 8)) {
        alignshift++;
      }
      size_base[lg] = next_class - ((size-1) >> alignshift);
      size_shift[lg] = alignshift;
    }

    class_to_size[next_class] = size;
    last_lg = lg;

    next_class++;
  }
  if (next_class >= kNumClasses) {
    MESSAGE("used up too many size classes: %d\n", next_class);
    abort();
  }

  for (size_t cl = 1; cl < next_class; cl++) {
    size_t psize = kPageSize;
    const size_t s = class_to_size[cl];
    while ((psize % s) > (psize >> 3)) {
      psize += kPageSize;
    }
    class_to_pages[cl] = psize >> kPageShift;
  }

  for (size_t size = 0; size <= kMaxSize; size++) {
    const int sc = SizeClass(size);
    if (sc == 0) {
      MESSAGE("Bad size class %d for %""\n", sc, size);
      abort();
    }
    if (sc > 1 && size <= class_to_size[sc-1]) {
      MESSAGE("Allocating unnecessarily large class %d for %"
              "\n", sc, size);
      abort();
    }
    if (sc >= kNumClasses) {
      MESSAGE("Bad size class %d for %""\n", sc, size);
      abort();
    }
    const size_t s = class_to_size[sc];
    if (size > s) {
      MESSAGE("Bad size %" " for %" " (sc = %d)\n", s, size, sc);
      abort();
    }
    if (s == 0) {
      MESSAGE("Bad size %" " for %" " (sc = %d)\n", s, size, sc);
      abort();
    }
  }

  for (size_t cl = 1; cl  < kNumClasses; ++cl) {
    num_objects_to_move[cl] = NumMoveSize(ByteSizeForClass(cl));
  }
}

static uint64_t metadata_system_bytes = 0;
static void* MetaDataAlloc(size_t bytes) {
  void* result = TCMalloc_SystemAlloc(bytes);
  if (result != NULL) {
    metadata_system_bytes += bytes;
  }
  return result;
}

template <class T>
class PageHeapAllocator {
 private:
  static const int kAllocIncrement = 128 << 10;
  static const size_t kAlignedSize
  = (((sizeof(T) + kAlignment - 1) / kAlignment) * kAlignment);
  char* free_area_;
  size_t free_avail_;
  void* free_list_;
  int inuse_;

 public:
  void Init() {
    ASSERT(kAlignedSize <= kAllocIncrement);
    inuse_ = 0;
    free_area_ = NULL;
    free_avail_ = 0;
    free_list_ = NULL;
    Delete(New());
  }

  T* New() {
    void* result;
    if (free_list_ != NULL) {
      result = free_list_;
      free_list_ = *(reinterpret_cast<void**>(result));
    } else {
      if (free_avail_ < kAlignedSize) {
        free_area_ = reinterpret_cast<char*>(MetaDataAlloc(kAllocIncrement));
        if (free_area_ == NULL) abort();
        free_avail_ = kAllocIncrement;
      }
      result = free_area_;
      free_area_ += kAlignedSize;
      free_avail_ -= kAlignedSize;
    }
    inuse_++;
    return reinterpret_cast<T*>(result);
  }

  void Delete(T* p) {
    *(reinterpret_cast<void**>(p)) = free_list_;
    free_list_ = p;
    inuse_--;
  }

  int inuse() const { return inuse_; }
};

typedef uintptr_t PageID;
typedef uintptr_t Length;

static inline Length pages(size_t bytes) {
  return ((bytes + kPageSize - 1) >> kPageShift);
}

static size_t AllocationSize(size_t bytes) {
  if (bytes > kMaxSize) {
    return pages(bytes) << kPageShift;
  } else {
    return ByteSizeForClass(SizeClass(bytes));
  }
}

struct Span {
  PageID        start;         
  Length        length;        
  Span*         next;          
  Span*         prev;          
  void*         objects;      
  unsigned int  free : 1;    
  unsigned int  sample : 1;    
  unsigned int  sizeclass : 8;  
  unsigned int  refcount : 11;  

#undef SPAN_HISTORY
#ifdef SPAN_HISTORY
  int nexthistory;
  char history[64];
  int value[64];
#endif
};

#ifdef SPAN_HISTORY
void Event(Span* span, char op, int v = 0) {
  span->history[span->nexthistory] = op;
  span->value[span->nexthistory] = v;
  span->nexthistory++;
  if (span->nexthistory == sizeof(span->history)) span->nexthistory = 0;
}
#else
#define Event(s,o,v) ((void) 0)
#endif

static PageHeapAllocator<Span> span_allocator;
static Span* NewSpan(PageID p, Length len) {
  Span* result = span_allocator.New();
  memset(result, 0, sizeof(*result));
  result->start = p;
  result->length = len;
#ifdef SPAN_HISTORY
  result->nexthistory = 0;
#endif
  return result;
}

static void DeleteSpan(Span* span) {
#ifndef NDEBUG
  memset(span, 0x3f, sizeof(*span));
#endif
  span_allocator.Delete(span);
}

static void DLL_Init(Span* list) {
  list->next = list;
  list->prev = list;
}

static void DLL_Remove(Span* span) {
  span->prev->next = span->next;
  span->next->prev = span->prev;
  span->prev = NULL;
  span->next = NULL;
}

static inline bool DLL_IsEmpty(const Span* list) {
  return list->next == list;
}

static int DLL_Length(const Span* list) {
  int result = 0;
  for (Span* s = list->next; s != list; s = s->next) {
    result++;
  }
  return result;
}

#if 0 
static void DLL_Print(const char* label, const Span* list) {
  MESSAGE("%-10s %p:", label, list);
  for (const Span* s = list->next; s != list; s = s->next) {
    MESSAGE(" <%p,%u,%u>", s, s->start, s->length);
  }
  MESSAGE("\n");
}
#endif

static void DLL_Prepend(Span* list, Span* span) {
  ASSERT(span->next == NULL);
  ASSERT(span->prev == NULL);
  span->next = list->next;
  span->prev = list;
  list->next->prev = span;
  list->next = span;
}

static void DLL_InsertOrdered(Span* list, Span* span) {
  ASSERT(span->next == NULL);
  ASSERT(span->prev == NULL);
  Span* x = list;
  while ((x->next != list) && (x->next->start < span->start)) {
    x = x->next;
  }
  span->next = x->next;
  span->prev = x;
  x->next->prev = span;
  x->next = span;
}

static const int kMaxStackDepth = 31;
struct StackTrace {
  uintptr_t size;          
  int       depth;       
  void*     stack[kMaxStackDepth];
};
static PageHeapAllocator<StackTrace> stacktrace_allocator;
static Span sampled_objects;

template <int BITS> class MapSelector {
 public:
  typedef TCMalloc_PageMap3<BITS-kPageShift> Type;
};

template <> class MapSelector<32> {
 public:
  typedef TCMalloc_PageMap2<32-kPageShift> Type;
};

class TCMalloc_PageHeap {
 public:
  TCMalloc_PageHeap();
  Span* New(Length n);
  void Delete(Span* span);
  void RegisterSizeClass(Span* span, size_t sc);
  Span* Split(Span* span, Length n);
  inline Span* GetDescriptor(PageID p) const {
    return reinterpret_cast<Span*>(pagemap_.get(p));
  }

  void Dump(TCMalloc_Printer* out);
  inline uint64_t SystemBytes() const { return system_bytes_; }
  uint64_t FreeBytes() const {
    return (static_cast<uint64_t>(free_pages_) << kPageShift);
  }

  bool Check();
  bool CheckList(Span* list, Length min_pages, Length max_pages);

 private:
  typedef MapSelector<8*sizeof(uintptr_t)>::Type PageMap;
  PageMap pagemap_;
  Span large_;
  Span free_[kMaxPages];
  uintptr_t free_pages_;
  uint64_t system_bytes_;

  bool GrowHeap(Length n);
  void Carve(Span* span, Length n);

  void RecordSpan(Span* span) {
    pagemap_.set(span->start, span);
    if (span->length > 1) {
      pagemap_.set(span->start + span->length - 1, span);
    }
  }
};

TCMalloc_PageHeap::TCMalloc_PageHeap() : pagemap_(MetaDataAlloc),
                                         free_pages_(0),
                                         system_bytes_(0) {
  DLL_Init(&large_);
  for (int i = 0; i < kMaxPages; i++) {
    DLL_Init(&free_[i]);
  }
}

Span* TCMalloc_PageHeap::New(Length n) {
  ASSERT(Check());
  if (n == 0) return NULL;

  for (Length s = n; s < kMaxPages; s++) {
    if (!DLL_IsEmpty(&free_[s])) {
      Span* result = free_[s].next;
      Carve(result, n);
      ASSERT(Check());
      free_pages_ -= n;
      return result;
    }
  }
  for (int i = 0; i < 2; i++) {
    Span *best = NULL;
    for (Span* span = large_.next; span != &large_; span = span->next) {
      if (span->length >= n &&
          (best == NULL || span->length < best->length)) {
        best = span;
      }
    }
    if (best != NULL) {
      Carve(best, n);
      ASSERT(Check());
      free_pages_ -= n;
      return best;
    }
    if (i == 0) {
      if (!GrowHeap(n)) {
        ASSERT(Check());
        return NULL;
      }
    }
  }
  return NULL;
}

Span* TCMalloc_PageHeap::Split(Span* span, Length n) {
  ASSERT(0 < n);
  ASSERT(n < span->length);
  ASSERT(!span->free);
  ASSERT(span->sizeclass == 0);
  Event(span, 'T', n);

  const int extra = span->length - n;
  Span* leftover = NewSpan(span->start + n, extra);
  Event(leftover, 'U', extra);
  RecordSpan(leftover);
  pagemap_.set(span->start + n - 1, span);
  span->length = n;

  return leftover;
}

void TCMalloc_PageHeap::Carve(Span* span, Length n) {
  ASSERT(n > 0);
  DLL_Remove(span);
  span->free = 0;
  Event(span, 'A', n);

  const int extra = span->length - n;
  ASSERT(extra >= 0);
  if (extra > 0) {
    Span* leftover = NewSpan(span->start + n, extra);
    leftover->free = 1;
    Event(leftover, 'S', extra);
    RecordSpan(leftover);
    if (extra < kMaxPages) {
      DLL_Prepend(&free_[extra], leftover);
    } else {
      DLL_InsertOrdered(&large_, leftover);
    }
    span->length = n;
    pagemap_.set(span->start + n - 1, span);
  }
}

void TCMalloc_PageHeap::Delete(Span* span) {
  ASSERT(Check());
  ASSERT(!span->free);
  ASSERT(span->length > 0);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start + span->length - 1) == span);
  span->sizeclass = 0;
  span->sample = 0;

  const PageID p = span->start;
  const Length n = span->length;
  Span* prev = GetDescriptor(p-1);
  if (prev != NULL && prev->free) {
    ASSERT(prev->start + prev->length == p);
    const Length len = prev->length;
    DLL_Remove(prev);
    DeleteSpan(prev);
    span->start -= len;
    span->length += len;
    pagemap_.set(span->start, span);
    Event(span, 'L', len);
  }
  Span* next = GetDescriptor(p+n);
  if (next != NULL && next->free) {
    ASSERT(next->start == p+n);
    const Length len = next->length;
    DLL_Remove(next);
    DeleteSpan(next);
    span->length += len;
    pagemap_.set(span->start + span->length - 1, span);
    Event(span, 'R', len);
  }
  Event(span, 'D', span->length);
  span->free = 1;
  if (span->length < kMaxPages) {
    DLL_Prepend(&free_[span->length], span);
  } else {
    DLL_InsertOrdered(&large_, span);
  }
  free_pages_ += n;
  ASSERT(Check());
}

void TCMalloc_PageHeap::RegisterSizeClass(Span* span, size_t sc) {
  ASSERT(!span->free);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start+span->length-1) == span);
  Event(span, 'C', sc);
  span->sizeclass = sc;
  for (Length i = 1; i < span->length-1; i++) {
    pagemap_.set(span->start+i, span);
  }
}

void TCMalloc_PageHeap::Dump(TCMalloc_Printer* out) {
  int nonempty_sizes = 0;
  for (int s = 0; s < kMaxPages; s++) {
    if (!DLL_IsEmpty(&free_[s])) nonempty_sizes++;
  }
  out->printf("------------------------------------------------\n");
  out->printf("PageHeap: %d sizes; %6.1f MB free\n", nonempty_sizes,
              (static_cast<double>(free_pages_) * kPageSize) / 1048576.0);
  out->printf("------------------------------------------------\n");
  uint64_t cumulative = 0;
  for (int s = 0; s < kMaxPages; s++) {
    if (!DLL_IsEmpty(&free_[s])) {
      const int list_length = DLL_Length(&free_[s]);
      uint64_t s_pages = s * list_length;
      cumulative += s_pages;
      out->printf("%6u pages * %6u spans ~ %6.1f MB; %6.1f MB cum\n",
                  s, list_length,
                  (s_pages << kPageShift) / 1048576.0,
                  (cumulative << kPageShift) / 1048576.0);
    }
  }

  uint64_t large_pages = 0;
  int large_spans = 0;
  for (Span* s = large_.next; s != &large_; s = s->next) {
    out->printf("   [ %6"" pages ]\n", s->length);
    large_pages += s->length;
    large_spans++;
  }
  cumulative += large_pages;
  out->printf(">255   large * %6u spans ~ %6.1f MB; %6.1f MB cum\n",
              large_spans,
              (large_pages << kPageShift) / 1048576.0,
              (cumulative << kPageShift) / 1048576.0);
}

static void RecordGrowth(size_t growth) {
  StackTrace* t = stacktrace_allocator.New();
  t->depth = GetStackTrace(t->stack, kMaxStackDepth-1, 3);
  t->size = growth;
  t->stack[kMaxStackDepth-1] = reinterpret_cast<void*>(growth);
  growth = t;
}

bool TCMalloc_PageHeap::GrowHeap(Length n) {
  ASSERT(kMaxPages >= kMinSystemAlloc);
  Length ask = (n>kMinSystemAlloc) ? n : static_cast<Length>(kMinSystemAlloc);
  void* ptr = TCMalloc_SystemAlloc(ask << kPageShift, kPageSize);
  if (ptr == NULL) {
    if (n < ask) {
      ask = n;
      ptr = TCMalloc_SystemAlloc(ask << kPageShift, kPageSize);
    }
    if (ptr == NULL) return false;
  }
  RecordGrowth(ask << kPageShift);

  uint64_t old_system_bytes = system_bytes_;
  system_bytes_ += (ask << kPageShift);
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  ASSERT(p > 0);

  if (old_system_bytes < kPageMapBigAllocationThreshold
      && system_bytes_ >= kPageMapBigAllocationThreshold) {
    pagemap_.PreallocateMoreMemory();
  }

  if (pagemap_.Ensure(p-1, ask+2)) {
    Span* span = NewSpan(p, ask);
    RecordSpan(span);
    Delete(span);
    ASSERT(Check());
    return true;
  } else {
    return false;
  }
}

bool TCMalloc_PageHeap::Check() {
  ASSERT(free_[0].next == &free_[0]);
  CheckList(&large_, kMaxPages, 1000000000);
  for (Length s = 1; s < kMaxPages; s++) {
    CheckList(&free_[s], s, s);
  }
  return true;
}

bool TCMalloc_PageHeap::CheckList(Span* list, Length min_pages, Length max_pages) {
  for (Span* s = list->next; s != list; s = s->next) {
    CHECK_CONDITION(s->free);
    CHECK_CONDITION(s->length >= min_pages);
    CHECK_CONDITION(s->length <= max_pages);
    CHECK_CONDITION(GetDescriptor(s->start) == s);
    CHECK_CONDITION(GetDescriptor(s->start+s->length-1) == s);
  }
  return true;
}

class TCMalloc_ThreadCache_FreeList {
 private:
  void*    list_;       
  uint16_t length_;    
  uint16_t lowater_;   

 public:
  void Init() {
    list_ = NULL;
    length_ = 0;
    lowater_ = 0;
  }

  int length() const {
    return length_;
  }

  bool empty() const {
    return list_ == NULL;
  }

  int lowwatermark() const { return lowater_; }
  void clear_lowwatermark() { lowater_ = length_; }

  void Push(void* ptr) {
    SLL_Push(&list_, ptr);
    length_++;
  }

  void* Pop() {
    ASSERT(list_ != NULL);
    length_--;
    if (length_ < lowater_) lowater_ = length_;
    return SLL_Pop(&list_);
  }

  void PushRange(int N, void *start, void *end) {
    SLL_PushRange(&list_, start, end);
    length_ += N;
  }

  void PopRange(int N, void **start, void **end) {
    SLL_PopRange(&list_, N, start, end);
    ASSERT(length_ >= N);
    length_ -= N;
    if (length_ < lowater_) lowater_ = length_;
  }
};

class TCMalloc_ThreadCache {
 private:
  typedef TCMalloc_ThreadCache_FreeList FreeList;

  size_t  size_;                  
  pthread_t  tid_;                 
  bool in_setspecific_;       
  FreeList list_[kNumClasses];     

  uint32_t      rnd_;                 
  size_t        bytes_until_sample_;   

 public:
  TCMalloc_ThreadCache* next_;
  TCMalloc_ThreadCache* prev_;

  void Init(pthread_t tid);
  void Cleanup();

  int freelist_length(size_t cl) const { return list_[cl].length(); }
  size_t Size() const { return size_; }

  void* Allocate(size_t size);
  void Deallocate(void* ptr, size_t size_class);
  void FetchFromCentralCache(size_t cl);
  void ReleaseToCentralCache(size_t cl, int N);
  void Scavenge();
  void Print() const;

  bool SampleAllocation(size_t k);
  void PickNextSample();

  static void  InitModule();
  static void  InitTSD();
  static TCMalloc_ThreadCache* GetCache();
  static TCMalloc_ThreadCache* GetCacheIfPresent();
  static void*   CreateCacheIfNecessary();
  static void  DeleteCache(void* ptr);
  static void RecomputeThreadCacheSize();
};

class TCMalloc_Central_FreeList {
 public:
  void Init(size_t cl);
  void InsertRange(void *start, void *end, int N);
  void RemoveRange(void **start, void **end, int *N);

  int length() {
    SpinLockHolder h(&lock_);
    return counter_;
  }

  int tc_length() {
    SpinLockHolder h(&lock_);
    return used_slots_ * num_objects_to_move[size_class_];
  }

 private:
  void* FetchFromSpans();
  void* FetchFromSpansSafe();
  void ReleaseListToSpans(void *start);
  void ReleaseToSpans(void* object);
  void Populate();
  bool MakeCacheSpace();
  static bool EvictRandomSizeClass(int locked_size_class, bool force);
  bool ShrinkCache(int locked_size_class, bool force);

  SpinLock lock_;
  size_t   size_class_;   
  Span   empty_;       
  Span   nonempty_;      
  size_t  counter_;        

  TCEntry tc_slots_[kNumTransferEntries];
  int32_t used_slots_;
  int32_t cache_size_;
};

class TCMalloc_Central_FreeListPadded : public TCMalloc_Central_FreeList {
 private:
  char pad_[(64 - (sizeof(TCMalloc_Central_FreeList) % 64)) % 64];
};

static TCMalloc_Central_FreeListPadded central_cache[kNumClasses];
static SpinLock pageheap_lock = SPINLOCK_INITIALIZER;
static char pageheap_memory[sizeof(TCMalloc_PageHeap)];
static bool phinited = false;

#define pageheap ((TCMalloc_PageHeap*) pageheap_memory)

static bool tsd_inited = false;
static pthread_key_t heap_key;
static PageHeapAllocator<TCMalloc_ThreadCache> threadheap_allocator;
static TCMalloc_ThreadCache* thread_heaps = NULL;
static int thread_heap_count = 0;
static size_t overall_thread_cache_size = kDefaultOverallThreadCacheSize;
static volatile size_t per_thread_cache_size = kMaxThreadCacheSize;

void TCMalloc_Central_FreeList::Init(size_t cl) {
  lock_.Init();
  size_class_ = cl;
  DLL_Init(&empty_);
  DLL_Init(&nonempty_);
  counter_ = 0;

  cache_size_ = 1;
  used_slots_ = 0;
  ASSERT(cache_size_ <= kNumTransferEntries);
}

void TCMalloc_Central_FreeList::ReleaseListToSpans(void* start) {
  while (start) {
    void *next = SLL_Next(start);
    ReleaseToSpans(start);
    start = next;
  }
}

void TCMalloc_Central_FreeList::ReleaseToSpans(void* object) {
  const PageID p = reinterpret_cast<uintptr_t>(object) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);
  ASSERT(span != NULL);
  ASSERT(span->refcount > 0);

  if (span->objects == NULL) {
    DLL_Remove(span);
    DLL_Prepend(&nonempty_, span);
    Event(span, 'N', 0);
  }

  if (false) {
    int got = 0;
    for (void* p = span->objects; p != NULL; p = *((void**) p)) {
      ASSERT(p != object);
      got++;
    }
    ASSERT(got + span->refcount ==
           (span->length<<kPageShift)/ByteSizeForClass(span->sizeclass));
  }

  counter_++;
  span->refcount--;
  if (span->refcount == 0) {
    Event(span, '#', 0);
    counter_ -= (span->length<<kPageShift) / ByteSizeForClass(span->sizeclass);
    DLL_Remove(span);

    lock_.Unlock();
    {
      SpinLockHolder h(&pageheap_lock);
      pageheap->Delete(span);
    }
    lock_.Lock();
  } else {
    *(reinterpret_cast<void**>(object)) = span->objects;
    span->objects = object;
  }
}

bool TCMalloc_Central_FreeList::EvictRandomSizeClass(
    int locked_size_class, bool force) {
  static int race_counter = 0;
  int t = race_counter++;  
  if (t >= kNumClasses) {
    while (t >= kNumClasses) {
      t -= kNumClasses;
    }
    race_counter = t;
  }
  ASSERT(t >= 0);
  ASSERT(t < kNumClasses);
  if (t == locked_size_class) return false;
  return central_cache[t].ShrinkCache(locked_size_class, force);
}

bool TCMalloc_Central_FreeList::MakeCacheSpace() {
  if (used_slots_ < cache_size_) return true;
  if (cache_size_ == kNumTransferEntries) return false;
  if (EvictRandomSizeClass(size_class_, false) ||
      EvictRandomSizeClass(size_class_, true)) {
    cache_size_++;
    return true;
  }
  return false;
}

namespace {
class LockInverter {
 private:
  TCMalloc_SpinLock *held_, *temp_;
 public:
  inline explicit LockInverter(TCMalloc_SpinLock* held, TCMalloc_SpinLock *temp)
    : held_(held), temp_(temp) { held_->Unlock(); temp_->Lock(); }
  inline ~LockInverter() { temp_->Unlock(); held_->Lock();  }
};
}

bool TCMalloc_Central_FreeList::ShrinkCache(int locked_size_class, bool force) {
  if (cache_size_ == 0) return false;
  if (force == false && used_slots_ == cache_size_) return false;

  LockInverter li(&central_cache[locked_size_class].lock_, &lock_);
  ASSERT(used_slots_ <= cache_size_);
  ASSERT(0 <= cache_size_);
  if (cache_size_ == 0) return false;
  if (used_slots_ == cache_size_) {
    if (force == false) return false;
    cache_size_--;
    used_slots_--;
    ReleaseListToSpans(tc_slots_[used_slots_].head);
    return true;
  }
  cache_size_--;
  return true;
}

void TCMalloc_Central_FreeList::InsertRange(void *start, void *end, int N) {
  SpinLockHolder h(&lock_);
  if (N == num_objects_to_move[size_class_] &&
    MakeCacheSpace()) {
    int slot = used_slots_++;
    ASSERT(slot >=0);
    ASSERT(slot < kNumTransferEntries);
    TCEntry *entry = &tc_slots_[slot];
    entry->head = start;
    entry->tail = end;
    return;
  }
  ReleaseListToSpans(start);
}

void TCMalloc_Central_FreeList::RemoveRange(void **start, void **end, int *N) {
  int num = *N;
  ASSERT(num > 0);

  SpinLockHolder h(&lock_);
  if (num == num_objects_to_move[size_class_] && used_slots_ > 0) {
    int slot = --used_slots_;
    ASSERT(slot >= 0);
    TCEntry *entry = &tc_slots_[slot];
    *start = entry->head;
    *end = entry->tail;
    return;
  }

  void *tail = FetchFromSpansSafe();
  if (!tail) {
    *start = *end = NULL;
    *N = 0;
    return;
  }

  SLL_SetNext(tail, NULL);
  void *head = tail;
  int count = 1;
  while (count < num) {
    void *t = FetchFromSpans();
    if (!t) break;
    SLL_Push(&head, t);
    count++;
  }
  *start = head;
  *end = tail;
  *N = count;
}

void* TCMalloc_Central_FreeList::FetchFromSpansSafe() {
  void *t = FetchFromSpans();
  if (!t) {
    Populate();
    t = FetchFromSpans();
  }
  return t;
}

void* TCMalloc_Central_FreeList::FetchFromSpans() {
  if (DLL_IsEmpty(&nonempty_)) return NULL;
  Span* span = nonempty_.next;

  ASSERT(span->objects != NULL);
  span->refcount++;
  void* result = span->objects;
  span->objects = *(reinterpret_cast<void**>(result));
  if (span->objects == NULL) {
    DLL_Remove(span);
    DLL_Prepend(&empty_, span);
    Event(span, 'E', 0);
  }
  counter_--;
  return result;
}

void TCMalloc_Central_FreeList::Populate() {
  lock_.Unlock();
  const size_t npages = class_to_pages[size_class_];

  Span* span;
  {
    SpinLockHolder h(&pageheap_lock);
    span = pageheap->New(npages);
    if (span) pageheap->RegisterSizeClass(span, size_class_);
  }
  if (span == NULL) {
    MESSAGE("allocation failed: %d\n", errno);
    lock_.Lock();
    return;
  }

  void** tail = &span->objects;
  char* ptr = reinterpret_cast<char*>(span->start << kPageShift);
  char* limit = ptr + (npages << kPageShift);
  const size_t size = ByteSizeForClass(size_class_);
  int num = 0;
  while (ptr + size <= limit) {
    *tail = ptr;
    tail = reinterpret_cast<void**>(ptr);
    ptr += size;
    num++;
  }
  ASSERT(ptr <= limit);
  *tail = NULL;
  span->refcount = 0; 

  lock_.Lock();
  DLL_Prepend(&nonempty_, span);
  counter_ += num;
}
inline bool TCMalloc_ThreadCache::SampleAllocation(size_t k) {
  if (bytes_until_sample_ < k) {
    PickNextSample();
    return true;
  } else {
    bytes_until_sample_ -= k;
    return false;
  }
}

void TCMalloc_ThreadCache::Init(pthread_t tid) {
  size_ = 0;
  next_ = NULL;
  prev_ = NULL;
  tid_  = tid;
  in_setspecific_ = false;
  for (size_t cl = 0; cl < kNumClasses; ++cl) {
    list_[cl].Init();
  }

  rnd_ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
  for (int i = 0; i < 100; i++) {
    PickNextSample();
  }
}

void TCMalloc_ThreadCache::Cleanup() {

  for (int cl = 0; cl < kNumClasses; ++cl) {
    if (list_[cl].length() > 0) {
      ReleaseToCentralCache(cl, list_[cl].length());
    }
  }
}

inline void* TCMalloc_ThreadCache::Allocate(size_t size) {
  ASSERT(size <= kMaxSize);
  const size_t cl = SizeClass(size);
  FreeList* list = &list_[cl];
  if (list->empty()) {
    FetchFromCentralCache(cl);
    if (list->empty()) return NULL;
  }
  size_ -= ByteSizeForClass(cl);
  return list->Pop();
}

inline void TCMalloc_ThreadCache::Deallocate(void* ptr, size_t cl) {
  size_ += ByteSizeForClass(cl);
  FreeList* list = &list_[cl];
  list->Push(ptr);
  if (list->length() > kMaxFreeListLength) {
    ReleaseToCentralCache(cl, num_objects_to_move[cl]);
  }
  if (size_ >= per_thread_cache_size) Scavenge();
}

void TCMalloc_ThreadCache::FetchFromCentralCache(size_t cl) {
  int fetch_count = num_objects_to_move[cl];
  void *start, *end;
  central_cache[cl].RemoveRange(&start, &end, &fetch_count);
  list_[cl].PushRange(fetch_count, start, end);
  size_ += ByteSizeForClass(cl) * fetch_count;
}

void TCMalloc_ThreadCache::ReleaseToCentralCache(size_t cl, int N) {
  ASSERT(N > 0);
  FreeList* src = &list_[cl];
  if (N > src->length()) N = src->length();
  size_ -= N*ByteSizeForClass(cl);

  int batch_size = num_objects_to_move[cl];
  while (N > batch_size) {
    void *tail, *head;
    src->PopRange(batch_size, &head, &tail);
    central_cache[cl].InsertRange(head, tail, batch_size);
    N -= batch_size;
  }
  void *tail, *head;
  src->PopRange(N, &head, &tail);
  central_cache[cl].InsertRange(head, tail, N);
}

void TCMalloc_ThreadCache::Scavenge() {
  for (int cl = 0; cl < kNumClasses; cl++) {
    FreeList* list = &list_[cl];
    const int lowmark = list->lowwatermark();
    if (lowmark > 0) {
      const int drop = (lowmark > 1) ? lowmark/2 : 1;
      ReleaseToCentralCache(cl, drop);
    }
    list->clear_lowwatermark();
  }
}

inline TCMalloc_ThreadCache* TCMalloc_ThreadCache::GetCache() {
  void* ptr = NULL;
  if (!tsd_inited) {
    InitModule();
  } else {
    ptr = perftools_pthread_getspecific(heap_key);
  }
  if (ptr == NULL) ptr = CreateCacheIfNecessary();
  return reinterpret_cast<TCMalloc_ThreadCache*>(ptr);
}

inline TCMalloc_ThreadCache* TCMalloc_ThreadCache::GetCacheIfPresent() {
  if (!tsd_inited) return NULL;
  return reinterpret_cast<TCMalloc_ThreadCache*>
    (perftools_pthread_getspecific(heap_key));
}

void TCMalloc_ThreadCache::PickNextSample() {
  static const uint32_t kPoly = (1 << 22) | (1 << 2) | (1 << 1) | (1 << 0);
  uint32_t r = rnd_;
  rnd_ = (r << 1) ^ ((static_cast<int32_t>(r) >> 31) & kPoly);
  const int flag_value = FLAGS_tcmalloc_sample_parameter;
  static int last_flag_value = -1;

  if (flag_value != last_flag_value) {
    SpinLockHolder h(&sample_period_lock);
    int i;
    for (i = 0; i < (sizeof(primes_list)/sizeof(primes_list[0]) - 1); i++) {
      if (primes_list[i] >= flag_value) {
        break;
      }
    }
    sample_period = primes_list[i];
    last_flag_value = flag_value;
  }
  bytes_until_sample_ = rnd_ % sample_period;
}

void TCMalloc_ThreadCache::InitModule() {
  SpinLockHolder h(&pageheap_lock);
  if (!phinited) {
    InitSizeClasses();
    threadheap_allocator.Init();
    span_allocator.Init();
    span_allocator.New(); 
    span_allocator.New(); 
    stacktrace_allocator.Init();
    DLL_Init(&sampled_objects);
    for (int i = 0; i < kNumClasses; ++i) {
      central_cache[i].Init(i);
    }
    new ((void*)pageheap_memory) TCMalloc_PageHeap;
    phinited = 1;
  }
}

void TCMalloc_ThreadCache::InitTSD() {
  ASSERT(!tsd_inited);
  perftools_pthread_key_create(&heap_key, DeleteCache);
  tsd_inited = true;

  //may have used a fake pthread_t for the main thread. 
  pthread_t zero;
  memset(&zero, 0, sizeof(zero));
  SpinLockHolder h(&pageheap_lock);
  for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
    if (h->tid_ == zero) {
      h->tid_ = pthread_self();
    }
  }
}

void* TCMalloc_ThreadCache::CreateCacheIfNecessary() {
  TCMalloc_ThreadCache* heap = NULL;
  {
    SpinLockHolder h(&pageheap_lock);
    pthread_t me;
    if (!tsd_inited) {
      memset(&me, 0, sizeof(me));
    } else {
      me = pthread_self();
    }

    for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
      if (h->tid_ == me) {
        heap = h;
        break;
      }
    }

    if (heap == NULL) {
      heap = threadheap_allocator.New();
      heap->Init(me);
      heap->next_ = thread_heaps;
      heap->prev_ = NULL;
      if (thread_heaps != NULL) thread_heaps->prev_ = heap;
      thread_heaps = heap;
      thread_heap_count++;
      RecomputeThreadCacheSize();
    }
  }

  if (!heap->in_setspecific_ && tsd_inited) {
    heap->in_setspecific_ = true;
    perftools_pthread_setspecific(heap_key, heap);
    heap->in_setspecific_ = false;
  }
  return heap;
}

void TCMalloc_ThreadCache::DeleteCache(void* ptr) {
  TCMalloc_ThreadCache* heap;
  heap = reinterpret_cast<TCMalloc_ThreadCache*>(ptr);
  heap->Cleanup();

  SpinLockHolder h(&pageheap_lock);
  if (heap->next_ != NULL) heap->next_->prev_ = heap->prev_;
  if (heap->prev_ != NULL) heap->prev_->next_ = heap->next_;
  if (thread_heaps == heap) thread_heaps = heap->next_;
  thread_heap_count--;
  RecomputeThreadCacheSize();

  threadheap_allocator.Delete(heap);
}

void TCMalloc_ThreadCache::RecomputeThreadCacheSize() {
  int n = thread_heap_count > 0 ? thread_heap_count : 1;
  size_t space = overall_thread_cache_size / n;

  if (space < kMinThreadCacheSize) space = kMinThreadCacheSize;
  if (space > kMaxThreadCacheSize) space = kMaxThreadCacheSize;

  per_thread_cache_size = space;
}

void TCMalloc_ThreadCache::Print() const {
  for (int cl = 0; cl < kNumClasses; ++cl) {
    MESSAGE("      %5"  " : %4d len; %4d lo\n",
            ByteSizeForClass(cl),
            list_[cl].length(),
            list_[cl].lowwatermark());
  }
}

struct TCMallocStats {
  uint64_t system_bytes;       
  uint64_t thread_bytes;       
  uint64_t central_bytes;      
  uint64_t transfer_bytes;     
  uint64_t pageheap_bytes;   
  uint64_t metadata_bytes;     
};

static void ExtractStats(TCMallocStats* r, uint64_t* class_count) {
  r->central_bytes = 0;
  r->transfer_bytes = 0;
  for (int cl = 0; cl < kNumClasses; ++cl) {
    const int length = central_cache[cl].length();
    const int tc_length = central_cache[cl].tc_length();
    r->central_bytes += static_cast<uint64_t>(ByteSizeForClass(cl)) * length;
    r->transfer_bytes +=
      static_cast<uint64_t>(ByteSizeForClass(cl)) * tc_length;
    if (class_count) class_count[cl] = length + tc_length;
  }

  r->thread_bytes = 0;
  { 
    SpinLockHolder h(&pageheap_lock);
    for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
      r->thread_bytes += h->Size();
      if (class_count) {
        for (int cl = 0; cl < kNumClasses; ++cl) {
          class_count[cl] += h->freelist_length(cl);
        }
      }
    }
  }

  { 
    SpinLockHolder h(&pageheap_lock);
    r->system_bytes = pageheap->SystemBytes();
    r->metadata_bytes = metadata_system_bytes;
    r->pageheap_bytes = pageheap->FreeBytes();
  }
}

static void DumpStats(TCMalloc_Printer* out, int level) {
  TCMallocStats stats;
  uint64_t class_count[kNumClasses];
  ExtractStats(&stats, (level >= 2 ? class_count : NULL));

  if (level >= 2) {
    out->printf("------------------------------------------------\n");
    uint64_t cumulative = 0;
    for (int cl = 0; cl < kNumClasses; ++cl) {
      if (class_count[cl] > 0) {
        uint64_t class_bytes = class_count[cl] * ByteSizeForClass(cl);
        cumulative += class_bytes;
        out->printf("class %3d [ %8"  " bytes ] : "
                "%8"  " objs; %5.1f MB; %5.1f cum MB\n",
                cl, ByteSizeForClass(cl),
                class_count[cl],
                class_bytes / 1048576.0,
                cumulative / 1048576.0);
      }
    }

    SpinLockHolder h(&pageheap_lock);
    pageheap->Dump(out);
  }

  const uint64_t bytes_in_use = stats.system_bytes
                                - stats.pageheap_bytes
                                - stats.central_bytes
                                - stats.transfer_bytes
                                - stats.thread_bytes;

  out->printf("------------------------------------------------\n"
              "MALLOC: %12"  " Heap size\n"
              "MALLOC: %12"  " Bytes in use by application\n"
              "MALLOC: %12"  " Bytes free in page heap\n"
              "MALLOC: %12"  " Bytes free in central cache\n"
              "MALLOC: %12"  " Bytes free in transfer cache\n"
              "MALLOC: %12"  " Bytes free in thread caches\n"
              "MALLOC: %12"  " Spans in use\n"
              "MALLOC: %12"  " Thread heaps in use\n"
              "MALLOC: %12"  " Metadata allocated\n"
              "------------------------------------------------\n",
              stats.system_bytes,
              bytes_in_use,
              stats.pageheap_bytes,
              stats.central_bytes,
              stats.transfer_bytes,
              stats.thread_bytes,
              uint64_t(span_allocator.inuse()),
              uint64_t(threadheap_allocator.inuse()),
              stats.metadata_bytes);
}

static void PrintStats(int level) {
  const int kBufferSize = 16 << 10;
  char* buffer = new char[kBufferSize];
  TCMalloc_Printer printer(buffer, kBufferSize);
  DumpStats(&printer, level);
  write(STDERR_FILENO, buffer, strlen(buffer));
  delete[] buffer;
}

static void** DumpStackTraces() {
  // Count how much space we need
  int needed_slots = 0;
  {
    SpinLockHolder h(&pageheap_lock);
    for (Span* s = sampled_objects.next; s != &sampled_objects; s = s->next) {
      StackTrace* stack = reinterpret_cast<StackTrace*>(s->objects);
      needed_slots += 3 + stack->depth;
    }
    needed_slots += 100;            // Slop in case sample grows
    needed_slots += needed_slots/8; // An extra 12.5% slop
  }

  void** result = new void*[needed_slots];
  if (result == NULL) {
    MESSAGE("tcmalloc: could not allocate %d slots for stack traces\n",
            needed_slots);
    return NULL;
  }

  SpinLockHolder h(&pageheap_lock);
  int used_slots = 0;
  for (Span* s = sampled_objects.next; s != &sampled_objects; s = s->next) {
    ASSERT(used_slots < needed_slots); 
    StackTrace* stack = reinterpret_cast<StackTrace*>(s->objects);
    if (used_slots + 3 + stack->depth >= needed_slots) {
      break;
    }

    result[used_slots+0] = reinterpret_cast<void*>(1);
    result[used_slots+1] = reinterpret_cast<void*>(stack->size);
    result[used_slots+2] = reinterpret_cast<void*>(stack->depth);
    for (int d = 0; d < stack->depth; d++) {
      result[used_slots+3+d] = stack->stack[d];
    }
    used_slots += 3 + stack->depth;
  }
  result[used_slots] = reinterpret_cast<void*>(0);
  return result;
}

static void** DumpHeapGrowthStackTraces() {
  int needed_slots = 0;
  {
    SpinLockHolder h(&pageheap_lock);
    for (StackTrace* t = stacktrace_allocator;
         t != NULL;
         t = reinterpret_cast<StackTrace*>(t->stack[kMaxStackDepth-1])) {
      needed_slots += 3 + t->depth;
    }
    needed_slots += 100;            // Slop in case list grows
    needed_slots += needed_slots/8; // An extra 12.5% slop
  }

  void** result = new void*[needed_slots];
  if (result == NULL) {
    MESSAGE("tcmalloc: could not allocate %d slots for stack traces\n",
            needed_slots);
    return NULL;
  }

  SpinLockHolder h(&pageheap_lock);
  int used_slots = 0;
  for (StackTrace* t = stacktrace_allocator;
       t != NULL;
       t = reinterpret_cast<StackTrace*>(t->stack[kMaxStackDepth-1])) {
    ASSERT(used_slots < needed_slots);  //need to leave room for faggot
    if (used_slots + 3 + t->depth >= needed_slots) {
      break;
    }

    result[used_slots+0] = reinterpret_cast<void*>(1);
    result[used_slots+1] = reinterpret_cast<void*>(t->size);
    result[used_slots+2] = reinterpret_cast<void*>(t->depth);
    for (int d = 0; d < t->depth; d++) {
      result[used_slots+3+d] = t->stack[d];
    }
    used_slots += 3 + t->depth;
  }
  result[used_slots] = reinterpret_cast<void*>(0);
  return result;
}

class TCMallocImplementation : public MallocExt {
 public:
  virtual void GetStats(char* buffer, int buffer_length) {
    ASSERT(buffer_length > 0);
    TCMalloc_Printer printer(buffer, buffer_length);
    if (buffer_length < 10000) {
      DumpStats(&printer, 1);
    } else {
      DumpStats(&printer, 2);
    }
  }

  virtual void** ReadStackTraces() {
    return DumpStackTraces();
  }

  virtual void** ReadHeapGrowthStackTraces() {
    return DumpHeapGrowthStackTraces();
  }

  virtual bool GetNumericProperty(const char* name, size_t* value) {
    ASSERT(name != NULL);

    if (strcmp(name, "generic.current_allocated_bytes") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.system_bytes
               - stats.thread_bytes
               - stats.central_bytes
               - stats.pageheap_bytes;
      return true;
    }

    if (strcmp(name, "generic.heap_size") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.system_bytes;
      return true;
    }

    if (strcmp(name, "tcmalloc.slack_bytes") == 0) {
      SpinLockHolder l(&pageheap_lock);
      *value = pageheap->FreeBytes();
      return true;
    }

    if (strcmp(name, "tcmalloc.max_total_thread_cache_bytes") == 0) {
      SpinLockHolder l(&pageheap_lock);
      *value = overall_thread_cache_size;
      return true;
    }

    if (strcmp(name, "tcmalloc.current_total_thread_cache_bytes") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.thread_bytes;
      return true;
    }

    return false;
  }

  virtual bool SetNumericProperty(const char* name, size_t value) {
    ASSERT(name != NULL);

    if (strcmp(name, "tcmalloc.max_total_thread_cache_bytes") == 0) {
      if (value < kMinThreadCacheSize) value = kMinThreadCacheSize;
      if (value > (1<<30)) value = (1<<30);   

      SpinLockHolder l(&pageheap_lock);
      overall_thread_cache_size = static_cast<size_t>(value);
      TCMalloc_ThreadCache::RecomputeThreadCacheSize();
      return true;
    }

    return false;
  }
};

static Span* DoSampledAllocation(size_t size) {
  SpinLockHolder h(&pageheap_lock);
  Span* span = pageheap->New(pages(size == 0 ? 1 : size));
  if (span == NULL) {
    return NULL;
  }

  StackTrace* stack = stacktrace_allocator.New();
  if (stack == NULL) {
    return span;
  }

  stack->depth = GetStackTrace(stack->stack, kMaxStackDepth, 1);
  stack->size = size;
  span->sample = 1;
  span->objects = stack;
  DLL_Prepend(&sampled_objects, span);
  return span;
}

static inline void* do_malloc(size_t size) {
  void* ret = NULL;

  if (TCMallocDebug::level >= TCMallocDebug::kVerbose) {
    MESSAGE("In tcmalloc do_malloc(%" ")\n", size);
  }
  TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCache();
  if ((FLAGS_tcmalloc_sample_parameter > 0) && heap->SampleAllocation(size)) {
    Span* span = DoSampledAllocation(size);
    if (span != NULL) {
      ret = reinterpret_cast<void*>(span->start << kPageShift);
    }
  } else if (size > kMaxSize) {
    SpinLockHolder h(&pageheap_lock);
    Span* span = pageheap->New(pages(size));
    if (span != NULL) {
      ret = reinterpret_cast<void*>(span->start << kPageShift);
    }
  } else {
    ret = heap->Allocate(size);
  }
  if (ret == NULL) errno = ENOMEM;
  return ret;
}

static inline void do_free(void* ptr) {
  if (TCMallocDebug::level >= TCMallocDebug::kVerbose)
    MESSAGE("In tcmalloc do_free(%p)\n", ptr);
  if (ptr == NULL) return;
  ASSERT(pageheap != NULL); 
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);

  ASSERT(span != NULL);
  ASSERT(!span->free);
  const size_t cl = span->sizeclass;
  if (cl != 0) {
    ASSERT(!span->sample);
    TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCacheIfPresent();
    if (heap != NULL) {
      heap->Deallocate(ptr, cl);
    } else {
      SLL_SetNext(ptr, NULL);
      central_cache[cl].InsertRange(ptr, ptr, 1);
    }
  } else {
    SpinLockHolder h(&pageheap_lock);
    ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
    ASSERT(span->start == p);
    if (span->sample) {
      DLL_Remove(span);
      stacktrace_allocator.Delete(reinterpret_cast<StackTrace*>(span->objects));
      span->objects = NULL;
    }
    pageheap->Delete(span);
  }
}
static void* do_memalign(size_t align, size_t size) {
  ASSERT((align & (align - 1)) == 0);
  ASSERT(align > 0);
  if (size + align < size) return NULL;        
  if (pageheap == NULL) TCMalloc_ThreadCache::InitModule();
  if (size == 0) size = 1;

  if (size <= kMaxSize && align < kPageSize) {
    int cl = SizeClass(size);
    while (cl < kNumClasses && ((class_to_size[cl] & (align - 1)) != 0)) {
      cl++;
    }
    if (cl < kNumClasses) {
      TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCache();
      return heap->Allocate(class_to_size[cl]);
    }
  }
  SpinLockHolder h(&pageheap_lock);

  if (align <= kPageSize) {
    Span* span = pageheap->New(pages(size));
    if (span == NULL) return NULL;
    return reinterpret_cast<void*>(span->start << kPageShift);
  }

  const int alloc = pages(size + align);
  Span* span = pageheap->New(alloc);
  if (span == NULL) return NULL;

  int skip = 0;
  while ((((span->start+skip) << kPageShift) & (align - 1)) != 0) {
    skip++;
  }
  ASSERT(skip < alloc);
  if (skip > 0) {
    Span* rest = pageheap->Split(span, skip);
    pageheap->Delete(span);
    span = rest;
  }

  const int needed = pages(size);
  ASSERT(span->length >= needed);
  if (span->length > needed) {
    Span* trailer = pageheap->Split(span, needed);
    pageheap->Delete(trailer);
  }
  return reinterpret_cast<void*>(span->start << kPageShift);
}

class TCMallocGuard {
 public:
  TCMallocGuard() {
    char *envval;
    if ((envval = getenv("TCMALLOC_DEBUG"))) {
      TCMallocDebug::level = atoi(envval);
      MESSAGE("Set tcmalloc debugging level to %d\n", TCMallocDebug::level);
    }
    do_free(do_malloc(1));
    TCMalloc_ThreadCache::InitTSD();
    do_free(do_malloc(1));
    MallocExt::Register(new TCMallocImplementation);
  }

  ~TCMallocGuard() {
    const char* env = getenv("MALLOCSTATS");
    if (env != NULL) {
      int level = atoi(env);
      if (level < 1) level = 1;
      PrintStats(level);
    }
  }
};

static TCMallocGuard module_enter_exit_hook;
extern "C" void* malloc(size_t size) {
  void* result = do_malloc(size);
  MallocHk::InvokeNewHook(result, size);
  return result;
}

extern "C" void free(void* ptr) {
  MallocHk::InvokeDeleteHook(ptr);
  do_free(ptr);
}

extern "C" void* calloc(size_t n, size_t elem_size) {
  const size_t size = n * elem_size;
  if (elem_size != 0 && size / elem_size != n) return NULL;

  void* result = do_malloc(size);
  if (result != NULL) {
    memset(result, 0, size);
  }
  MallocHk::InvokeNewHook(result, size);
  return result;
}

extern "C" void cfree(void* ptr) {
  MallocHk::InvokeDeleteHook(ptr);
  do_free(ptr);
}

extern "C" void* realloc(void* old_ptr, size_t new_size) {
  if (old_ptr == NULL) {
    void* result = do_malloc(new_size);
    MallocHk::InvokeNewHook(result, new_size);
    return result;
  }
  if (new_size == 0) {
    MallocHk::InvokeDeleteHook(old_ptr);
    do_free(old_ptr);
    return NULL;
  }

  const PageID p = reinterpret_cast<uintptr_t>(old_ptr) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);
  size_t old_size;
  if (span->sizeclass != 0) {
    old_size = ByteSizeForClass(span->sizeclass);
  } else {
    old_size = span->length << kPageShift;
  }

  if ((new_size > old_size) || (AllocationSize(new_size) < old_size)) {
    void* new_ptr = do_malloc(new_size);
    if (new_ptr == NULL) {
      return NULL;
    }
    MallocHk::InvokeNewHook(new_ptr, new_size);
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
    MallocHk::InvokeDeleteHook(old_ptr);
    do_free(old_ptr);
    return new_ptr;
  } else {
    return old_ptr;
  }
}

#ifndef COMPILER_INTEL
#define OP_THROWNOTHING
#define OP_THROWBADALLOC
#else
#define OP_THROWNOTHING throw()
#define OP_THROWBADALLOC throw(std::bad_alloc)
#endif

static SpinLock set_new_handler_lock = SPINLOCK_INITIALIZER;

static inline void* cpp_alloc(size_t size, bool nothrow) {
  for (;;) {
    void* p = do_malloc(size);
#ifdef PREANSINEW
    MallocHk::InvokeNewHook(p, size);
    return p;
#else
    if (p == NULL) { 
      std::new_handler nh;
      {
        SpinLockHolder h(&set_new_handler_lock);
        nh = std::set_new_handler(0);
        (void) std::set_new_handler(nh);
      }
      if (!nh) {
        if (nothrow) return 0;
        throw std::bad_alloc();
      }
      try {
        (*nh)();
      } catch (const std::bad_alloc&) {
        if (!nothrow) throw;
        MallocHk::InvokeNewHook(p, size);
        return p;
      }
    } else { 
      MallocHk::InvokeNewHook(p, size);
      return p;
    }
#endif
  }
}

void* operator new(size_t size) OP_THROWBADALLOC {
  return cpp_alloc(size, false);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept OP_THROWNOTHING {
  return cpp_alloc(size, true);
}

void operator delete(void* p) OP_THROWNOTHING {
  MallocHk::InvokeDeleteHook(p);
  do_free(p);
}

extern "C" void* memalign(size_t align, size_t size) {
  void* result = do_memalign(align, size);
  MallocHk::InvokeNewHook(result, size);
  return result;
}

extern "C" int posix_memalign(void** result_ptr, size_t align, size_t size) {
  if (((align % sizeof(void*)) != 0) ||
      ((align & (align - 1)) != 0) ||
      (align == 0)) {
    return EINVAL;
  }

  void* result = do_memalign(align, size);
  MallocHk::InvokeNewHook(result, size);
  if (result == NULL) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

static size_t pagesize = 0;

extern "C" void* valloc(size_t size) {
  if (pagesize == 0) pagesize = getpagesize();
  void* result = do_memalign(pagesize, size);
  MallocHk::InvokeNewHook(result, size);
  return result;
}

extern "C" void* pvalloc(size_t size) {
  if (pagesize == 0) pagesize = getpagesize();
  size = (size + pagesize - 1) & ~(pagesize - 1);
  void* result = do_memalign(pagesize, size);
  MallocHk::InvokeNewHook(result, size);
  return result;
}

extern "C" void malloc_stats(void) {
  PrintStats(1);
}

extern "C" int mallopt(int cmd, int value) {
  return 1;     
}

extern "C" struct mallinfo mallinfo(void) {
  TCMallocStats stats;
  ExtractStats(&stats, NULL);

  struct mallinfo info;
  memset(&info, 0, sizeof(info));
  info.arena     = static_cast<int>(stats.system_bytes);
  info.fsmblks   = static_cast<int>(stats.thread_bytes
                                    + stats.central_bytes
                                    + stats.transfer_bytes);
  info.fordblks  = static_cast<int>(stats.pageheap_bytes);
  info.uordblks  = static_cast<int>(stats.system_bytes
                                    - stats.thread_bytes
                                    - stats.central_bytes
                                    - stats.transfer_bytes
                                    - stats.pageheap_bytes);

  return info;
}

extern "C" {
#if defined(__GNUC__) && defined(HAVE___ATTRIBUTE__)
#define ALIAS(x) __attribute__ ((weak, alias (x)))
  void* __libc_malloc(size_t size)              ALIAS("malloc");
  void  __libc_free(void* ptr)                  ALIAS("free");
  void* __libc_realloc(void* ptr, size_t size)  ALIAS("realloc");
  void* __libc_calloc(size_t n, size_t size)    ALIAS("calloc");
  void  __libc_cfree(void* ptr)                 ALIAS("cfree");
  void* __libc_memalign(size_t align, size_t s) ALIAS("memalign");
  void* __libc_valloc(size_t size)              ALIAS("valloc");
  void* __libc_pvalloc(size_t size)             ALIAS("pvalloc");
  int __posix_memalign(void** r, size_t a, size_t s) ALIAS("posix_memalign");
#undef ALIAS
#else
  void* __libc_malloc(size_t size)              { return malloc(size);       }
  void  __libc_free(void* ptr)                  { free(ptr);                 }
  void* __libc_realloc(void* ptr, size_t size)  { return realloc(ptr, size); }
  void* __libc_calloc(size_t n, size_t size)    { return calloc(n, size);    }
  void  __libc_cfree(void* ptr)                 { cfree(ptr);                }
  void* __libc_memalign(size_t align, size_t s) { return memalign(align, s); }
  void* __libc_valloc(size_t size)              { return valloc(size);       }
  void* __libc_pvalloc(size_t size)             { return pvalloc(size);      }
  int __posix_memalign(void** r, size_t a, size_t s) {
    return posix_memalign(r, a, s);
  }
#endif
}

static void *MemalignOverride(size_t align, size_t size, const void *caller) {
  void* result = do_memalign(align, size);
  MallocHk::InvokeNewHook(result, size);
  return result;
}
void *(*__memalign_hook)(size_t, size_t, const void *) = MemalignOverride;
