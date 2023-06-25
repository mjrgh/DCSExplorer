// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Simple windows audio interface
// 
// This provides a basic interface to Direct Sound for playing back a
// PCM sample stream, with minimal work in the client program.
//

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <list>
#include <tchar.h>
#include "SimpleWindowsAudio.h"
#include "../HiResTimer/HiResTimer.h"

#pragma comment(lib, "HiResTimer")
#pragma comment(lib, "DSound")

// --------------------------------------------------------------------------
//
// High-precision timer
//
static HiResTimer hrt;

// --------------------------------------------------------------------------
// 
// Initialize
//
SimpleWindowsAudio::SimpleWindowsAudio(HWND uiHwnd, int sampleRate, int nChannels, int bufSize_ms) :
	uiHwnd(uiHwnd), sampleRate(sampleRate), nChannels(nChannels), bufSize_ms(bufSize_ms)
{
}

// Set the idle task
void SimpleWindowsAudio::SetIdleTask(void (*idleTask)(void *), void *context)
{
	this->idleTask = idleTask;
	this->idleTaskCtx = context;
}

// --------------------------------------------------------------------------
//
// Destruction
//
SimpleWindowsAudio::~SimpleWindowsAudio()
{
	// release buffers
	DestroyBuffers();

	// release the main DirectSound interface
	if (dsound != nullptr)
	{
		IDirectSound_Release(dsound);
		dsound = nullptr;
	}
}

// -----------------------------------------------------------------------
//
// Set the global audio volume
//
void SimpleWindowsAudio::SetVolume(int vol)
{
	// limit to -100dB to 0dB
	if (vol > 0)
		vol = 0;
	else if (vol < -100)
		vol = -100;

	// remember the volume internally
	volume = vol;
	
	// Set the volume in the stream buffer, if we have one.  Our volume
	// level is given in dB relative to the reference level (so 0 is the
	// maximum volume, and -100 is 100dB.  DirectSound uses the same
	// scale, but uses units of 0.01dB, so multiply our dB level by 100
	// to get the right units.
	if (stream_buffer != nullptr)
		stream_buffer->SetVolume(vol * 100);
}

// -----------------------------------------------------------------------
//
// Write samples to hardware audio buffer
//
void SimpleWindowsAudio::WriteAudioData(const INT16 *data, DWORD nSamples)
{
	// Wait for the section of the stream buffer we want to write
	// to become available.  Start by calculating the sections of
	// the buffer we want to write.
	DWORD ofs = write_pos;
	DWORD length_in_bytes = nSamples * sizeof(INT16);
	DWORD length1 = length_in_bytes, length2 = 0;
	if (ofs + length1 > stream_buffer_size)
	{
		length2 = ofs + length1 - stream_buffer_size;
		length1 -= length2;
	}
	double t0 = hrt.GetTime_seconds();
	bool waited = false;
	for (;;)
	{
		// read the current position
		DWORD playPos, writePos;
		if (!SUCCEEDED(stream_buffer->GetCurrentPosition(&playPos, &writePos)))
			return;

		// check if the play cursor is within the section we want to write
		int nWaitBytes;
		if (playPos >= ofs && playPos < ofs + length1)
		{
			// It's within the first chunk, so we have to finish out the
			// rest of the first chunk, plus the whole second chunk after
			// the pointer wraps back to the start of the buffer.
			nWaitBytes = ofs + length1 - playPos + length2;
		}
		else if (playPos >= 0 && playPos < length2)
		{
			// it's within the second chunk, so we have to wait out the
			// remainder of the second chunk
			nWaitBytes = length2 - playPos;
		}
		else
		{
			// the play pointer isn't within the write area, so we can
			// proceed with the write - break out of the spin loop
			break;
		}

		// perform idle-time spin tasks
		if (idleTask != nullptr)
			idleTask(idleTaskCtx);

		// We have to wait for the play pointer to move out of the write
		// region; as of the GetCurrentPosition() call, it had nWaitBytes
		// remaining bytes left to play.  To avoid hogging the CPU, do
		// a Windows sleep for a portion of the time.  Deduct a little bit
		// to account for the time that's already elapsed since the call
		// to GetCurrentPosition(), plus the overhead of performing the
		// sleep call.  Note that each audio sample is a pair of UINT16's
		// (16-bit sample size, in stereo pairs), so the read pointer
		// advances 4 bytes per audio sample clock.
		int ms = 1000 * (nWaitBytes / (sizeof(UINT16) * 2)) / sampleRate - 2;
		if (ms > 0)
			Sleep(ms);

		// note that a wait occurred
		waited = true;
	}

	// accumulate the wait time, if we had to wait
	if (waited)
		sleepTime += hrt.GetTime_seconds() - t0;

	// lock the stream buffer
	void *buffer1, *buffer2;
	DWORD copy_len = length_in_bytes;
	if (SUCCEEDED(stream_buffer->Lock(write_pos, copy_len, &buffer1, &length1, &buffer2, &length2, 0)))
	{
		// copy the first section
		DWORD cur_len = copy_len <= length1 ? copy_len : length1;
		memcpy(buffer1, data, cur_len);

		// account for the first copy
		data += cur_len/sizeof(UINT16);
		copy_len -= cur_len;

		// copy the second section, if there's more
		if (copy_len != 0)
		{
			// copy the second section, up to the available space
			cur_len = copy_len < length2 ? copy_len : length2;
			memcpy(buffer2, data, cur_len);

			// count the copy
			data += cur_len/sizeof(UINT16);
			copy_len -= cur_len;
		}

		// flag buffer overflows
		if (copy_len > 0)
			OutputDebugString(_T("hw_send_audio_data(): Audio buffer overflow\n"));

		// unlock the buffer
		stream_buffer->Unlock(buffer1, length1, buffer2, length2);

		// advance the write position
		write_pos += nSamples * sizeof(INT16);
		write_pos %= stream_buffer_size;
	}
}

// -----------------------------------------------------------------------
//
// Initialize the DirectSound layer
//
bool SimpleWindowsAudio::InitDirectSound()
{
	// if a UI window isn't set in the gUiHwnd global, pick a default window
	HWND hwnd = uiHwnd;
	if (hwnd == NULL && (hwnd = GetForegroundWindow()) == NULL)
		hwnd = GetDesktopWindow();

	// create the DirectSound interface, using the default audio output device
	HRESULT result = DirectSoundCreate(NULL, &dsound, NULL);
	if (result != DS_OK)
		return FormatErrorDesc("Error creating DirectSound: %08x\n", (UINT32)result), false;

	// get the capabilities
	dsound_caps.dwSize = sizeof(dsound_caps);
	result = IDirectSound_GetCaps(dsound, &dsound_caps);
	if (result != DS_OK)
		return FormatErrorDesc("Error getting DirectSound capabilities: %08x\n", (UINT32)result), false;

	// set the cooperative level
	if (hwnd != NULL)
	{
		result = dsound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
		if (result != DS_OK)
			return FormatErrorDesc("Error setting DirectSound cooperative level: %08x\n", (UINT32)result), false;
	}

	// make a format description for what we want
	stream_format.wBitsPerSample = 16;
	stream_format.wFormatTag = WAVE_FORMAT_PCM;
	stream_format.nChannels = nChannels;
	stream_format.nSamplesPerSec = sampleRate;
	stream_format.nBlockAlign = stream_format.wBitsPerSample * stream_format.nChannels / 8;
	stream_format.nAvgBytesPerSec = stream_format.nSamplesPerSec * stream_format.nBlockAlign;

	// Compute the buffer size in bytes:  start with one second
	// worth of samples times the sample size, then multiply
	// by the number of seconds in four internal frames.  We
	// try to bring the audio buffers up to date four times per
	// frame, so the aim is to keep the buffer about 3/4 full.
	stream_buffer_size = static_cast<UINT32>(
		1.0 * stream_format.nSamplesPerSec
		* stream_format.nBlockAlign
		* bufSize_ms / 1000.0);

	// round to the next higher 1k increment
	stream_buffer_size = ((stream_buffer_size + 1023) / 1024) * 1024;

	// create the buffers
	if (!CreateBuffers())
		return false;

	// set no attenuation (play at the native sample volume level)
	stream_buffer->SetVolume(DSBVOLUME_MAX);

	// start playback
	result = stream_buffer->Play(0, 0, DSBPLAY_LOOPING);
	if (result != DS_OK)
		return FormatErrorDesc("Error playing: %08x\n", (UINT32)result), false;

	// success
	return true;
}

// -----------------------------------------------------------------------
//
// Initialize Direct Sound and create audio buffers
//
bool SimpleWindowsAudio::CreateBuffers()
{
	HRESULT result;
	void *buffer;
	DWORD locked;

	// create a buffer desc for the primary buffer
	memset(&primary_desc, 0, sizeof(primary_desc));
	primary_desc.dwSize = sizeof(primary_desc);
	primary_desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_GETCURRENTPOSITION2;
	primary_desc.lpwfxFormat = NULL;

	// create the primary buffer
	result = IDirectSound_CreateSoundBuffer(dsound, &primary_desc, &primary_buffer, NULL);
	if (result != DS_OK)
		return FormatErrorDesc("Error creating primary DirectSound buffer: %08x\n", (UINT32)result), false;

	// attempt to set the primary format
	result = IDirectSoundBuffer_SetFormat(primary_buffer, &stream_format);
	if (result != DS_OK)
		return FormatErrorDesc("Error setting primary DirectSound buffer format: %08x\n", (UINT32)result), false;

	// get the primary format
	result = IDirectSoundBuffer_GetFormat(primary_buffer, &primary_format, sizeof(primary_format), NULL);
	if (result != DS_OK)
		return FormatErrorDesc("Error getting primary format: %08x\n", (UINT32)result), false;

	// create a buffer desc for the stream buffer
	memset(&stream_desc, 0, sizeof(stream_desc));
	stream_desc.dwSize = sizeof(stream_desc);
	stream_desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
	stream_desc.dwBufferBytes = stream_buffer_size;
	stream_desc.lpwfxFormat = &stream_format;

	// create the stream buffer
	result = IDirectSound_CreateSoundBuffer(dsound, &stream_desc, &stream_buffer, NULL);
	if (result != DS_OK)
		return FormatErrorDesc("Error creating DirectSound stream buffer: %08x\n", (UINT32)result), false;

	// lock the buffer
	result = stream_buffer->Lock(0, stream_buffer_size, &buffer, &locked, NULL, NULL, 0);
	if (result != DS_OK)
		return FormatErrorDesc("Error locking DirectSound stream buffer: %08x\n", (UINT32)result), false;

	// clear the buffer and unlock it
	memset(buffer, 0, locked);
	stream_buffer->Unlock(buffer, locked, NULL, 0);

	// success
	return true;
}

// -----------------------------------------------------------------------
//
// Release DirectSound resources
//
void SimpleWindowsAudio::DestroyBuffers()
{
	// release the stream buffer
	if (stream_buffer != nullptr)
	{
		// stop playback
		IDirectSoundBuffer_Stop(stream_buffer);

		// release the buffer
		IDirectSoundBuffer_Release(stream_buffer);
		stream_buffer = NULL;
	}

	// release the primary buffer
	if (primary_buffer != nullptr)
	{
		IDirectSoundBuffer_Release(primary_buffer);
		primary_buffer = NULL;
	}
}


// -----------------------------------------------------------------------
// 
// Format an error string
//
void SimpleWindowsAudio::FormatErrorDesc(const char *msg, ...)
{
	// format the message into a temp buffer
	char buf[512];
	va_list va;
	va_start(va, msg);
	sprintf_s(buf, msg, va);
	va_end(msg);

	// store it
	errorDesc = buf;
}
