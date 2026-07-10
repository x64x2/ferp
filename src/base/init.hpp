#ifndef _INIT_HPP
#define _INIT_HPP

class Initializer {
 public:
  typedef void (*void_function)(void);
  Initializer(const char* name, void_function f) {
    f();
  }
};

#define REGISTER_MODULE_INITIALIZER(name,body)                     \
  static void_init_module_##name () { body; }               \
  Initializer_initializer_module_##name(#name,        \
          _init_module_##name)

#endif 