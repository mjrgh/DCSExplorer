// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Windows high-resolution timer
//

#include <Windows.h>
#include "HiResTimer.h"

// for timeGetTime()
#pragma comment(lib, "Winmm.lib")

HiResTimer::HiResTimer()
{
		// Get the performance counter frequency.  If successful, set up
		// to read times using the performance counter.  Otherwise read via
		// the low-res timer.
		LARGE_INTEGER freq;
		if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0)
		{
			// QueryPerformanceCounter is available - use it to calculate
			// times.  Calculate the time in microseconds per QPC tick and
			// store this for use when calculating time intervals.
			qpcAvailable = true;
			tickTime_sec = 1.0 / freq.QuadPart;
			tickTime_us = 1.0e6 / freq.QuadPart;
		}
		else
		{
			// QPC isn't available on this system, so fall back on the
			// low-res timer.  That reads in milliseconds (although it
			// doesn't necessarily have millisecond precision), so the
			// "tick" time is 1ms in this case.
			qpcAvailable = false;
			tickTime_sec = 1.0e-3;
			tickTime_us = 1000.0;
		}
}

void HiResTimer::SleepFor(double intervalInSeconds)
{
	SleepUntil(GetTime_seconds() + intervalInSeconds);
}

void HiResTimer::SleepUntil(double target)
{
	// Keep waiting until we reach the desired end time.  We start off
	// by deferring to a Windows scheduler wait for *most* of the desired
	// interval, since that doesn't burn CPU time.  But we do the last
	// little bit in a spin loop, so that we can get closer to exactly
	// the desired interval - the Windows scheduler wait doesn't have
	// very strong guarantees about precision.
	for (;;)
	{
		// figure the remaining time until the target
		double rem = target - GetTime_seconds();
		if (rem <= 0)
			break;

		// if we have a sleep time of at least 1ms, use the system sleep;
		// if we're under a millisecond, just spin
		if (rem > .01)
			Sleep(static_cast<DWORD>((rem * 1000) - 5));
	}
}
