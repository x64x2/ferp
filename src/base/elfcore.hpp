#ifndef _ELFCORE_HPP
#define _ELFCORE_HPP

#if (defined(__i386__) || defined(__x86_64__) || defined(__ARM_ARCH_3__)) && \
    defined(__linux)

#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>

#define DUMPER "ELF"

/* By the time that we get a chance to read CPU registers in the
 * calling thread, they are already in a not particularly useful
 * state. Besides, there will be multiple frames on the stack that are
 * just making the core file confusing. To fix this problem, we take a
 * snapshot of the frame pointer, stack pointer, and instruction
 * pointer at an earlier time, and then insert these values into the
 * core file.
 */

#if defined(__i386__) || defined(__x86_64__)
  typedef struct i386_regs {    /* Normal (non-FPU) CPU registers            */
  #ifdef __x86_64__
    #define BP rbp
    #define SP rsp
    #define IP rip
    uint64_t  r15,r14,r13,r12,rbp,rbx,r11,r10;
    uint64_t  r9,r8,rax,rcx,rdx,rsi,rdi,orig_rax;
    uint64_t  rip,cs,eflags;
    uint64_t  rsp,ss;
    uint64_t  fs_base, gs_base;
    uint64_t  ds,es,fs,gs;
  #else
    #define BP ebp
    #define SP esp
    #define IP eip
    uint32_t  ebx, ecx, edx, esi, edi, ebp, eax;
    uint16_t  ds, __ds, es, __es;
    uint16_t  fs, __fs, gs, __gs;
    uint32_t  orig_eax, eip;
    uint16_t  cs, __cs;
    uint32_t  eflags, esp;
    uint16_t  ss, __ss;
  #endif
  } i386_regs;
#elif defined(__ARM_ARCH_3__)
  typedef struct arm_regs {     /* General purpose registers                 */
    #define BP uregs[11]        /* Frame pointer                             */
    #define SP uregs[13]        /* Stack pointer                             */
    #define IP uregs[15]        /* Program counter                           */
    #define LR uregs[14]        /* Link register                             */
    long uregs[18];
  } arm_regs;
#endif

#if defined(__i386__) && defined(__GNUC__)
  /* On x86 we provide an optimized version of the FRAME() macro, if the
   * compiler supports a GCC-style asm() directive. This results in somewhat
   * more accurate values for CPU registers.
   */
  typedef struct Frame {
    struct i386_regs uregs;
    int              errno_;
    pid_t            tid;
  } Frame;
  #define FRAME(f) Frame f;                                           \
                   do {                                               \
                     f.errno_ = errno;                                \
                     f.tid    = sys_gettid();                         \
                     __asm__ volatile (                               \
                       "push %%ebp\n"                                 \
                       "push %%ebx\n"                                 \
                       "mov  %%ebx,0(%%eax)\n"                        \
                       "mov  %%ecx,4(%%eax)\n"                        \
                       "mov  %%edx,8(%%eax)\n"                        \
                       "mov  %%esi,12(%%eax)\n"                       \
                       "mov  %%edi,16(%%eax)\n"                       \
                       "mov  %%ebp,20(%%eax)\n"                       \
                       "mov  %%eax,24(%%eax)\n"                       \
                       "mov  %%ds,%%ebx\n"                            \
                       "mov  %%ebx,28(%%eax)\n"                       \
                       "mov  %%es,%%ebx\n"                            \
                       "mov  %%ebx,32(%%eax)\n"                       \
                       "mov  %%fs,%%ebx\n"                            \
                       "mov  %%ebx,36(%%eax)\n"                       \
                       "mov  %%gs,%%ebx\n"                            \
                       "mov  %%ebx, 40(%%eax)\n"                      \
                       "call 0f\n"                                    \
                     "0:pop %%ebx\n"                                  \
                       "add  $1f-0b,%%ebx\n"                          \
                       "mov  %%ebx,48(%%eax)\n"                       \
                       "mov  %%cs,%%ebx\n"                            \
                       "mov  %%ebx,52(%%eax)\n"                       \
                       "pushf\n"                                      \
                       "pop  %%ebx\n"                                 \
                       "mov  %%ebx,56(%%eax)\n"                       \
                       "mov  %%esp,%%ebx\n"                           \
                       "add  $8,%%ebx\n"                              \
                       "mov  %%ebx,60(%%eax)\n"                       \
                       "mov  %%ss,%%ebx\n"                            \
                       "mov  %%ebx,64(%%eax)\n"                       \
                       "pop  %%ebx\n"                                 \
                       "pop  %%ebp\n"                                 \
                     "1:"                                             \
                       : : "a" (&f) : "memory");                      \
                     } while (0)
  #define SET_FRAME(f,r)                                              \
                     do {                                             \
                       errno = (f).errno_;                            \
                       (r)   = (f).uregs;                             \
                     } while (0)
#elif defined(__ARM_ARCH_3__) && defined(__GNUC__)
  /* ARM calling conventions are a little more tricky. A little assembly
   * helps in obtaining an accurate snapshot of all registers.
   */
  typedef struct Frame {
    struct arm_regs arm;
    int             errno_;
    pid_t           tid;
  } Frame;
  #define FRAME(f) Frame f;                                           \
                   do {                                               \
                     long cpsr;                                       \
                     f.errno_ = errno;                                \
                     f.tid    = sys_gettid();                         \
                     __asm__ volatile(                                \
                       "stmia %0, {r0-r15}\n" /* All integer regs   */\
                       : : "r"(&f.arm) : "memory");                   \
                     f.arm.uregs[16] = 0;                             \
                     __asm__ volatile(                                \
                       "mrs %0, cpsr\n"       /* Condition code reg */\
                       : "=r"(cpsr));                                 \
                     f.arm.uregs[17] = cpsr;                          \
                   } while (0)
  #define SET_FRAME(f,r)                                              \
                     do {                                             \
                       /* Don't override the FPU status register.   */\
                       /* Use the value obtained from ptrace(). This*/\
                       /* works, because our code does not perform  */\
                       /* any FPU operations, itself.               */\
                       long fps      = (f).arm.uregs[16];             \
                       errno         = (f).errno_;                    \
                       (r)           = (f).arm;                       \
                       (r).uregs[16] = fps;                           \
                     } while (0)
#else
  /* If we do not have a hand-optimized assembly version of the FRAME()
   * macro, we cannot reliably unroll the stack. So, we show a few additional
   * stack frames for the coredumper.
   */
  typedef struct Frame {
    pid_t tid;
  } Frame;
  #define FRAME(f) Frame f; do { f.tid = sys_gettid(); } while (0)
  #define SET_FRAME(f,r) do { } while (0)
#endif

int InternalGetCoreDump(void *frame, int num_threads, pid_t *thread_pids,
                        va_list ap
                     /* const char *PATH,
                        const struct CoredumperCompressor *compressors,
                        const struct CoredumperCompressor **selected_comp */);

#endif

#endif 