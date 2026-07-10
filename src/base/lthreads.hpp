#ifndef _LTHREADS_HPP
#define _LTHREADS_HPP

#if (defined(__i386__) || defined(__x86_64__) || defined(__ARM_ARCH_3__)) && \
    defined(__linux)

#define THREADS "linux /proc"
#endif
#endif 