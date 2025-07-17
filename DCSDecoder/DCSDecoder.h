// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder - base class for DCS audio player implementations
//
// This class defines the interface to C++ implementations of an
// audio player for DCS ROMs, replicating the functionality of the
// embedded software in the DCS audio boards installed in Williams/
// Bally/Midway pinball machines manufactured in the 1990s.  The
// interface is designed to allow for multiple implementations, so
// that client programs can be written independently of the concrete
// implementation used.  The main implementation options are running
// the original ADSP-2105 ROM code in emulation, and using a fully
// native, portable C++ re-implementation of the decoder.
// 
// This class is designed to be easily embedded in any host program,
// and can be used for real-time playback, or for streaming or capturing
// the decoded audio.  The audio stream is decoded into signed 16-bit
// PCM samples, suitable for direct playback through PC sound cards or
// even directly through a hardware DAC.  The decoded samples use the
// fixed DCS sample rate of 31250 samples per second.
// 
// Here's how to use this class in a client program:
// 
// - Define a concrete subclass of DCSDecoder::Host.  This lets
//   the host receive notifications of certain events in the
//   decoder that an attached WPC board would need to know about.
//   If you don't need any notifications, you can use MinHost
//   rather than defining your own custom subclass.
// 
// - If you're using the decoder with a WPC emulator, implement
//   the BootTimerControl() override in your host interface to
//   coordinate timing with the WPC subsystem via your emulator's
//   timing system.
// 
// - Create an instance of the desired concrete subclass.  
//   Choose DCSDecoderNative for the universal native version, or 
//   DCSDecoderEmulated for the ADSP-2105 emulator version.  The 
//   native version is much faster, so it's the one to use in most
//   situations.  The emulated version is primarily meant to serve 
//   as a reference point to verify that the native version yields
//   the same output as the original ADSP-2105 code.
// 
// - Load the ROM files:
// 
//   - If you're working with a ROM image zip file formatted
//     for use with PinMame, you can load it with one call to
//     LoadROMFromZipFile().
// 
//   - If you're working with your own data source for the
//     ROMs, load the images into memory using whatever means
//     you wish.  Each individual ROM image must simply be
//     loaded into a contiguous memory block.  Call AddROM()
//     for each ROM image to add it to the decoder's internal
//     list.  Then make one call to CheckROMs() to complete
//     the internal ROM setup based on the loaded data.
// 
// - Call HardBootDecoder()
// 
// - If you want to skip the startup "Bong", call
//   SoftBootDecoder()
//
// - If you're going to be playing back audio in real time, 
//   start your hardware sound player system.
// 
// - If you're using the decoder for something other than
//   playback (in other words, you won't just be calling
//   GetNextSample() in a loop), call SoftBootDecoder().  This
//   will happen automatically when doing simple decoding, but
//   you'll have to do it explicitly for other scenarios.
// 
// - Call GetNextSample() as many times as needed to fill (and
//   refill) your audio playback buffer.  For real-time playback,
//   you must monitor the hardware playback read position, and
//   refill the buffer as the hardware reader consumes samples.
//   (Remember that the decoder is single-threaded.  If your audio
//   player uses a separate thread to manage buffer refill, you
//   must ensure that it doesn't call GetNextSample() concurrently
//   with any other access to the object.)
// 
// - To send a command to the DCS decoder (for example, to start 
//   playback of a new track), call WriteDataPort().  This accepts
//   one byte at a time, as in the original hardware design, where
//   the system MPU board communicates with the sound board through
//   an 8-bit latch port.  DCS track commands are two bytes each,
//   which you can send by calling WriteDataPort() twice.  You can
//   write multiple bytes consecutively - no time delay is needed
//   between bytes.
// 
// The decoder is single-threaded.  It has no internal protection
// against concurrent access from multiple threads.  If your audio
// playback system is multi-threaded (for example, if it uses a
// background thread to refill its audio buffer), you must use your
// own means to ensure that decoder methods are never entered by
// multiple threads simultaneously.  The simplest way to do this
// is to ensure that all calls into the decoder are made from the
// same thread.  (Note that this doesn't have to be the foreground
// thread - all that matters is that it's always the *same* thread
// calling into the decoder.)  If single-threading isn't practical
// for your application, it is possible to use the decoder from
// multiple threads, as long as you implement your own mechanism
// to ensure that only one thread at a time calls into the decoder.
// For example, you could create a Windows mutex, and ensure that
// your code always acquires the mutex before calling any method
// in the decoder, and releases the mutex on return.
//

#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <string>
#include <list>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include "PlatformSpecific.h"

class DCSDecoder
{
public:
	// The DCS sample rate.  This is a fixed rate used by all of the
	// DCS pinball machines.
	static const int SAMPLE_RATE = 31250;

	// Host interface.  This lets the host implement the simulated
	// DCS->WPC data port.
	class Host
	{
	public:
		// Receive a byte on the DCS->WPC data port.  This transmits
		// status data from the sound board to the host system,
		// simulating the DCS->WPC data port of the original hardware.
		// This doesn't need to do anything if the host doesn't have
		// any use for the transmitted data.
		virtual void ReceiveDataPort(uint8_t data) = 0;

		// Clear any pending data in the DCS->WPC data port.  This
		// is called on a hard boot.  This doesn't need to do anything
		// if the host doesn't make use of the data port.
		virtual void ClearDataPort() = 0;

		// Set/clear the hard boot timer.  This simulates the timing
		// of the original DCS ROM code's hard boot process.  When
		// the DCS board is reset by an external signal (at power-on
		// or when the WPC board writes to the sound board's "control"
		// port), the original ROM software monitors the data port
		// for 250ms.  If a byte appears on the data port within
		// that time, the ROM immediately launches the main decoder
		// program.  This allows the WPC host to quickly reboot the
		// sound board at any time, to force it into a known state,
		// with no perceptible delay - the whole soft reset process
		// only takes a few milliseconds.  When the WPC wants to
		// perform a quick reset, it writes to the control port (to
		// initiate the hard reset) and then immediately writes a
		// byte to the data port to signal the soft reset.
		//
		// If nothing appears on the data port within the first
		// 250ms after a hard reset, the hard boot ROM program
		// proceeds to run its power-on self tests, which validate
		// the ROM checksums and run a read/write test on RAM.
		// If these pass, the board plays the startup "bong" and
		// soft boots into the main decoder program.
		//
		// The emulated version of the decoder doesn't perform any
		// of the power-on self-tests, as they're not particularly
		// meaningful in an emulation.  We still need to emulate
		// all of the timing behavior of the boot process, though,
		// for the sake of communicating properly with a separate WPC
		// emulator, if the host is using one.  That's what this host
		// interface is for.  It allows the host to provide an
		// asynchronous timer, using whatever mechanism is
		// appropriate for the host, that simulates the initial
		// 250ms delay.
		//
		// When we call this to set the timer, the host should
		// start a timer for 250ms (of real time or emulated time,
		// according to the host's design).  The new timer must
		// supersede any prior timer.  When the timer expires, the
		// host must call StartSelfTests().  Note that this class
		// assumes single threading, so the timer callback must be
		// on the same thread as all other calls to class methods.
		//
		// When we call this to clear the timer, the host should
		// simply cancel any previously set timer.
		//
		// This routine doesn't have to do anything at all if the
		// boot timing simulation isn't needed.
		virtual void BootTimerControl(bool set) = 0;

		// define a virtual destructor to ensure subclass cleanup
		// is properly invoked
		virtual ~Host() { }
	};

	// minimal host interface
	class MinHost : public Host
	{
		virtual void ReceiveDataPort(uint8_t data) override { }
		virtual void ClearDataPort() override { }
		virtual void BootTimerControl(bool set) override { }
	};

	// construction
	DCSDecoder(Host *host);

	// destruction
	virtual ~DCSDecoder();

	// descriptive name of the concrete decoder subclass type
	virtual const char *Name() const = 0;

	// Is the decoder running correctly?
	bool IsOK() const { return state != State::DecoderFatalError && state != State::InitializationError; }

	// Is the decoder booted and running?  This checks if the decoder is
	// finished with self tests and the startup "bong".  Returns false
	// during the boot phases or if in an error state.
	bool IsRunning() const { return state == State::Running; }

	// If IsOK() returns false, this returns a message describing
	// the error condition, for logging or display to the user
	std::string GetErrorMessage() const { return errorMessage; }

	// Zip file contents, for LoadROMFromZipFile
	struct ZipFileData
	{
		ZipFileData(const char *filename, size_t uncompressedSize) :
			filename(filename), data(new uint8_t[uncompressedSize]), dataSize(uncompressedSize)
		{ }

		std::string filename;
		int chipNum = -1;
		std::unique_ptr<uint8_t> data;
		size_t dataSize;
	};

	// Load ROMs from a Zip file.  This attempts to unpack the given
	// Zip into memory and parse the file contents as a PinMame ROM
	// image pack, identifying the DCS sound ROMs and loading them
	// into the ROM[] array.  
	// 
	// Returns ZipLoadStatus::Success on success, or one of the 
	// other enumerated status code values otherwise.   On failure,
	// a string with an explanation of the error will be placed in 
	// 'errorDetails', if that's not null.
	// 
	// 'explicitU2' can be set to the name of a file contained
	// within the Zip file that should be used as the U2 ROM image.
	// This can be used in cases where the heuristic we use to
	// figure out which file is which is unable to identify the
	// correct U2 image.
	// 
	// The list of ZipFileData objects is the container for the
	// binary data loaded from the Zip, which must be maintained
	// by the caller for the lifetime of the decoder object.  The
	// decoder contains pointers into the data buffers stored here,
	// so the buffers must stay valid as long as the decoder instance
	// exists.  (We foist the job of maintaining this container onto
	// the caller because we also want to allow the caller to set
	// up a decoder based on ROM data loaded separately by the
	// caller.  For efficiency, we don't want the decoder to have
	// to make its own private copies of the data in that case, so
	// by design, the decoder only keeps pointers to the ROM data,
	// not copies of it.  From the decoder's point of view, all ROM
	// data is owned by the client program.  So in the case where
	// the client program calls LoadROMFromZipFile() to load the
	// data, we still need the client to own the loaded data, thus
	// we pass back ownership via this list.)
	//
	// You can use this routine to load a PinMame ROM Zip pack
	// directly, without the caller needing to unpack the Zip itself
	// or parse their contents.  If instead you have raw ROM images 
	// that aren't packed into a Zip file, you should load them into
	// memory using whatever mechanism suits the data source, and
	// call AddROM() to add each ROM image to the decoder.  This
	// routine and AddROM() are mutually exclusive - use the one
	// that's suitable for your ROM data source. 
	enum class ZipLoadStatus
	{
		Success,         // ROMs successfully loaded
		OpenFileError,   // can't open the Zip file
		ExtractError,    // error extracting entry from Zip file
		NoU2,            // can't identify a valid U2 ROM image file
	};
	ZipLoadStatus LoadROMFromZipFile(const char *zipFileName, 
		std::list<ZipFileData> &zipFileData,
		const char *explicitU2 = nullptr,
		std::string *errorDetails = nullptr);

	// Find the catalog in a ROM U2 image.  The catalog contains
	// a list of the game's ROMs, with their sizes and checksums,
	// and the nubmer of tracks.  The different DCS versions place
	// the catalog at different points in U2, so this searches the
	// known locations for data that matches the catalog format.
	// The DCS system software version can be inferred from this
	// location.  Returns the offset of the first byte of the
	// catalog, or 0 if the catalog wasn't found at any of the
	// usual locations.
	//
	// Note that AddROM() automatically looks up the catalog when
	// adding the U2 image, so it's not necessary for clients to
	// call this routine.  It's provided for the sake of callers
	// who want to scan a ROM set directly.
	static uint32_t FindCatalog(const uint8_t *u2, size_t size);

	// Get the signature string from a ROM U2 image.  Checks
	// heuristically that the signature looks valid, and returns
	// a human-readable string if so.  Returns an empty string
	// if the signature doesn't match the usual conventions for
	// a DCS U2 image.  With no argument, this uses the loaded
	// U2 image; with an argument, you can check for a signature
	// in an arbitrary binary data block, without first loading
	// into the decoder.
	std::string GetSignature();
	static std::string GetSignature(const uint8_t *u2);

	// Add a ROM.  n is the nominal chip number from the original
	// DCS board layout, 2-9 for ROM chips U2-U9.  The DCS ROMs
	// are always either 512K or 1M.
	//
	// The decoder instance keeps a pointer to the ROM data, so 
	// the memory must remain valid for the lifetime of the decoder
	// object.
	void AddROM(int n, const uint8_t *data, size_t size);

	// Check if the ROMs are properly populated.  This can be called
	// after adding all of the game's ROMs via AddROM() to make sure
	// that the full set has been added.  The return codes are the
	// same as the original ADSP-2105 decoder status codes from the
	// power-on self tests:
	//
	//   0x01   = success
	//   0x02   = ROM U2 failed
	//   0x03   = ROM U3 failed
	//  ...
	//   0x09   = ROM U9 failed
	//   0x0A   = RAM test failed (never used)
	//   
	// 
	// (Code 0x0A is included for reference, because it's one of the
	// status codes that the original ADSP-2105 decoder's power-on
	// self-test can return, but it's never used here because we don't
	// bother with a RAM test.  We assume that the host computer did
	// its own hardware-appropriate RAM tests during the physical
	// power-up process, so there's nothing to be gained from a
	// generic RAM test here.)
	uint8_t CheckROMs();

	// Get the catalog offset in ROM U2.  Returns zero if the
	// catalog wasn't found or if the U2 image hasn't been added
	// yet via AddROM().
	uint32_t GetCatalogOffset() const { return catalog.ofs; }

	// Get the soft boot program offset.  This is the byte offset
	// in ROM U2 of the main decoder program.  It's always either
	// $01000 or $02000, depending on the DCS OS version.
	uint32_t GetSoftBootOffset() const;

	// Get the maximum valid track number from the catalog
	uint16_t GetMaxTrackNumber() const { return catalog.nTracks - 1; }

	// Get the details of a track program.  Returns true if the track
	// is valid, false if not.  The track number is from 0 to the
	// maximum track number from GetMaxTrackNumber().  The track address
	// is the 24-bit linear ROM address of the track's program data.
	// Channel is the playback channel, 0..5, where the track is to be
	// played.  The track type is 1 for a track that starts playing
	// immediately when loaded, 2 for a track that's deferred until
	// another channel starts it (this is used to make transitions
	// occur at pre-programmed music beats), and 3 for "indirect"
	// deferred play.  Type 3 is complicated; it sets things up so
	// that one of a list of possible tracks will be played at the
	// deferred transition, depending on the playback location of the
	// outgoing track when it triggers the transition.
	struct TrackInfo
	{
		// 24-bit linear ROM address of the track
		uint32_t address = 0;

		// Channel where the track plays
		int channel = 0;

		// Track type:
		// 
		//   1 - immediate: track plays immediately when loaded
		// 
		//   2 - deferred: track is loaded into channel, but doesn't
		//       play until another track program triggers it (this
		//       allows transitions to be programmed on music beats)
		//
		//   3 - deferred indirect: track is loaded into channel,
		//       and specifies a different track via a lookup table
		//       that's loaded when the program in another channel
		//       triggers the deferred track
		int type = 0;

		// Deferred track command/code.  For track types 2 and 3,
		// this is the 16-bit deferral code for the track.  This
		// is unused for track type 1 - type 1 tracks have full
		// byte-code programs instead of defer codes.
		uint16_t deferCode = 0xFFFF;

		// Playback time, expressed in units of frames (1 frame = 
		// 240 PCM samples = 7.68ms).  For a track that loops
		// forever, this is the time it takes to play back one
		// iteration of the outermost loop.
		uint32_t time = 0;

		// Does the track loop forever?  This is set to true if the
		// track has an infinite loop.  When this is true, the
		// playback time reflects one iteration of the outer loop.
		// This is only set to true when the track has an infinite
		// loop, not for loops that repeat a fixed number of times.
		bool looping = false;
	};
	bool GetTrackInfo(uint16_t trackNumber, TrackInfo &info);

	// Explain a track program.  This generates a human-readable
	// text listing of the contents of the track program.
	//
	// linePrefix is a string that's prepended to each line of the
	// generated text.  This can be used to indent the lines in a
	// listing.
	std::string ExplainTrackProgram(uint16_t trackNumber, const char *linePrefix);

	// Decompile a track program.  This decodes the track program 
	// into a list of data structures describing the program's
	// instruction steps.  This contains the same information as the
	// program's byte code sequence, just in a higher level format
	// that's easier to work with.  If the track is invalid, this
	// returns an empty vector.
	struct Opcode
	{
		// Byte offset of the instruction within the track program.
		// This is the offset of the first byte of the instruction's
		// delay count prefix.
		int offset = 0;

		// Loop nesting level.  The first instruction is at level 0;
		// instructions within loops are at their parent level plus 1.
		int nestingLevel = 0;

		// Loop parent instruction number. For an instruction nested
		// within a loop, this is the vector index in the decompiled
		// program vector of the "push" instruction that started the
		// loop.  This is -1 for top-level instructions.
		int loopParent = -1;

		// Delay count.  Zero means no delay; 0xFFFF is an infinite
		// delay (meaning that program execution can never proceed
		// into this instruction; it stops forever in a delay loop
		// at this point).  As with all DCS time counters, the time
		// unit is one frame, or 7.68ms.
		uint16_t delayCount = 0;

		// Opcode number
		uint8_t opcode = 0;

		// Number of bytes of operands.  This includes only the
		// operand bytes, if any; it doesn't include the fixed
		// three bytes that are part of every instruction (the
		// delay count prefix and opcode number).
		int nOperandBytes = 0;

		// The operand bytes.  This array is only populated as
		// far as nOperandBytes.
		uint8_t operandBytes[8] { 0, 0, 0, 0, 0, 0, 0, 0 };

		// Mnemonic description of the instruction, with operands
		std::string desc;

		// Human-readable numerical description of the instruction's
		// byte coding, showing the count prefix, opcode, and operands
		// as hex numbers, grouped in 2- or 4-digit units according
		// to their interpretational grouping within the instruction.
		// That is, elements that are interpreted as BYTE values are
		// shown as 2-digit hex numbers, and elements interpreted as
		// WORD values as 4-digit numbers.
		std::string hexDesc;
	};
	std::vector<Opcode> DecompileTrackProgram(uint16_t trackNumber);

	// Get a list of streams contained in the ROM.  Each entry is
	// the linear ROM pointer to a stream; this can be converted into
	// a pointer into the ROM memory with MakeROMPointer().
	std::list<uint32_t> ListStreams();

	// Retrieve the Deferred Indirect tables.  This scans the track
	// programs to find all DI references, and returns the reachable
	// table entries.  Note that it's possible for a ROM to contain
	// additional DI tables or entries that aren't reachable, but
	// there's no metadata giving the number of sizes of the tables,
	// so the best we can do is infer the table layout from the
	// program references.
	struct DeferredIndirectInfo
	{
		// Opcode 0x06 variables.  Each entry represents a variable
		// assigned a value in an opcode 0x06 step, with maxVal set
		// to the maximum value assigned by any step.
		struct Var
		{
			Var(int id, int maxVal) : id(id), maxVal(maxVal) { }
			int id;       // variable number, 0..255
			int maxVal;   // maximum assigned value, 0..255
		};
		std::list<Var> vars;

		// Deferred indirect tables
		struct Table
		{
			Table(int id) : id(id) { }
			int id;                      // table ID, 0..255
			std::list<uint16_t> tracks;  // track numbers stored in the table
			std::list<uint8_t> vars;     // variable numbers used to acccess this table
		};
		std::list<Table> tables;
	};
	DeferredIndirectInfo GetDeferredIndirectTables();

	// Enable/disable fast boot mode.  The point of fast boot mode is
	// to let the user optionally bypass the power-up self tests that
	// many original pinball ROM programs perform, for faster startup.
	// The native decoder doesn't actually perform any of the original
	// self-tests that the ROM code performs, regardless of the fast
	// boot option setting, since the tests are really not of much
	// value in an emulation.  But the simulator does at least play
	// the startup "bong" for the sake of re-creating the full user
	// experience (minus the tedious startup test delay).  Fast boot
	// mode bypasses the startup bong, allowing playback to begin
	// almost instantly.
	//
	// Note that fast boot mode has nothing to do with the hard/soft
	// reset functions.  Fast boot mode only controls whether or not
	// the simulated startup test procedure runs (which, again, only
	// amounts to the "bong" in this version).  The hard/soft reset
	// functions, in contrast, simulate the run-time control flow in
	// the decoder program.
	//
	// Set this to true for fast boot (bypassing the bong), false for 
	// the normal slow boot process (playing the bong at startup).
	void SetFastBootMode(bool fast) { fastBootMode = fast; }

	// Set the master volume, 0..255.  This has the same effect as
	// sending a set-volume command sequence (55 AA vol ~vol) to the
	// data port.
	virtual void SetMasterVolume(int vol) = 0;

	// Set the default volume, 0..255.  This is the volume setting that
	// goes into effect after a reset.  In the original pinball machine
	// environment, the DCS hardware doesn't have any non-volatile storage
	// of its own, so it depends upon the host (the WPC software) to send
	// a Set Volume command sequence (55 AA vol ~vol) through the data
	// port immediately after a reset.  For convenience in a non-pinball
	// software environment, we provide this routine, to let the caller
	// set a volume level that will be applied automatically after each
	// reset, so that the main program can set an initial volume without
	// any consideration for the startup timing, and without having to
	// repeat the volume command later if it resets the simulated board.
	void SetDefaultVolume(int vol) { defaultVolume = vol; }

	// Get the next sample (signed 16-bit PCM).  To implement audio
	// playback, the host program can simply call this routine in a
	// loop, writing each sample retrieved to its hardware audio
	// playback buffer.
	int16_t GetNextSample();

	// Hard Boot the decoder.  This starts the simulated hard boot
	// process.  On the original boards, this starts by monitoring the
	// data port for 250ms in a delay loop.  If a byte appears on the
	// data port, the program immediately soft boots into the normal
	// decoder.  If no byte arrives within 250ms, the program runs
	// the power-on self-tests (verifying ROM checksums and doing
	// read/write tests of RAM), then plays the startup "bong", and
	// finally boots into the decoder.  For the simulation, we simply
	// mark the internal state as booting; it's up to the host to
	// implement the 250ms delay timing.
	void HardBoot();

	// Complete the hard-boot 250ms delay.  The host can call this
	// after 250ms has elapsed (which can be either real time or
	// simulation time, depending on the host's needs) without any
	// data arriving on the sound data port.  The original ROM
	// program would start running its power-on self-tests at this
	// point (ROM checksum verification and RAM read/write tests),
	// which took about 10 seconds on the original boards.  If the
	// tests passed, the program played the startup "bong" and
	// then soft-booted into the decoder program.  This simulated
	// version simply plays the startup "bong" immediately (unless
	// we're in fast boot mode, in which case we simply boot into
	// the decoder without playing the bong).
	void StartSelfTests();

	// Soft Boot the decoder
	void SoftBoot();

	// Write a byte to the data port.  This simulates the WPC->DCS
	// data port latch.
	//
	// Command formats:
	//
	// aa bb  (where aa is in 00..54 and bb is any byte)
	//   Load and play track aabb (a 16-bit number).  The track
	//   number selects an entry in a master catalog found in U2,
	//   which points to a little byte-code program that in turn
	//   sequences audio playback.  The track numbers are
	//   idiosyncratic, but most of the games arrange things in
	//   groups of related tracks.  0000 is usually a "stop all"
	//   command, and the main music tracks are usually in the
	//   low 00xx range (0001 through 0020 or so).  Higher 00xx
	//   tracks are often shorter music cues and loops.  Higher
	//   numbers 01xx through 09xx are usually sound effects for
	//   event cues.
	// 
	// 55 AA vol ~vol
	//   Set the master volume.  (~vol is the bitwise NOT of the
	//   preceding byte, as an error check on the transmission.)
	//   Volume 00 is complete mute; FF is the highest setting,
	//   which plays samples at the recorded reference volume.
	//   Levels in between attenuate the PCM samples on a roughly
	//   logarithmic scale.
	// 
	// 55 Ax level ~level  (where Ax is AB..B0)
	//   Set the mixing level for the given channel (AB=1, AF=6).
	//   Internally, the decoder plays back up to six audio
	//   streams simultaneously, mixing them together to form
	//   the final output stream.  Note that "channel" doesn't
	//   refer to a stereo or surround channel - DCS for pinball
	//   was purely monophonic.  Channels are just internal
	//   constructs for playing back multiple clips at once and
	//   mixing them together in the output.  This command sets
	//   the relative volume of an individual channel in the
	//   final mix.
	// 
	// 55 Bx byte ~byte  (where Bx is BA..BF)
	//   This sets another per-track property that was never
	//   implemented in the DCS pinball ROMs.  The later ROM
	//   code (From version 1.04) recognizes this command, and
	//   stores the byte value in an internal data structure
	//   associated with the channel (BA = channel 1, BB=2,
	//   etc), but the value is never used anywhere else in
	//   the program.  This must have been something planned
	//   for future implementation that never made it into any
	//   of the pinball games.
	// 
	// 55 Cx  (where Cx is C2 or C3)
	//   Query the DCS software version number.  Sends back
	//   one byte on the DCS->WPC data port.  55C2 retrieves
	//   the major version number, 55C3 gets retrieves the
	//   minor version number.  This query only exists in the
	//   DCS-95 software, and it returns 1.03, 1.04, or 1.05,
	//   depending on the game.  (Our native code implementation
	//   returns 1.06 by default, to distinguish it from the
	//   emulated versions, but the host can override that
	//   if desired.)
	// 
	// I suspect that all of the 55xx codes beyond 55AA (Set
	// Master Volume) are for debugging purposes, or were for
	// features that were planned for future games but which were
	// never actually used, or which were added for DCS-based
	// video games rather than pinball.  I've never seen any of
	// these commands sent by WPC hosts.
	//
	void WriteDataPort(uint8_t b);

	// Clear any pending bytes on the data port
	void ClearDataPort();

	// ROM chips, U2-U9
	struct ROMInfo
	{
		void Set(int chipSelect, const uint8_t *data, size_t size, bool isDummy = false)
		{
			this->chipSelect = chipSelect;
			this->data = data;
			this->size = size;
			this->offsetMask = static_cast<uint32_t>(size - 1);
			this->isDummy = isDummy;
		}

		// chip select - 0=U2, 1=U3, etc
		int chipSelect = 0;

		// ROM data
		const uint8_t *data = nullptr;

		// size of ROM data
		size_t size = 0;

		// offset mask (size-1)
		uint32_t offsetMask = 0;

		// Flag: this is a dummy ROM entry.  During the ROM check, we
		// fill in empty ROM slots with placeholder memory set to all $FF
		// bytes, to simulate the original ADSP-2105 hardware's behavior
		// when reading from unpopulated ROM addresses.  This protects
		// against host platform memory access exceptions if the ROM data
		// loaded happens to contain internal pointers to unpopulated ROM
		// slots.  That sort of cross-reference to non-existent data is
		// inherently invalid, and implies that either a ROM image is
		// corrupted, or the ROM was compiled incorrectly.  It's entirely
		// possible that original factory ROMs could contain such errors,
		// and it's equally possible that new ROMs created with new tools
		// like DCSEncoder could contain such errors.  I'd rather that
		// the native decoder doesn't crash with a memory exception if it
		// should ever encounter such data.  One way to prevent crashes
		// would be to check each memory access when decoding a ROM
		// pointer, and we should do that *too*, but it's nice to have
		// a backup plan if we miss one of those, which is what this
		// provides.
		bool isDummy = false;

		// compute the ROM's checksum, using the DCS ROM POST procedure
		uint16_t Checksum() const { return Checksum(data, size); }

		// compute the checksum of a given ROM image
		static uint16_t Checksum(const uint8_t *p, size_t len);

		// read big-endian ints from the ROM at the given offset
		uint8_t ReadU8(uint32_t ofs) const { return data[ofs]; }
		uint16_t ReadU16(uint32_t ofs) const { return U16BE(data + ofs); }
		uint32_t ReadU24(uint32_t ofs) const { return U24BE(data + ofs); }
		uint32_t ReadU32(uint32_t ofs) const { return U32BE(data + ofs); }

	} ROM[8];

	// A ROMPointer encapsulates a pointer into a ROM's image
	// data.  This is mostly for internal use, but could be
	// useful for clients that want to parse the contents of
	// the ROMs.
	class ROMPointer
	{
	public:
		ROMPointer() : chipSelect(0), p(nullptr) { }

		ROMPointer(const ROMPointer &rp) :
			chipSelect(rp.chipSelect), p(rp.p) { }

		ROMPointer(int chipSelect, const uint8_t *p) : 
			chipSelect(chipSelect), p(p) { }

		// chip select code (0 = U2, 1 = U3, etc)
		int chipSelect = 0;

		// pointer to chip memory
		const uint8_t *p = nullptr;

		// Nominal chip number (the 'x' in the Ux/Sx reference designator
		// for the chip on the board).  This is simply the chip select
		// code plus 2.
		int NominalChipNumber() const { return chipSelect + 2; }

		bool IsNull() const { return p == nullptr; }
		void Clear() { chipSelect = 0; p = nullptr; }

		bool operator==(const ROMPointer &rp) const { return rp.p == p; }
		bool operator==(const uint8_t *p) const { return this->p == p; }

		// pointer arithmetic
		ROMPointer operator+(int ofs) const { return ROMPointer(chipSelect, p + ofs); }
		ROMPointer operator-(int ofs) const { return ROMPointer(chipSelect, p - ofs); }

		// dereferencing
		uint8_t operator *() const { return *p; }
		void operator++() { ++p; }
		void operator--() { --p; }
		ROMPointer operator++(int) { ROMPointer orig(chipSelect, p); ++p; return orig; }
		ROMPointer operator--(int) { ROMPointer orig(chipSelect, p); --p; return orig; }

		// general post-increment/post-decrement operator: bumps the internal
		// pointer by the given delta (which can be negative), and returns
		// the original unmodified pointer
		ROMPointer Modify(int d) { ROMPointer orig(chipSelect, p); p += d; return orig; }

		// read various types without increment
		uint8_t PeekU8() const { return *p; }
		uint16_t PeekU16() const { return U16BE(p); }
		uint32_t PeekU24() const { return U24BE(p); }
		uint32_t PeekU32() const { return U32BE(p); }

		// read various types with post-increment
		uint8_t GetU8() { return *p++; }
		uint16_t GetU16() { return Modify(2).PeekU16(); }
		uint32_t GetU24() { return Modify(3).PeekU24(); }
		uint32_t GetU32() { return Modify(4).PeekU32(); }
	};

	// Convert a UINT24 linear ROM address value read from ROM image
	// data into a pointer into the ROM.  This takes into account the
	// hardware platform that the ROM was formatted for, which is
	// necessary because the DCS-95 ROMs and earlier ROMs use slightly
	// different addressing formats.
	ROMPointer MakeROMPointer(uint32_t linearAddress) const;

	// Get the current offset from a ROM pointer.  This returns the
	// offset of the current address from the base of the ROM.
	uint32_t ROMPointerOffset(const ROMPointer &rp) const { return static_cast<uint32_t>(rp.p - ROM[rp.chipSelect].data); }

	// Hardware version.  This reflects the target hardware platform that
	// the loaded ROM was designed to work with.  We need to know which
	// platform the ROM data targets because the 24-bit linear ROM address
	// representation is different in the two versions - whenever we read
	// a 24-bit ROM address that's encoded within the ROM data, we have to
	// interpret it according to which hardware platform the ROM data was
	// designed to use.  
	// 
	// The target hardware platform for a given ROM can be inferred in a
	// number of ways.  The most definitive would be to parse the ADSP-2015
	// machine code in ROM U2 to determine how it's accessing certain
	// memory-mapped registers.  But that's rather difficult.  A much
	// simpler approach that appears to be completely reliable is to look
	// at the location of the "catalog" data in ROM U2.  The catalog is
	// always stored at one of three locations: $03000, $04000, or $06000.
	// DCS-95 games always (as far as I can tell) place it at $06000, and
	// the earlier games always place it at $04000, except for ST:TNG,
	// where it's at $03000.
	enum class HWVersion
	{
		Unknown,    // not yet detected
		Invalid,    // detection attempted and failed
		DCS93,      // original DCS sound board
		DCS95       // DCS-95 audio/video board
	};

	// Software version.  There appear to be around six distinct 
	// releases of the DCS software for pinball titles from 1993 to
	// 1998.  The later versions (starting with Attack From Mars) 
	// contain a formal version number, starting at 1.03 for AFM and
	// running through 1.05 for Cactus Canyon and Champion Pub (1998).
	// The earlier versions (prior to 1.03) don't have an apparent 
	// version number encoded within the software, but it seems
	// likely that the versions from 1.03 were so numbered because
	// earlier builds had internal version labels 1.00, 1.01, and 
	// 1.02.  For our purposes, though, we don't care about those
	// seven exact versions.  What matters to us is the degree of
	// compatibility of the data format in the ROMs.  Empirically,
	// there are three significant data formats: the format used
	// by the three DCS titles from 1993 (STTNG, IJTPA, JD); that
	// used by all remaining games released with the original
	// audio-only DCS board, through 1995; and the format used
	// by the titles released with DCS-95 audio/video boards.
	// We'll call these OS93, OS94, and OS95 - OS for Operating
	// System (to distinguish it from the hardware version), and
	// the year as the identifier, since that's the easiest way
	// for me to remember which is which.
	//
	enum class OSVersion
	{
		// not yet detected
		Unknown,

		// invalid - detection attempted and failed
		Invalid, 

		// OS93a - DCS hardware with first 1993 software release.  This
		// was used for exactly two games: Indiana Jones: The Pinball
		// Adventure, and Judge Dredd.  The 1993 titles use a different
		// audio frame format from the 1994+ titles, so a customized
		// decompression algorithm is needed for these titles.  They
		// also use a slightly different implementation (vs the 1994+
		// software) of the algorithm that transforms the frequency-
		// domain data contained in the compressed audio stream to
		// time-domain PCM samples for output.  The 1993 and 1994
		// algorithms perform identical math, but they use slightly
		// different intermediate calculations along the way, so the
		// final PCM results differ by a few parts per thousand due to
		// different amounts of accumulated rounding error.  That is,
		// you *can* feed 1993 frames into the 1994 transform (or vice
		// versa) and get almost the same results, but not *exactly*
		// the same results.  For perfect fidelity to the original
		// implementation, we have to use the separate 1993 algorithm
		// when decoding 1993 data.
		OS93a,

		// OS93b - DCS hardware with second 1993 software release.  This
		// was used for only one game, Star Trek: The Next Generation.
		// This uses the identical frame format and trasnformation
		// algorithm as the OS93a games, but it has a tiny difference
		// in how it calculates the mixing levels that requires
		// recognizing this as a separate version.
		OS93b,

		// OS94 - DCS hardware with 1994+ software.  This applies to all
		// titles using the original DCS audio-only boards, apart from
		// the three titles using the OS93 versions.  This uses a
		// completely different frame format compared to the OS93 ROMs,
		// and uses a slightly different algorithm to transform the
		// frequency-domain data in the audio streams to time-domain
		// PCM data for output.
		OS94,

		// OS95 - DCS-95 audio/video board with 1995+ software.  This
		// applies to all titles using the new hardware.  This version
		// uses the identical audio coding formats and decoding algorithms
		// as the OS94 software.  The only difference is in how the ROM
		// data represents pointers to other items in the ROM.  The
		// address representation shifts the "chip select" field one
		// bit to the left in this version, to accommodate the change
		// to the memory-mapped register that selects which ROM page is
		// mapped into the CPU address space.
		OS95
	};

	// Recognition codes for individual games.  Each DCS game has its
	// own copy of the ROM software, so it's entirely possible for a
	// game to include unique features in its DCS implementation.  For
	// the most part, the games *don't* do this; they mostly run the
	// same operating system software, using whichever revision was
	// current at the time of the game's release.  So most of the
	// differences can be attributed to the OS revision, not anything
	// idiosyncratic to the individual game.  However, I've run into
	// one instance so far where a game had a completely unique hack
	// in its ROM code, so it seems to be necessary to be able to
	// recognize individual games.  This is an enumeration of the
	// known DCS titles.  For reference, we'll include documentation
	// of any known title-specific hacks required per game in the
	// comments below.  
	// 
	// Note: the universal decoder should NEVER, EVER make exceptions 
	// or use special coding for individual games.  Never, ever, ever,
	// never, with no exceptions whatsoever, zero exceptions, simply
	// never... except when absolutely necessary.  The one current
	// example is TOTAN (see the list below), which has an
	// idiosyncracy that's so obviously a hack in the original ROM
	// code that there's simply no general way to handle it.
	//
	// Also: we DON'T use the recognized game to determine which OS
	// version the ROM is using.  We rely instead on known patterns
	// in the ROM program and data set.  This should be more reliable
	// because it's what's in the actual ROM program that matters to
	// us when selecting feature variations.
	enum class GameID
	{
		// Unknown - game ID has not yet been detected, or the
		// signature at the start fo the U2 ROM image isn't
		// recognizable as one of the known DCS titles.
		Unknown,

		//
		// DCS Pinball ROMs
		//

		// Attack from Mars, 1995 (no known hacks)
		AFM,

		// Cactus Canyon, 1998 (no known hacks)
		CC,

		// The Champion Pub, 1998 (no known hacks)
		CP,

		// Cirqus Voltaire, 1997 (no known hacks
		CV,

		// Corvette, 1994 (no known hacks)
		Corvette,

		// Demoliion Man, 1994 (no known hacks)
		DM,

		// Dirty Harry, 1995 (no known hacks)
		DH,

		// The Flintstones, 1994 (no known hacks)
		FS,

		// Indiana Jones: The Pinball Adventures (no known hacks)
		IJ,

		// Indianapolis 500, 1995 (no known hacks)
		I500,

		// Jack*Bot, 1995 (no known hacks)
		JB,

		// Johnny Mnemonic, 1995 (no known hacks)
		JM,

		// Judge Dredd, 1993 (no known hacks)
		JD,

		// Medieval Madness, 1997 (no known hacks)
		MM,

		// Monster Bash, 1998 (no known hacks)
		MB,

		// NBA Fastbreak, 1997 (no known hacks)
		NBAFB,

		// No Fear Dangerous Sports, 1995 (no known hacks)
		NF,

		// No Good Gofers, 1997 (no known hacks)
		NGG,

		// Popeye Saves the Earth, 1994 (no known hacks)
		Popeye,

		// Red & Ted's Road Show, 1994 (no known hacks)
		RS,

		// Safecracker, 1996 (no known hacks)
		SC,

		// Scared Stiff, 1996 (no known hacks)
		SS,

		// The Shadow, 1994 (no known hacks)
		TS,

		// Star Trek: The Next Generation (no known hacks)
		STTNG,

		// Tales of the Arabian Nights, 1996
		// 
		// Unique hack: IRQ2 handler processes command 03 E7 by sending
		// byte $11 to the host via the data port.  This processing is
		// hard-coded into the IRQ2 handler and bypasses the normal track
		// program invocation.  There is a track program 03E7 in this ROM,
		// which if invoked sends byte $10 instead, but because of the
		// IRQ2 hack the track program is never invoked.  This has the
		// distinct appearance of a last-minute bug-fix patch, perhaps
		// by a summer intern whose work was never reviewed (because
		// surely the cleaner and much simpler way to fix it would have
		// been to edit the track program for 03E7).  When TOTAN is
		// detected, we apply the same special handling in our IRQ2
		// handler to obtain the identical behavior to the original.
		TOTAN,

		// Theatre of Magic, 1995 (no known hacks)
		ToM,

		// World Cup Soccer, 1994 (no known hacks)
		WCS,

		// Who Dunnit, 1995 (no known hacks)
		WDI,


		//
		// DCS video game ROMs
		//

		// Killer Instinct, 1994
		KINST,

		// Mortal Kombat 2, 1993
		MK2,

		// Mortal Kombat 3, 1994
		MK3,

		// NBA Hangtime, 1994
		NBAHT,

		// Rampage World Tour
		RMPGWT,

		// WWF Wrestlemania Arcade, 1993
		WWFW,
	};

	// infer the game ID from a ROM U2 signature string
	static GameID InferGameID(const char *signature);

	// get the human-readable title for a game ID
	static const char *GetGameTitle(GameID id);

	// Get the game ID for the loaded game.  This will only
	// return value information after the U2 ROM image has been
	// loaded.
	GameID GetGameID() const { return gameID; }

	// Get the version information.  This will only return
	// valid information after the U2 ROM image has been loaded,
	// since they're detected from the contents of that ROM.
	// Fills in 'hw' and 'os' with the version information if
	// desired (they can be passed as null pointers if the enum
	// values aren't needed), and returns a human-readable string
	// describing the version.  
	std::string GetVersionInfo(HWVersion *hw = nullptr, OSVersion *os = nullptr) const;

	// Get the nominal version number.  This returns a 16-bit
	// value with the major version number in the high byte and
	// the minor version number in the low byte (e.g., version
	// 1.03 is expressed as 0x0103).  Returns zero if the version
	// information isn't available (e.g., if the system software
	// version couldn't be detected).
	//
	// For the DCS-95 boards, this returns the version number
	// embedded in the ROM program for the 55C2/55C3 command
	// handlers, if found.  These numbers range from 1.03 to 1.05
	// for titles released from 1995 to 1998.  Software for the
	// original DCS boards didn't have any embedded version number
	// information, so this code is unable to provide an official
	// version.  However, we can guess that the earliest release
	// in 1993 was probably internally labeled 1.0, and that the
	// releases between 1.0 and the first official labeled version
	// 1.03 were probably 1.01 and 1.02.  This routine therefore
	// returns 1.0 (0x0100) for an OS93 ROM and 1.01 for an OS94
	// ROM.  I don't currently have a way to tell if there are
	// any differences among the OS94 releases, so this routine
	// labels them all as 1.01 for now.
	int GetVersionNumber() const;

	// Get the number of channels that this ROM version supports.
	// The earliest 1993 ROMs only have 4 channels; most of the 
	// later systems have 6 channels, but a couple of 1998 games
	// have 8 channels.
	int GetNumChannels() const;

	// Subclass registration.  Concrete subclasses can define a
	// static instance of this subclass to add themselves to the
	// list of available implementations.
	class Registration
	{
	public:
		using FactoryFunc = std::function<DCSDecoder*(Host *host)>;
		Registration(const char *name, const char *desc, FactoryFunc factory);

		const char *name;
		const char *desc;
		FactoryFunc factory;
	};

	// map of available registrations
	using RegistrationMap = std::map<std::string, const Registration&>;
	static RegistrationMap &GetRegistrationMap();

protected:
	// Register a subclass
	static void Register(const Registration &reg);
	
	// Initialize the decoder from a software reset.  Returns true
	// on success, false on failure.  On failure, this should set
	// errorMessage to a string describing the problem.
	virtual bool Initialize() = 0;

	// IRQ2 handler - process queued data in the sound port
	virtual void IRQ2Handler() = 0;

	// Run the main decoder loop once to fill half of the autobuffer
	virtual void MainLoop() = 0;

	// Default volume setting to apply after a soft reset
	int defaultVolume = 0x67;

	// Reset exception.  This can be used to simulate a CPU reset
	// in the decoder if a fatal error occurs.  The original ROM
	// code resets the CPU if it encounters a bad byte-code value
	// in a track program.  This can be thrown out of the main loop
	// to accomplish the same effect; the caller will reinitialize
	// the decoder so that whatever bad state it got itself into
	// is cleared up, and then can resume decoding if desired.
	// Handling this with an exception lets the decoder unwind the
	// native stack back to the caller, just as the original
	// hardware would have reset its flow control state to the
	// reset entrypoint.
	class ResetException { };

	// Host environment interface
	Host *host;

	// Catalog information.  The catalog is a data structure in
	// ROM U2 that contains information on the ROM chips (which
	// chips are populated, their sizes, and checksums) and the
	// audio tracks contained in the ROMs.
	struct Catalog
	{
		// byte offset in ROM U2 of the start of the catalog
		uint32_t ofs = 0;

		// number of tracks
		uint16_t nTracks = 0;

		// Pointer to the start of the track index.  The track
		// index is an array of 3-byte entries giving the ROM
		// address of the start of each track's byte-code
		// program.  (This comes from catalog[$0046].)
		const uint8_t *trackIndex = nullptr;

		// Indirect deferred track load index.  This is used when
		// processing track program opcode 0x05.
		const uint8_t *indirectTrackIndex = nullptr;
	} catalog;

	// read big-endian ints
	static uint16_t U16BE(const uint8_t *p) {
		return (static_cast<uint16_t>(p[0]) << 8) | p[1];
	}
	static uint32_t U24BE(const uint8_t *p) {
		return (static_cast<uint32_t>(p[0]) << 16) 
			| (static_cast<uint32_t>(p[1]) << 8) 
			| p[2];
	}
	static uint32_t U32BE(const uint8_t *p) {
		return (static_cast<uint32_t>(p[0]) << 24) 
			| (static_cast<uint32_t>(p[1]) << 16) 
			| (static_cast<uint32_t>(p[2]) << 8) | p[3];
	}

	// Search a memory block containing ROM data for a sequence of 
	// ADSP-2105 opcodes.  The code sequenceis specified as a string, 
	// with hex digits for the opcodes in groups of 6.  Wildcards can 
	// be specified with letters outside of the A-F range or with 
	// asterisks.  When letters are used, they form variable names 
	// that will be populated in the variable map, if the instruction
	// pattern is matched.  If the variable map isn't needed, it can
	// be passed as null, in which case no variables will be returned.
	// Returns the address of the first instruction of the match on
	// success, or -1 on failure.
	//
	// There are two versions of this: one takes an array of uint32_t
	// memory containing instructions already loaded; the other takes
	// raw ROM data.  Data already loaded into uint32_t's is assumed
	// to have been translated into local byte order, whereas raw ROM
	// data is always in big-endian byte order.
	//
	// The return value is an offset from the memory, in units of the
	// pointer type provided.  startingAddr is likewise an offset at
	// which to start looking, in units of the pointer type.
	
	// search for opcodes in raw ROM data
	static int SearchForOpcodes(const char *opcodes,
		const uint8_t *romData, size_t romDataSizeBytes,
		int startingAddr = 0, std::unordered_map<char, uint32_t> *vars = nullptr);

	// search for opcodes in loaded PM() space
	static int SearchForOpcodes(const char *opcodes,
		const uint32_t *pmData, size_t pmSizeInOpCodes,
		int startingAddr = 0, std::unordered_map<char, uint32_t> *vars = nullptr);

	// common search handler
	static int SearchForOpcodes(const char *opcodes, std::function<uint32_t(int opcodeOffset)> fetch,
		size_t searchSpaceSizeInOpcodes, int startingAddr = 0, std::unordered_map<char, uint32_t> *vars = nullptr);

	// Placeholder data for ROMs not installed.  We'll set this
	// up with an 8K array to simulate the behavior of the original
	// hardware when reading from a missing ROM.  The hardware just
	// read missing locations as $FF bytes, without signaling any
	// errors.
	std::unique_ptr<uint8_t> missingRomData;

	// Detected version information
	HWVersion hwVersion = HWVersion::Unknown;
	OSVersion osVersion = OSVersion::Unknown;
	uint16_t nominalVersion = 0x0000;

	// Detected game ID
	GameID gameID = GameID::Unknown;

	// Decoder state
	enum class State
	{
		// Performing a hard boot
		HardBoot,

		// Playing the startup bong
		Bong,

		// Running
		Running,

		// Halted due to fatal error in decoder
		DecoderFatalError,

		// Halted due to initialization error
		InitializationError
	};
	State state = State::HardBoot;

	// Error message.  This is set to a descriptive error message
	// when in an error state.
	std::string errorMessage;

	// Current mode sample counter.  When we're in hard boot
	// mode or startup bong mode, we keep track of the number
	// of samples returned from that mode, to use as a proxy
	// for the time spent in the mode, for automatically
	// switching to the next mode after the appropriate time
	// in the mode elapses.
	int modeSampleCounter = 0;

	// Bong count remaining.  When in State::Bong mode, this
	// tracks how many more bong iterations are to be played,
	// including the current one.
	int bongCount = 0;

	// fast boot mode - if enabled, we skip the startup bong
	bool fastBootMode = false;

	// Read the data port
	uint8_t ReadDataPort();

	// data port queue
	std::list<uint8_t> dataPortQueue;
	uint8_t lastDataPortByte = 0;

	// Autobuffer.  This keeps track of the simulated ADSP-2105 autobuffer.
	// (The autobuffer is a native ADSP-2105 features that works like a DMA
	// controller to clock data out through an on-chip serial port.  On the
	// DCS boards, the serial port output is wired to the audio DAC input,
	// so the autobuffer serves as the DAC data source and sample clock.)
	struct
	{
		// Set the buffer parameters
		void Set(const uint16_t *base, uint16_t length, uint16_t step)
		{
			this->base = base;
			this->length = length;
			this->step = step;
		}

		// Start of the buffer
		const uint16_t *base = nullptr;

		// Buffer length, in 16-bit words
		uint16_t length = 0;

		// Sample step size, in 16-bit words
		uint16_t step = 0;
	} autobuffer;

	// Startup bong.  This simulates the distinctive startup self-test
	// bong of the DCS (and earlier WPC) sound boards.  The bong is a
	// a 195Hz square wave with a 750ms exponential decay envelope.
	struct Bong
	{
		// Set up the parameters for the start of the waveform
		void Start();

		// Sample generator.  This calculates the next sample in the waveform.
		// The audio stream update routine calls as many times as necessary
		// to refill the stream buffer.  Note that we'll generate samples
		// forever (although the envelope will eventually decay to silence).
		// The stream update routine takes care of detecting when the desired
		// duration has been reached, and switches to generating samples from
		// the DCS decoder instead.
		int16_t GetNextSample();

		// Cycle counter.  This counts the number of 31-sample cycles
		// we've played.
		int cycles = 0;

		// sample counters for the envelope and sign
		int envelopeSamples = 0;
		int signSamples = 0;

		// current attenuation level
		uint16_t level = 0x0000;

		// current sample sign (inverts every 50 samples)
		int sign = -1;

	} startupBong;

	// current ROM bank pointer
	const uint8_t *ROMBankPtr = nullptr;

	// utility routine - construct a std::string from a printf format
	static std::string vformat(const char *fmt, va_list va);
	static std::string format(const char *fmt, ...);
};

// Utility class to perform arbitrary cleanup when going out of scope.
class Cleanup {
public:
	Cleanup(std::function<void()> func) : func(func) { }
	~Cleanup() { func(); }
	std::function<void()> func;
};

