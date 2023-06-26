// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder implementation using interpreted ADSP-2105 machine code
//
// This implements the DCSDecoder interface using Aaron Giles's ADSP-2100
// emulator to run the original machine code found in the DCS ROMs.  This
// is the way that PinMame has always implemented DCS emulation.  
// 
// The most important goal of the DCSDecoder class is to provide a full
// native implementation of the decoder, which is implemented separately.
// Native means no emulation - a full C++ translation of the decoder.
// So why are we talking about emulation now???  Because we're providing
// the emulator implementation *in addition to* the native decoder.  The
// emualtor is a reference point to compare the native deocder against
// to determine if it's working correctly.  Running the original ROM code
// in emulation is as close as we can get to a reference standard, short 
// of running the original ROM code on the original hardware.  Of course,
// the emulator isn't guaranteed to be a perfect replica, since it could
// have errors in the ADSP-2105 CPU emulator, or in our virtualization
// of the peripheral hardware environment, that cause the emulator to
// behave differently from the original boards.  But at least we're
// reasonably assured that the same sequence of ADSP-2105 instructions
// is being executed.
//
// Note that this class can only be instantiated once at a time, because
// the ADSP-2105 emulator that it's based on is defined using static
// state variables.  This limitation could in principle be lifted by
// saving and restoring the emulator's entire state on each call (this
// is what PinMame does with many of its CPU emulators, most of which
// also use statics to maintain their state), but that would incur some
// overhead, and I don't see any compelling reason anyone would want to
// instantiate more than one emulator-based decoder at a time anyway.
//

#pragma once
#include <unordered_map>
#include <string>
#include "DCSDecoder.h"

// include ADSP-2101 and ADSP-2105 sub-implementations
#define HAS_ADSP2105 1
#define HAS_ADSP2101 1
#include "adsp2100/adsp2100types.h"


class DCSDecoderEmulated : public DCSDecoder
{
public:
	// Construction.  'enableSpeedup' specifies whether or not the
	// PinMame "speedup" code is enabled.  The speedup code replaces
	// a small, performance-critical section of the original ROM code
	// with equivalent native code, greatly improving real-time
	// performance of the decoder.  This is optional so that callers
	// can ensure that the exact original code is being used, for
	// strict testing against the original behavior.  (To the extent
	// possible in an emulator, anyway; it's still possible for the
	// output to differ from the original due to errors in the
	// ADSP-2105 opcode interpreter or discrepancies in the virtual
	// DCS hardware environment we provide for the ROM code.)
	DCSDecoderEmulated(Host *host, bool enableSpeedup);
	virtual ~DCSDecoderEmulated();

	// set the master volume
	virtual void SetMasterVolume(int vol) override;

	// get the decoder name
	virtual const char *Name() const override { return "ADSP-2105 emulator"; }

	// enable the debugger
	void EnableDebugger();

	// break into the ADSP-2105 debugger mode, if available
	void DebugBreak();

protected:
	// friend functions
	friend uint32_t adsp2100_host_read_dm(uint32_t);
	friend void adsp2100_host_write_dm(uint32_t, uint32_t);
	friend uint32_t adsp2100_host_read_pm(uint32_t);
	friend void adsp2100_host_write_pm(uint32_t, uint32_t);

	// memory handlers
	uint16_t ReadDM(uint16_t addr);
	void WriteDM(uint16_t addr, uint16_t data);
	uint32_t ReadPM(uint16_t addr);
	void WritePM(uint16_t addr, uint32_t data);

	// Global singleton.  We provide the DM() and PM() memory arrays
	// for the ADSP-2105 emulator, so the emulator needs to be able to
	// find the decoder object it's associated with.  It doesn't have
	// a notion of a "client" or "host" conext object; it just calls 
	// global functions to access memory.  So the only way for these
	// to find their way back to the decoder object is through a
	// static singleton.  This means tha the decoder can only be
	// instantiated once at a time.
	static DCSDecoderEmulated *instance;

	// initialize the decoder
	virtual bool Initialize() override;

	// call IRQ2 to process queued sound port bytes
	virtual void IRQ2Handler() override;

	// run the main decoder loop
	virtual void MainLoop() override;

	// Search PM space for a sequence of opcodes.  The code sequence
	// is specified as a string, with hex digits for the opcodes in
	// groups of 6.  Wildcards can be specified with letters outside
	// of the A-F range or with asterisks.  When letters are used,
	// they form variable names that will be populated in the variable
	// map, if the instruction pattern is matched.  If the variable map
	// isn't needed, it can be passed as null, in which case no
	// variables will be returned.  Returns the address of the first
	// instruction of the match on success, or -1 on failure.
	int SearchForOpcodes(const char *opcodes, int startingAddr = 0,
		std::unordered_map<char, uint32_t> *vars = nullptr);

	// enable the PinMame DCS speedup code
	bool enableSpeedup = false;

	// speedup patch location
	uint32_t speedupPatchAddr = 0;

	// speedup patch routines for 1993 and 1994+ titles
	void (*speedupFunc)(adsp2100_Regs&, uint16_t*, uint32_t*);
	static void DCSSpeedup1993(adsp2100_Regs &regs, uint16_t *DM, uint32_t *PM);
	static void DCSSpeedup1994(adsp2100_Regs &regs, uint16_t *DM, uint32_t *PM);

	// Backing store for PM() and DM() memory spaces in the ADSP-2105 emulator
	uint32_t PM[0x4000];
	uint16_t DM[0x4000];

	// Current banked ROM base pointer
	ROMPointer curRomBank;

	// PM() space address of the start of the DCS decoder's main loop
	int mainLoopEntry = -1;
	
	// Address of master volume setting in DM() space
	int masterVolumeAddr = -1;
};
