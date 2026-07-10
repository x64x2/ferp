#ifndef _ADDRESSMAP_HPP
#define _ADDRESSMAP_HPP

#include <cstdint>
#include <stddef.h>
#include <string.h>
#if defined HAVE_STDINT_H
#include <stdint.h>             
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           
#else
#include <sys/types.h>        
#endif

template <class Value>
class AddressMap {
 public:
  typedef void* (*Allocator)(size_t);
  typedef void  (*DeAllocator)(void*);
  typedef void* Key;
  typedef void  (*IteratorCallback)(Key, Value);

  AddressMap(Allocator alloc, DeAllocator dealloc);
  ~AddressMap();

  bool Find(Key key, Value* result);
  void Insert(Key key, Value value);
  bool FindAndRemove(Key key, Value* removed_value);
  void Iterate(IteratorCallback callback) const;

 private:
  typedef uintptr_t Number;

  static const int kBlockBits = 7;
  static const int kBlockSize = 1 << kBlockBits;

  struct Entry {
    Entry* next;
    Key    key;
    Value  value;
  };

  static const int kClusterBits = 13;
  static const int kClusterSize = 1 << (kBlockBits + kClusterBits);
  static const int kClusterBlocks = 1 << kClusterBits;

  struct Cluster {
    Cluster* next;                      
    Number   id;                        
    Entry*   blocks[kClusterBlocks];   
  };

  static const int kHashBits = 12;
  static const int kHashSize = 1 << 12;
  static const int ALLOC_COUNT = 64;

  Cluster**  hashtable_;     
  Entry*  free_;                 

  static const uint32_t kHashMultiplier = 2654435769u;
  static int HashInt(Number x) {
    const uint32_t m = static_cast<uint32_t>(x) * kHashMultiplier;
    return static_cast<int>(m >> (32 - kHashBits));
  }

  Cluster* FindCluster(Number address, bool create) {
    const Number cluster_id = address >> (kBlockBits + kClusterBits);
    const int h = HashInt(cluster_id);
    for (Cluster* c = hashtable_[h]; c != NULL; c = c->next) {
      if (c->id == cluster_id) {
        return c;
      }
    }

    if (create) {
      Cluster* c = New<Cluster>(1);
      c->id = cluster_id;
      c->next = hashtable_[h];
      hashtable_[h] = c;
      return c;
    }
    return NULL;
  }

  static int BlockID(Number address) {
    return (address >> kBlockBits) & (kClusterBlocks - 1);
  }

  struct Object {
    Object* next;
  };

  Allocator  alloc_;               
  DeAllocator  dealloc_;           
  Object*  allocated_;             

  template <class T> T* New(int num) {
    void* ptr = (*alloc_)(sizeof(Object) + num*sizeof(T));
    memset(ptr, 0, sizeof(Object) + num*sizeof(T));
    Object* obj = reinterpret_cast<Object*>(ptr);
    obj->next = allocated_;
    allocated_ = obj;
    return reinterpret_cast<T*>(reinterpret_cast<Object*>(ptr) + 1);
  }
};

template <class Value>
AddressMap<Value>::AddressMap(Allocator alloc, DeAllocator dealloc)
  : free_(NULL),
    alloc_(alloc),
    dealloc_(dealloc),
    allocated_(NULL) {
  hashtable_ = New<Cluster*>(kHashSize);
}

template <class Value>
AddressMap<Value>::~AddressMap() {
  for (Object* obj = allocated_; obj != NULL; ) {
    Object* next = obj->next;
    (*dealloc_)(obj);
    obj = next;
  }
}

template <class Value>
bool AddressMap<Value>::Find(Key key, Value* result) {
  const Number num = reinterpret_cast<Number>(key);
  const Cluster* const c = FindCluster(num, false);
  if (c != NULL) {
    for (const Entry* e = c->blocks[BlockID(num)]; e != NULL; e = e->next) {
      if (e->key == key) {
        *result = e->value;
        return true;
      }
    }
  }
  return false;
}

template <class Value>
void AddressMap<Value>::Insert(Key key, Value value) {
  const Number num = reinterpret_cast<Number>(key);
  Cluster* const c = FindCluster(num, true);

  const int block = BlockID(num);
  for (Entry* e = c->blocks[block]; e != NULL; e = e->next) {
    if (e->key == key) {
      e->value = value;
      return;
    }
  }

  if (free_ == NULL) {
    Entry* array = New<Entry>(ALLOC_COUNT);
    for (int i = 0; i < ALLOC_COUNT-1; i++) {
      array[i].next = &array[i+1];
    }
    array[ALLOC_COUNT-1].next = free_;
    free_ = &array[0];
  }
  Entry* e = free_;
  free_ = e->next;
  e->key = key;
  e->value = value;
  e->next = c->blocks[block];
  c->blocks[block] = e;
}

template <class Value>
bool AddressMap<Value>::FindAndRemove(Key key, Value* removed_value) {
  const Number num = reinterpret_cast<Number>(key);
  Cluster* const c = FindCluster(num, false);
  if (c != NULL) {
    for (Entry** p = &c->blocks[BlockID(num)]; *p != NULL; p = &(*p)->next) {
      Entry* e = *p;
      if (e->key == key) {
        *removed_value = e->value;
        *p = e->next;         
        e->next = free_;    
        free_ = e;
        return true;
      }
    }
  }
  return false;
}

template <class Value>
void AddressMap<Value>::Iterate(IteratorCallback callback) const {
  for (int h = 0; h < kHashSize; ++h) {
    for (const Cluster* c = hashtable_[h]; c != NULL; c = c->next) {
      for (int b = 0; b < kClusterBlocks; ++b) {
        for (const Entry* e = c->blocks[b]; e != NULL; e = e->next) {
          callback(e->key, e->value);
        }
      }
    }
  }
}

#endif 
