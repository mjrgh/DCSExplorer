// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder Tester
//
// This is a simple command-line tool for exercising the DCSEncoder
// class.  The user enters the name of a WAV file, and we read the
// file, encode it into a DCS audio stream, and play back the new
// DCS stream on the speakers.  This lets us conveniently test the
// basic audio encoding function.
//

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <memory>
#include <regex>
#include <unordered_map>
#include <string>
#include <mutex>
#include <thread>
#include "../DCSEncoder/DCSEncoder.h"
#include "../DCSDecoder/DCSDecoder.h"
#include "../DCSDecoder/DCSDecoderNative.h"
#include "../SimpleWindowsAudio/SimpleWindowsAudio.h"
#include "../HiResTimer/HiResTimer.h"

#pragma comment(lib, "DCSDecoder")
#pragma comment(lib, "HiResTimer")
#pragma comment(lib, "SimpleWindowsAudio")


// --------------------------------------------------------------------------
//
// Decoder thread entrypoint
//
// We run the decoder on a background thread, with the command-line UI
// on the main thread, so that we can use the basic blocking stdio
// functions for command-line interaction with the user while
// simultaneously servicing the audio player.  The DCSDecoder class is
// explicitly not thread-safe, but that doesn't mean that the overall
// application has to be single-threaded.  It just means that 
// whichever thread is accessing DCSDecoder has to be the ONLY thread
// acessing it.  (In fact, it would even be fine to access it from
// multiple threads, as long as we were to provide our own mechanism
// to serialize access across threads, such as locking a mutex or
// critical section before each call.  But we have no need for multi-
// threaded access, so we don't have to worry about anything so 
// elaborate.)
struct ThreadContext
{
	// mutex to protect access to shared data in the context
	std::timed_mutex mutex;

	// Track play request.  The main thread places a track object
	// here to request playback on the audio thread.  Access to this
	// field must be serialized via the mutex.
	const uint8_t *trackRequest = nullptr;

	// shutdown requested from main thread
	volatile bool shutdownRequested = false;

	// STOP command requested - stop current track playback
	volatile uint32_t stopRequested = 0;

	// is a track currently playing?
	volatile bool isPlaying = false;

	// Windows audio player interface
	std::unique_ptr<SimpleWindowsAudio> audio;
};
static DWORD WINAPI DecoderThreadMain(void *lparam)
{
	// get the context, properly case
	auto ctx = reinterpret_cast<ThreadContext*>(lparam);

	// set up a decoder object
	class HostIfc : public DCSDecoder::Host
	{
		virtual void ReceiveDataPort(uint8_t) override { }
		virtual void ClearDataPort() override { }
		virtual void BootTimerControl(bool) { }
	} hostIfc;
	DCSDecoderNative decoder(&hostIfc);
	decoder.InitStandalone(DCSDecoder::OSVersion::OS95);
	decoder.SetDefaultVolume(255);

	// run until shutdown requested
	while (!ctx->shutdownRequested)
	{
		// fetch some sample from the decoder
		int16_t buf[256];
		for (size_t i = 0 ; i+1 < _countof(buf) ; )
		{
			// read a sample, and copy it out to both stereo channels
			int16_t s = decoder.GetNextSample();
			buf[i++] = s;
			buf[i++] = s;
		}

		// send the samples to the player
		ctx->audio->WriteAudioData(buf, _countof(buf));

		// check if a track is playing
		ctx->isPlaying = decoder.IsStreamPlaying(0);

		// Check for a new track playback request.  We don't have to
		// acquire the mutex to merely read the pointer, so we can
		// do the initial check without any added overhead.
		if (ctx->trackRequest != nullptr)
		{
			// acquire the mutex so that we can update the pointer
			const uint8_t *newTrack = nullptr;
			{
				if (ctx->mutex.try_lock_for(std::chrono::milliseconds(20)))
				{
					// get the track object
					newTrack = ctx->trackRequest;

					// clear the request
					ctx->trackRequest = nullptr;

					// done with the mutex
					ctx->mutex.unlock();
				}
			}

			// if there's a new track, start playback
			if (newTrack != nullptr)
			{
				// clear any prior stream
				decoder.ClearTracks();

				// start playback
				DCSDecoder::ROMPointer rp(0, newTrack);
				decoder.LoadAudioStream(0, rp, 0x7f);
			}
		}		

		// check for STOP requests
		if (ctx->stopRequested != 0)
		{
			// clear the request
			InterlockedDecrement(&ctx->stopRequested);

			// clear any track we're playing
			decoder.ClearTracks();
		}
	}

	// done
	return 0;
}

// Play a track
void PlayTrack(ThreadContext &threadContext, const uint8_t *stream)
{
	// Pass the track to the audio thread for playback.  The track
	// request pointer is a shared field, so we have to acquire the
	// mutex before writing it.
	if (threadContext.mutex.try_lock_for(std::chrono::milliseconds(100)))
	{
		// Pass the new track data object to the thread via the play
		// request field.  This transfers ownership of the track object
		// to the thread.
		threadContext.trackRequest = stream;

		// done with the mutex
		threadContext.mutex.unlock();
	}
	else
	{
		printf("Error starting playback (can't acquire mutex)\n");
	}
}

// --------------------------------------------------------------------------
//
// Main program entrypoint
//
int main(int argc, char **argv)
{
	// set up the audio player
	ThreadContext threadContext;
	threadContext.audio.reset(new SimpleWindowsAudio(NULL, 31250, 2, 60));
	if (!threadContext.audio->InitDirectSound())
	{
		printf("Error initializing audio player: %s\n", threadContext.audio->GetErrorDesc());
		exit(2);
	}

	// Start a thread for the decoder
	std::thread thread(DecoderThreadMain, &threadContext);

	// create an encoder
	DCSEncoder encoder;

	// high-res timer for statistics
	HiResTimer hrt;

	// keep tracks we've decoded in memory, in case we want to replay them
	struct Track
	{
		Track(const std::string name, uint8_t *stream) : name(name), stream(stream) { }
		std::string name;
		std::unique_ptr<uint8_t> stream;
	};
	std::unordered_map<std::string, Track> tracks;

	// last track played
	Track *lastTrackPlayed = nullptr;

	// process input
	const char *helpMessage =
		"DCS Encoder Tester\n"
		"  ENCODE <file>     - encode and play back the named file (WAV, MP3, OGG, FLAC)\n"
		"  PARAMS            - set the encoding parameters (type PARAMS HELP for more)\n"
		"  REPLAY            - replay the last track played\n"
		"  CLEAR             - clear the replay cache\n"
		"  STOP              - stop current track playback\n"
		"  QUIT              - exit\n"
		"\n";
	printf(helpMessage);
	for (;;)
	{
		// get a filename
		char cmd[256];
		printf(">");
		if (fgets(cmd, sizeof(cmd), stdin) == nullptr)
			break;

		// parse the command
		std::match_results<const char*> m;
		auto icase = std::regex_constants::icase;
		if (std::regex_match(cmd, std::regex("^\\s*quit\\s*$", icase)))
		{
			// quit
			break;
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*encode\\s+(.+?)\\s*$", icase)))
		{
			// Encode a file file file
			std::string filename = m[1].str();

			// check to see if we already have this file in memory
			Track *newTrack = nullptr;
			if (auto it = tracks.find(filename); it != tracks.end())
			{
				// We already have it in memory - replay it
				printf("Track previously encoded; replaying\n\n");
				newTrack = &it->second;
			}
			else
			{
				// It's new to us - encode it
				DCSEncoder::DCSAudio dcsObj;
				std::string errmsg;
				double t0 = hrt.GetTime_seconds();
				if (encoder.EncodeFile(filename.c_str(), dcsObj, errmsg))
				{
					// success - show statistics
					FILE *fp = nullptr;
					long fileSize = 0;
					if (fopen_s(&fp, filename.c_str(), "rb") == 0 && fp != nullptr)
					{
						fseek(fp, 0, SEEK_END);
						fileSize = ftell(fp);
						fclose(fp);
					}
					double dt = hrt.GetTime_seconds() - t0;
					unsigned long uncompressedSize = dcsObj.nFrames * 240 * 2;
					printf("Successfully encoded %s\n"
						"Original file size: %lu bytes\n"
						"Uncompressed size:  %lu bytes\n"
						"DCS frames:         %u\n"
						"DCS stream size:    %u bytes\n"
						"Compression ratio:  %.1f:1 (%.2f%%)\n"
						"Playback time:      %.2f seconds\n"
						"Encoding time:      %.2lf seconds\n"
						"\n",
						filename.c_str(), fileSize, uncompressedSize,
						dcsObj.nFrames, static_cast<unsigned long>(dcsObj.nBytes),
						static_cast<float>(fileSize) / static_cast<float>(dcsObj.nBytes),
						(1.0f - static_cast<float>(dcsObj.nBytes) / static_cast<float>(uncompressedSize)) * 100.0f,
						static_cast<float>(dcsObj.nFrames) * .00768f, dt);

					// add it to our list of tracks
					auto itNew = tracks.emplace(std::piecewise_construct,
						std::forward_as_tuple(filename),
						std::forward_as_tuple(filename, dcsObj.data.release()));

					// get the track pointer
					newTrack = &itNew.first->second;
				}
				else
				{
					// encoding failed
					printf("Failed: %s\n\n", errmsg.c_str());
				}
			}

			// if there's a new track, play it
			if (newTrack != nullptr)
			{
				PlayTrack(threadContext, newTrack->stream.get());
				lastTrackPlayed = newTrack;
			}
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*encode\\s*$", icase)))
		{
			// ENCODE without argument - explain the error
			printf("ENCODE command requires a WAV file name\n\n");
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*params\\b(.*)\\s*$", icase)))
		{
			// PARAMS - get the parameter string
			auto params = m[1].str();

			// check for a help query
			if (std::regex_match(params, std::regex("^\\s*(help|\\?|/\\?)\\s*$", icase)))
			{
				printf("PARAMS: Enter one or more <name>=<value> options:\n"
					"  TYPE       - set the major format type (0 or 1)\n"
					"  SUBTYPE    - set the format subtype (0, 1, 2, 3)\n"
					"  POWERCUT   - set the power cut-off percentage (0-100, fractional values allowed)\n"
					"  MINRANGE   - set the minimum dynamic range for retained frames (1-65535)\n"
					"  MAXERROR   - set the maximum quantization error (1-65535)\n"
					"\n"
					"Enter PARAMS with no options to list the current parameters\n\n");
			}
			else
			{
			// parse the entries
				int nParamsFound = 0;
				int nParamsErr = 0;
				auto &cp = encoder.compressionParams;
				while (!std::regex_match(params, std::regex("^\\s*$")))
				{
					// count the entry
					++nParamsFound;

					// get the next token
					std::match_results<std::string::const_iterator> m2;
					bool ok = true;
					if (std::regex_match(params, m2, std::regex("^\\s*([a-z]+)=([\\d.]+)\\b(.*)$", icase)))
					{
						// get this parameter
						auto name = m2[1].str();
						auto val = m2[2].str();

						// get the remaining parameters
						params = m2[3].str();

						// check the parameter name
						std::transform(name.begin(), name.end(), name.begin(), ::toupper);
						if (name == "TYPE")
							cp.streamFormatType = atoi(val.c_str());
						else if (name == "SUBTYPE")
							cp.streamFormatSubType = atoi(val.c_str());
						else if (name == "POWERCUT")
							cp.powerBandCutoff = static_cast<float>(atof(val.c_str())) / 100.0f;
						else if (name == "MINRANGE")
							cp.minimumDynamicRange = static_cast<float>(atoi(val.c_str())) / 32768.0f;
						else if (name == "MAXERROR")
							cp.maximumQuantizationError = static_cast<float>(atoi(val.c_str())) / 32768.0f;
						else
						{
							printf("Unrecognized parameter \"%s\"\n\n", name.c_str());
							++nParamsErr;
						}
					}
					else
					{
						printf("Invalid parameter starting at \"%s\" - expect <name>=<value>\n", params.c_str());
						break;
					}
				}

				// if we updated anything, say so
				if (nParamsFound > nParamsErr)
					printf("Parameters updated\n");

				// show the new parameters
				printf(
					"TYPE=%-8d        (Major format type)\n"
					"SUBTYPE=%-8d     (Format subtype)\n"
					"POWERCUT=%-8.2f    (Power band cutoff percentage)\n"
					"MINRANGE=%-8d    (Minimum dynamic range for frame retention)\n"
					"MAXERROR=%-8d    (Maximum quantization error)\n\n",
					cp.streamFormatType,
					cp.streamFormatSubType,
					cp.powerBandCutoff * 100.0f,
					static_cast<int>(roundf(cp.minimumDynamicRange * 32768.0f)),
					static_cast<int>(roundf(cp.maximumQuantizationError * 32768.0f)));
			}
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*replay\\s*$", icase)))
		{
			// replay the last track
			if (lastTrackPlayed != nullptr)
			{
				printf("Replaying %s\n\n", lastTrackPlayed->name.c_str());
				PlayTrack(threadContext, lastTrackPlayed->stream.get());
			}
			else
			{
				printf("No previous track is available.  Use ENCODE to encode a new track.\n\n");
			}
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*stop\\s*$", icase)))
		{
			// stop current track playback
			printf("Stopping playback\n\n");
			InterlockedIncrement(&threadContext.stopRequested);
		}
		else if (std::regex_match(cmd, m, std::regex("^\\s*clear\\s*$", icase)))
		{
			// clear the replay cache
			printf("Stopping playback and clearing the replay cache\n\n");
			InterlockedIncrement(&threadContext.stopRequested);

			// wait for playback to stop
			while (threadContext.isPlaying)
				Sleep(10);

			// clear the cache
			tracks.clear();
		}
		else if (std::regex_match(cmd, std::regex("^\\s*(help|\\?)\\s*$", icase)))
		{
			// show the help message
			printf(helpMessage);
		}
		else if (std::regex_match(cmd, std::regex("^\\s*$")))
		{
			// empty command - ignore it
		}
		else
		{
			// bad command
			printf("Unrecognized command  (type HELP for a list of commands)\n\n");
		}
	}

	// before exiting, wait for the decoder thread to exit (but don't wait too long)
	threadContext.shutdownRequested = true;
	thread.join();

	// done
	return 0;
}
