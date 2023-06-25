// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder - OS-specific definitions
//
// This defines portable interfaces to functions that must be implemented
// per platform.  Each platform-specific implementation should be separated
// into its own C++ module, so that the build script for each platform can
// simply include the relevant implementation module and ignore the ones
// for other platforms.

#pragma once

// OS-specific initialization.  Call once from main entrypoint.
void OSInit();
