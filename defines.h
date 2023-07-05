#ifndef DEFINES_H
#define DEFINES_H

#include <stdint.h>

typedef unsigned char byte;
typedef byte bool;
#define true 1
#define false 0

#if _WIN64
#define NVFBC_LIB_NAME "NvFBC64.dll"
#elif _WIN32
#define NVFBC_LIB_NAME "NvFBC.dll"
#else
#define NVFBC_LIB_NAME "libnvidia-fbc.so.1"
#endif

#endif