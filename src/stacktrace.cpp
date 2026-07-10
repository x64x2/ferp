#include <stdlib.h>

#undef IMPLEMENTED_STACK_TRACE

#if defined(__i386__) && defined(__linux) && !defined(NO_FRAME_POINTER)
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_x86-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && defined(__x86_64__) && HAVE_LIBUNWIND_H
#define IMPLEMENTED_STACK_TRACE
#define UNW_LOCAL_ONLY
#include "stacktrace_libunwind-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && defined(__x86_64__) && HAVE_UNWIND_H
// this implementation suffers from deadlocks. 
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_x86_64-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && !defined(__x86_64__) && HAVE_EXECINFO_H
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_generic-inl.h"
#endif

#ifndef IMPLEMENTED_STACK_TRACE
#endif
