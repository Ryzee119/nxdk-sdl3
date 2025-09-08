// SPDX-License-Identifier: CC0-1.0
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#ifndef SDL_build_config_nxdk_h_
#define SDL_build_config_nxdk_h_
#define SDL_build_config_h_

// Prevent inclusion of unwanted Windows calls
#define SDL_windows_h_
#undef _WIN32
#define TEXT(a) a

#include <SDL3/SDL_platform_defines.h>
#include <windows.h>

HMODULE GetModuleHandle(LPCSTR lpModuleName);
int WIN_SetError(const char *prefix);

// Use sync locks for spinlocks
#define HAVE_GCC_SYNC_LOCK_TEST_AND_SET 1

// Disable SDL dynamic linking
#define DYNAPI_NEEDS_DLOPEN 1

// Headers
#define HAVE_LIBC         1
#define HAVE_STDARG_H     1
#define HAVE_STDDEF_H     1
#define HAVE_STDINT_H     1
#define HAVE_STDIO_H      1
#define HAVE_STDLIB_H     1
#define HAVE_STRING_H     1
#define HAVE_MATH_H       1
#define HAVE_FLOAT_H      1
#define HAVE_LIMITS_H     1
#define LACKS_SYS_MMAN_H  1
#define LACKS_FCNTL_H     1
#define LACKS_SYS_PARAM_H 1
#define LACKS_SCHED_H     1

// stdlib.h
#define HAVE_MALLOC 1
#define HAVE__EXIT  1
// #define HAVE_ATOF   1
// #define HAVE_ATOI   1

// math.h
#define HAVE_ACOS              1
#define HAVE_ASIN              1
#define HAVE_ATAN              1
#define HAVE_ATAN2             1
#define HAVE_CEIL              1
#define HAVE_COS               1
#define HAVE_EXP               1
#define HAVE_FABS              1
#define HAVE_FLOOR             1
#define HAVE_FMOD              1
#define HAVE_ISINF             1
#define HAVE_ISINF_FLOAT_MACRO 1
#define HAVE_ISNAN             1
#define HAVE_ISNAN_FLOAT_MACRO 1
#define HAVE_LOG               1
#define HAVE_LOG10             1
#define HAVE_POW               1
#define HAVE_SIN               1
#define HAVE_SQRT              1
#define HAVE_TAN               1
#define HAVE_ACOSF             1
#define HAVE_ASINF             1
#define HAVE_ATANF             1
#define HAVE_ATAN2F            1
#define HAVE_CEILF             1
#define HAVE_COPYSIGN          1
// #define HAVE__COPYSIGN 1
#define HAVE_COSF 1
#define HAVE_EXPF 1
// #define HAVE_FABSF 1
#define HAVE_FLOORF 1
#define HAVE_FMODF  1
#define HAVE_LOGF   1
#define HAVE_LOG10F 1
#define HAVE_POWF   1
#define HAVE_SINF   1
#define HAVE_SQRTF  1
#define HAVE_TANF   1
#define HAVE_ABS    1

// string.h
#define HAVE_STRLEN 1
// #define HAVE__STRREV 1
#define HAVE_STRCHR  1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR  1
// #define HAVE_STRTOK_R
// #define HAVE__LTOA
// #define HAVE__ULTOA
#define HAVE_STRTOL  1
#define HAVE_STRTOUL 1
// #define HAVE_STRTOD 1
#define HAVE_MEMSET  1
#define HAVE_MEMCPY  1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP  1

// Platform Drivers
#define SDL_AUDIO_DRIVER_NXDK 1

#define SDL_CAMERA_DRIVER_DUMMY 1

#define SDL_DIALOG_DUMMY 1

#define SDL_FILESYSTEM_NXDK 1

#define SDL_FSOPS_NXDK 1

#define SDL_HAPTIC_DUMMY 1

#define SDL_JOYSTICK_XBOX 1

#define SDL_LOADSO_DUMMY 1

#define SDL_LOCALE_DUMMY 1

#define SDL_MISC_DUMMY 1

#define SDL_PROCESS_DUMMY 1

#define SDL_SENSOR_DUMMY 1

#define SDL_THREAD_WINDOWS               1
#define SDL_THREAD_GENERIC_RWLOCK_SUFFIX 1
#define SDL_THREAD_GENERIC_COND_SUFFIX   1

#define SDL_TIMER_NXDK 1

#define SDL_TIME_NXDK 1

#define SDL_TRAY_DUMMY 1

#define SDL_VIDEO_DRIVER_NXDK 1
// #define SDL_VIDEO_RENDER_SW 1
#define SDL_VIDEO_RENDER_XGU 1

// To avoid having to modify the SDL3 source code, we use the dummy driver
// to hook our own audio and video drivers.
#ifdef SDL_AUDIO_DRIVER_NXDK
#define SDL_AUDIO_DRIVER_DUMMY 1
#endif

#ifdef SDL_VIDEO_DRIVER_NXDK
#define SDL_VIDEO_DRIVER_DUMMY 1
#endif

#ifdef SDL_JOYSTICK_XBOX
#define SDL_JOYSTICK_DUMMY
#endif

#ifdef SDL_VIDEO_RENDER_XGU
#define SDL_VIDEO_RENDER_GPU      1
#define SDL_VIDEO_RENDER_NXDK_XGU 1
#endif

#endif /* SDL_build_config_nxdk_h_ */
