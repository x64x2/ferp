#include <assert.h>
#include <cstdint>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include "ferp/perftools/hash.hpp"
#include "ferp/malloc_ext.hpp"
#include "mthreads.hpp"

using STL_NAMESPACE::string;

void MallocExt::Initialize() {
  static bool initialize_called = false;

  if (initialize_called) return;
  initialize_called = true;
  setenv("GLIBCPP_FORCE_NEW", "1", false);
  setenv("GLIBCXX_FORCE_NEW", "1", false);
  std::string dummy("I need to be allocated");
  dummy += "!";         
}

MallocExt::~MallocExt() { }
bool MallocExt::VerifyAllMemory() { return true; }
bool MallocExt::VerifyNewMemory(void* p) { return true; }
bool MallocExt::VerifyArrayNewMemory(void* p) { return true; }
bool MallocExt::VerifyMallocMemory(void* p) { return true; }

bool MallocExt::GetNumericProperty(const char* property, size_t* value) {
  return false;
}

bool MallocExt::SetNumericProperty(const char* property, size_t value) {
  return false;
}

void MallocExt::GetStats(char* buffer, int length) {
  assert(length > 0);
  buffer[0] = '\0';
}

bool MallocExt::MallocMemoryStats(int* blocks, size_t* total,
                                       int histogram[kMallocHistogramSize]) {
  *blocks = 0;
  *total = 0;
  memset(histogram, sizeof(histogram), 0);
  return true;
}

void** MallocExt::ReadStackTraces() {
  return NULL;
}

void** MallocExt::ReadHeapGrowthStackTraces() {
  return NULL;
}

static pthread_once_t module_init = PTHREAD_ONCE_INIT;
static MallocExt* default_instance = NULL;
static MallocExt* current_instance = NULL;

static void InitModule() {
  default_instance = new MallocExt;
  current_instance = default_instance;
}

MallocExt* MallocExt::instance() {
  perftools_pthread_once(&module_init, InitModule);
  return current_instance;
}

void MallocExt::Register(MallocExt* implementation) {
  perftools_pthread_once(&module_init, InitModule);
  current_instance = implementation;
}

namespace {
  
  uintptr_t Count(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[0]);
}
uintptr_t Size(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[1]);
}
uintptr_t Depth(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[2]);
}

void* PC(void** entry, int i) {
  return entry[3+i];
}

struct StackTraceHash {
  size_t operator()(void** entry) const {
    uintptr_t h = 0;
    for (unsigned int i = 0; i < Depth(entry); i++) {
      uintptr_t pc = reinterpret_cast<uintptr_t>(PC(entry, i));
      h = (h << 8) | (h >> (8*(sizeof(h)-1)));
      h += (pc * 31) + (pc * 7) + (pc * 3);
    }
    return h;
  }
  bool operator()(void** entry1, void** entry2) const {
    if (Depth(entry1) != Depth(entry2))
      return Depth(entry1) < Depth(entry2);
    for (int i = 0; i < Depth(entry1); i++) {
      if (PC(entry1, i) != PC(entry2, i)) {
        return PC(entry1, i) < PC(entry2, i);
      }
    }
    return false;  
  }
  static const size_t bucket_size = 4;
  static const size_t min_buckets = 8;
};

struct StackTraceEqual {
  bool operator()(void** entry1, void** entry2) const {
    if (Depth(entry1) != Depth(entry2)) return false;
    for (int i = 0; i < Depth(entry1); i++) {
      if (PC(entry1, i) != PC(entry2, i)) {
        return false;
      }
    }
    return true;
  }
};

typedef HASH_NAMESPACE::hash_set<void**, StackTraceHash, StackTraceEqual> StackTraceTable;

void PrintHeader(string* result, const char* label, void** entries) 
{
  uintptr_t total_count = 0;
  uintptr_t total_size = 0;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    total_count += Count(entry);
    total_size += Size(entry);
  }

  char buf[200];
  snprintf(buf, sizeof(buf),
           "heap profile: %6lld: %8lld [%6lld: %8lld] @ %s\n",
           static_cast<long long>(total_count),
           static_cast<long long>(total_size),
           static_cast<long long>(total_count),
           static_cast<long long>(total_size),
           label);
  *result += buf;
}

void PrintStackEntry(string* result, void** entry) {
  char buf[100];
  snprintf(buf, sizeof(buf), "%6d: %8d [%6d: %8d] @",
           int(Count(entry)), int(Size(entry)),
           int(Count(entry)), int(Size(entry)));
  *result += buf;
  for (int i = 0; i < Depth(entry); i++) {
    snprintf(buf, sizeof(buf), " %p", PC(entry, i));
    *result += buf;
  }
  *result += "\n";
}

}

void MallocExt::GetHeapSample(string* result) {
  void** entries = ReadStackTraces();
  if (entries == NULL) {
    *result += "This malloc implementation does not support sampling.\n"
               "As of 2005/01/26, only tcmalloc supports sampling, and you\n"
               "are probably running a binary that does not use tcmalloc.\n";
    return;
  }

  StackTraceTable table;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    StackTraceTable::iterator iter = table.find(entry);
    if (iter == table.end()) {
      table.insert(entry);
    } else {
      void** canonical = *iter;
      canonical[0] = reinterpret_cast<void*>(Count(canonical) + Count(entry));
      canonical[1] = reinterpret_cast<void*>(Size(canonical) +  Size(entry));
    }
  }

  PrintHeader(result, "heap", entries);
  for (StackTraceTable::iterator iter = table.begin();
       iter != table.end();
       ++iter) {
    PrintStackEntry(result, *iter);
  delete[] entries;
}

void MallocExt::GetHeapGrowthStacks(std::string* result) {
  void** entries = ReadHeapGrowthStackTraces();
  if (entries == NULL) {
    *result += "This malloc implementation does not support "
               "ReadHeapGrowhStackTraces().\n"
               "As of 2005/09/27, only tcmalloc supports this, and you\n"
               "are probably running a binary that does not use tcmalloc.\n";
    return;
  }

  PrintHeader(result, "growth", entries);
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    PrintStackEntry(result, entry);
  }
  delete[] entries;
}
