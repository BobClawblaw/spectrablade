/* Settings for building LAME's encoder library (libmp3lame) without its configure script.
   They hold for every compiler and processor SpectraBlade is built for (Apple clang on arm64
   and x86_64, MSVC on x64). The processor-specific shortcuts (assembly, SSE, the fast logarithm and
   the IEEE 754 hack) are left off, so both halves of a universal build encode alike. */
#ifndef SPECTRABLADE_LAME_CONFIG_H
#define SPECTRABLADE_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#ifndef _WIN32
#define HAVE_UNISTD_H 1
#endif

#define LAME_LIBRARY_BUILD 1
#define PACKAGE "lame"
#define VERSION "4.0"

/* The types configure would add. */
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;

#endif
