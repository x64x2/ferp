#ifndef TCMALLOC_PAGEMAP_H__
#define TCMALLOC_PAGEMAP_H__

#include <cstdint>
#include <cstring>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include "interlog.hpp"

template <int BITS>
class TCMalloc_PageMap1 {
 private:
  void** array_;

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap1(void* (*allocator)(size_t)) {
    array_ = reinterpret_cast<void**>((*allocator)(sizeof(void*) << BITS));
    memset(array_, 0, sizeof(void*) << BITS);
  }

  bool Ensure(Number x, size_t n) {
    return true;
  }

  void PreallocateMoreMemory() {}
  void* get(Number k) const {
    return array_[k];
  }
  void set(Number k, void* v) {
    array_[k] = v;
  }
};

template <int BITS>
class TCMalloc_PageMap2 {
 private:
  static const int ROOT_BITS = 5;
  static const int ROOT_LENGTH = 1 << ROOT_BITS;

  static const int LEAF_BITS = BITS - ROOT_BITS;
  static const int LEAF_LENGTH = 1 << LEAF_BITS;

  struct Leaf {
    void* values[LEAF_LENGTH];
  };

  Leaf* root_[ROOT_LENGTH];             
  void* (*allocator_)(size_t);         

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap2(void* (*allocator)(size_t)) {
    allocator_ = allocator;
    memset(root_, 0, sizeof(root_));
  }

  void* get(Number k) const {
    ASSERT(k >> BITS == 0);
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH-1);
    return root_[i1]->values[i2];
  }

  void set(Number k, void* v) {
    ASSERT(k >> BITS == 0);
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH-1);
    root_[i1]->values[i2] = v;
  }

  bool Ensure(Number start, size_t n) {
    for (Number key = start; key <= start + n - 1; ) {
      const Number i1 = key >> LEAF_BITS;

      if (root_[i1] == NULL) {
        Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
        if (leaf == NULL) return false;
        memset(leaf, 0, sizeof(*leaf));
        root_[i1] = leaf;
      }
      key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
    }
    return true;
  }

  void PreallocateMoreMemory() {
    Ensure(0, 1 << BITS);
  }
};

template <int BITS>
class TCMalloc_PageMap3 {
 private:
  static const int INTERIOR_BITS = (BITS + 2) / 3; 
  static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;
  static const int LEAF_BITS = BITS - 2*INTERIOR_BITS;
  static const int LEAF_LENGTH = 1 << LEAF_BITS;

  struct Node {
    Node* ptrs[INTERIOR_LENGTH];
  };

  struct Leaf {
    void* values[LEAF_LENGTH];
  };

  Node* root_;                         
  void* (*allocator_)(size_t);          

  Node* NewNode() {
    Node* result = reinterpret_cast<Node*>((*allocator_)(sizeof(Node)));
    if (result != NULL) {
      memset(result, 0, sizeof(*result));
    }
    return result;
  }

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap3(void* (*allocator)(size_t)) {
    allocator_ = allocator;
    root_ = NewNode();
  }

  void* get(Number k) const {
    ASSERT(k >> BITS == 0);
    const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
    const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH-1);
    const Number i3 = k & (LEAF_LENGTH-1);
    return reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3];
  }

  void set(Number k, void* v) {
    ASSERT(k >> BITS == 0);
    const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
    const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH-1);
    const Number i3 = k & (LEAF_LENGTH-1);
    reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
  }

  bool Ensure(Number start, size_t n) {
    for (Number key = start; key <= start + n - 1; ) {
      const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
      const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH-1);

      if (root_->ptrs[i1] == NULL) {
        Node* n = NewNode();
        if (n == NULL) return false;
        root_->ptrs[i1] = n;
      }

      if (root_->ptrs[i1]->ptrs[i2] == NULL) {
        Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
        if (leaf == NULL) return false;
        memset(leaf, 0, sizeof(*leaf));
        root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
      }

      key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
    }
    return true;
  }

  void PreallocateMoreMemory() {
  }
};
#endif  
