// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Simple Windows Audio interface
//
// This defines a very basic interface to Windows DirectSound for
// playing back PCM audio.

#pragma once

#include <string>
#include <memory>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
struct IUnknown;  // works around VS2019 compiler error in Windows SDK 8.1\um\combaseapi.h
#include <Windows.h>
#include <mmsystem.h>
#include <Shlwapi.h>

// undef WINNT for dsound.h to prevent duplicate definition
#undef WINNT
#include <dsound.h>


class SimpleWindowsAudio
{
public:
	// Set up the audio player.  uiHwnd is the window handle for the main
	// UI window, which DirectSound uses to mediate access to the audio
	// hardware from different applications.  If this is null, the DSound
	// uses the current foreground window by default.  The sample rate is
	// in PCM samples per second.  bufferSize_ms provides guidance for the
	// desired buffer size in terms of the amount of time, in milliseconds,
	// that the buffer should hold.
	SimpleWindowsAudio(HWND uiHwnd, int sampleRate, int nChannels, int bufferSize_ms);

	// Set the idle callback.  WriteAudioData() calls this when it's about
	// to enter a wait loop, to allow the program to carry out low-priority
	// or deferred tasks that can wait until the thread has to pause anyway
	// to let the audio playback catch up with buffered audio.
	void SetIdleTask(void (*idleTask)(void *), void *context);

	// Initialize DirectSound - returns true on success, false on failure.
	// Sets the error status string to a descriptive message on error.
	bool InitDirectSound();

	// destruction
	~SimpleWindowsAudio();

	// Get the error status string.  If an error occurs during setup
	// or playback, this will be set to a descriptive messages.  Returns
	// an empty string if there's no error.
	const char *GetErrorDesc() { return errorDesc.c_str(); }

	// Write audio data to the hardware buffer.  This blocks until there's
	// space available in the buffer, so this can be used to naturally pace
	// playback in real time, as long as the caller can produce samples
	// faster than they're played.
	void WriteAudioData(const INT16 *data, DWORD nSamples);

	// Set the volume.  The volume level uses the DirectSound units of
	// dB below reference level:  -100 (mute) to 0 (maximum).
	void SetVolume(int vol);

	// get the current volume setting
	int GetVolume() const { return volume; }

	// Get the sleep time.  This gets the recorded cumulative time spent
	// in OS sleep waits in WriteAudioData().
	double GetSleepTime() const { return sleepTime; }


protected:
	// create/destroy buffers
	bool CreateBuffers();
	void DestroyBuffers();

	// UI window
	HWND uiHwnd = NULL;

	// PCM sample rate
	int sampleRate = 0;

	// channels
	int nChannels = 0;

	// caller's desired buffer size in milliseconds
	int bufSize_ms = 0;

	// volume level, using the DirectSound units of dB below reference 
	// level (-100..0)
	int volume = 0;

	// cumulative sleep time waiting for buffer write space, in seconds
	double sleepTime = 0.0;

	// idle task callback and context
	void (*idleTask)(void*) = nullptr;
	void *idleTaskCtx = nullptr;

	// error string, if applicable
	std::string errorDesc;
	void FormatErrorDesc(const char *msg, ...);

	// DirectSound objects
	LPDIRECTSOUND dsound = nullptr;
	DSCAPS dsound_caps;

	// sound buffers
	LPDIRECTSOUNDBUFFER primary_buffer = nullptr;
	LPDIRECTSOUNDBUFFER stream_buffer = nullptr;
	UINT32 stream_buffer_size = 0;
	UINT32 stream_buffer_in = 0;

	// descriptors and formats
	DSBUFFERDESC primary_desc;
	DSBUFFERDESC stream_desc;
	WAVEFORMATEX primary_format;
	WAVEFORMATEX stream_format;

	// current buffer write position
	DWORD write_pos = 0;
};
