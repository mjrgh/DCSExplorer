// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder - platform-specific definitions
//
#pragma once

#include <stdlib.h>

// Array size macro.  Some compilers (e.g., MSVC) define this in stdlib.h,
// but it's not a standard macro, so define it if necessary.
#ifndef _countof
#define _countof(array) (sizeof(array)/sizeof((array)[0]))
#endif

