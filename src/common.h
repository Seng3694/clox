#ifndef clox_common_h
#define clox_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEBUG_PRINT_CODE
#define DEBUG_TRACE_EXECUTION

#define DEBUG_STRESS_GC
//#define DEBUG_LOG_GC

#ifdef DEBUG_LOG_GC
#include <stdarg.h>
#include <stdio.h>
#define DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define DEBUG_LOG(...)
#endif

#define UINT8_COUNT (UINT8_MAX + 1)

#endif