// The version being built. The Makefile passes the three numbers (mingw32-make APP_VERSION=1.2.3, which the
// release workflow does for a tag like v1.2.3); a plain build is 1.0.0. The resource file reads this header too.
#pragma once

#ifndef MURMUR_VER_MAJOR
#define MURMUR_VER_MAJOR 1
#define MURMUR_VER_MINOR 0
#define MURMUR_VER_PATCH 0
#endif

#define MURMUR_STR_(x) #x
#define MURMUR_STR(x) MURMUR_STR_(x)
#define MURMUR_VERSION MURMUR_STR(MURMUR_VER_MAJOR.MURMUR_VER_MINOR.MURMUR_VER_PATCH)
#define MURMUR_FILE_VERSION MURMUR_STR(MURMUR_VER_MAJOR.MURMUR_VER_MINOR.MURMUR_VER_PATCH.0)
