#ifndef _FERP_MALLOC_EXT_HPP__
#define _FERP_MALLOC_EXT_HPP__

#include <stddef.h>
#include <string>

static const int kMallocHistogramSize = 64;

class MallocExt {
 public:
  virtual ~MallocExt();
  static void Initialize();

  virtual bool VerifyAllMemory();
  virtual bool VerifyNewMemory(void* p);
  virtual bool VerifyArrayNewMemory(void* p);
  virtual bool VerifyMallocMemory(void* p);
  virtual bool MallocMemoryStats(int* blocks, size_t* total,
                                 int histogram[kMallocHistogramSize]);

  
  virtual void GetStats(char* buffer, int buffer_length);
  virtual void GetHeapSample(std::string* result);
  virtual void GetHeapGrowthStacks(std::string* result);  
  virtual bool GetNumericProperty(const char* property, size_t* value);
  virtual bool SetNumericProperty(const char* property, size_t value);

  static MallocExt* instance();
  static void Register(MallocExt* implementation);

 protected:
  virtual void** ReadStackTraces();
  virtual void** ReadHeapGrowthStackTraces();
};

#endif 
