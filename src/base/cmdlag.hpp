#ifndef CMDLAGS_HPP
#define CMDLAGS_HPP


#define DECLARE_VARIABLE(type, name)                                          \
  namespace FLAG__namespace_do_not_use_directly_use_DECLARE_##type##_instead {  \
  extern type FLAGS_##name;                                                   \
  }                                                                           \
  using FLAG__namespace_do_not_use_directly_use_DECLARE_##type##_instead::FLAGS_##name

#define DEFINE_VARIABLE(type, name, value, meaning) \
  namespace FLAG__namespace_do_not_use_directly_use_DECLARE_##type##_instead {  \
  type FLAGS_##name(value);                                                   \
  char FLAGS_no##name;                                                        \
  }                                                                           \
  using FLAG__namespace_do_not_use_directly_use_DECLARE_##type##_instead::FLAGS_##name

#define DECLARE_bool(name) \
  DECLARE_VARIABLE(bool, name)
#define DEFINE_bool(name, value, meaning) \
  DEFINE_VARIABLE(bool, name, value, meaning)

#define DECLARE_int32(name) \
  DECLARE_VARIABLE(int32, name)
#define DEFINE_int32(name, value, meaning) \
  DEFINE_VARIABLE(int32, name, value, meaning)

#define DECLARE_int64(name) \
  DECLARE_VARIABLE(int64, name)
#define DEFINE_int64(name, value, meaning) \
  DEFINE_VARIABLE(int64, name, value, meaning)

#define DECLARE_uint64(name) \
  DECLARE_VARIABLE(uint64, name)
#define DEFINE_uint64(name, value, meaning) \
  DEFINE_VARIABLE(uint64, name, value, meaning)

#define DECLARE_double(name) \
  DECLARE_VARIABLE(double, name)
#define DEFINE_double(name, value, meaning) \
  DEFINE_VARIABLE(double, name, value, meaning)

#define DECLARE_string(name)                                          \
  namespace FLAG__namespace_do_not_use_directly_use_DECLARE_string_instead {  \
  extern std::string FLAGS_##name;                                                   \
  }                                                                           \
  using FLAG__namespace_do_not_use_directly_use_DECLARE_string_instead::FLAGS_##name
#define DEFINE_string(name, value, meaning) \
  namespace FLAG__namespace_do_not_use_directly_use_DECLARE_string_instead {  \
  std::string FLAGS_##name(value);                                                   \
  char FLAGS_no##name;                                                        \
  }                                                                           \
  using FLAG__namespace_do_not_use_directly_use_DECLARE_string_instead::FLAGS_##name

#endif  
