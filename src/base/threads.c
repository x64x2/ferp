#include "lthreads.hpp"

#ifdef THREADS

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <asm/posix_types.h>
#include <asm/types.h>

#include "syscall.hpp"
#include "tlister.hpp"

#ifndef CLONE_UNTRACED
#define CLONE_UNTRACED 0x00800000
#endif

static char *local_itoa(char *buf, int i) {
  if (i < 0) {
    *buf++ = '-';
    return local_itoa(buf, -i);
  } else {
    if (i >= 10)
      buf = local_itoa(buf, i/10);
    *buf++ = (i%10) + '0';
    *buf   = '\000';
    return buf;
  }
}


#ifdef __GNUC__
#if __GNUC__ == 3 && __GNUC_MINOR__ >= 1 || __GNUC__ > 3

static int local_clone (int (*fn)(void *), void *arg, ...)
  __attribute__ ((noinline));
#endif
#endif

static int local_clone (int (*fn)(void *), void *arg, ...) {
  return clone(fn, (char *)&arg - 4096,
               CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_UNTRACED, arg);
}


static int local_atoi(const char *s) {
  int n   = 0;
  int neg = *s == '-';
  if (neg)
    s++;
  while (*s >= '0' && *s <= '9')
    n = 10*n + (*s++ - '0');
  return neg ? -n : n;
}

#define NO_INTR(fn)   do {} while ((fn) < 0 && errno == EINTR)

static int c_open(const char *fname, int flags, int mode) {
  ssize_t rc;
  NO_INTR(rc = sys_open(fname, flags, mode));
  return rc;
}

static volatile int *sig_pids, sig_num_threads, sig_proc, sig_marker;
static void SignalHandler(int signum, siginfo_t *si, void *data) {
  if (sig_pids != NULL) {
    if (signum == SIGABRT) {
      while (sig_num_threads-- > 0) {
        sys_sched_yield();
        sys_ptrace(PTRACE_KILL, sig_pids[sig_num_threads], 0, 0);
      }
    } else if (sig_num_threads > 0) {
      ResumeAllProcessThreads(sig_num_threads, (int *)sig_pids);
    }
  }
  sig_pids = NULL;
  if (sig_marker >= 0)
    NO_INTR(sys_close(sig_marker));
  sig_marker = -1;
  if (sig_proc >= 0)
    NO_INTR(sys_close(sig_proc));
  sig_proc = -1;

  sys__exit(signum == SIGABRT ? 1 : 2);
}

static void DirtyStack(size_t amount) {
  char buf[amount];
  memset(buf, 0, amount);
  sys_read(-1, buf, amount);
}

#define ALT_STACKSIZE (MINSIGSTKSZ + 4096)

struct ListerParams {
  int         result, err;
  char        *altstack_mem;
  ListAllProcessThreadsCallBack callback;
  void        *parameter;
  va_list     ap;
};


static void ListerThread(struct ListerParams *args) {
  static const int  signals[]  = { SIGABRT, SIGILL, SIGFPE, SIGSEGV, SIGBUS,
                                   SIGXCPU, SIGXFSZ };
  int               found_parent = 0;
  pid_t             clone_pid  = sys_gettid(), ppid = sys_getppid();
  char              proc_self_task[80], marker_name[48], *marker_path;
  const char        *proc_paths[3];
  const char *const *proc_path = proc_paths;
  int               proc = -1, marker = -1, num_threads = 0;
  int               max_threads = 0, sig;
  struct stat       marker_sb, proc_sb;
  stack_t           altstack;

  if ((marker = sys_socket(PF_LOCAL, SOCK_DGRAM, 0)) < 0 ||
      sys_fcntl(marker, F_SETFD, FD_CLOEXEC) < 0) {
  failure:
    args->result = -1;
    args->err    = errno;
    if (marker >= 0)
      NO_INTR(sys_close(marker));
    sig_marker = marker = -1;
    if (proc >= 0)
      NO_INTR(sys_close(proc));
    sig_proc = proc = -1;
    sys__exit(1);
  }

  local_itoa(strrchr(strcpy(proc_self_task, "/proc/"), '\000'), ppid);
  marker_path = strrchr(strcpy(marker_name, proc_self_task), '\000');
  strcat(proc_self_task, "/task/");
  proc_paths[0] = proc_self_task; 
  proc_paths[1] = "/proc/";   
  proc_paths[2] = NULL;

  local_itoa(strcpy(marker_path, "/fd/") + 4, marker);
  if (sys_stat(marker_name, &marker_sb) < 0) {
    goto failure;
  }

  memset(&altstack, 0, sizeof(altstack));
  altstack.ss_sp    = args->altstack_mem;
  altstack.ss_flags = 0;
  altstack.ss_size  = ALT_STACKSIZE;
  sys_sigaltstack(&altstack, (void *)NULL);

  sig_marker = marker;
  sig_proc   = -1;
  for (sig = 0; sig < sizeof(signals)/sizeof(*signals); sig++) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SignalHandler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags     = SA_ONSTACK|SA_SIGINFO|SA_RESETHAND;
    sys_sigaction(signals[sig], &sa, (void *)NULL);
  }
  
  for (;;) {
   
    if ((sig_proc = proc = c_open(*proc_path, O_RDONLY|O_DIRECTORY, 0)) < 0) {
      if (*++proc_path != NULL)
        continue;
      goto failure;
    }
    if (sys_fstat(proc, &proc_sb) < 0)
      goto failure;
    
    if (max_threads < proc_sb.st_nlink + 100)
      max_threads = proc_sb.st_nlink + 100;
    
      pid_t pids[max_threads];
      int   added_entries = 0;
      sig_num_threads     = num_threads;
      sig_pids            = pids;
      for (;;) {
        struct dirent *entry;
        char buf[4096];
        ssize_t nbytes = sys_getdents(proc, (struct dirent *)buf,
                                      sizeof(buf));
        if (nbytes < 0)
          goto failure;
        else if (nbytes == 0) {
          if (added_entries) {
         
            added_entries = 0;
            sys_lseek(proc, 0, SEEK_SET);
            continue;
          }
          break;
        }
        for (entry = (struct dirent *)buf;
             entry < (struct dirent *)&buf[nbytes];
             entry = (struct dirent *)((char *)entry + entry->d_reclen)) {
          if (entry->d_ino != 0) {
            const char *ptr = entry->d_name;
            pid_t pid;
            
            if (*ptr == '.')
              ptr++;
            
            if (*ptr < '0' || *ptr > '9')
              continue;
            pid = local_atoi(ptr);

            if (pid && pid != clone_pid) {
              struct stat tmp_sb;
              char fname[entry->d_reclen + 48];
              strcat(strcat(strcpy(fname, "/proc/"),
                            entry->d_name), marker_path);
            
              if (sys_stat(fname, &tmp_sb) >= 0 &&
                  marker_sb.st_ino == tmp_sb.st_ino) {
                long i, j;

  
                for (i = 0; i < num_threads; i++) {
                  if (pids[i] == pid) {
                    goto next_entry;
                  }
                }
                
                if (num_threads >= max_threads) {
                  NO_INTR(sys_close(proc));
                  goto detach_threads;
                }

                pids[num_threads++] = pid;
                sig_num_threads     = num_threads;
                if (sys_ptrace(PTRACE_ATTACH, pid, (void *)0,
                               (void *)0) < 0) {
                  num_threads--;
                  sig_num_threads = num_threads;
                  goto next_entry;
                }
                while (sys_waitpid(pid, (void *)0, __WALL) < 0) {
                  if (errno != EINTR) {
                    sys_ptrace_detach(pid);
                    num_threads--;
                    sig_num_threads = num_threads;
                    goto next_entry;
                  }
                }
                
                if (sys_ptrace(PTRACE_PEEKDATA, pid, &i, &j) || i++ != j ||
                    sys_ptrace(PTRACE_PEEKDATA, pid, &i, &j) || i   != j) {
                  sys_ptrace_detach(pid);
                  num_threads--;
                  sig_num_threads = num_threads;
                } else {
                  found_parent |= pid == ppid;
                  added_entries++;
                }
              }
            }
          }
        next_entry:;
        }
      }
      NO_INTR(sys_close(proc));
      sig_proc = proc = -1;

      if (num_threads > 1 || !*++proc_path) {
        NO_INTR(sys_close(marker));
        sig_marker = marker = -1;
        if (!found_parent) {
          ResumeAllProcessThreads(num_threads, pids);
          sys__exit(3);
        }

        args->result = args->callback(args->parameter, num_threads,
                                      pids, args->ap);
        args->err = errno;

        if (ResumeAllProcessThreads(num_threads, pids)) {
          args->err    = EINVAL;
          args->result = -1;
        }

        sys__exit(0);
      }
    detach_threads:
      ResumeAllProcessThreads(num_threads, pids);
      sig_pids = NULL;
      num_threads = 0;
      sig_num_threads = num_threads;
      max_threads += 100;
    }
}

int ListAllProcessThreads(void *parameter,
                          ListAllProcessThreadsCallBack callback, ...) {
  char                altstack_mem[ALT_STACKSIZE];
  struct ListerParams args;
  pid_t               clone_pid;
  int                 dumpable = 1;

  va_start(args.ap, callback);
  memset(altstack_mem, 0, sizeof(altstack_mem));
  DirtyStack(32768);
  dumpable = sys_prctl(PR_GET_DUMPABLE, 0);
  if (!dumpable)
    sys_prctl(PR_SET_DUMPABLE, 1);

  args.result       = -1;
  args.err          = 0;
  args.altstack_mem = altstack_mem;
  args.parameter    = parameter;
  args.callback     = callback;

  if ((clone_pid = local_clone((int (*)(void *))ListerThread, &args)) >= 0) {
    int status;
    while (sys_waitpid(clone_pid, &status, __WALL) < 0 &&
           errno == EINTR) {
    }
    if (WIFEXITED(status)) {
      switch (WEXITSTATUS(status)) {
      case 0: break;          
      case 2: args.err = EFAULT; 
              args.result = -1;
              break;
      case 3: args.err = EPERM;  
              args.result = -1;
              break;
      default:args.err = ECHILD; 
              args.result = -1;
              break;
      }
    } else if (!WIFEXITED(status)) {
      args.err    = EFAULT;    
      args.result = -1;
    }
  } else {
    args.result = -1;
    args.err    = errno;
  }

  if (!dumpable)
    sys_prctl(PR_SET_DUMPABLE, dumpable);
  va_end(args.ap);
  errno = args.err;
  return args.result;
}

int ResumeAllProcessThreads(int num_threads, pid_t *thread_pids) {
  int detached_at_least_one = 0;
  while (num_threads-- > 0) {
    detached_at_least_one |= sys_ptrace_detach(thread_pids[num_threads]) >= 0;
  }
  return detached_at_least_one;
}

#endif

