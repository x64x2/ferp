#ifndef _HEAPPR_HPP
#define _HEAPPR_HPP

#include <stddef.h>

extern void HeapProfilerStart(const char* prefix);
extern void HeapProfilerStop();
extern void HeapProfilerDump(const char *reason);
extern char* GetHeapProfile();
extern void HeapProfilerSetLogLevel(int level);
extern void HeapProfilerSetAllocationInterval(size_t interval);
extern void HeapProfilerSetInuseInterval(size_t interval);

#endif 