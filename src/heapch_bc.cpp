#include <stdlib.h>      

bool heap_leak_checker_bcad_variable;

extern void HeapLeakCheckecker_BeforeConstructors();  
extern void HeapLeakCheckecker_AfterDestructors();  

class HeapLeakCheckGlobalPrePost {
 public:
  HeapLeakCheckGlobalPrePost() {
    if (count_ == 0)  HeapLeakCheckecker_BeforeConstructors();
    ++count_;
  }
  ~HeapLeakCheckGlobalPrePost() {
    if (count_ <= 0)  abort();
    --count_;
    if (count_ == 0)  HeapLeakCheckecker_AfterDestructors();
  }
 private:
  static int count_;
};

int HeapLeakCheckGlobalPrePost::count_ = 0;
static const HeapLeakCheckGlobalPrePost heap_leak_checker_global_pre_post;
