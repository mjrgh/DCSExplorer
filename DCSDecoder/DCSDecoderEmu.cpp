// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCSDecoder implementation based on ADSP-2105 opcode emulation
//
// This implements the DCS decoder class using ADSP-2105 emulation.  It's
// loosely based on the PinMame implementation of the DCS audio board, but
// there are no dependencies on anything from PinMame - this is an entirely
// separate, stand-alone class.  (We use the same ADSP-2105 emulator that
// PinMame uses, but the version we use is embedded as part of this project;
// don't import the one from PinMame.)
//
// This class has an important restriction: only one instance of the
// class can exist at any one time, because the ADSP-2105 emulator uses
// static variables to maintain the emulated machine state.  Note that you
// can still create instances of *other* DCSDecoder subclasses alongside an
// instance of DCSDecoderEmulated - you just can't create multiple emulator
// subclass instances at the same time.

#include <list>
#include <stdio.h>
#include <string.h>
#include "DCSDecoderEmu.h"
#include "adsp2100/adsp2100.h"

// subclass registration for the "strict" version, without the PinMame native speedups
static DCSDecoder::Registration registrationStrict("emulator-strict", 
	"ADSP-2105 emulator decoder (strict mode, PinMame native speedups disabled)",
	[](DCSDecoder::Host *host) { return new DCSDecoderEmulated(host, false); });

// subclass registration for the fast mode, with PinMame native speedups enabled
static DCSDecoder::Registration registrationFast("emulator-fast", 
	"ADSP-2105 emulator decoder (fast mode, PinMame native speedups enabled)",
	[](DCSDecoder::Host *host) { return new DCSDecoderEmulated(host, true); });


// static singleton instance pointer, to ensure that only one instance
// is using the ADSP-2105 static variables at a time
DCSDecoderEmulated *DCSDecoderEmulated::instance = nullptr;

DCSDecoderEmulated::DCSDecoderEmulated(Host *host, bool enableSpeedup) :
	DCSDecoder(host), enableSpeedup(enableSpeedup)
{
	// It's an error if another instance already exists, because the ADSP-2105
	// emulator uses static data for its pointers back into its client (this
	// class), which forces us to use static data as well.  We could work around
	// this with something like PinMame's scheme of swapping static data in and
	// out each time it invokes a CPU emulator, but that adds some performance
	// overhead, and there doesn't seem to be any need for multiple instances
	// anyway in expected use cases.
	if (instance != nullptr)
	{
		throw "Another DCSDecoderEmulated object already exists; this class "
			"contains static data, so it can only be instantiated as a singleton";
	}

	// set the singleton pointer
	instance = this;

	// clear the simulated ADSP-2105 memory spaces
	memset(PM, 0, sizeof(PM));
	memset(DM, 0, sizeof(DM));

	// initialize the ADSP-2015 emulator
	adsp2105_init();

	// set the emulator's internal PM() space pointer
	adsp2100_op_rom = &PM[0];
}

void DCSDecoderEmulated::EnableDebugger()
{
	// initialize the ADSP-2100 debugger
	adsp2100_init_debugger();
}

void DCSDecoderEmulated::DebugBreak()
{
	adsp2100_debug_break();
}

DCSDecoderEmulated::~DCSDecoderEmulated()
{
	// forget the global singleton
	if (instance == this)
		instance = nullptr;
}

void DCSDecoderEmulated::SetMasterVolume(int vol)
{
	// If we've identified the master volume level global variable
	// location, we can update the volume simply by poking the new
	// value into this location.  The ROM main loop recalculates the
	// PCM multiplier based on the volume level variable on every
	// iteration, so any change we make to the global takes effect
	// on the next main loop pass.
	if (masterVolumeAddr >= 0)
		DM[masterVolumeAddr] = static_cast<uint16_t>(vol);
}

bool DCSDecoderEmulated::Initialize()
{
	// Reset the emulated ADSP-2105
	adsp2105_reset(nullptr);

	// load the soft-boot program
	adsp2105_load_boot_data(ROM[0].data + GetSoftBootOffset(), &PM[0]);

	// set the ROM bank pointer to the base of U2
	curRomBank = MakeROMPointer(0);
	
	// variable map for opcode search results
	std::unordered_map<char, uint32_t> vars;

	// status message we set if we can't find the standard ROM
	// code fragments we use for necessary patch points
	static const char *incompatibleRomMessage =
		"The emulator was unable to find required data in the ROM.  The "
		"ROM image might be invalid, or it might contain a version of the "
		"DCS software that's not compatible with this emulator.";
	
	// Our first task is to find the critical point in the main
	// decoder loop in the ROM code where the decoder syncs up
	// with the hardware autobuffer.  We need to patch the code
	// there to trap out to the host, so that our caller can do
	// the *actual* hardware sync with the native audio system.
	// 
	// On the oldest versions of the ROM, the sync point is part
	// of the main bootstrap section that's brought into program
	// memory on boot.  On later versions, it's in an overlay
	// section that the bootstrap section loads dynamically.  To
	// deal with both cases, we have to look to see if it's in
	// memory first, and if not, let the initialization code run
	// long enough to load the overlay.  Let's start by looking
	// for the sync loop, which always starts with the same
	// opcode sequence in every ROM version:
	// 
	//    0D 02 A3  AR = I7
	//    4x xx x4  AY0 = $xxxx  ; address of the halfway point in the buffer
	//    26 E2 0F  AF = AR - AY0
	//    1y yy y4  IF LT JUMP $yyyy
	// 
	int syncLoopStart = SearchForOpcodes("0D02A3 4xxxx4 26E20F 1yyyy4");
	if (syncLoopStart >= 0)
	{
		// Found it - we can patch it immediately, without
		// waiting for overlays to load.  In fact, we *must*
		// patch it immediately, because the ROM program would
		// go directly into the sync loop after completing
		// initialization if we didn't, and we'd be stuck
		// there in an infinite loop, waiting for the auto-
		// buffer read point to advance, which it never will
		// in this emulated version.
		//
		// Patch the first instruction of the loop to trap out
		// to the host ($100000 is an illegal instruction that
		// our modified version of the emulator interprets as
		// a trap to the host).
		PM[syncLoopStart] = 0x010000;
	}

	// All version of the ROM code contain a little snippet of
	// code at the very top of their main loop, which is always
	// in the initial boot loader block that's loaded from a
	// soft reset, thus we can search for it immediately.  This
	// is a perfect point to insert a trap for the end of
	// initialization.
	//
	//   3C 10 25 CNTR = $0102
	//   3x xx x0 Io = $xxxx  ; location of an internal decoding buffer
	//   1x xx xE DO $xxxx UNTIL CE
	//
	mainLoopEntry = SearchForOpcodes("3C1025 3xxxx0 1xxxxE");
	if (mainLoopEntry < 0)
	{
		// this ROM is not compatible
		errorMessage = incompatibleRomMessage;
		return false;
	}

	// Patch a trap at the first instruction of the loop, then
	// invoke the ROM code via the RESET vector to run the
	// initialization routines.  The opcode interpreter will return
	// when the program counter reaches the trap instruction.
	//
	// In the case of the older code, the autobuffer sync wait
	// comes first, but we've already patched that above for the
	// older code, so we'll trap out there.
	PM[mainLoopEntry] = 0x010000;  // special TRAP TO HOST opcode
	adsp2105_execute(INT_MAX);
	
	// Un-patch that first initialization routine instruction we
	// pathced earlier.  It's always CNTR=$0102, opcode 3C 10 25.
	// Reset the program pointer back to that point, so that we
	// continue into the initizliation code when we reenter the
	// interpreter.
	PM[mainLoopEntry] = 0x3C1025;

	// If we're working with newer code where the sync loop is in
	// the dynamically loaded overlay code, we should be able to
	// find it now that the ROM's initialization code has had a
	// chance to run and load the rest of the program.
	if (syncLoopStart < 0)
		syncLoopStart = SearchForOpcodes("0D02A3 4xxxx4 26E20F 1yyyy4");

	// If we still can't find the sync loop, we can't use this ROM
	if (syncLoopStart < 0)
	{
		errorMessage = incompatibleRomMessage;
		return false;
	}

	// Now search for the *end* of the sync loop.  We need to know this
	// location, because this is where we re-enter the code after it
	// traps back to the host and then the host calls back into the
	// decoder.
	//
	// The contents of the sync loop vary slightly by versions, so we
	// can't just skip a fixed number of opcodes.  Fortunately, all
	// versions end with the same opcode sequence.  Search for the
	// *second* instance of this sequence following the start of the
	// loop:
	//
	//   4x xx xA  AR = $xxxx      ; address of autobuffer start OR midpoint
	//   9y yy yA  DM($yyyy) = AR  ; buffer write pointer for next output block
	//
	int syncLoopEnd;
	if ((syncLoopEnd = SearchForOpcodes("4xxxxA 9zzzzA", syncLoopStart + 1, &vars)) < 0
		|| (syncLoopEnd = SearchForOpcodes("4yyyyA 9zzzzA", syncLoopEnd + 1, &vars)) < 0)
	{
		// again, we can't use this ROM if we can't find this code
		errorMessage = incompatibleRomMessage;
		return false;
	}

	// skip the two instruction sat the end of the sync loop
	syncLoopEnd += 2;

	// Patch the start of the sync loop with a jump to the end of the
	// sync loop, so that we just bypass the whole thing every time the
	// main loop runs.  We also need to set the autobuffer write pointer,
	// so patch in the following sequence:
	//
	//   4x xx xA  AR = $xxxx   ; where xxxx is the lower of the values we found in the search
	//   9y yy yA  DM($zzzz) = AR
	//   18 zz zF  JUMP syncLoopEnd
	//
	uint32_t xVar = vars.find('x')->second, yVar = vars.find('y')->second;
	PM[syncLoopStart]   = 0x40000A | (((xVar < yVar ? xVar : yVar) & 0x3FFF) << 4);
	PM[syncLoopStart+1] = 0x90000A | ((vars.find('z')->second & 0x3FFF) << 4);
	PM[syncLoopStart+2] = 0x18000F | ((syncLoopEnd & 0x3FFF) << 4);

	// The 1993 software uniquely places the autobuffer sync wait at the 
	// top of the main loop, rather than near the end.  This changes our
	// estimation of where the "main loop" actually begins, because the
	// jump back to the top of the loop will be to a location two opcodes
	// before the wait loop.  We can detect this situation by looking to
	// see if what we took as the start of the main loop (the CNTR setup)
	// comes after the sync loop - if so, the sync loop (minus two
	// instruction slots) is the actual start of the main loop.
	if (mainLoopEntry > syncLoopStart)
		mainLoopEntry = syncLoopStart - 2;
	
	// Finally, find the JUMP back to the top of the main loop.  That's
	// the point where we want to trap back out of the emulator, since it
	// marks the end of one complete pass of the main loop.
	uint32_t mainLoopJumpOp = 0x18000F | (static_cast<int32_t>(mainLoopEntry) << 4);
	bool foundMainLoopJump = false;
	for (int addr = mainLoopEntry ; addr < 0x4000 ; ++addr)
	{
		if (PM[addr] == mainLoopJumpOp)
		{
			// patch a trap to the host here, and stop searching
			PM[addr] = 0x010000;
			foundMainLoopJump = true;
			break;
		}
	}

	// the main loop jump is another required identification point
	if (!foundMainLoopJump)
	{
		errorMessage = incompatibleRomMessage;
		return false;
	}


	// If desired, find the section of code covered by the PinMame
	// "speedup" routine, and patch it with a trap to the native
	// speedup code.
	if (enableSpeedup)
	{
		// the speedup patch is different for 1993 games and later games
		if (osVersion == OSVersion::OS93a || osVersion == OSVersion::OS93b)
		{
			// 1993 game - use the 1993 speedup, which starts with:
			//
			//    37 8F E1 I1 = $38FE
			//    37 90 02 I2 = $3900
			//    37 9F E3 I3 = $39FE
			speedupPatchAddr = SearchForOpcodes("378FE1 379002 379FE3");
			speedupFunc = &DCSSpeedup1993;
		}
		else
		{
			// It's not a 1993 game - use the 1994 speedup, which starts with:
			//
			//    00 00 00 NOP
			//    0C 00 80 DIS BIT_REV
			//    0C 20 00 DIS M_MODE
			speedupPatchAddr = SearchForOpcodes("000000 0C0080 0C2000");
			speedupFunc = &DCSSpeedup1994;
		}

		// if we found the speedup, patch it
		if (speedupPatchAddr > 0)
		{
			speedupPatchAddr += 3;
			PM[speedupPatchAddr] = 0x010000;
		}
		else
		{
			errorMessage = "The emulator was unable to find the ROM location for the "
				"\"speedup\" patch.  This ROM can't be used with the speedup, but it "
				"might still work with the speedup disabled via \"strict\" mode.";
			return false;
		}
	}

	// Search for the master volume level variable.  This is the byte value
	// of the last 55AA command.  All of the ROM versions have an identical
	// rouitine that derives the PCM volume level multiplier from this value,
	// so we can find the memory location by searching for the routine's
	// signature.  The ROM code recalcualtes the multiplier based on the
	// master volume variable on every pass of the main loop, so we can
	// change the volume at any time simply by poking a new value into the
	// variable's DM() location.
	//
	//   8x xx xF  SR1 = DM($xxxx)      ; this DM() location is the master volume variable
	//   2E 7F EF  AF = SR1, SR0 = SR1
	//   1y yy y0  IF EQ JUMP $yyyy
	//
	if (auto addr = SearchForOpcodes("8xxxxF 2E7FEF 1yyyy0", 0, &vars); addr > 0)
		masterVolumeAddr = static_cast<int>(vars.find('x')->second);

	// put the default volume into effect
	SetMasterVolume(defaultVolume);

	// success
	return true;
}

void DCSDecoderEmulated::IRQ2Handler()
{
	// diretcly invoke the IRQ2 handler via a recursive call to the interpreter
	adsp2100_host_invoke_irq(ADSP2100_IRQ2, 0, INT_MAX);
}

void DCSDecoderEmulated::MainLoop()
{
	// If we didn't find the main loop entrypoint, there's nothing
	// we can do here.  We also can't proceed if the autobuffer
	// hasn't been set up.
	if (mainLoopEntry < 0 || autobuffer.base == nullptr)
		return;

	// set the interpreter to enter at the top of the main loop
	auto &regs = adsp2100_get_regs();
	regs.pc = static_cast<int16_t>(mainLoopEntry);

	for (;;)
	{
		// Invoke the interpreter - it will return to the host when it
		// reaches the trap code we patched at the top of the main loop. 
		// This means that it will always execute for one decoder pass,
		// generating one frame (240 samples) of PCM samples in the
		// output buffer in DM() space.
		adsp2105_execute(INT_MAX);

		// If it trapped at the speedup code, invoke the speedup and
		// re-enter the interpreter.  Otherwise exit the loop.
		if (regs.pc == speedupPatchAddr + 1)
		{
			// entering the speedup code - invoke the native speedup,
			// and continue in the interpreter
			speedupFunc(regs, DM, PM);
		}
		else
		{
			// it's not the speedup, so it must be the trap that marks
			// the end of the main loop - return to the caller
			break;
		}
	}
}

// Original DCS (1993) board memory map
// D/P  Range        Description
//  D   0000..1FFF    RAM (16K)
//  D   2000..2FFF    Banked ROM (read only)
//  D   3000..3000    ROM bank select (sets upper 11 bits of banked ROM base address)
//  D   3800..39FF    RAM (1K)
//  D   3FE0..3FFF    ADSP control registers (write only) (adsp_control_w)
//  P   0000..0800    RAM (internal boot RAM) (8K + 3)
//  P   1000..2FFF    RAM (external RAM) (32k)
//  P   3000..3000    WPC main board data port (bidirectional)
//
// DCS-95 memory map
// D/P  Range         Description
//  D   0000..07FF    Banked ROM (read only)
//  D   1000..1FFF    RAM
//  D   2000..2FFF    Banked RAM - not actually used by any DCS-95 software
//  D   3000..3000    ROM bank select 1 (write only)
//  D   3100..3100    ROM bank select 2 (write only)
//  D   3200..3200    RAM bank select
//  D   3300..3300    Sound data port read=receive/write=send
//  D   3800..39ff    RAM
//  D   3fe0..3fff    ADSP-2105 memory-mapped control registers
//  P   0000..0800    RAM (internal boot RAM)
//  P   1000..3fff    RAM (external RAM)
// 
// Note that the DCS-95 "banked RAM" section can be implemented as
// ordinary RAM.  The hardware implemented this as a way to access an
// expanded RAM space by mapping the 8K byte region at DM($2000.$2FFF)
// to one of two banks of physical RAM.  The currently active bank is
// selected by writing to the memory-mapped register at DM($3200).
// The DCS-95 software does write to the bank-select register, but it
// doesn't actually care if the feature works.  It doesn't even test
// that it works in the POST;.  The software just treats the banked
// RAM area as scratch memory and doesn't care if values in the
// "swapped out" bank are truly swapped out - that is, the software
// doesn't care whether or not the "swapped out" memory is preserved
// when it's swapped back in.  The only thing the DCS-95 software
// does with the banked memory is periodically write to the bank
// select register in the main loop to switch between the two banks.
// That bank select write can be entirely ignored without any effect
// on the software's operation.  Why did they bother to write to the
// bank select register at all, then?  My guess is that they added
// the bank swap code as an experiment to test out the feature, and
// then they never got around to doing anything with it, but also
// never got around to deleting the experimental code.  That's what
// most production software is like, full of mysterious code that no
// one wants to touch because no one's sure if it does something
// important.
// 
// Most of the ADSP-2105 memory-mapped registers can be ignored.
// The only things that are important to the ROM emulation are the
// "reset" bits in the System Control Register ($3FFF bit $0200), and
// the autobuffer setup codes in register $3FEF.  PinMame recognizes a
// great many more of the defined bits, but it doesn't really have to,
// because the DCS code always sets the same registers to the same
// bit patterns, and those bit patterns all select things that both
// emulators (PinMame's and this one) are hard-wired to do anyway.
// If we tried to run ADSP-2105 code that *didn't* set all the same
// bit patterns, that code wouldn't work properly in either emulator.
// So there's no practical value in bothering to process any of the
// irrelevant registers or mode settings.
//
uint16_t DCSDecoderEmulated::ReadDM(uint16_t addr)
{
	// check for reads from special memory-mapped registers and peripherals
	if (hwVersion == HWVersion::DCS93)
	{
		// Original DCS (1993) board
		if (addr >= 0x2000 && addr <= 0x2FFF)
		{
			// Banked ROM access.  Note that ROM accesses only read
			// one byte at a time - the DCS boards only wired 8 bits
			// of the data bus to the ROMs.
			return curRomBank.p[addr - 0x2000];
		}
	}
	else
	{
		// DCS-95 board memory
		if (addr >= 0x0000 && addr <= 0x07FF)
		{
			// banked ROM
			return curRomBank.p[addr];
		}
		else if (addr == 0x3300)
		{
			// data port
			return ReadDataPort();
		}
	}

	// no special handling for this location - use the DM[] array
	return DM[addr & 0x3FFF];
}

void DCSDecoderEmulated::WriteDM(uint16_t addr, uint16_t data)
{
	// store the location in the DM array
	DM[addr & 0x3FFF] = data;

	// check for writes to special locations with side effects
	if (hwVersion == HWVersion::DCS93)
	{
		// Original DCS (1993) board
		if (addr == 0x3000)
		{
			// ROM bank select.  On the original DCS boards, this sets
			// the upper 11 bits of the address, with the low 12 bits
			// provided by the DM($2000) offset.
			curRomBank = MakeROMPointer(static_cast<uint32_t>(data) << 12);
		}
	}
	else
	{
		// DCS-95 board memory
		if (addr == 0x3000 || addr == 0x3100)
		{
			// ROM bank select 1/2.  Writing either one updates the ROM
			// base pointer by piecing together the two registers to
			// form the address.
			uint32_t chipSelect = (DM[0x3100] >> 2) & 0x07;
			uint32_t offset = ((static_cast<uint32_t>(DM[0x3100]) & 0x01) << 19) + (static_cast<uint32_t>(DM[0x3000] & 0xff) << 11);
			curRomBank = MakeROMPointer((chipSelect << 21) | offset);
		}
		else if (addr == 0x3300)
		{
			// data port write
			host->ReceiveDataPort(static_cast<uint8_t>(data));
		}
	}

	// the memory-mapped CPU control registers are the same in all versions
	if (addr == 0x3FEF)
	{
		// Autobuffer control register.  If bit $0002 is set, it
		// enables SPORT1 and sets the autobuffer parameters.  For
		// the purposes of the emulation, we only care about noting
		// the autobuffer register settings, which tell us the
		// location in DM() space of the autobuffer.  We actually
		// know this anyway from the version of the software, since
		// it's always the same in each version, but it's better to
		// respect the explicit declaration from the ROM code just
		// in case there's an oddball version out there somewhere.
		if ((data & 0x0002) != 0)
		{
			// decode the bit fields to get the Ix and Mx registers
			// defining the autobuffer (the Ix register implies the
			// associated Lx register, since they're always paried)
			int ireg = (data >> 9) & 7;
			int mreg = ((data >> 7) & 3) | (ireg & 0x04);

			// Remember the parameters
			auto &regs = adsp2100_get_regs();
			autobuffer.Set(&DM[regs.i[ireg]], regs.l[ireg], regs.m[mreg]);
		}
	}
	else if (addr == 0x3FFF)
	{
		// System control register.  The only bit that matters for
		// the purposes of the emulation is $0200, which soft-boots
		// the decoder.  To implement this, throw a reset exception
		// to unwind the host stack out to the caller, which can
		// reset the CPU and resume operation.
		if ((data & 0x0200) != 0)
			throw ResetException();
	}
}

uint32_t DCSDecoderEmulated::ReadPM(uint16_t addr)
{
	// The PM space has only one special location, and only on the original
	// (1993) boards: PM($3000) is the data port.  All other locations on the
	// original boards - and all locations on the DCS-95 boards - are ordinary
	// RAM.  Note tha this routine will never actually be called for any
	// location other than PM($3000), because the ADSP-2105 emulator treats
	// this as a special case in order to minimize the amount of overhead on
	// opcode reads.  But we'll implement this as though it could be called
	// for any location, to protect against any changes in those assumptions
	// in the emulator.
	if (hwVersion == HWVersion::DCS93 && addr == 0x3000)
		return ReadDataPort();

	// for all other locations, read from the PM array
	return PM[addr & 0x3FFF];
}

void DCSDecoderEmulated::WritePM(uint16_t addr, uint32_t data)
{
	// save the update to the PM[] array
	PM[addr] = data;

	// PM($3000) is the data port, only for the original DCS boards (not DCS-95)
	if (hwVersion == HWVersion::DCS93 && addr == 0x3000)
		host->ReceiveDataPort(static_cast<uint8_t>(data));
}

int DCSDecoderEmulated::SearchForOpcodes(const char *opcodes,
	int startingAddr, std::unordered_map<char, uint32_t> *vars)
{
	return DCSDecoder::SearchForOpcodes(opcodes, PM, _countof(PM), startingAddr, vars);
}

// --------------------------------------------------------------------------
//
// Client-defined globals required by the ADSP-2105 emulator
//

// CPU ROM - memory space provided by the client program
uint32_t *adsp2100_op_rom;

// These routines are special-cased for the DCSDecoderEmulator.  We redirect
// memory reads and writes to external handlers provided by the client program.
// Note that PM() read/write operations are only directed to the client for
// the special location PM($3000) - other addresses are handled directly via
// the oprom array.
uint32_t adsp2100_host_read_dm(uint32_t addr)
{
	return DCSDecoderEmulated::instance->ReadDM(static_cast<uint16_t>(addr));
}

void adsp2100_host_write_dm(uint32_t addr, uint32_t data)
{
	DCSDecoderEmulated::instance->WriteDM(static_cast<uint16_t>(addr), static_cast<uint16_t>(data));
}

uint32_t adsp2100_host_read_pm(uint32_t addr)
{
	return DCSDecoderEmulated::instance->ReadPM(static_cast<uint16_t>(addr));
}

void adsp2100_host_write_pm(uint32_t addr, uint32_t data)
{
	DCSDecoderEmulated::instance->WritePM(static_cast<uint16_t>(addr), data);
}


// --------------------------------------------------------------------------
//
// DCS Speedups - hand-coded translations of the DCS decoder inner loops
// from the original ADSP-2105 machine code to portable C.  The translated
// code performs the compute-intensive part of the decoding as native code
// on the host CPU, rather than in emulation, which makes it fast enough
// to keep up with real-time playback even on slower machines.
//

// Speedup for all games 1994 and later.  This is common code for all
// games excluding the first three releases of 1993 (IJTPA, JD, STTNG).
void DCSDecoderEmulated::DCSSpeedup1994(adsp2100_Regs &regs, uint16_t *DM, uint32_t *PM)
{
	// figure which hardware variation we're working with
	uint16_t *ram1source, *ram2source, volume;
	uint32_t volumeOP = PM[regs.pc + 0x2b84 - 0x2b45];
	if (regs.pc > 0x2000)
	{
		ram1source = &DM[0x1000];
		ram2source = &DM[0x2000];
		volume = ram1source[((volumeOP >> 4) & 0x3fff) - 0x1000];
	}
	else
	{
		ram1source = &DM[0x0700];
		ram2source = &DM[0x3800];
		volume = ram2source[((volumeOP >> 4) & 0x3fff) - 0x3800];
	}

	// perform the frequency domain to time domain transform
	uint16_t *i0, *i2;
	i0 = &ram2source[0];
	i2 = &ram2source[0x0080];
	/* M0 = 0, M1 = 1, M2 = -1 */
	for (int ii = 0; ii < 0x0040; ii++) {
		int16_t ax0, ay0, ax1, ay1, ar;
		ax0 = *i0++;
		ay0 = *i2;
		ax1 = *i0--;
		ar = ax0 + ay0;
		*i0++ = ar;
		ar = ax0 - ay0;
		*i2++ = ar;
		ay1 = *i2;
		ar = ax1 + ay1;
		*i0++ = ar;
		ar = ax1 - ay1;
		*i2++ = ar;
	}

	int mem63d, mem63e, mem63f;
	mem63d = 2;
	mem63e = 0x40;
	mem63f = mem63e >> 1;
	for (int ii = 0; ii < 6; ii++)
	{
		uint16_t *i0, *i1, *i2, *i4, *i5;
		int16_t m2, m3;
		i4 = &ram1source[0x0080];
		i5 = &ram1source[0x0000];
		i0 = &ram2source[0x0000];
		i1 = &ram2source[0x0000];
		m2 = mem63e;
		i1 += m2;
		i2 = i1;
		m3 = mem63e - 1;
		for (int jj = 0; jj < mem63d; jj++)
		{
			int16_t mx0, my0, my1;
			my0 = *i4++;
			my1 = *i5++;
			mx0 = *i1++;
			for (int kk = 0; kk < mem63f; kk++) {
				int16_t mx1, ax0, ay0, ay1, ar;
				int32_t tmp, mr;
				mx1 = *i1++;
				tmp = (((int32_t)mx0 * my0)<<1);
				mr = tmp;
				ay0 = *i0++;
				tmp = (((int32_t)mx1 * my1)<<1);
				mr = (mr - tmp + 0x8000) & (((tmp & 0xffff) == 0x8000) ? 0xfffeffff : 0xffffffff);
				ax0 = mr>>16;
				tmp = (mx1 * my0)<<1;
				mr = tmp;
				ay1 = *i0--; /* M0 = -1 */
				tmp = (((int32_t)mx0 * my1)<<1);
				mr = (mr + tmp + 0x8000) & (((tmp & 0xffff) == 0x8000) ? 0xfffeffff : 0xffffffff);
				mx0 = *i1++;
				ar = ay0 - ax0;
				*i0++ = ar;
				ar = ax0 + ay0;
				*i2++ = ar;
				ar = ay1 - (mr>>16);
				*i0++ = ar;
				ar = (mr>>16) + ay1;
				*i2++ = ar;
			}
			i2 += m2;
			i1 += m3;
			i0 += m2;
		}
		mem63d <<= 1;
		mem63e = mem63f;
		mem63f >>= 1;
	}

	uint16_t my0;
	i0 = &ram2source[0x0000];
	my0 = volume < 0x8000 ? volume : 0x8000;
	for (int ii = 0; ii < 0x0100; ii++)
	{
		int16_t mx0;
		int32_t mr;
		mx0 = *i0;
		mr = ((int32_t)mx0 * my0);
		*i0++ = mr>>15; // >>16;
	}
	regs.pc = regs.pc + 0x2b89 - 0x2b44;
}

// Speedup for the first three DCS titles, all released in 1993
// (IJTPA, JD, STTNG).  These three games use a different algorithm
// from all of the later titles to perform the frequency domain to
// time domain transformation to produce the final PCM data.
void DCSDecoderEmulated::DCSSpeedup1993(adsp2100_Regs &regs, uint16_t *DM, uint32_t *PM)
{
	// The first time this is invoked, build the bit reversal addressing table
	static uint16_t reverse_bits[0x4000];
	if (reverse_bits[1] == 0)
	{
		for (int i = 0 ; i < 0x4000 ; ++i)
		{
			// calculate the bit reversal for the 14-bit index
			int rev = 0;
			for (int j = 0, bit = 1; j < 14; ++j, bit <<= 1)
			{
				rev <<= 1;
				if ((i & bit) != 0)
					rev |= 1;
			}
			reverse_bits[i] = rev;
		}
	}

	uint32_t volumeOP = PM[regs.pc-5 + 0x0135 - 0x00e8];
	uint16_t *ram = DM;
	uint16_t volume = ram[((volumeOP >> 4) & 0x3fff)];

	uint16_t *i0, *i1, *i2, *i3;
	i0 = &ram[0x3800];
	i1 = &ram[0x38fe];
	i2 = &ram[0x3900];
	i3 = &ram[0x39fe];
	*i2++ = *i0++;
	*i0++ = 0;
	*i2++ = 0;
	for (int ii = 0 ; ii < 0x0040 ; ++ii)
	{
		int16_t ax0, ay0, ax1, ay1, ar;
		ax0 = *i0++;
		ay0 = *i1++;
		ar = ax0 + ay0;
		ax1 = *i0--;
		*i0++ = ar;
		ay1 = *i1--;
		*i1++ = ar;
		ar = ax0 - ay0;
		*i2++ = ar;
		ar = ay0 - ax0;
		*i3++ = ar;
		ar = ax1 + ay1;
		*i2++ = ar;
		*i3 = ar; i3 -= 3;
		ar = ax1 - ay1;
		*i0++ = ar;
		ar = ay1 - ax1;
		*i1 = ar;
		i1 -= 3;
	}

	adsp2100_set_mstat(0x0000);

	int mem621, mem622, mem623;
	mem621 = 2;
	mem622 = 0x80;
	mem623 = 0x40;
	for (int ii = 0 ; ii < 7 ; ++ii)
	{
		uint16_t *i0, *i1, *i2;
		uint32_t *i4, *i5;
		int16_t m2, m3;

		i4 = &PM[0x1780];
		i5 = &PM[0x1700];
		i0 = &ram[0x3800];
		i1 = &ram[0x3800];
		m2 = mem622;
		i1 += m2;
		i2 = i1;
		m3 = mem622 - 1;
		for (int jj = 0 ; jj < mem621 ; ++jj)
		{
			int16_t mx0, my0, my1;
			my0 = (*i4++) >> 8;
			my1 = (*i5++) >> 8;
			mx0 = *i1++;
			for (int kk = 0 ; kk < mem623 ; ++kk)
			{
				int16_t mx1, ax0, ay0, ay1, ar;
				int32_t tmp, mr;
				mx1 = *i1++;
				mr = ((int32_t)mx0 * my0) << 1;
				ay0 = *i0++;
				tmp = ((int32_t)mx1 * my1) << 1;
				mr = (mr - tmp + 0x8000) & (((tmp & 0xffff) == 0x8000) ? 0xfffeffff : 0xffffffff);
				ax0 = mr >> 16;
				mr = ((int32_t)mx1 * my0) << 1;
				ay1 = *i0--;
				tmp = (((int32_t)mx0 * my1)<<1);
				mr = (mr + tmp + 0x8000) & (((tmp & 0xffff) == 0x8000) ? 0xfffeffff : 0xffffffff);
				mx0 = *i1++;
				ar = ay0 - ax0;
				*i0++ = ar;
				ar = ax0 + ay0;
				*i2++ = ar;
				ar = ay1 - (mr >> 16);
				*i0++ = ar;
				ar = (mr >> 16) + ay1;
				*i2++ = ar;
			}
			i2 += m2;
			i1 += m3;
			i0 += m2;
		}
		mem621 <<= 1;
		mem622 = mem623;
		mem623 >>= 1;
	}

	uint16_t *i4;
	int16_t mx0;
	uint16_t my0;
	uint16_t idx1 = 7;
	i4 = &ram[0x3801];
	my0 = volume < 0x8000 ? volume : 0x8000;
	mx0 = DM[reverse_bits[idx1]];
	idx1 += 0x20;
	for (int ii = 0 ; ii < 0x100 ; ii++)
	{
		uint32_t mr = ((int32_t)mx0 * my0); // << 1; // see shift below
		mx0 = ram[reverse_bits[idx1]];
		idx1 += 0x20;
		*i4 = mr >> 15; // >> 16; // see above
		i4 += 2;
	}

	regs.pc = regs.pc - 4 + (0x13a - 0x00e8);
}
