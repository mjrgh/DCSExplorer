// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Windows implementation of OS-specific functions for DCS Encoder
//
// Include this module only for a WIN32 build.

#include "OSSpecific.h"

#if defined(_WIN32)
//
// Windows-specific
//

#include <crtdbg.h>
void OSInit()
{
	// disable buffer pre-fill for MSVC's secure "_s" functions, since (a)
	// pre-fill violates the documented behavior of some of the functions
	// (by modifying bytes beyond the intended range to be modified), and
	// (b) it's horribly slow with large buffers (such as our ROM images)
	_CrtSetDebugFillThreshold(0);
}

#endif // WIN32
