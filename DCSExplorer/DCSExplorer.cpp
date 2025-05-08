// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Explorer
//
// This is a command-line program that demonstrates and exercises the
// DCSDecoder classes, which implement DCS audio decoding in native C++ code.
// 
// This program plays back tracks from DCS ROMs interactively, and also lets
// you disassemble the machine code of the embedded ADSP-2105 program, list
// all of the audio tracks contained in the ROM set, and get details on the
// playback programs encoded in the tracks.  (A DCS audio track is more than
// just a compressed audio stream; it's a miniature program that can express
// looping sections and timed events, to do things like coordinating the
// playback of multiple tracks at once, adjusting the mixing level among the
// simultaneously playing tracks, timing transitions between tracks so that
// they fall on music beats, and sending event notifications back to the
// pinball controller program so that it can synchronize its own effects
// with the music.)
// 
// "Validation mode" lets you test the native C++ decoder's output against
// that of the original ADSP-2105 machine code contained in the game's ROM,
// running in emulation.  The emulated ROM code is the closest we can get
// on a PC to the original hardware.  When  validation mode is selected,
// the program runs the native C++ decoder and the emulated decoder in
// parallel, and compares their output, logging any differences.
// 
// "Autoplay" plays each of the tracks contained in the ROM in sequence.
// This is especially useful with validation mode, since it lets you run a
// complete test of all of everything in the ROM.  You can use "silent"
// mode to speed up these tests, by skipping the actual audio output and
// thus running as fast as the decoders can decode data from the ROMs,
// without waiting for audio playback to finish.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>
#include <regex>
#include <memory>
#include <list>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <Windows.h>
#include <conio.h>
#include "../Utilities/BuildDate.h"
#include "../SimpleWindowsAudio/SimpleWindowsAudio.h"
#include "../HiResTimer/HiResTimer.h"
#include "../DCSDecoder/DCSDecoder.h"
#include "../DCSDecoder/DCSDecoderNative.h"
#include "../DCSDecoder/DCSDecoderEmu.h"

// include the DCSDecoder library and libsamplerate
#pragma comment(lib, "DCSDecoder")
#pragma comment(lib, "libsamplerate")
#pragma comment(lib, "SimpleWindowsAudio")
#pragma comment(lib, "HiResTimer")

// --------------------------------------------------------------------------
//
// forward/external declarations
//
static void Disassemble(FILE *fp, const uint8_t *u2, uint16_t offset, uint16_t length, uint16_t loadAddr);
static void ExtractTracksOrStreams(bool streams, DCSDecoder *decoder, const char *prefix, const char *format);
static void IdleTask(void*);
extern unsigned adsp2100_dasm(char *buffer, unsigned long op);


// --------------------------------------------------------------------------
//
// Helper objects
//

// high-resolution timer
static HiResTimer hrt;

// Windows audio interface
std::unique_ptr<SimpleWindowsAudio> audioPlayer;


// --------------------------------------------------------------------------
//
// Audio output buffer
//
class AudioBuffer
{
public:
	AudioBuffer() { }

	// add a stereo sample
	void AddSample(int16_t l, int16_t r)
	{
		AddSample(l);
		AddSample(r);
	}

	// add a mono sample
	void AddSample(int16_t s)
	{
		// if the write pointer will bump into the read pointer, discard the oldest
		// sample at the read pointer
		if (((writeIdx + 1) % static_cast<int>(_countof(buf))) == readIdx)
			readIdx = (readIdx + 1) % static_cast<int>(_countof(buf));

		// store the sample
		buf[writeIdx] = s;

		// advance the write pointer
		writeIdx = (writeIdx + 1) % static_cast<int>(_countof(buf));
	}

	// are any samples available?
	bool IsSampleAvailable() const { return readIdx != writeIdx; }

	// read the next sample
	int16_t ReadSample()
	{
		int16_t s = 0;
		if (IsSampleAvailable())
		{
			s = buf[readIdx];
			readIdx = (readIdx + 1) % static_cast<int>(_countof(buf));
		}
		return s;
	}

	// sample buffer
	int16_t buf[1024];

	// current read/write pointers, treating the buffer as circular
	int readIdx = 0;
	int writeIdx = 0;
};
AudioBuffer stream;

// --------------------------------------------------------------------------
//
// pending keyboard input buffer
//
static char kbBuf[128];
static int kbBufLen = 0;

// command lines awaiting processing
static std::list<std::string> pendingCmdLine;
static bool quitRequested = false;
static bool interactiveMode = true;

static void WriteConsole(const char *fmt, ...)
{
	// overwrite the current command line input
	printf("\r ");
	for (int i = 0 ; i < kbBufLen ; ++i)
		printf(" ");
	printf("\r");

	// show the message
	va_list va;
	va_start(va, fmt);
	vprintf(fmt, va);
	va_end(va);

	// redraw the command line in progress, if in interactive mode
	if (interactiveMode)
		printf(">%.*s", kbBufLen, kbBuf);
}


// --------------------------------------------------------------------------
//
// Disassembly helpers
//
static inline uint32_t ReadOpcode(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}
static inline uint16_t ReadU16(const uint8_t *p) {
	return (static_cast<uint16_t>(p[0]) << 8) | (static_cast<uint16_t>(p[1]));
}
static inline uint32_t ReadU32(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
		| (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
static inline uint32_t ReadU24(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}
static inline bool IsRTI(const uint8_t *p) { return p[0] == 0x0A && p[1] == 0x00 && p[2] == 0x1F; }
static inline bool IsJUMP(const uint8_t *p) { return (p[0] & 0xFC) == 0x18 && (p[2] & 0x0F) == 0x0F; }
static inline bool IsCALL(const uint8_t *p) { return (p[0] & 0xFC) == 0x1C && (p[2] & 0x0F) == 0x0F; }


// --------------------------------------------------------------------------
//
// DCSDecoder host interface
//
class HostImpl : public DCSDecoder::Host 
{
public:
	HostImpl(const char *name, bool validationMode, bool terse) : 
		name(name), validationMode(validationMode), terse(terse)
	{ }

	virtual void ReceiveDataPort(uint8_t data) override
	{
		// log it to the console
		if (!terse)
			WriteConsole("DCS->%s data (%02x)\n", name.c_str(), data);

		// keep a history log if we're in validation mode
		if (validationMode)
			history.emplace_back(data);
	}

	virtual void ClearDataPort() override { }
	
	virtual void BootTimerControl(bool set) override
	{
		// flag whether or not the boot timer is running, and note the new expiry
		bootTimerRunning = set;
		bootTimerExpiry = hrt.GetTime_seconds() + 0.250;
	}

	std::string name;
	DCSDecoder *decoder = nullptr;
	bool validationMode = false;
	bool terse = false;
	bool bootTimerRunning = false;
	double bootTimerExpiry = 0.0;

	// when in validation mode, we keep a history of command bytes received,
	// so that the program can check for differences between the main decioder
	// and the reference decoder
	std::list<uint8_t> history;

	// display the history
	void LogHistory(FILE *fp)
	{
		fprintf(fp, "Data port bytes sent to host from %s:", decoder->Name());
		for (auto b : history)
			fprintf(fp, " %02x", b);
		fprintf(fp, "\n");
	}

	// clear the history
	void ClearHistory() { history.clear(); }
};

// --------------------------------------------------------------------------
//
// Main program entrypoint
//
int main(int argc, char **argv)
{
	// Set the initial volume level to the full reference level by default.
	// The soft volume setting really represents an attenuation level from 
	// the recorded level of the samples, so anything below full volume
	// reduces the precision of the output.  It's thus better to crank the
	// decoder up to max and set the desired listening volume somewhere
	// later in the audio chain, such as the Windows system volume, or
	// the volume knob on your speakers.
	int initialVolume = 255;

	// parse arguments
	int argi = 1;
	const char *explicitU2 = nullptr;
	const char *dasmFile = nullptr;
	bool listTracks = false;
	bool listStreams = false;
	bool listPrograms = false;
	bool listDITables = false;
	const char *decoderVersion = nullptr;
	bool validationMode = false;
	const char *validationFile = nullptr;
	FILE *validationFp = nullptr;
	bool adspDebugMode = false;
	bool autoplay = false;
	bool silent = false;
	bool terse = false;
	bool infoOnly = false;
	const char *extractTracksPrefix = nullptr;
	const char *extractStreamsPrefix = nullptr;
	const char *extractFormat = "wav";
	bool ignoreChecksumErrors = false;
	for (; argi < argc && argv[argi][0] == '-' ; ++argi)
	{
		const char *argp = argv[argi];
		if (strcmp(argp, "-") == 0)
		{
			// end of options - stop scanning for them
			break;
		}
		else if (strcmp(argp, "-D") == 0)
		{
			// secret (unlisted in usage) ADSP-2105 emulator debug option
			adspDebugMode = true;
		}
		else if (strncmp(argp, "--u2=", 5) == 0)
		{
			// explicit U2 filename designation with '='
			explicitU2 = argp + 5;
		}
		else if (strcmp(argp, "--u2") == 0 && argi + 1 < argc)
		{
			// explicit U2 filename designation with next option
			explicitU2 = argv[++argi];
		}
		else if (strcmp(argp, "--dasm") == 0)
		{
			// disassembler file, implied filename based on ZIP file
			dasmFile = "*";
		}
		else if (strncmp(argp, "--dasm=", 7) == 0)
		{
			// disassembler file, explicitly named
			dasmFile = argp + 7;
		}
		else if (strcmp(argp, "--tracks") == 0 || strcmp(argp, "-t") == 0)
		{
			// generate a track listing
			listTracks = true;
		}
		else if (strcmp(argp, "--streams") == 0 || strcmp(argp, "-s") == 0)
		{
			// generate a stream listing
			listStreams = true;
		}
		else if (strcmp(argp, "--programs") == 0 || strcmp(argp, "-p") == 0)
		{
			// generate a track listing with program code listing
			listPrograms = true;
		}
		else if (strcmp(argp, "--ditables") == 0)
		{
			// generate a list of Deferred Indirect tables
			listDITables = true;
		}
		else if (strcmp(argp, "--extract-streams") == 0 || strncmp(argp, "--extract-streams=", 18) == 0)
		{
			// extract streams
			if (argp[17] == '=')
				extractStreamsPrefix = argp + 18;
			else if (argi + 1 < argc)
				extractStreamsPrefix = argv[++argi];
		}
		else if (strncmp(argp, "--extract-format=", 17) == 0)
		{
			// exract format - raw, wav
			extractFormat = argp + 17;
			if (strcmp(extractFormat, "wav") != 0 && strcmp(extractFormat, "raw") != 0)
			{
				printf("Invalid extract format '%s'; expected 'raw' or 'wav'\n", extractFormat);
				exit(2);
			}
		}
		else if (strcmp(argp, "--extract-tracks") == 0 || strncmp(argp, "--extract-tracks=", 17) == 0)
		{
			// extract tracks
			if (argp[16] == '=')
				extractTracksPrefix = argp + 17;
			else if (argi + 1 < argc)
				extractTracksPrefix = argv[++argi];
		}
		else if (strncmp(argp, "--decoder=", 10) == 0)
		{
			// decoder version
			decoderVersion = argp + 10;
		}
		else if (strcmp(argp, "--decoder") == 0 && argi + 1 < argc)
		{
			// decoder version
			decoderVersion = argv[++argi];
		}
		else if (strcmp(argp, "--decoder") == 0)
		{
			// decoder option without an argument - ask for help
			decoderVersion = "?";
		}
		else if (strcmp(argp, "-I") == 0 || strcmp(argp, "--ignore-checksum-errors") == 0)
		{
			// ignore checksum errors
			ignoreChecksumErrors = true;
		}
		else if (strncmp(argp, "--vol=", 6) == 0)
		{
			// initial volume
			initialVolume = atoi(argv[argi] + 6);
		}
		else if (strncmp(argp, "--volume=", 9) == 0)
		{
			// initial volume
			initialVolume = atoi(argv[argi] + 9);
		}
		else if ((strcmp(argp, "--vol") == 0 || strcmp(argp, "--volume") == 0) && argi + 1 < argc)
		{
			// initial volume
			initialVolume = atoi(argv[++argi]);
		}
		else if (strcmp(argp, "--validate") == 0)
		{
			// enable validation mode
			validationMode = true;
		}
		else if (strcmp(argp, "--autoplay") == 0)
		{
			// enable autoplay mode
			autoplay = true;
		}
		else if (strcmp(argp, "--silent") == 0)
		{
			// enable silent testing mode
			silent = true;
		}
		else if (strncmp(argp, "--validate=", 11) == 0)
		{
			// enable validation mode with file capture
			validationMode = true;
			validationFile = argv[argi] + 11;
			if (fopen_s(&validationFp, validationFile, "w") != 0 || validationFp == nullptr)
			{
				printf("Unable to open validation report file \"%s\"\n", validationFile);
				exit(2);
			}
		}
		else if (strcmp(argp, "--terse") == 0)
		{
			// terse mode - minimize status reports
			terse = true;
		}
		else if (strcmp(argp, "--info") == 0)
		{
			// information request only
			infoOnly = true;
		}
		else if (strcmp(argp, "-A") == 0)
		{
			// automated test mode: --autoplay --silent --terse --validate
			autoplay = true;
			silent = true;
			terse = true;
			validationMode = true;
		}
		else
		{
			// unknown option - consume the rest of the arguments and stop scanning
			argi = argc;
			break;
		}
	}

	// set up our host interface
	HostImpl hostIfc("Host", validationMode, terse);

	// if a decoder version was specified, validate it
	std::unique_ptr<DCSDecoder> decoder;
	if (decoderVersion != nullptr)
	{
		// Scan the registered decoder list for leading substring matches
		int matchCnt = 0;
		const DCSDecoder::Registration *match = nullptr;
		for (auto &r : DCSDecoder::GetRegistrationMap())
		{
			// if it matches exactly, accept this one uniquely, even it 
			// matches other leading substrings
			if (_stricmp(r.first.c_str(), decoderVersion) == 0)
			{
				matchCnt = 1;
				match = &r.second;
				break;
			}

			// it's not an exact match, so check for a leading substring match
			if (_strnicmp(r.first.c_str(), decoderVersion, strlen(decoderVersion)) == 0)
			{
				++matchCnt;
				match = &r.second;
			}
		}

		// if we found exactly one match, accept it, otherwise fail
		if (matchCnt == 1)
		{
			// got it - create the selected decoder
			decoder.reset(match->factory(&hostIfc));
		}
		else
		{
			// show an error, unless they specifically asked for a list with --decoder=?
			if (strcmp(decoderVersion, "?") == 0)
				printf("Available decoders:\n");
			else
				printf("Invalid --decoder type specified; use one of the following:\n");

			// show the list
			auto &regMap = DCSDecoder::GetRegistrationMap();
			for (auto &r : regMap)
				printf("    %-15s  %s\n", r.second.name, r.second.desc);

			// show some more detail, depending on which versions are linked
			if (regMap.find("native") != regMap.end() && regMap.find("emulator-strict") != regMap.end())
			{
				printf(
					"\n"
					"The universal native decoder is the default.  This is the best decoder in\n"
					"most cases, unless you have a reason to suspect it's not working properly.\n"
					"In that case, you can test the same ROM with the emulator to check for\n"
					"differences.  The emulator directly interprets the ADSP-2105 program\n"
					"contained in the ROM in real time, whereas the universal version is an\n"
					"all-new portable version of the decoder that runs as pure machine code on\n"
					"your PC.  The native version is faster than the emulator, but it doesn't\n"
					"use any of the ROM code, so it's not a simulation of the original hardware\n"
					"in the same way the emulator is.  The native version has been tested against\n"
					"many ROMs, though, and has been verified to produce identical audio output\n"
					"for all ROMs tested.\n"
					"\n"
					"By default, when the emulator is selected, it runs with PinMame speedups\n"
					"enabled.  The speedups are native replacements for small performance-critical\n"
					"sections of the ROM code, to improve real-time playback performance.  The\n"
					"\"strict\" emulator runs the ROM code only, without the speedups enabled.\n"
					"This is slower but ensures the closest possible simulation of the original\n"
					"DCS equipment.  There are no known variations in the ROM code that would\n"
					"make strict mode necessary, but it's provided for the sake of testing,\n"
					"to let you compare results in case of any doubt.  Note that validation\n"
					"mode always uses the strict emulation, to eliminate the possibility of\n"
					"any discrepancies from the speedup code.\n");
			}
			exit(1);
		}
	}
	else
	{
		// no decoder version specified - use the unified native decoder by default
		decoder.reset(new DCSDecoderNative(&hostIfc));
	}

	// Enable debug mode for the emulator, if applicable
	if (auto emu = dynamic_cast<DCSDecoderEmulated*>(decoder.get()); emu != nullptr && adspDebugMode)
		dynamic_cast<DCSDecoderEmulated*>(decoder.get())->EnableDebugger();

	// Validation can't be run against the emulator, because only the ADSP-2105 CPU
	// emulator can only be used as a singleton, since it uses static variables.
	if (validationMode && dynamic_cast<DCSDecoderEmulated*>(decoder.get()) != nullptr)
	{
		printf("Validation mode can't be used with the emulator as the test decoder.\n");
		exit(1);
	}

	// make sure we have a single filename argument remaining
	if (argi + 1 != argc)
	{
		ProgramBuildDate buildDate;
		printf(
			"DCS Explorer   Version 1.0, Build %s\n"
			"(c)%s Michael J Roberts / BSD 3-clause license / NO WARRANTY\n"
			"\n"
			"Usage: dcsexplorer [options] <rom-zip-file>\n"
			"Options:\n"
			"   -A               automated test mode: --autoplay --silent --terse --validate\n"
			"   -I               ignore checksum errors\n"
			"   -p               list track program contents (same as --programs)\n"
			"   -s               list streams (same as --streams)\n"
			"   -t               list tracks (same as --tracks)\n"
			"   --autoplay       automatically play each track once, exit after last track\n"
			"   --dasm=<file>    generate disassembly (<file> is optional; default is <rom-zip-file>.dasm\n"
			"   --decoder=<dec>  select decoder version (--decoder=? lists options)\n"
			"   --ditables       list the \"deferred indirect\" tables\n"
			"   --extract-format=<fmt>     set the stream extract format (raw, wav [default])\n"
			"   --extract-streams=<pre>    extract all streams to WAV files, prefixing each filename with <pre>\n"
			"   --extract-tracks=<pre>     extract all tracks to WAV files, prefixing each filename with <pre>\n"
			"   --ignore-checksum-errors   ignore checksum errors (same as -I)"
			"   --info           information only; show ROM information and other requested listings, then exit\n"
			"   --programs       show full program opcode listings for all tracks\n"
			"   --silent         run in silent mode (no audio output, for fast validation testing)\n"
			"   --terse          minimize status reports\n"
			"   --tracks         show a listing of the tracks found in the ROM catalog\n"
			"   --u2=<file>      designate <file> as the sound ROM image for U2\n"
			"   --vol=<level>    set the initial volume level, 0 to 255\n"
			"   --validate       validation mode (run the emulator alongside the selected decoder to compare)\n",
			buildDate.YYYYMMDD().c_str(), buildDate.CopyrightYears(2023).c_str());
		exit(1);
	}

	// get the ROM .zip file name
	std::string romZipFile = argv[argi];

	// if the ROM file doesn't exist, and the name doesn't end with .zip, try adding .zip
	{
		struct stat statbuf;
		if (stat(romZipFile.c_str(), &statbuf) != 0 && stat((romZipFile + ".zip").c_str(), &statbuf) == 0)
			romZipFile += ".zip";
	}

	// get the base filename with the path removed
	std::string romZipFileBase = std::regex_replace(romZipFile, std::regex("^([A-Za-z]:)?(.*[/\\\\])?"), "");

	// if the disassembly option was selected but no file was specified, generate the default name
	std::string dasmFileBuf;
	if (dasmFile != nullptr && strcmp(dasmFile, "*") == 0)
	{
		dasmFileBuf = std::regex_replace(romZipFile, std::regex("\\.zip$", std::regex_constants::icase), "");
		dasmFileBuf += ".dasm";
		dasmFile = dasmFileBuf.c_str();
	}

	// load the ROMs from the ZIP file
	std::string zipLoadError;
	std::list<DCSDecoder::ZipFileData> zipFileData;
	switch (decoder->LoadROMFromZipFile(romZipFile.c_str(), zipFileData, explicitU2, &zipLoadError))
	{
	case DCSDecoder::ZipLoadStatus::Success:
		// success
		break;

	case DCSDecoder::ZipLoadStatus::NoU2:
		// can't identify ROM U2 image - tell them about the --u2 option
		printf("No file in %s could be identified as ROM U2.  You can designate "
			"which file contains the U2 image via the --u2=<file> option.\n", romZipFile.c_str());
		exit(2);

	default:
		// display the message passed back from the loader
		printf("Unable to load ROMs: %s\n", zipLoadError.c_str());
		exit(2);
	}

	// list the ROM files loaded
	DCSDecoder::ZipFileData *roms[8] ={ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	for (auto &rd : zipFileData)
	{
		if (rd.chipNum >= 2 && rd.chipNum <= 9)
			roms[rd.chipNum - 2] = &rd;
	}

	// show ROM information, unless in terse mode
	auto sig = DCSDecoder::GetSignature(decoder->ROM[0].data);
	const char *gameTitle = DCSDecoder::GetGameTitle(DCSDecoder::InferGameID(sig.c_str()));
	if (!terse)
	{
		// show the loaded ROM files
		printf("Loaded ROM files from %s:\n", romZipFile.c_str());
		for (int i = 0 ; i < 8 ; ++i)
		{
			if (roms[i] != nullptr)
				printf("  [U%d] %s, %lu bytes\n", i + 2, roms[i]->filename.c_str(), static_cast<unsigned long>(roms[i]->dataSize));
		}
		printf("\n");

		// show the ROM signature
		if (sig.size() != 0)
			printf("U2 Signature: %s\n", sig.c_str());
		else
			printf("Warning: U2 ROM image does not appear to have a valid DCS sound ROM signature\n");
		printf("Known pinball machine: %s\n", gameTitle);
	}

	// test the ROM checksums and detect the system version
	if (int code = decoder->CheckROMs() ; code != 1)
	{
		printf("%s: ROM checksum failed for ROM image U%d (%s)\n",
			ignoreChecksumErrors ? "Warning" : "Error",
			code, roms[code-2] != nullptr ? roms[code-2]->filename.c_str() : "<no file loaded>");

		if (!ignoreChecksumErrors)
			exit(2);
	}

	// display the system version
	DCSDecoder::HWVersion hwVersion = DCSDecoder::HWVersion::Unknown;
	DCSDecoder::OSVersion osVersion = DCSDecoder::OSVersion::Unknown;
	if (!terse)
		printf("Version: %s\n", decoder->GetVersionInfo(&hwVersion, &osVersion).c_str());

	// set the host's decoder pointer, now that we have the final decoder selected
	hostIfc.decoder = decoder.get();

	// Find the catalog in U2
	auto catalogOfs = decoder->GetCatalogOffset();
	auto maxTrackNum = decoder->GetMaxTrackNumber();
	if (catalogOfs != 0)
	{
		// show the catalog information
		if (!terse)
		{
			printf(
				"Soft boot program offset: $%05lx\n"
				"ROM U2 catalog offset: $%05lx\n"
				"Maximum track number: %02x %02x\n"
				"Number of audio channels: %d\n",
				decoder->GetSoftBootOffset(), catalogOfs, (maxTrackNum >> 8) & 0xFFu, maxTrackNum & 0xFFu,
				decoder->GetNumChannels());
		}

		// list the streams, if desired
		if (listStreams)
		{
			// List streams.  There's no direct index of the streams in a ROM; the
			// streams are simply referenced via pointers from the track programs.
			// So to get a list of streams, we have to go through all of the track
			// programs looking for "play stream" opcodes.  A stream might be
			// referenced multiple times, from one track or from multiple tracks,
			// so we need to keep track of which streams we've already visited to
			// skip repeats.  In addition, we'd like to list the streams in some
			// kind of sensible order, and the most natural sort key is the stream
			// address.  So let's start by building an index with one entry per
			// stream, keyed and ordered by address.
			std::set<uint32_t> streams;

			// we can only proceed with a DCSDecoderNative decoder
			auto d9xx = dynamic_cast<DCSDecoderNative*>(decoder.get());
			if (d9xx == nullptr)
			{
				printf("A stream listing can only be generated when the native decoder is selected\n");
				exit(2);
			}

			// initialize the decoder
			decoder->SoftBoot();

			// scan all of the tracks for stream references
			for (uint16_t i = 0 ; i <= maxTrackNum ; ++i)
			{
				// traverse the track program
				for (auto &instr : decoder->DecompileTrackProgram(i))
				{
					if (instr.opcode == 0x01)
					{
						// Operands: UINT8 channel, UINT24 stream address, UINT8 
						// loop counter
						int streamChannel = instr.operandBytes[0];
						uint32_t addr = ReadU24(&instr.operandBytes[1]);

						// if this stream isn't already in the set, add it
						if (streams.find(addr) == streams.end())
							streams.emplace(addr);
					}
				}
			}

			// now run through the list of streams and display each one's information
			printf("\nAddress             Fmt Stream Header                                    Time (sec)   Bytes Compressed  Uncompressed  Ratio\n");
			for (auto addr : streams)
			{
				// get the stream information
				auto romPtr = decoder->MakeROMPointer(addr);
				auto info = d9xx->GetStreamInfo(romPtr);

				// Format the stream type.  The 1994+ format has a type and subtype;
				// the 1993 formats only have a major type.
				char typeCode[10];
				if (osVersion == DCSDecoder::OSVersion::OS93a || osVersion == DCSDecoder::OSVersion::OS93b)
					sprintf_s(typeCode, "%d", info.formatType);
				else
					sprintf_s(typeCode, "%d.%d", info.formatType, info.formatSubType);

				// figure the statistics
				float playbackTime = static_cast<float>(info.nFrames) * 0.00768f;
				int uncompressedBytes = info.nFrames * 240 * 2;
				float ratio = static_cast<float>(uncompressedBytes) / static_cast<float>(info.nBytes);
				float pct = (1.0f - (1.0f/ratio)) * 100.0f;

				// display the track data
				printf("%07lx [U%d %05x]  %-3s %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %9.2f            %6u      %8u     %.1f:1 (%.1f%%)\n",
					addr, romPtr.NominalChipNumber(), decoder->ROMPointerOffset(romPtr), typeCode,
					info.header[0], info.header[1], info.header[2], info.header[3], info.header[4], info.header[5], info.header[6], info.header[7],
					info.header[8], info.header[9], info.header[10], info.header[11], info.header[12], info.header[13], info.header[14], info.header[15],
					playbackTime, info.nBytes, uncompressedBytes, ratio, pct);
			}
		}

		// list the tracks, if desired
		if (listTracks)
		{
			printf("\n----- Track index -----\nCommand  Ch  Type       Time               Address\n");
			for (uint16_t i = 0 ; i <= maxTrackNum ; ++i)
			{
				// get the track data
				DCSDecoder::TrackInfo ti;
				if (decoder->GetTrackInfo(i, ti))
				{
					// make a ROM pointer from the address
					auto trackPtr = decoder->MakeROMPointer(ti.address);

					// format the time separately
					char time[40];
					sprintf_s(time, "%.2fms%s",
						static_cast<float>(ti.time) * 7.68f,
						ti.looping ? "(loop)" : "      ");

					// display the track description
					static const char *typeName[] ={ "NA", "Play", "Defer", "Indirect" };
					printf("%02x %02x    %d   %d %-8s %-18s %06X [U%d %05X]\n",
						(i >> 8) & 0xFF, i & 0xFF,
						ti.channel, ti.type, typeName[ti.type], time,
						ti.address, trackPtr.NominalChipNumber(), decoder->ROMPointerOffset(trackPtr));
				}
			}
		}

		// list the Deferred Indirect tables, if desired
		if (listDITables)
		{
			// show the section header
			auto dii = decoder->GetDeferredIndirectTables();
			printf("\nDeferred Indirect Tables\n");
			if (dii.tables.size() == 0)
				printf("  *** None ***\n");
			else
				printf("  Table   Tracks\n");

			// list the tables
			for (auto &t : dii.tables)
			{
				// list the table contents
				printf("  $%02x     ", t.id);
				const char *sep = "";
				for (auto track : t.tracks)
					printf("%s$%04x", sep, track), sep = ", ";
				printf("\n");
			}

			// add a newline at the end of the section
			printf("\n");
		}

		// list programs, if desired
		if (listPrograms)
		{
			for (uint16_t i = 0 ; i <= maxTrackNum ; ++i)
			{
				// get the track information
				DCSDecoder::TrackInfo ti;
				if (decoder->GetTrackInfo(i, ti))
				{
					// identify the track, using the encoder/compiler notation 
					printf("Track $%04x Channel %d", i, ti.channel);

					// make a ROM pointer from the address
					auto trackPtr = decoder->MakeROMPointer(ti.address);

					// format the address
					char addr[64];
					sprintf_s(addr, "Address $%07lx [%c%d $%05lx]",
						ti.address, hwVersion == DCSDecoder::HWVersion::DCS95 ? 'S' : 'U',
						trackPtr.NominalChipNumber(), decoder->ROMPointerOffset(trackPtr));

					// format the time
					char time[40];
					sprintf_s(time, "Time %.2fms%s",
						static_cast<float>(ti.time) * 7.68f,
						ti.looping ? " (loop)" : "");

					// check the type
					switch (ti.type)
					{
					case 1:
						// Type 1 -> byte code program
						{
							// build the program listing
							std::string program = decoder->ExplainTrackProgram(i, "    ");

							// open the listing
							printf(" {    // %s, %s\n", addr, time);

							// display the track program
							if (program.size() != 0)
								printf("%s\n", program.c_str());

							// end the program
							printf("};\n");
						}
						break;

					case 2:
						// Type 2 -> deferred track load
						printf(" Defer ($%04x);    // %s\n", ti.deferCode, addr);
						break;

					case 3:
						// Type 3 -> deferred indirect
						printf(" Defer Indirect ($%02x[$%02x]);    // %s\n",
							ti.deferCode & 0xFF, (ti.deferCode >> 8) & 0xFF, addr);
						break;
					}
				}
			}
		}
	}
	else
	{
		// warn about the missing catalog
		printf("Warning: Catalog not found in ROM U2 image\n");
	}

	// generate disassembly of the ADSP-2105 program code in U2, if desired
	if (dasmFile != nullptr)
	{
		// open the file
		FILE *fp = nullptr;
		if (fopen_s(&fp, dasmFile, "w") != 0 || fp == nullptr)
		{
			printf("Unable to create disassembly file \"%s\"\n (system error %d)", dasmFile, errno);
			exit(2);
		}

		// get the soft-boot program address
		uint32_t softBootOffset = decoder->GetSoftBootOffset();

		// Figure out which hardware version we're dealing with, so that we can
		// display the appropriate memory map.  We can sense the hardwaer version by
		// looking for the first overlay subroutine call in the boot-loader program.
		// This will always be at $0800 for the original DCS sound boards, and $2800
		// for the DCS-95 audio/video boards.
		const char *DMMap = ";    Unknown\n";
		const char *PMMap = ";    Unknown\n";
		const char *hwVersion = "Not detected";
		for (const uint8_t *p = decoder->ROM[0].data + softBootOffset, *endp = p + 0x400 ; p < endp ; p += 4)
		{
			if (IsCALL(p))
			{
				uint16_t target = static_cast<uint16_t>((ReadOpcode(p) >> 4) & 0x3FFF);
				if (target == 0x0800)
				{
					// CALL $0800 -> original DCS audio board
					hwVersion = "DCS audio board";
					DMMap =
						";    $0000..$1FFF    RAM\n"
						";    $2000..$2FFF    Banked ROM (read-only)\n"
						";    $3000           ROM bank select register\n"
						";    $3800..$39FF    RAM\n"
						";    $3FE0..$3FFF    ADSP-2105 control registers\n";

					PMMap =
						";    $0000..$0800    Internal boot RAM\n"
						";    $1000..$2FFF    External RAM\n"
						";    $3000           Sound data port register\n";

					// found it - stop looking
					break;
				}
				else if (target == 0x2800)
				{
					// CALL $2800 -> DCS-95 audio/video board
					hwVersion = "DCS-95 audio/video board";
					DMMap =
						";    $0000..$07FF    Banked ROM (read-only)\n"
						";    $1000..$1FFF    RAM\n"
						";    $2000..$2FFF    Banked RAM\n"
						";    $3000           ROM bank select low (low 8 bits of bank select register)\n"
						";    $3100           ROM bank select high (high 5 bits of bank select register)\n"
						";    $3200           RAM bank select (one bit at $0080)\n"
						";    $3300           Sound data port register\n"
						";    $3800..$39FF    RAM\n"
						";    $3FE0..$3FFF    ADSP-2105 control registers\n";

					PMMap =
						";    $0000..$0800    Internal boot RAM\n"
						";    $1000..$3FFF    External RAM\n";

					// found it - stop looking
					break;
				}
			}
		}

		// write a header comment
		if (sig.size() != 0)
			fprintf(fp, "; %s\n", sig.c_str());
		fprintf(fp, "; ROM U2 ADSP-2105 disassembly\n"
			"; Extracted from %s[%s]\n"
			";\n"
			"; Target hardware: %s\n"
			";\n"
			"; ADSP-2105 Program Memory (PM) map:\n"
			"%s"
			";\n"
			"; ADSP-2105 Data Memory (DM) map:\n"
			"%s"
			";\n"
			"; The program listing only includes regions loaded by the automatic boot loader\n"
			"; or by the DCS overlay loader.  Locations marked as .BYTE do not appear to be\n"
			"; reachable during program execution so are assumed to be pre-initialized data,\n"
			"; writable variables/buffers, or unused.\n"
			"\n",
			romZipFileBase.c_str(), roms[0]->filename.c_str(), hwVersion, PMMap, DMMap);

		// disassemble the boot loader: auto-load from U2[$00000] at program location $0000
		Disassemble(fp, decoder->ROM[0].data, 0x0000, 0, 0x0000);
		fprintf(fp, "\n\n");

		// Disassemble the main program.  This auto-loads from either $01000 or $02000,
		// depending on the program version.  We can find it by looking for the JUMP
		// instruction at the beginning of the segment.
		Disassemble(fp, decoder->ROM[0].data, softBootOffset, 0, 0x0000);
	}

	// extract tracks if desired
	if (extractTracksPrefix != nullptr)
		ExtractTracksOrStreams(false, decoder.get(), extractTracksPrefix, nullptr);

	// extract streams if desired
	if (extractStreamsPrefix != nullptr)
		ExtractTracksOrStreams(true, decoder.get(), extractStreamsPrefix, extractFormat);

	// if we're listing tracks or programs, extracting tracks, or generating
	// ADSP-2105 disassembly, don't enter interactive mode
	if (listTracks || listPrograms || listStreams || listDITables
		|| dasmFile != nullptr || infoOnly
		|| extractTracksPrefix != nullptr || extractStreamsPrefix != nullptr)
		exit(0);

	// for interactive sessions, silent mode only applies in autoplay mode
	if (silent && !autoplay)
	{
		printf("Note: --silent can only be used in combination with --autoplay; ignored\n");
		silent = false;
	}

	// silent mode disables interactive mode
	if (silent)
		interactiveMode = false;

	// if validation mode is enabled, create an emulated decoder alongside
	// the main decoder, so that we can play back from both simultaenously
	// and compare the outputs, to ensure that the native version is
	// producing identical output.
	std::unique_ptr<DCSDecoder> refDecoder;
	HostImpl refHostIfc("Host(Emulator)", validationMode, terse);
	if (validationMode)
	{
		// Create the emulator.    Run the emulated version in strict
		// mode, without any speedup code, to ensure that we're testing agsint
		// the closest thing we can get to the real system.
		auto emu = new DCSDecoderEmulated(&refHostIfc, false);
		refDecoder.reset(emu);
		refHostIfc.decoder = emu;

		// enable debug mode in the emulator, if desired
		if (adspDebugMode)
			emu->EnableDebugger();

		// load the emulator from the same ROMs we loaded for the main decoder
		for (auto &rd : zipFileData)
			emu->AddROM(rd.chipNum, rd.data.get(), rd.dataSize);

		// provide feedback
		if (!terse)
		{
			printf("\nValidation mode enabled: Left channel=%s, Right channel=%s\n",
				decoder->Name(), emu->Name());
		}

		// write a preamble to the log file
		if (validationFp != nullptr)
		{
			time_t t;
			time(&t);
			char timestr[128];
			struct tm tm;
			localtime_s(&tm, &t);
			asctime_s(timestr, &tm);
			if (auto *p = strchr(timestr, '\n'); p != nullptr)
				*p = 0;

			fprintf(validationFp, "DCSExplorer - validation mode log, ROM %s, %s\n"
				"Listing frames containing differences in PCM output\n"
				"%s output shown on left | Reference emulator output shown on right\n\n",
				romZipFileBase.c_str(), timestr, decoder->Name());
		}
	}

	// show interactive-mode or autoplay instructions
	if (terse)
	{
		// terse mode - just show the ROM loaded
		printf("ROM %s loaded\n", romZipFile.c_str());
	}
	else if (autoplay)
	{
		// autoplay mode - note this and show instructions to drop into interactive mode
		// (unless in silent mode, which disables input)
		printf("\nStarting the decoder (%s) in autoplay mode\n%s\n\n",
			decoder->Name(),
			silent ? "" : "Press Enter at any time to cancel autoplay and switch to interactive mode");
	}
	else
	{
		// interactive mode - show brief instructions
		printf("\nStarting the decoder (%s) in interactive mode\n"
			"Enter data port commands as hex bytes, and press Enter to execute\n"
			"Type QUIT to exit\n\n>", 
			decoder->Name());
	}

	// start the audio player
	audioPlayer.reset(new SimpleWindowsAudio(NULL, 31250, 2, 60));
	if (!audioPlayer->InitDirectSound())
	{
		printf("\nError starting audio player: %s\n", audioPlayer->GetErrorDesc());
		exit(2);
	}

	// set up the idle task
	audioPlayer->SetIdleTask(IdleTask, nullptr);

	// boot the decoder and set the default volume
	decoder->HardBoot();
	decoder->StartSelfTests();
	decoder->SetDefaultVolume(initialVolume);

	// if there's a validation emulator, boot it as well
	if (refDecoder != nullptr)
	{
		refDecoder->SetDefaultVolume(initialVolume);
		refDecoder->HardBoot();
		refDecoder->StartSelfTests();
	}

	// keep track of differences if we're in validation mode
	struct
	{
		uint64_t lastDiffFrameNo = UINT64_MAX;
		uint64_t nConsecutiveDiffFrames = 0;
		uint64_t nSampleErrors = 0;
		uint64_t nFrameDiffs = 0;
		uint64_t nDataPortDiffs = 0;

		// Recent command history ring - when validating, we keep a log
		// of recent commands sent, to log with the next validation error,
		// as an aid to reproducing the problem.
		struct
		{
			uint64_t frameNo = 0;
			uint8_t cmdByte = 0;
		} recentCommands[16];
		int recentCmdWrite = 0;
		size_t numRecentCmds = 0;

		void LogCommand(uint64_t frameNo, uint8_t b)
		{
			recentCommands[recentCmdWrite++] ={ frameNo, b };
			recentCmdWrite %= _countof(recentCommands);
			numRecentCmds += numRecentCmds >= _countof(recentCommands) ? 0 : 1;
		}

		void PrintCommandLog(FILE *fp)
		{
			if (numRecentCmds != 0)
			{
				fprintf(fp, "Recent commands: ");
				uint64_t fn = UINT64_MAX;
				int idx = static_cast<int>(recentCmdWrite - numRecentCmds);
				if (idx < 0)
					idx += _countof(recentCommands);
				for (size_t i = 0 ; i < numRecentCmds ; ++i, idx = (idx + 1) % _countof(recentCommands))
				{
					auto &c = recentCommands[idx];
					if (c.frameNo != fn)
					{
						fprintf(fp, "%sFrame %I64u:", fn == UINT64_MAX ? "" : "; ", c.frameNo);
						fn = c.frameNo;
					}
					fprintf(fp, " %02x", c.cmdByte);
				}
				fprintf(fp, "\n");
			}
		}

		void ClearCommandLog() { numRecentCmds = 0; }
	}
	validationData;

	// autoplay state
	struct
	{
		// current track number
		int trackNo = -1;

		// number of tracks started
		int numTracksPlayed = 0;

		// total play time of tracks played, in frames
		uint64_t totalPlayTime = 0;

		// frame number for next autoplay command entry
		uint64_t nextCommandFrameNo = 0;

	} autoplayState;

	// loop indefinitely, filling the audio buffer and processing commands
	uint64_t frameNo = 0;
	for ( ; !quitRequested ; ++frameNo)
	{
		// check for errors
		if (!decoder->IsOK())
		{
			printf("\n\nDecoder error: %s\n", decoder->GetErrorMessage().c_str());
			break;
		}
		if (refDecoder != nullptr && !refDecoder->IsOK())
		{
			printf("\n\nError in emulator: %s\n", refDecoder->GetErrorMessage().c_str());
			break;
		}

		// process pending commands
		for (; pendingCmdLine.size() != 0 ; pendingCmdLine.pop_front())
		{
			// any command entry cancels autoplay mode
			if (autoplay)
			{
				autoplay = false;
				WriteConsole("Autoplay terminated - entering interactive mode\n"
					"Enter data port commands as hex bytes, and press Enter to execute\n"
					"To exit, press Ctrl+Z or Ctrl+D, or type QUIT\n\n>");
			}

			// get the command, tranlsated to upper-case
			auto &cmd = pendingCmdLine.front();
			std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

			// check for command input - a special command such as QUIT or DEBUG,
			// or a sequence of hex command codes to send to the decoder via its
			// virtual data port
			if (std::regex_match(cmd, std::regex("\\s*quit\\s*", std::regex_constants::icase)))
			{
				quitRequested = true;
				break;
			}
			else if (adspDebugMode && std::regex_match(cmd, std::regex("\\s*debug\\s*", std::regex_constants::icase)))
			{
				if (auto refAsEmu = dynamic_cast<DCSDecoderEmulated*>(refDecoder.get()); refAsEmu != nullptr)
					refAsEmu->DebugBreak();
				else if (auto asEmu = dynamic_cast<DCSDecoderEmulated*>(decoder.get()); asEmu != nullptr)
					asEmu->DebugBreak();

			}
			else if (std::regex_match(cmd, std::regex("\\s*[0-9A-F ]+")))
			{
				for (const char *p = cmd.c_str() ; *p != 0 ; )
				{
					char c = *p++;
					if (c != ' ')
					{
						int n = c >= 'A' ? c - 'A' + 10 : c - '0';
						if ((c = *p) != ' ' && c != 0)
						{
							++p;
							n = (n << 4) | (c >= 'A' ? c - 'A' + 10 : c - '0');
						}
						uint8_t b = static_cast<uint8_t>(n);

						// send it to the decoder
						WriteConsole("WPC->DCS %02x\n", b);
						decoder->WriteDataPort(b);

						// copy it to the validation emulator, if also running
						if (refDecoder != nullptr)
							refDecoder->WriteDataPort(b);

						// if validating, log the command
						if (validationMode)
							validationData.LogCommand(frameNo, b);
					}
				}
			}
		}

		// If we're in autoplay mode, check for the next command entry frame
		if (autoplay && decoder->IsRunning() && frameNo >= autoplayState.nextCommandFrameNo)
		{
			// find the next playable command
			for (;;)
			{
				// go to the next track
				uint16_t n = static_cast<uint16_t>(++autoplayState.trackNo);

				// if past the last track, we're done
				if (n > maxTrackNum)
				{
					if (!terse)
						WriteConsole("Autoplay: last track finished; exiting\n");

					quitRequested = true;
					break;
				}

				// Check if the track number is valid, and get the track information.
				// Skip track types other than 1 (Play Immediate), since the other
				// types are deferred programs that do nothing when loaded.  Deferred
				// tracks can only be triggered by other programs, and since we're
				// just exhaustively scanning the tracks individually, that would
				// never happen.
				DCSDecoder::TrackInfo ti;
				if (decoder->GetTrackInfo(n, ti) && ti.type == 1)
				{
					// it's valid - log the new track
					uint8_t b0 = static_cast<uint8_t>((n >> 8) & 0xFF), b1 = static_cast<uint8_t>(n & 0xFF);
					if (!terse)
					{
						WriteConsole("Autoplay: starting track %02x %02x (run time %.2fms)\n",
							b0, b1, static_cast<float>(ti.time) * 7.68);
					}

					// send the data port command to start the track
					decoder->WriteDataPort(b0);
					decoder->WriteDataPort(b1);
					if (refDecoder != nullptr)
					{
						refDecoder->WriteDataPort(b0);
						refDecoder->WriteDataPort(b1);
					}

					// log it to the command history
					validationData.LogCommand(frameNo, b0);
					validationData.LogCommand(frameNo, b1);

					// note the new active track number
					autoplayState.trackNo = static_cast<int>(n);

					// count the track
					autoplayState.numTracksPlayed += 1;
					autoplayState.totalPlayTime += ti.time;

					// Note the time (as a frame number) to start the next track.  This
					// is the current time plus the full single iteration time of the
					// new track.
					autoplayState.nextCommandFrameNo = frameNo + ti.time + 1;

					// stop searching and carry on with regular decoding activity
					break;
				}
			}
		}

		// fetch the next frame's worth of samples from the main decoder
		int16_t mainbuf[240];
		for (int i = 0 ; i < 240 ; ++i)
			mainbuf[i] = decoder->GetNextSample();

		// if we're in validation mode, compare the frame from the main decoder
		// with the frame from the emulator, and send the data as a stereo signal
		// (main decoder in left channel, emulator in right channel) to the audio
		// output; otherwise just send out the main decoder output as a mono signal
		if (refDecoder != nullptr)
		{
			// Validation mode - we have two decoders running.  Read the latest frame
			// from each decoder, write them to the audio buffer in the left and right
			// stereo channels respectively, and test for differences.

			// read the frame from the reference decoder (the emulator)
			int16_t refbuf[240];
			for (int i = 0 ; i < 240 ; ++i)
				refbuf[i] = refDecoder->GetNextSample();

			// check for differences
			int nSampleDiffs = 0;
			for (int i = 0 ; i < 240 ; ++i)
			{
				// get the two samples
				auto a = mainbuf[i];
				auto b = refbuf[i];

				// count diffs
				if (a != b)
					++nSampleDiffs;

				// add it to the audio stream as a stereo pair, with the main decoder
				// output in the left channel and the emulator in the right channel
				if (!silent)
					stream.AddSample(a, b);
			}

			// check for differences in data port traffic from DCS to host
			bool dataPortHasDiffs = (refHostIfc.history != hostIfc.history);
			if (dataPortHasDiffs)
				validationData.nDataPortDiffs += 1;

			// if there are any PCM or data port diffs, and we're logging to a file,
			// log the latest host-to-DCS commands for context
			if ((nSampleDiffs != 0 || dataPortHasDiffs) && validationFp != nullptr)
			{
				validationData.PrintCommandLog(validationFp);
				validationData.ClearCommandLog();
			}

			// log differences in the PCM data
			if (nSampleDiffs != 0)
			{
				// There are diffs.  Don't log diffs to the console immediately,
				// in case we have a situation where the native decoder just isn't
				// working properly and every frame is diffing.  Instead, keep
				// track of consecutive runs, and log only when reaching certain
				// thresholds.
				if (validationData.lastDiffFrameNo + 1 == frameNo)
				{
					// the last frame diff'd as well, so just update the count
					// of consecutive errors
					validationData.nConsecutiveDiffFrames += 1;

					// check for thresholds for intermediate reports
					if (validationData.nConsecutiveDiffFrames == 10
						|| validationData.nConsecutiveDiffFrames == 50
						|| validationData.nConsecutiveDiffFrames == 100
						|| (validationData.nConsecutiveDiffFrames % 1000) == 0)
						WriteConsole("Validation failing - frames %I64u to %I64u (current) (%I64u frames so far)\n",
							frameNo - validationData.nConsecutiveDiffFrames + 1, frameNo, validationData.nConsecutiveDiffFrames);
				}
				else
				{
					// this is a new run of diff'ing frames
					validationData.nConsecutiveDiffFrames = 1;
				}

				// remember this as the last diff'ing frame
				validationData.lastDiffFrameNo = frameNo;

				// count the sample and frame diffs
				validationData.nSampleErrors += nSampleDiffs;
				validationData.nFrameDiffs += 1;

				// log all difference frames to the file
				if (validationFp != nullptr)
				{
					// log PCM sample differences, if any
					if (nSampleDiffs != 0)
					{
						fprintf(validationFp, "--- Frame %I64u - %d sample differences ---\n", frameNo, nSampleDiffs);
						for (int i = 0 ; i < 240 ; i += 16)
						{
							fprintf(validationFp,
								"%6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d | "
								"%6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d\n",
								mainbuf[i + 0], mainbuf[i + 1], mainbuf[i + 2], mainbuf[i + 3],
								mainbuf[i + 4], mainbuf[i + 5], mainbuf[i + 6], mainbuf[i + 7],
								mainbuf[i + 8], mainbuf[i + 9], mainbuf[i + 10], mainbuf[i + 11],
								mainbuf[i + 12], mainbuf[i + 13], mainbuf[i + 14], mainbuf[i + 15],
								refbuf[i + 0], refbuf[i + 1], refbuf[i + 2], refbuf[i + 3],
								refbuf[i + 4], refbuf[i + 5], refbuf[i + 6], refbuf[i + 7],
								refbuf[i + 8], refbuf[i + 9], refbuf[i + 10], refbuf[i + 11],
								refbuf[i + 12], refbuf[i + 13], refbuf[i + 14], refbuf[i + 15]);
						}
						fprintf(validationFp, "\n");
					}
				}
			}
			else
			{
				// This frame matched.  If we were in the middle of a run of
				// differences, log the run to the console now, since we've
				// apparently reached the end of the diff'ing section.
				if (validationData.nConsecutiveDiffFrames != 0)
				{
					// log the end of the diff run
					if (validationData.nConsecutiveDiffFrames > 1)
						WriteConsole("Validation failed - frames %I64u to %I64u (%I64u frames)\n",
							validationData.lastDiffFrameNo - validationData.nConsecutiveDiffFrames + 1,
							validationData.lastDiffFrameNo, validationData.nConsecutiveDiffFrames);
					else
						WriteConsole("Validation failed - frame %I64u\n", validationData.lastDiffFrameNo);

					// reset the counter
					validationData.nConsecutiveDiffFrames = 0;
				}
			}

			// log differences in the data port traffic
			if (dataPortHasDiffs && validationFp != nullptr)
			{
				fprintf(validationFp, "--- Frame %I64u - data port traffic was different ---\n", frameNo);
				hostIfc.LogHistory(validationFp);
				refHostIfc.LogHistory(validationFp);
				fprintf(validationFp, "\n");
			}

			// clear the data port logs
			hostIfc.ClearHistory();
			refHostIfc.ClearHistory();
		}
		else
		{
			// Single decoder mode (no parallel emulator running).  Copy the samples from 
			// the main decoder into both stereo channels.
			if (!silent)
			{
				for (int i = 0 ; i < 240 ; ++i)
					stream.AddSample(mainbuf[i], mainbuf[i]);
			}
		}

		// send samples to the audio player (unless we're in silent mode)
		if (!silent)
		{
			while (stream.IsSampleAvailable())
			{
				// copy out some samples
				const int bufCount = 512;
				int16_t buf[bufCount*2], *dst = buf;
				for (int i = 0 ; i < bufCount && stream.IsSampleAvailable() ; ++i)
					*dst++ = stream.ReadSample();

				// Send it to the audio layer.  Note that this will block until the
				// hardware playback position has opened up enough space in the buffer,
				// so this naturally keeps the decoding rate in sync with the actual
				// audio output.
				audioPlayer->WriteAudioData(buf, static_cast<DWORD>(dst - buf));
			}
		}
	}

	// report on the validation status
	if (validationMode)
	{
		// generate a report to the given file output
		auto Report = [&validationData, &decoder, &refDecoder, &romZipFileBase](FILE *fp) 
		{
			fprintf(fp, "***** Validation Test Report *****\n\n"
				"ROM file:          %s\n"
				"Decoder tested:    %s\n"
				"Reference decoder: %s\n"
				"Result:            %s\n\n",
				romZipFileBase.c_str(), decoder->Name(), refDecoder->Name(),
				validationData.nFrameDiffs == 0 && validationData.nDataPortDiffs == 0 ? "Validation Succeeded" : "Validation Failed");

			if (validationData.nFrameDiffs == 0)
				fprintf(fp, "No PCM sample differences detected - playback from both sources matched exactly\n");
			else
				fprintf(fp, "PCM sample differences were detected:\n"
					"  Total number of non-matching PCM samples: %I64u\n"
					"  Number of frames containing differences:  %I64u\n",
					validationData.nSampleErrors, validationData.nFrameDiffs);

			if (validationData.nDataPortDiffs == 0)
				fprintf(fp, "No data port traffic differences detected\n");
			else
				fprintf(fp, "Data port traffic differences were detected\n"
					"  Number of frames with differing data port bytes: %I64u\n",
					validationData.nDataPortDiffs);
		};

		// report to the console
		if (terse && autoplay && validationData.nFrameDiffs == 0 && validationData.nDataPortDiffs == 0)
		{
			// terse mode, no diffs -> show a summary only
			double seconds = static_cast<double>(autoplayState.totalPlayTime) * 7.68 / 1000.0;
			int mm = static_cast<int>(seconds / 60.0);
			int ss = static_cast<int>(seconds - mm*60.0);
			printf("%s: Validation succeeded: %u tracks tested (%d:%02d play time, %I64u frames), no differences detected\n",
				romZipFileBase.c_str(), autoplayState.numTracksPlayed, mm, ss, frameNo);
		}
		else
		{
			// not terse mode OR diffs detected -> show a full report
			printf("\n\n");
			Report(stdout);
		}

		// if there's a file, report to the file and close it out
		if (validationFp != nullptr)
		{
			Report(validationFp);
			fclose(validationFp);
		}
	}

	// done
 	return 0;
}

// --------------------------------------------------------------------------
//
// Handle Windows sound system idle tasks.  The audio system calls this
// when the audio thread is blocked waiting for the hardware audio buffer
// playback position to advance to the next buffer segment, to allow
// processing of deferred or scheduled asynchronous events on the audio
// player thread.
//
static void IdleTask(void*)
{
	// check for keyboard input
	while (_kbhit())
	{
		int c = _getch();
		if (c == 0 || c == 0xE0)
		{
			// extended key  - ignore
			c = _getch();
		}
		else if (c == '\n' || c == '\r')
		{
			printf("\n>");
			kbBuf[kbBufLen] = 0;
			pendingCmdLine.emplace_back(kbBuf);

			kbBufLen = 0;
		}
		else if (c == '\b')
		{
			if (kbBufLen > 0)
			{
				printf("\b \b");
				--kbBufLen;
			}
		}
		else if (c >= 32 && c < 128)
		{
			if (kbBufLen + 1 < static_cast<int>(_countof(kbBuf)))
			{
				printf("%c", c);
				kbBuf[kbBufLen++] = c;
			}
		}
		else if (c == 4 || c == 26)
		{
			// ^D/^Z - quit
			quitRequested = true;
		}
	}
}

// --------------------------------------------------------------------------
//
// Track/stream extraction.  This extracts all of the streams OR tracks 
// (according to trhe 'extractStreams' parameter) in the ROM.
//
static void ExtractTracksOrStreams(bool extractStreams, DCSDecoder *decoderBase, 
	const char *prefix, const char *format)
{
	// if we're not extracting streams, then we're doing the tracks,
	// and vice verse
	bool extractTracks = !extractStreams;

	// we need the native decoder for this function
	auto *decoder = dynamic_cast<DCSDecoderNative*>(decoderBase);
	const char *mode = extractStreams ? "stream" : "track";
	if (decoder == nullptr)
	{
		printf("Unable to perform %s extraction.  This function can only be used\n"
			"when the universal native decoder is selected.\n", mode);
		return;
	}

	// get the OS version from the decoder
	DCSDecoder::OSVersion osVer;
	DCSDecoder::HWVersion hwVer;
	decoder->GetVersionInfo(&hwVer, &osVer);

	// show what we're doing
	printf("\n*** Extracting %ss ***\n", mode);

	// boot the decoder - go directly to the soft boot mode so that
	// it's ready to start decoding (bypassing the startup "bong")
	decoder->SoftBoot();
	decoder->SetMasterVolume(255);

	// We want to extra all of the streams, but there's no index of the
	// streams in the ROM.  The streams are all referenced from track
	// programs, and we can enumerate all of the track programs, so that
	// lets us find all of the streams.  But a stream can be referenced
	// from multiple track programs, so merely traversing all of the
	// tracks might visit some streams repeatedly.  We thus have to build
	// our own index of the streams as we go, to make sure we only visit
	// each one once.
	std::unordered_set<uint32_t> streams;

	// Extract frames to a wave file
	int nOk = 0, nError = 0;
	auto ExtractToWAV = [decoder, &nOk, &nError, mode](const char *filename, const char *desc, uint16_t nFrames)
	{
		// Add two extra frames, to ensure that we finish tapering to 
		// silence at the end of the stream or track
		nFrames += 2;

		// open a WAV file to store the output
		FILE *fp = nullptr;
		bool ok = true;
		if (fopen_s(&fp, filename, "wb") != 0 || fp == nullptr)
		{
			printf("Unable to open extraction output file \"%s\" (system error %d)\n", filename, errno);
			nError += 1;
		}
		else
		{
			// construct the WAV file header
			uint8_t hdr[44];
			memset(hdr, 0, sizeof(hdr));
			memcpy(&hdr[0], "RIFF\0\0\0\0WAVEfmt ", 16);   // RIFF, WAVE, fmt tags
			*reinterpret_cast<uint32_t*>(&hdr[4]) = nFrames * 240 * 2 + 44 - 8;  // overall file size, minus 8 bytes
			*reinterpret_cast<uint32_t*>(&hdr[16]) = 16;    // fmt chunk length
			*reinterpret_cast<uint16_t*>(&hdr[20]) = 1;     // type = 1 (PCM)
			*reinterpret_cast<uint16_t*>(&hdr[22]) = 1;     // number of channels
			*reinterpret_cast<uint32_t*>(&hdr[24]) = 31250;    // samples per second
			*reinterpret_cast<uint32_t*>(&hdr[28]) = 31250 * 16 / 8;   // bytes per second
			*reinterpret_cast<uint16_t*>(&hdr[32]) = 2;     // block align
			*reinterpret_cast<uint16_t*>(&hdr[34]) = 16;    // bits per sample
			memcpy(&hdr[36], "data", 4);                    // data chunk tag
			*reinterpret_cast<uint32_t*>(&hdr[40]) = nFrames * 240 * 2;   // data chunk length in bytes

			// write the header
			if (fwrite(hdr, 44, 1, fp) != 1)
				ok = false;

			// decode and capture that number of frames
			for (uint16_t frame = 0 ; frame < nFrames ; ++frame)
			{
				// fetch the next 240 samples
				int16_t buf[240];
				for (int si = 0 ; si < 240 ; ++si)
					buf[si] = decoder->GetNextSample();

				// write the frame
				if (fwrite(buf, sizeof(buf), 1, fp) != 1)
					ok = false;

				// one frame before the end, cancel playback, to ensure that we
				// taper to silence if the track is looping
				if (frame + 2 >= nFrames)
					decoder->ClearTracks();
			}

			// close the file
			if (fclose(fp) != 0)
				ok = false;

			// report status
			if (ok)
			{
				printf("OK %s\n", desc);
				nOk += 1;
			}
			else
			{
				printf("Error extracting %s\n", desc);
				nError += 1;
			}
		}
	};

	// run through all of the track programs
	for (uint16_t trackNum = 0 ; trackNum <= decoder->GetMaxTrackNumber() ; ++trackNum)
	{
		// Get the track description; ignore empty tracks and track types other
		// than type 1, which is the byte code program type.  The other types
		// (2 and 3) only do deferred loading of other tracks, so they don't
		// have any content themselves.
		DCSDecoder::TrackInfo ti;
		if (!decoder->GetTrackInfo(trackNum, ti) || ti.type != 1)
			continue;

		// decompile the track program
		auto v = decoder->DecompileTrackProgram(trackNum);

		// For stream mode, we need to track the mixing levels set in the
		// instruction sequence, to determine an appropriate mixing level
		// for decoding the subsequent streams.  Each stream seems to be
		// intended to be played at a particular level, but this isn't
		// encoded directly into the stream; instead, this seems to be up
		// to the track programs.  They're very consistent about it; track
		// programs always (as far as I've seen) set a level before loading
		// a stream.  So this seems to be a good basis for selecting a level.
		int mixerLevel[8] ={ 0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64, 0x64 };

		// traverse the track program
		int streamNum = 0;
		for (auto &instr : v)
		{
			if (instr.opcode == 0x07 || instr.opcode == 0x0A)
			{
				// set mixer level
				mixerLevel[instr.operandBytes[0]] = instr.operandBytes[1];
			}
			else if (instr.opcode == 0x08 || instr.opcode == 0x0B)
			{
				// increase mixer level
				mixerLevel[instr.operandBytes[0]] += instr.operandBytes[1];
			}
			else if (instr.opcode == 0x09 || instr.opcode == 0x0A)
			{
				// decrease mixer level
				mixerLevel[instr.operandBytes[0]] -= instr.operandBytes[1];
			}
			else if (instr.opcode == 0x01)
			{
				// Operands: UINT8 channel, UINT24 stream address, 
				// UINT8 loop counter.  Read the stream address.
				int streamChannel = instr.operandBytes[0];
				uint32_t addr = ReadU24(&instr.operandBytes[1]);

				// If we're in track extract mode, simply count the stream, so
				// that we know that this track has content to extract.  If we're
				// in stream mode, extract the stream only if we haven't extracted
				// it already.  (The check for previous extraction is necessary
				// because the same stream can be referenced multiple times per
				// track, and/or from multiple tracks, and we only want one copy
				// of each one no matter how many times it's referenced.)
				if (extractTracks)
				{
					// We're in track mode, so we're not interested in extracting
					// streams individually.  Just count the stream, so that we
					// know that the track has content to extract.
					streamNum += 1;
				}
				else if (streams.find(addr) == streams.end())
				{
					// This is the first time we've seen this stream, so extract 
					// it.  Mark it as done so that we don't extract it again if
					// we see it later.
					streams.emplace(addr);

					// count the stream within the track
					streamNum += 1;

					// get a ROM pointer to the stream
					auto streamPtr = decoder->MakeROMPointer(addr);

					// generate the filename and description
					char filename[MAX_PATH];
					char desc[128];
					sprintf_s(filename, "%s_%04X_%02X_%06X.", prefix, trackNum, streamNum, addr);
					sprintf_s(desc, "track %04x, stream #%d, address $%06x", trackNum, streamNum, addr);

					// extract to the desired format
					if (format != nullptr && strcmp(format, "raw") == 0)
					{
						// Extract the raw stream data, without decoding.  This uses
						// our custom-defined DCS container file format, which is just
						// some header information plus the raw DCS stream data.
						FILE *fp = nullptr;
						strcat_s(filename, "dcs");
						if (fopen_s(&fp, filename, "wb") == 0 && fp != nullptr)
						{
							// Build the header:
							// 
							//  "DCSa"         4 bytes, literal text, file type signature (for "DCS audio")
							//  <fmtVersion>   UINT16, big-endian; 0x9301 = DCS-93a (IJTPA, JD), 
							//                 0x9302 = DCS-93b (STTNG), 0x9400 = DCS-94+, all games
							//                 after the first three DCS-93 titles
							//  <channels>     UINT16, big-endian; number of audio channels, always 0x0001
							//  <rate>         UINT16, big-endian; samples per second, always $7A12 (31250)
							//  <reserved*22>  22 bytes, reserve for future use, currently all zeroes
							//  <dataSize>     UINT32, big-endian, size of data section
							//  <streamData>   Stream data, size given by <dataSize>
							// 
							// All integer fields are big-endian, in keeping with the ubiquitous use
							// of big-endian encoding for everything else in the DCS ROMs, which
							// stemmed from the native byte ordering of the ADSP-2105 hardware platform
							// they used.
							uint8_t hdr[36];
							memset(hdr, 0, sizeof(hdr));
							memcpy(&hdr[0], "DCSa", 4);
							hdr[4] = (osVer == DCSDecoder::OSVersion::OS93a || osVer == DCSDecoder::OSVersion::OS93b) ? 0x93 : 0x94;
							hdr[5] = osVer == DCSDecoder::OSVersion::OS93a ? 0x01 : osVer == DCSDecoder::OSVersion::OS93b ? 0x02 : 0x00;
							hdr[6] = 0x00;
							hdr[7] = 0x01;
							hdr[8] = 0x7A;
							hdr[9] = 0x12;

							// get the stream size
							auto info = decoder->GetStreamInfo(streamPtr);

							// write the stream data section size
							hdr[32] = static_cast<uint8_t>((info.nBytes >> 24) & 0xFF);
							hdr[33] = static_cast<uint8_t>((info.nBytes >> 16) & 0xFF);
							hdr[34] = static_cast<uint8_t>((info.nBytes >> 8) & 0xFF);
							hdr[35] = static_cast<uint8_t>((info.nBytes >> 0) & 0xFF);

							// write the header and stream data
							if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)
								|| fwrite(streamPtr.p, 1, info.nBytes, fp) != info.nBytes)
							{
								printf("Error writing extraction output file \"%s\" (system error %d)\n", filename, errno);
								nError += 1;
							}

							// close the file
							fclose(fp);

							// success
							printf("OK %s\n", desc);
							++nOk;
						}
						else
						{
							printf("Unable to open extraction output file \"%s\" (system error %d)\n", filename, errno);
							nError += 1;
						}
					}
					else
					{
						// Extract to WAV
						// 
						// Load it into the decoder, using the current mixing level we're
						// tracking in the target channel.  Note that we just load it into
						// channel 0, regardless of the channel coding in the opcode;
						// we're only playing back one stream at a time, since our goal
						// is to isolate and extract each stream individually.  The channel
						// is just for mixing purposes, and we're not doing any mixing.
						decoder->LoadAudioStream(0, streamPtr, mixerLevel[streamChannel]);

						// the first U16 in the stream is its frame count
						uint16_t nFrames = streamPtr.GetU16();

						// extract it to a WAV file
						strcat_s(filename, "wav");
						ExtractToWAV(filename, desc, nFrames);
					}
				}
			}
		}

		// If we're in track extraction mode, extract the track if it
		// loaded any streams.
		if (extractTracks && streamNum != 0)
		{
			// clear out everything playing
			decoder->ClearTracks();

			// send the command to load the track
			decoder->AddTrackCommand(trackNum);

			// extract it to a WAV file
			char filename[MAX_PATH];
			char desc[128];
			sprintf_s(filename, "%s_%04x.wav", prefix, trackNum);
			sprintf_s(desc, "track %04x", trackNum);
			ExtractToWAV(filename, desc, ti.time);
		}
	}

	// print a summary
	printf("\n*** Extraction summary ***\n"
		"%-24s%d\n"
		"Successfully extracted: %d\n"
		"Errors:                 %d\n",
		extractStreams ? "Streams found:" : "Tracks found",
		nOk + nError, nOk, nError);
}

// --------------------------------------------------------------------------
// 
// Disassembly
// 

// annotate a code location
typedef std::unordered_map<uint16_t /*addr*/, std::string /*comment*/> Annotations;
void Annotate(Annotations &annotations, uint16_t addr, const char *txt)
{
	if (auto it = annotations.find(addr); it != annotations.end())
		it->second.append(txt);
	else
		annotations.emplace(addr, txt);
};


// Trace and disassemble ADSP-2105 machine code.  Finds reachable code
// starting at the given set of uint16_t entrypoints.  If an entrypoint is 
// specified (a value that's not 0xFFFF), we'll trace only from that
// entrypoint.  If entrypoint is 0xFFFF, we'll trace from the hardware
// interrupt vector locations.
void TraceAndDisassemble(FILE *fp, const uint8_t *code, Annotations &annotations,
	uint16_t entrypoint, size_t bootLength, 
	uint16_t overlayLoaderSubAddr, uint16_t overlayBase, uint16_t overlayEnd)
{
	// Add the specified entrypoint or standard entrypoints to the work queue
	std::list<uint16_t> workQueue;
	if (entrypoint != 0xFFFF)
	{
		// an entrypoint was specified - trace only from the entrypoint
		workQueue.emplace_back(entrypoint);
	}
	else
	{
		// no entrypoint was provided - trace from the interrupt vectors
		workQueue.emplace_back(0x0000);   // RESET
		workQueue.emplace_back(0x0004);   // IRQ2
		workQueue.emplace_back(0x0010);   // TX1/IRQ1
		workQueue.emplace_back(0x0014);   // RX1/IRQ0
		workQueue.emplace_back(0x0018);   // Timer
	}

	// Set up an array of locations to record the processing
	// status.  Initially, everything is set to unknown.  Once
	// we visit a location, it's marked as reachable.  When we
	// take an item off the work queue, if it's already marked
	// as reachable, we've already processed it, so we can ignore
	// the work queue item.
	std::unique_ptr<bool> reachableBuf(new bool[0x4000]);
	bool *reachable = reachableBuf.get();
	memset(reachable, 0, 0x4000 * sizeof(bool));

	// process the work queue
	while (workQueue.size() != 0)
	{
		// take the next item off the queue
		uint16_t addr = workQueue.front();
		workQueue.pop_front();

		// process instructions sequentially from the current address until we
		// encounter an item we've already reached, an unconditional branch,
		// or the end of the program data space
		for (; addr < 0x4000 && !reachable[addr] ; ++addr)
		{
			// mark this address as reachable
			reachable[addr] = true;

			// figure other addresses reachable from here
			uint32_t op = ReadOpcode(code + addr*4);
			uint8_t opmain = (op >> 16) & 0xff;
			switch (opmain)
			{
			case 0x03:
				// call/jump on flag in - conditional jump
				workQueue.emplace_back(((op >> 4) & 0x0fff) | ((op << 10) & 0x3000));
				break;

			case 0x0a:
				// conditional RTS - if the condition is 0x0F (always), we can't
				// fall through to the next instruction
				if ((op & 0x00000f) == 0x00000f)
					addr = 0x4000;
				break;

			case 0x0b:
				// JUMP (Ix) - Conditional JUMP (Ix) (jump indirect).  This is a
				// difficult instruction to include in a static trace, because in
				// principle it can jump to any location in the address space, since
				// the Ix register can contain any valid address at run-time and we
				// can't in general predict or constrain its contents statically. 
				// However, the DCS code happens to use this opcode in a consistent
				// way that suggests it was generated by a C compiler translating a
				// 'switch' statement's case branches into a jump table.  Prior to
				// the JUMP (Ix), Ix is loaded with a register added to an immediate
				// (constant) integer value.  The immediate value is the start of
				// a jump table - a sequence of consecutive JUMP <immediate> opcodes
				// that jump to the 'case' handlers for the 'switch'.  What makes
				// this nice for us is that the JUMPs are always consecutive, so
				// we can infer the size of the case table simply by scanning from
				// the starting address until we find a non-JUMP instruction.  The
				// starting address in turn can be inferred by looking for the
				// nearest register load with an immediate value preceding the
				// JUMP (Ix).
				// 
				// To be clear, this is just a heuristic, and it's not a general-
				// purpose heuristic for arbitrary ADSP-2105 machine code.  It only
				// works for DCS code because the DCS code happens to use this
				// instruction in a consistent way that lets us make these guesses
				// about its scope.
				{
					// note the Ix register number
					int ireg = 4 + static_cast<int>((op >> 6) & 0x03);

					// scan backwards for <register> = <immediate> ($4x xx x4)
					for (uint16_t ofsBack = 0 ; ofsBack < 16 && ofsBack < addr ; ++ofsBack)
					{
						auto op = ReadOpcode(code + (addr - ofsBack)*4);
						if ((op & 0xF00000) == 0x400000)
						{
							// found it - the immediate value is the table length
							uint16_t jumpTableStart = static_cast<uint16_t>((op >> 4) & 0xFFFF);
							uint16_t jumpTableAddr = jumpTableStart;
							
							// scan the jump table - it consists of all of the
							// consecutive unconditional JUMPs starting here
							for (; jumpTableAddr < 0x4000 ; ++jumpTableAddr)
							{
								// verify that the opcode is an unconditional JUMP
								const uint8_t *jp = code + jumpTableAddr*4;
								auto jop = ReadOpcode(jp);
								if (!IsJUMP(jp))
								{
									// add an annotation for the assembly listing, showing
									// the jump table location
									char anobuf[128];
									sprintf_s(anobuf, " ; I%d in ($%04X..$%04X)", ireg, jumpTableStart, jumpTableAddr - 1);
									Annotate(annotations, addr, anobuf);

									// we can stop looking now
									break;
								}

								// add the target address to the work queue
								workQueue.emplace_back(jumpTableAddr);
							}

							// we found the AY0 load, so we're done
							break;
						}
					}
				}

				// if the jump is unconditional, we can stop here
				if ((op & 0x00000f) == 0x00000f)
					addr = 0x4000;
				break;

			case 0x18: case 0x19: case 0x1a: case 0x1b: // conditional JUMP
				// queue the branch target for testing
				workQueue.emplace_back((op >> 4) & 0x3fff);

				// if it's unconditional (condition 0x0F == always), we can't fall through
				if ((op & 0x00000F) == 0x00000F)
					addr = 0x4000;
				break;

			case 0x1c: case 0x1d: case 0x1e: case 0x1f: // conditional CALL
				// queue the branch target for testing
				workQueue.emplace_back((op >> 4) & 0x3fff);
				break;
			}
		}
	}

	// generate comment text for a data byte - the byte as an ASCII character if
	// it's in the printable range, otherwise '.'
	auto CommentByte = [](uint8_t c) { return c >= 32 && c < 127 ? static_cast<char>(c) : '.'; };

	// The DCS-95 code contains a few sections that appear to be valid code,
	// but which don't show up as reachable from our static tracer.  They're
	// probably just dead code that the C compiler or linker failed to catch
	// and optimize out, but I'm inclined to keep them in the listing, with an
	// annotation that they appear to be unreachable, in case my simplistic
	// static analysis of what's reachable is missing some cases.  So go back
	// through the code, excluding the interrupt vector area, and look for
	// any short blocks of unreachable code that are surrounded by reachable
	// code; mark them as reachable, with annotations.
	for (uint16_t addr = 0x001c ; addr < 0x4000 ; ++addr)
	{
		if (!reachable[addr] && reachable[addr-1])
		{
			// count consecutive unreachable items
			uint16_t n;
			for (n = 0 ; n < 64 && addr + n < 0x4000 && !reachable[addr + n] ; ++n);

			// if we stopped at a reachable item, mark this section as
			// reachable under protest
			if (addr + n < 0x4000 && reachable[addr + n])
			{
				for (; !reachable[addr] ; ++addr)
				{
					char msg[128];
					const uint8_t *p = code + addr*4;
					sprintf_s(msg, " ; unreachable code, bytes $%02x $%02x $%02x $%02x [%c%c%c%c]",
						p[0], p[1], p[2], p[3],
						CommentByte(p[0]), CommentByte(p[1]), CommentByte(p[2]), CommentByte(p[3]));

					reachable[addr] = true;
					Annotate(annotations, addr, msg);
				}
				--addr;
			}
		}
	}

	// disassemble the code
	uint16_t addr = 0;
	for (const uint8_t *p = code ; addr < 0x4000 ; p += 4, ++addr)
	{
		// check if it's reachable code
		if (reachable[addr])
		{
			// disassemble the code
			char asmbuf[256];
			uint32_t op = ReadOpcode(p);
			adsp2100_dasm(asmbuf, op);

			// Check for items we can comment.  For any vector locations, add the
			// vector name if it actually appears to be an interrupt handler.  (The
			// DCS code always uses a JUMP or RTI in the vectors, and IRQ2 is
			// always populated.  The others are sometimes left unused.)
			std::string comment;
			if (addr == 0x0000)
				comment += " ; RESET vector";
			if (addr == 0x0004)
				comment += " ; IRQ2 vector";
			if (addr == 0x0010 && (IsJUMP(p) || IsRTI(p)))
				comment += " ; TX1/IRQ1 vector";
			if (addr == 0x0014 && (IsJUMP(p) || IsRTI(p)))
				comment += " ; RX1/IRQ0 vector";
			if (addr == 0x0018 && (IsJUMP(p) || IsRTI(p)))
				comment += " ; Timer vector";
			if (auto it = annotations.find(addr) ; it != annotations.end())
				comment += it->second;

			if (overlayLoaderSubAddr != 0 && addr == overlayLoaderSubAddr)
				fprintf(fp, "\n; Overlay loader subroutine\n");
			if (overlayBase != 0 && addr == overlayBase && entrypoint != overlayBase)
				fprintf(fp, "\n; Overlay section\n");

			// write out the disassembled code
			fprintf(fp, "%04X %02X %02X %02X %s%s\n", addr, p[0], p[1], p[2], asmbuf, comment.c_str());
		}
		else if (addr < 0x001C)
		{
			// It's non-reachable code in the interrupt vector table, between
			// actual vector locations.  The ADSP-2105 reserves four opcode
			// slots per interrupt vector, to allow small handlers routines
			// to be encoded entirely inline with no jumps.  The DCS code
			// doesn't take advantage of this, so the three extra locations
			// between adjacent vectors is just left unused.  It's not
			// meaningful as opcodes or data, so just leave it out of the
			// listing entirely - it's just noise.
		}
		else if ((addr > 0x001B && addr < bootLength/4)
			|| (addr >= overlayBase && addr < overlayEnd))
		{
			// This code doesn't appear to be reachable, but it was loaded either
			// by the boot loader or by the overlay loader, so it might have some
			// kind of significance as data - the DCS code does use PM() space for
			// some static const data tables.  So show it as data bytes.  (I think
			// most of these locations are simply unused or are used as scratch
			// variable locations, so even showing them as bytes probably isn't
			// too useful, but I'm keeping them in the listing just in case they're
			// meaningful.  Maybe at some point I'll analyze the code further and
			// try to determine which locations are actually used, and what for.)
			// Also, note that the fourth byte isn't actually present on the
			// ADSP-2105 hardware - PM() locations are 24 bits wide.  But the
			// corresponding data in the ROM is stored in 4-byte chunks, and
			// we're really dumping the ROM here, not the loaded PM() data, so
			// show what's there in the ROM - again, just in case they stashed
			// something meaningful there that I haven't noticed yet.  There is
			// one location where something meaningful definitely is stashed:
			// the fourth byte of the first DWORD, which encodes the number of
			// program words to read in the automatic boot loader sequence.
			// But beyond that I think they're all just ignored.

			// The DCS ROMs mark unused sections of the boot and overlay code
			// with $FF bytes.  They tend to have long runs of $FF bytes at the
			// end of each section, and some of the overlays have $FF runs in
			// the middle as well (probably because they were laid out by a
			// linker as .TEXT and .DATA segments, with some $FF padding between
			// the two for alignment or the like).  For better readability,
			// collapse runs of $FF bytes in the listing.
			uint16_t addr2;
			auto p2 = p;
			for (addr2 = addr + 1 ; 
				(addr2 < bootLength/4 || addr2 >= overlayBase && addr2 < overlayEnd) && memcmp(p2, "\xFF\xFF\xFF\xFF", 4) == 0 ; 
				++addr2, p2 += 4);
			if (addr2 > addr + 4)
			{
				// show the consolidated entry
				fprintf(fp, "%04X FF FF FF .BYTE $FF REPEAT $%04X  ; %u bytes (%u DWORDs) of $FF fill, $%04X through $%04X\n",
					addr, (addr2 - addr) * 4, (addr2 - addr) * 4, addr2 - addr, addr, addr2 - 1);

				// advance to the final FF FF FF address
				addr = addr2 - 1;
				p = code + (addr *4);
			}
			else
			{
				// it's not an FF FF FF FF run, or it's too isolated - show it
				// as a single opcode slot with the data bytes
				fprintf(fp, "%04X %02X %02X %02X .BYTE $%02X, $%02X, $%02X, $%02X  ; [%c%c%c%c]\n",
					addr, p[0], p[1], p[2], p[0], p[1], p[2], p[3],
					CommentByte(p[0]), CommentByte(p[1]), CommentByte(p[2]), CommentByte(p[3]));
			}
		}
	}
}

// 
// Disassemble ADSP-2105 code
//
static void Disassemble(FILE *fp, const uint8_t *u2, uint16_t offset, uint16_t length, uint16_t loadAddr)
{
	// If length is zero, it means that we're to infer the length from the
	// ADSP-2105 automatic boot record at the given offset.  The auto-boot
	// length is in the fourth byte, in units of 8 DWORD entries, excess 1.
	if (length == 0)
		length = 4 * 8 * (u2[offset + 3] + 1);

	// Make a copy of the loaded data, so that we can make changes to
	// reflect the self-modifications that the DCS applies after startup.
	std::unique_ptr<uint8_t> codeBuf(new uint8_t[0x4000*4]);
	uint8_t *code = codeBuf.get();
	memset(code, 0, 0x4000*4);
	memcpy(code, u2 + offset, length);

	// generated comments per location
	Annotations annotations;

	// If we're loading from U2 $00000, the first 32-80 bytes of the ROM
	// always contain a human-readable signature string (after the first
	// four bytes, which necessarily encode the JUMP $xxxx that's loaded
	// at the RESET vector at PM($0000)).  The DCS boot loader code always
	// immediately patches the rest of the vector table from code stored
	// shortly after the signature.  This always consists of a series of
	// JUMP instructions, so it's fairly easy to find and patch manually.
	// Note that this wouldn't work in general for arbitrary ADSP-2105
	// code - it just happens that all of the DCS boot loaders work this
	// way, so we can make this assumption when we know we're working
	// with DCS ROMs.
	const char *caption = "", *subCaption = "";
	if (offset == 0x000)
	{
		// it's the hard boot loader
		caption = "Hard reset boot loader";
		subCaption =
			"; This program is loaded at initial power-on and when the board is\n"
			"; reset by an external signal.  The reset signal can be generated by\n"
			"; the WPC MPU board by writing to the sound control port, which the\n"
			"; WPC software uses to reset the board to initial conditions at\n"
			"; various times, such as when entering and exiting the operator menus.\n";

		// Scan for the first 18 xx xF instruction
		const uint8_t *p = u2 + (0x19 * 4);
		for (int limit = 128 ; limit != 0 && !IsJUMP(p) ; --limit, p += 4);

		// If we found the JUMP table, copy it.  Copy up to four vectors
		// ($0004, $0008, $000C, $0010), or until we run out of JUMP or RTI
		// instructions to copy.  Note that the patch table only contains
		// one opcode per vector, even though each vector has 4 opcode
		// slots.  The three other slots are left unpopulated.
		uint8_t *dst = code + (4 * 4);
		for (int limit = 4 ; limit != 0 && IsRTI(p) || IsJUMP(p) ; --limit, p += 4, dst += 4*4)
			memcpy(dst, p, 16);
	}
	else
	{
		// it's the soft boot loader
		caption = "Main DCS Decoder Program";
		subCaption =
			"; This program is loaded when the hard-boot loader completes (or bypasses)\n"
			"; its power-on self-tests, and resets the ADSP-2105 internally by writing\n"
			"; bit $0200 to the System Control Register at DM($3FFF).  This contains the\n"
			"; main DCS decoder program, which runs in a loop, accepting commands from\n"
			"; the WPC board via the sound data port and decoding the active audio\n"
			"; tracks into the PCM samples for transmission to the audio DAC via the\n"
			"; ADSP-2105's on-board serial port #1 (SPORT1).\n";
	}

	// If we're loading the soft-boot program (from a U2 source block
	// higher than $00000), the DCS program uses dynamic overlays to
	// load additional code beyond the automatic boot loader region.
	// The program needs more program memory space than is available
	// on the ADSP-2105, so it loads two sets of overlays to fit
	// everything into memory: an initialization overlay that's loaded
	// at startup, invoked as a subroutine just once, and then
	// discarded; and then a second overlay that's loaded into the
	// same space, ovewriting the initialization overlay, and then
	// kept resident in memory from that point on.  The code that loads
	// the overlays follows a standard template across DCS system
	// versions, so we can search for the characteristic instruction
	// sequences to infer the source and load location of the overlays,
	// to include them in the listing.
	uint16_t overlayLoaderSub = 0;
	uint16_t initOverlaySub = 0;
	uint16_t addrAfterInitCall = 0;
	int curOverlay = 0;
	struct OverlayInfo
	{
		void Add(uint16_t base, uint16_t length, uint32_t romOffset)
		{
			// if we don't have a base address yet, note the new base and
			// ROM bank source address
			if (this->base == 0)
			{
				this->base = base;
				this->romOffset = romOffset;
			}

			// note the new high-water mark
			if (base + length > this->end)
				this->end = base + length;
		}

		uint16_t base = 0;       // starting address of the overlay in PM space
		uint16_t end = 0;        // ending address of the overlay in PM space
		uint32_t romOffset = 0;  // offset in U2 of the start of the overlay code
	};
	OverlayInfo overlay[2];    // two overlays - initializer, main decoder
	if (offset != 0x0000)
	{
		// Trace from the reset vector to the first CALL.  The target
		// the overlay loader, and the parameters are given by the
		// last load of registers SI (ROM bank select), AX0 (number
		// of DWORDs to load), I0 (DM source address, in the ROM bank
		// select region), and I4 (PM destination address).  The DCS-95
		// software explicitly sets I4 before each call, and makes 
		// multiple calls to load multiple blocks.  The older software
		// only makes one call, and the caller doesn't set I4; the
		// destination is hard-coded as PM($0800).
		// 
		uint16_t si = 0, ax0 = 0, i0 = 0, i4 = 0x0800;
		for (uint16_t addr = 0 ; addr < 0x4000 ; ++addr)
		{
			const uint8_t *p = code + 4*addr;
			uint32_t op = ReadOpcode(p);
			if ((op & 0xF0000F) == 0x400000)						// AX0 = immediate
				ax0 = static_cast<uint16_t>((op >> 4) & 0x3FFF);
			else if ((op & 0xF0000F) == 0x400008)                   // SI = immediate
				si = static_cast<uint16_t>((op >> 4) & 0xFFFF);
			else if ((op & 0xFC000F) == 0x340000)					// I0 = immediate
				i0 = static_cast<uint16_t>((op >> 4) & 0x3FFF);
			else if ((op & 0xFC000F) == 0x380000)					// I4 = immediate
				i4 = static_cast<uint16_t>((op >> 4) & 0x3FFF);
			else if ((op & 0xFF000F) == 0x1C000F || (op & 0xFF000F) == 0x1D000F
				|| (op & 0xFF000F) == 0x1E000F || (op & 0xFF000F) == 0x1F000F)  // CALL
			{
				// It's a subroutine call.  If this is the first unique target
				// we've seen, it's the overlay routine.  If it's the second,
				// it's the initialization subroutine.
				uint16_t target = (op >> 4) & 0x3FFF;
				if (overlayLoaderSub == 0 || target == overlayLoaderSub)
				{
					// This is the first CALL we've seen, or it's another call
					// to the same target as the first call, so this is a call to 
					// the overlay loader subroutine.
					overlayLoaderSub = target;

					// Figure the translation from SI to U2 offset:
					//
					// - For the original board, the overlay is loaded at PM($0800),
					// and the ROM bank select in SI selects a 4K window in U2
					//
					// - For the DCS-95 board, the overlay is loaded at PM($2800),
					// and the ROM bank select in SI selects a 2K window in U2
					uint32_t romBankOffset;
					if (i4 < 0x2800)
					{
						// DCS audio board (1993).  The banked ROM window is at $2000, 
						// and the ROM bank select contains bits 12-19 of the ROM offset.
						romBankOffset = (si << 12) + (i0 - 0x2000);
					}
					else
					{
						// DCS-95 board.  The banked ROM window is at $0000, and the
						// ROM bank select contains bits 11-19 of the ROM offset.
						romBankOffset = (si << 11) + (i0 - 0x0000);
					}

					// add the region to the current overlay
					overlay[curOverlay].Add(i4, ax0, romBankOffset);

					// Copy the selected code into the program memory space
					memcpy(code + (i4 * 4), u2 + romBankOffset, ax0 * 4);

					// annotate the instruction
					char msg[128];
					sprintf_s(msg, " ; Load program overlay to PM($%04X) from ROM U2[$%05X], %d opcodes (%d bytes)",
						i4, romBankOffset, ax0, ax0*4);
					Annotate(annotations, addr, msg);
				}
				else if (initOverlaySub == 0)
				{
					// This is the second unique call target we've seen, so it's the
					// initialization subroutine.
					initOverlaySub = target;

					// remember the address of the start of the main program, following
					// the call to the initialization subroutine
					addrAfterInitCall = addr + 1;

					// The next call to the overlay loader will be loading the second
					// set of overlays, for the main decoder.
					curOverlay += 1;

					// annotate it
					Annotate(annotations, addr, " ; Call initialization overlay subroutine");
				}
				else
				{
					// This is a call to something other than the overlay loader
					// or the initializer.  The DCS code always loads all of the
					// post-initialization overlays immediately after the
					// initializer sub returns, and before doing anything else,
					// so as soon as we encounter a CALL that's to a third target,
					// we know that the overlay loading process is completed, and
					// we thus have the entire program in memory now.
					break;
				}
			}
		}
	}

	// Trace the reachable code
	// 
	// - For the hard-boot loader, there are no overlays, so we can
	//   simply trace from all of the interrupt vector locations -
	//   those are always reacahable by processor fiat.
	// 
	// - The soft-boot program contains overlays that are only active
	//   for part of the time the program is running, so the reachable
	//   code is in two parts:
	//
	//   - First, the main program, which is the set of code including
	//     the boot code loaded by the boot loader plus all of the
	//     code loaded in the second set of overlays.  The second set
	//     of overlays remains resident once loaded, so we can consider
	//     this plus the boot-loaded section to be the single combined
	//     main program.  To trace this, we consider the memory space
	//     as it is after the second set of overlays is loaded, and
	//     trace from all of the interrupt vectors.
	// 
	//   - Second, the initialization program, which is the set of
	//     code from the boot load only up to the first initializer
	//     subroutine call, plus the initialization overlay code.
	//     Since we've already traced all of the boot loader code 
	//     above as part of the "main program" set, we can exclude
	//     that for the purposes of the initialization overlay, and
	//     simply trace from the start of the initialization sub (at
	//     the very start of the overlay).  That's the only external
	//     entrypoint into this overlay, so there's no need to trace
	//     from the interrupt vectors for this portion.
	// 

	// trace and disassemble the main code body (everything loaded by the
	// ADSP-2105 automatic boot loader, and all of the final overlays)
	fprintf(fp, 
		"; ----------------------------------------------------------------------------\n"
		";\n"
		"; %s\n"
		"; %u DWORDs (%u bytes) loaded from ROM U2[%05x]\n"
		";\n"
		"%s"
		"\n",
		caption, length, length * 4, offset, subCaption);
	TraceAndDisassemble(fp, code, annotations, 0xFFFF, length, overlayLoaderSub, overlay[1].base, overlay[1].end);

	// if this section has overlays, trace and disassemble the initializer
	// overlay
	if (curOverlay != 0)
	{
		// Copy the overlay code from the U2 section into the code space.
		// Note that the ROM data is stored as bytes, but the PM space
		// addresses and sizes are expressed as DWORDs.
		memcpy(code + overlay[0].base*4, u2 + overlay[0].romOffset, (overlay[0].end - overlay[0].base)*4);

		// trace and disassemble the code
		fprintf(fp,
			"\n"
			";\n"
			"; Initialization overlay subroutine\n"
			";\n"
			"; This code is loaded at startup, invoked once, and then replaced\n"
			"; by the main program overlay.\n"
			"\n");
		TraceAndDisassemble(fp, code, annotations, overlay[0].base, 0, overlayLoaderSub, overlay[0].base, overlay[1].end);
	}
}
