#ifndef _FERP_PROFILE_HPP
#define _FERP_PROFILE_HPP

#include <time.h>       

extern bool ProfilerStart(const char* fname);
extern void ProfilerStop();
extern void ProfilerFlush();
extern void ProfilerEnable();
extern void ProfilerDisable();
extern bool ProfilingIsEnabledForAllThreads();
extern void ProfilerRegisterThread();

struct ProfilerState {
  bool   enabled;                
  time_t start_time;        
  char  profile_name[1024];     
  int  samples_gathered;      
};
extern void ProfilerGetCurrentState(ProfilerState* state);

class ProfilerThreadState {
 public:
  ProfilerThreadState() { }
  void ThreadCheck() { }
};
#endif 