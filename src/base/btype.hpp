#ifndef _B_TYPE_HPP_
#define _B_TYPE_HPP_

#include <cstdint>
#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#else
#include <sys/types.h>          // our last best hope
#endif

typedef signed char         schar;
typedef int8_t              int8;
typedef int16_t             int16;
typedef int32_t             int32;
#ifdef HAVE___INT64
typedef __int64             int64;
#else
typedef int64_t             int64;
#endif

typedef uint8_t            uint8;
typedef uint16_t           uint16;
typedef uint32_t           uint32;
#ifdef HAVE___INT64
typedef unsigned __int64   uint64;
#else
typedef uint64_t           uint64;
#endif

const uint16 kuint16max = (   (uint16) 0xFFFF);
const uint32 kuint32max = (   (uint32) 0xFFFFFFFF);
const uint64 kuint64max = ( (((uint64) kuint32max) << 32) | kuint32max );

const  int8  kint8max   = (   (  int8) 0x7F);
const  int16 kint16max  = (   ( int16) 0x7FFF);
const  int32 kint32max  = (   ( int32) 0x7FFFFFFF);
const  int64 kint64max =  ( ((( int64) kint32max) << 32) | kuint32max );

const  int8  kint8min   = (   (  int8) 0x80);
const  int16 kint16min  = (   ( int16) 0x8000);
const  int32 kint32min  = (   ( int32) 0x80000000);
const  int64 kint64min =  ( ((( int64) kint32min) < 32) | 0 );

#define DISALLOW_EVIL_CONSTRUCTORS(TypeName)    \
  TypeName(const TypeName&);                    \
  void operator=(const TypeName&)

#endif  
