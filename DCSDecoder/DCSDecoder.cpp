// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder - base class and client interface
//

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <regex>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include "DCSDecoder.h"

DCSDecoder::DCSDecoder(Host *host) : host(host)
{
}

DCSDecoder::~DCSDecoder()
{
}

void DCSDecoder::AddROM(int n, const uint8_t *data, size_t size)
{
	// Validate the chip number
	if (n < 2 || n > 9)
		return;

	// Ignore zero-size ROMs
	if (size == 0)
		return;

	// save the ROM information
	ROM[n-2].Set(n - 2, data, size);

	// do some special processing for ROM U2
	if (n == 2)
	{
		// set the initial default bank pointer to U2[$00000]
		if (ROMBankPtr == nullptr)
			ROMBankPtr = data;

		// find the catalog
		catalog.ofs = FindCatalog(data, size);

		// if we found the catalog, read the table pointers
		if (catalog.ofs != 0)
		{
			// read the primary track index pointer from catalog + $0040
			catalog.trackIndex = ROM[0].data + ROM[0].ReadU24(catalog.ofs + 0x0040);

			// read the indirect deferred track index pointer from catalog + $0043
			catalog.indirectTrackIndex = ROM[0].data + ROM[0].ReadU24(catalog.ofs + 0x0043);

			// get the track count from catalog + $0046
			catalog.nTracks = ROM[0].ReadU16(catalog.ofs + 0x0046);
		}

		// infer the game ID from the signature string
		gameID = InferGameID(GetSignature(data).c_str());
	}
}

DCSDecoder::ROMPointer DCSDecoder::MakeROMPointer(uint32_t linearAddress) const
{
	// Figure the chip select, based on the hardware version.
	// For the DCS-95 boards, the chip select portion of the address 
	// is encoded in bits 21-24; on the original DCS boards, it's in
	// bits 20-23.
	int chipSelect = static_cast<int>((linearAddress >> (hwVersion == HWVersion::DCS95 ? 21 : 20)) & 0x07);
	auto const &rom = ROM[chipSelect];
	return ROMPointer(chipSelect, rom.data + (linearAddress & rom.offsetMask));
}

// test for ADSP-2105 JUMP opcode
static inline bool IsJUMP(const uint8_t *p) { return (p[0] & 0xFC) == 0x18 && (p[2] & 0x0F) == 0x0F; }

uint32_t DCSDecoder::GetSoftBootOffset() const
{
	// Determine where the soft-boot program is located.  It's in the U2 block
	// starting at either $01000 or $02000.  We can tell which it is by looking
	// for a JUMP instruction in the first three bytes of the location, since
	// the first instruction slot corresponds to the RESET vector, which in the
	// DCS programs is always populated with a JUMP to the main entrypoint.
	return IsJUMP(ROM[0].data + 0x1000) ? 0x1000 : 0x2000;
}

std::string DCSDecoder::GetSignature()
{
	return ROM[0].data != nullptr ? GetSignature(ROM[0].data) : "";
}

// Get the signature string from the start of U2.  Validates that the
// signature looks valid; returns an empty string if validation fails.
// Otherwise returns the human-readable portion of the signature string.
std::string DCSDecoder::GetSignature(const uint8_t *u2)
{
	// Test that there's a JUMP instruction at offset 0.  The data from
	// offset 0 in the ROM is always loaded directly into ADSP-2105 program
	// memory starting at PM($0000) at each hard reset, so the first three
	// bytes must form a valid ADSP-2105 instruction for the RESET vector
	// at PM($0000).  The DCS boot loader always starts with a JUMP to the
	// actual code, which has instruction format $18 xx xF.
	bool isJump = ((u2[0] & 0xFC) == 0x18 && (u2[2] & 0x0F) == 0x0F);
	if (!isJump)
		return "";

	// The signature always starts at byte offset 4, and continues until
	// the first $00 byte.  Sanity-check that it actually is a signature
	// by ensuring that it contains only printable characters and doesn't
	// go on for too long.
	const uint8_t *p = u2 + 4;
	int len = 0;
	for (; len < 120 && *p >= 32 && *p < 127 ; ++len, ++p);
	if (*p != 0)
		return "";

	// Looks good - return the null-terminated string starting at offset 4
	return reinterpret_cast<const char *>(u2 + 4);
}

// Table of known DCS games by title, internal Game ID enum, and 
// ROM signature recognition pattern
static const struct
{
	DCSDecoder::GameID id;		// internal ID for the game
	const char *title;			// official title of the game
	const char *regex;			// regex pattern to apply to a U2 ROM signature to identify the game
}
dcsGameInfo[] ={
	// Pinball roms
	{ DCSDecoder::GameID::AFM, "Attack from Mars", "Attack from Mars" },
	{ DCSDecoder::GameID::CC, "Cactus Canyon", "Cactus Canyon" },
	{ DCSDecoder::GameID::CP, "The Champion Pub", "Champion Pub" },
	{ DCSDecoder::GameID::CV, "Cirqus Voltaire", "Cirqus Voltaire" },
	{ DCSDecoder::GameID::Corvette, "Corvette", "Corvette Pinball" },
	{ DCSDecoder::GameID::DM, "Demolition Man", "Demolition Man" },
	{ DCSDecoder::GameID::DH, "Dirty Harry", "Dirty Harry" },
	{ DCSDecoder::GameID::FS, "The Flintstones", "WMS Gaming Stones Sounds" },
	{ DCSDecoder::GameID::IJ, "Indiana Jones: The Pinball Adventure", "Indiana Jones" },
	{ DCSDecoder::GameID::I500, "Indianapolis 500", "Indy 500" },
	{ DCSDecoder::GameID::JB, "Jack*bot", "Jackbot" },
	{ DCSDecoder::GameID::JM, "Johnny Mnemonic", "Johnny Mnemonic" },
	{ DCSDecoder::GameID::JD, "Judge Dredd", "Judge Dredd" },
	{ DCSDecoder::GameID::MM, "Medieval Madness", "Medieval Madness" },
	{ DCSDecoder::GameID::MB, "Monster Bash", "Monster Pinball" },
	{ DCSDecoder::GameID::NBAFB, "NBA Fastbreak", "Fastbreak Game Sounds" },
	{ DCSDecoder::GameID::NF, "No Fear Dangerous Sports", "No Fear Pinball" },
	{ DCSDecoder::GameID::NGG, "No Good Gofers", "Gofers Pinball" },
	{ DCSDecoder::GameID::Popeye, "Popeye Saves the Earth", "Popeye" },
	{ DCSDecoder::GameID::RS, "Red & Ted's Roadshow", "Roadshow" },
	{ DCSDecoder::GameID::SC, "Safe Cracker", "Safe Cracker" },
	{ DCSDecoder::GameID::SS, "Scared Stiff ", "Elv2 AV Pinball" },
	{ DCSDecoder::GameID::TS, "The Shadow", "The Shadow" },
	{ DCSDecoder::GameID::STTNG, "Star Trek: The Next Generation", "Star Trek The Next Generation" },
	{ DCSDecoder::GameID::TOTAN, "Tales of the Arabian Nights", "Arabian Nights" },
	{ DCSDecoder::GameID::ToM, "Theatre of Magic", "Theatre of Magic" },
	{ DCSDecoder::GameID::WCS, "World Cup Soccer", "World Cup Soccer" },
	{ DCSDecoder::GameID::WDI, "Who Dunnit", "WDI Pinball" },

	// Video game roms
	{ DCSDecoder::GameID::KINST, "Killer Instinct", "Killer Instinct (c)" },
	{ DCSDecoder::GameID::MK2, "Mortal Kombat 2", "Mortal Kombat II (c) 1993 Williams - DWF" },
	{ DCSDecoder::GameID::MK3, "Mortal Kombat 3", "Mortal Kombat III(c) 1994 Williams - DWF" },
	{ DCSDecoder::GameID::NBAHT, "NBA Hangtime", "NBA HANGTIME GAME SOUND ROMS" },
	{ DCSDecoder::GameID::NBAHT, "NBA Hangtime (Hack)", "NBA SUPER HANGTIME" },
	{ DCSDecoder::GameID::RMPGWT, "Rampage World Tour", "WMS Rampage II Video" },
	{ DCSDecoder::GameID::WWFW, "WWF Wrestlemania Arcade", "WWF Video (c) 1993 Williams Electronics Games, Inc." },
};

// Infer the game ID from a signature string
DCSDecoder::GameID DCSDecoder::InferGameID(const char *signature)
{
	// search the game list for a game matching the signature
	for (auto &game : dcsGameInfo)
	{
		// test the signature - if it matches, return the game ID
		if (std::regex_search(signature, std::regex(game.regex, std::regex_constants::icase)))
			return game.id;
	}
	
	// not found
	return GameID::Unknown;
}


const char *DCSDecoder::GetGameTitle(GameID id)
{
	// Search the game list for the ID.  Note that this isn't particularly
	// efficient - if we wanted it to be, we could create an unordered_map
	// keyed on the game ID.  But there's no reason to go to even that
	// slight amount of trouble, as it's hard to imagine any scenario
	// where a caller would need to use this lookup in bulk.
	for (auto &game : dcsGameInfo)
	{
		if (game.id == id)
			return game.title;
	}

	// not found
	return "[Unknown]";
}

uint32_t DCSDecoder::FindCatalog(const uint8_t *u2, size_t u2size)
{
	// try the known offsets
	static const uint32_t offsets[] ={ 0x3000, 0x4000, 0x6000 };
	for (int i = 0 ; i < static_cast<int>(_countof(offsets)); ++i)
	{
		// The catalog always starts with the three-UINT16 entry
		// for U2 itself, with its size in 4K units, its ROM bank
		// select code, and its checksum.  The bank select and
		// checksum are both always zero for U2.
		auto Read16 = [](const uint8_t* &src) {
			uint16_t hi = *src++;
			return (hi << 8) | *src++;
		};
		const uint8_t *src = u2 + offsets[i];
		uint32_t size = Read16(src) * 4096;
		uint16_t chipSel = Read16(src) >> 8;
		uint16_t cksum = Read16(src);

		// if it has a zero checksum, a zero chip select, and a
		// matching size, this must be the catalog
		if (chipSel == 0 && cksum == 0 && size == u2size)
			return offsets[i];
	}

	// not found
	return 0;
}

uint8_t DCSDecoder::CheckROMs()
{
	// this will attempt system detection, so presume failure until we
	// find otherwise
  	hwVersion = HWVersion::Invalid;
	osVersion = OSVersion::Invalid;
	nominalVersion = 0x0000;

	// make sure that ROM U2 is present - we can't do anything without
	// this one, since it contains the index data
	if (ROM[0].data == nullptr)
		return 2;

	// test sizes and compute checksums
	const int nRoms = static_cast<int>(_countof(ROM));
	int nRomsPopulated = 0;
	uint16_t checksum[_countof(ROM)];
	memset(checksum, 0, sizeof(checksum));
	auto *rom = &ROM[0];
	for (int i = 0 ; i < nRoms ; ++i, ++rom)
	{
		// only test loaded ROMs
		if (rom->data != nullptr && rom->size != 0 && !rom->isDummy)
		{
			// compute the checksum and store it for later
			checksum[i] = rom->Checksum();

			// count it as populated
			nRomsPopulated += 1;
		}
		else
		{
			// For any ROM that's not loaded, set up a dummy ROM, in
			// case the decoder tries to read it.  The original hardware
			// was tolerant of accesses to missing ROMs; it would simply
			// return $FF for all bytes read from ROMs not installed.
			// I've observed the decoder occasionally reading from such
			// locations, so it seems important to provide placeholder
			// data.
			const size_t missingRomDataSize = 0x2000;
			if (missingRomData.get() == nullptr)
			{
				missingRomData.reset(new uint8_t[missingRomDataSize]);
				memset(missingRomData.get(), 0xFF, missingRomDataSize);
			}
 			rom->Set(i, missingRomData.get(), missingRomDataSize, true);
		}
	}

	// Try to figure out where the ROM checksum index is stored,
	// by checking that there's an index containing the checksum
	// values we computed.  The index should be in U2 at $03000,
	// $04000, or $06000, depending on the DCS decoder version.
	//
	// I haven't discovered anything in the ROMs that explicitly
	// marks which version of the system they're using, but if
	// the checksum test passes, we can be pretty sure that we
	// have the right location and thus know which system version
	// this is.  The odds of random data matching the checksum
	// are low.
	static const uint32_t offsets[] ={ 0x3000, 0x4000, 0x6000 };
	for (int i = 0 ; i < static_cast<int>(_countof(offsets)); ++i)
	{
		// Try interpreting the data at this offset as though it
		// were the ROM checksum index.  This won't do any harm if
		// this is the wrong offset, since we know for sure the ROM 
		// is big enough to contain this section, and we're just 
		// reading bytes at this point.
		//
		// The index consists of 25 big-endian UINT16's.  Each entry
		// contains three UINT16's:
		//
		//   UINT16 length of ROM measured in 4K byte units
		//   UINT16 ROM bank select for base of ROM
		//   UINT16 checksum
		//
		// The ROM bank select is the chip number (0..7 for U2..U9)
		// shifted left by 8 bits.  The checksum consists of the sum
		// of all of the even-offset bytes in the ROM, masked with
		// $FF, in the high byte, and the sum of all of the odd-offset
		// bytes, masked with $FF, in the low byte.  If there are
		// fewer than 8 ROMs, the table is only partially populated,
		// with the last entry marked with $0000 in the length field.
		const uint8_t *src = ROM[0].data + offsets[i];
		int nRomsInTable = 0;
		int nValidated = 0;
		int firstFailedEntry = -1;
		for (int entryNo = 0 ; entryNo < 9 ; ++entryNo)
		{
			// read the next three UINT16's
			auto Read16 = [](const uint8_t* &src) {
				uint16_t hi = *src++;
				return (hi << 8) | *src++;
			};
			uint32_t size = Read16(src) * 4096;
			uint16_t chipSel = Read16(src) >> 8;
			uint16_t ck = Read16(src);

			// if the size field is zero, this is the end-of-table marker
			if (size == 0)
				break;

			// count it
			++nRomsInTable;

			// For the DCS95 ROMs, where the table is at offset $06000,
			// the chip selects are shifted left by one more bit due
			// to the smaller ROM banking window on the DCS95 boards.
			if (offsets[i] == 0x6000)
				chipSel >>= 1;

			// if the chip select looks valid, compare the length
			// and checksum
			if (chipSel < nRoms 
				&& ROM[chipSel].data != nullptr
				&& ROM[chipSel].size == size
				&& checksum[chipSel] == ck)
			{
				// success!
				nValidated += 1;
			}
			else
			{
				// failed - stop scanning this section
				firstFailedEntry = entryNo;
				break;
			}
		}

		// If at least one of the checksums succeeded, chances are
		// that we've found the correct ROM section.
		if (nValidated > 0)
		{
			// Infer the hardware and software versions.  The hardware
			// version can be determined from the location of the catalog:
			// if it's $06000, it's for the DCS-95 boards, otherwise it's
			// for the original DCS audio board.
			if (offsets[i] == 0x6000)
			{
				// Offset $06000 is always used for the DCS-95 board.  This 
				// also means that the software must be the 1995 version 
				// or later.
				hwVersion = HWVersion::DCS95;
				osVersion = OSVersion::OS95;

				// The 1995 software encoded a version number label in the code, in
				// the IRQ2 code that handles the 55C2/55C3 commands, which I take
				// to be version number queries.  The machine code sequence that
				// handles those commands always looks like this (the 'x' values
				// indicate elements that vary by version):
				//
				//  40 10 xE SR0 = $010x  ; the immediate value is the major:minor version number
				//	0F 16 F8 SR = LSHIFT SR0 BY -8 (LO)
				//	93 30 0E DM($3300) = SR0
				//	18 xx xF JUMP $0xxx
				//	40 10 xE SR0 = $010x  ; the same major:minor version number as an immediate
				//	0F 16 08 SR = LSHIFT SR0 BY 8 (LO)
				//	0F 16 F8 SR = LSHIFT SR0 BY -8 (LO)
				//	93 30 0E DM($3300) = SR0
				//  18 xx xF JUMP $0xxx
				//
				// The code always appears in the $0390..$03C0 range in
				// the main decoder program, loaded from ROM sector $02000.
				std::unordered_map<char, uint32_t> vars;
				const uint8_t *p = ROM[0].data + 0x2000 + (0x0300 * 4);
				if (SearchForOpcodes("4vvvvE 0F16F8 93300E 18***F 4wwwwE 0F1608 0F16F8 93300E 18***F",
					p, 0x180*4, 0, &vars) >= 0)
				{
					// Found it - extract the version number from the immediate value
					// loaded into the initial SR0 = <immediate> instruction.  That
					// encodes the major version number in the high byte ($01 for all
					// extant ROMs) and the minor version in the low byte ($03..$05).
					// The immediate value is packed into bits 4-11 of the 24-bit
					// opcode, as a binary integer in big-endian format.
					nominalVersion = static_cast<uint16_t>(vars.find('v')->second);
				}
			}
			else
			{
				// Offsets below $03000 and $04000 are used for the original
				// DCS audio-only board.
				hwVersion = HWVersion::DCS93;

				// The software for these boards can be either the mainstream
				// 1994 software, used for all of the titles in 1994 and 1995
				// (until the switch to the DCS-95 boards), or the 1993 version,
				// used only for ST:TNG, IJ:TPA, and JD.  The software doesn't
				// include any metadata that identifies its version, so we have
				// to infer the version by looking for byte patterns that we've
				// determined by observation to be unique to each version.
				// The following instruction sequence appears in all of the
				// 1993 ROMs, and only in the 1993 ROMs, so we can use it to
				// distinguish those from the mainstream 1994-1994 software:
				//
				//   38 00 26 M6 = $0002
				//   3C 10 05 CNTR = $0100
				//   0C 00 C0 ENA BIT_REV
				//
				// Start by presuming it's the 1994 software, then search for
				// the instruction sequence above.  If we find it, override
				// the presumption.  The sequence should be in the mid-$0100
				// range, in the ROM segment loaded from U2 $01000.
				osVersion = OSVersion::OS94;
				const uint8_t *p = ROM[0].data + 0x1000 + (0x0100 * 4);
				if (SearchForOpcodes("380026 3C1005 0C00C0", p, 0x180*4) >= 0)
				{
					// found it - set it as OS93b as a first guess
					// (but we might revise this to OS93a later; see below)
					osVersion = OSVersion::OS93b;
				}

				// If we identified OS93, there's one more sub-variant that we
				// have to worry about:  OS93a for IJTPA and JD, and OS93b for
				// STTNG.  We can detect this by another unique sequence in the
				// OS93a games only, found in the $2000 overlay code:
				//
				//   47 FF F2 MX0 = $7FFF
				//   47 C9 46 MY0 = $7C94
				//
				if (osVersion == OSVersion::OS93b)
				{
					p = ROM[0].data + 0x2000 + (0x0200 * 4);
					if (SearchForOpcodes("47FFF2 47C946", p, 0x100*4) >= 0)
					{
						// found it - it's the earlier OS93 version
						osVersion = OSVersion::OS93a;
					}
				}

				// Note that there's no official version number embedded in
				// any of the 1993/1994 versions, as far as I can tell, nor
				// in the earliest DCS-95 releases.  The data port command
				// handler for 55C2/55C3 version query commands only appears
				// in the 1996 and later DCS-95 software, and the lowest
				// version number that appears in that form is 1.03.  We can
				// infer the version numbers of earlier releases by process
				// of elimination, if we assume that the numbering started
				// at 1.00 and that the nominal versions were increased by
				// .01 for each numbered revision.  The three original 1993
				// titles are thus nominal 1.00, which leaves us with 1.01
				// and 1.02 unaccounted for.  Since there was one release
				// for the DCS-95 boards that didn't have 55C2/55C3 coding,
				// that must have been the one immediately preceding 1.03,
				// thus it's 1.02.  That leaves 1.01, which must apply to
				// all of the other original hardware releases after the
				// three 1993 titles.
				//
				//   1.00 = 1993 titles (IJTPA, JD, STTNG) [inferred]
				//   1.01 = original DCS board titles, 1994-1995 [inferred]
				//   1.02 = DCS-95 releases, 1995-1996 [inferred]
				//   1.03 }
				//   1.04 } DCS-95 releases, 1996-1998 [explicitly numbered]
				//   1.05 }
			}

			// If we validated all of the populated ROMs, return success (code 1)
			if (nValidated == nRomsPopulated && nRomsPopulated == nRomsInTable)
				return 1;

			// return the ROM Ux number of the first failed entry
			return static_cast<uint8_t>(firstFailedEntry + 2);
		}
	}

	// We couldn't find a valid ROM index in the designated U2 index.
	// This is equivalent to a ROM U2 checksum failure, because it
	// means that the U2 image isn't valid.
	return 2;
}

int DCSDecoder::GetVersionNumber() const
{
	return nominalVersion != 0 ? nominalVersion :
		osVersion == OSVersion::OS93a || osVersion == OSVersion::OS93b ? 0x0100 :
		osVersion == OSVersion::OS94 ? 0x0101 :
		0x0000;
}

std::string DCSDecoder::GetVersionInfo(HWVersion *hw, OSVersion *os) const
{
	// fill in the caller's hw and os version variables, if provided
	if (hw != nullptr)
		*hw = hwVersion;
	if (os != nullptr)
		*os = osVersion;

	char nbuf[32] = "";
	if (nominalVersion != 0)
	{
		const char *year = "1995+";
		switch (nominalVersion)
		{
		case 0x0103:
			year = "1995";
			break;

		case 0x0104:
			year = "1997";
			break;

		case 0x0105:
			year = "1997";
			break;
		}
		snprintf(nbuf, _countof(nbuf), "Software %d.%02d (%s)", nominalVersion >> 8, nominalVersion & 0xFF, year);
	}

	// build a human-readable version description
	const char *s = "Unknown";
	switch (osVersion)
	{
	case OSVersion::Invalid:
		s = "Not detected";
		break;

	case OSVersion::Unknown:
		s = "Unknown";
		break;

	case OSVersion::OS93a:
		// This is the earliest release, which isn't officially labeled
		// anywhere in the code, so we'll call it 1.0a.
		s = "Software 1.0a (1993)";
		break;

	case OSVersion::OS93b:
		// This is the very slightly modified version of 1.0 used in
		// ST:TNG, which we call it 1.0a.
		s = "Software 1.0b (1993)";
		break;

	case OSVersion::OS94:
		// There's no official version number in the OS94 releases, but
		// they must be 1.01, by process of elimination.  We can safely
		// assume that the earliest OS93 releases must be 1.00, and
		// 1.03 is taken by the first labeled DCS-95 release.  That
		// leaves 1.02 for the earliest unlabeled DCS-95 release, and
		// thus leaves only 1.01 for the OS94 build.
		s = "Software 1.01 (1993)";
		break;

	case OSVersion::OS95:
		// the labeled DCS-95 versions start at 1.03, so the early unlabeled
		// ones must be 1.02
		s = nominalVersion != 0 ? nbuf : "Software 1.02 (1995)";
		break;
	}

	const char *h = "Unknown hardware type";
	switch (hwVersion)
	{
	case HWVersion::Invalid:
		h = "Hardware type not detected";
		break;

	case HWVersion::Unknown:
		h = "Unknown hardware type";
		break;

	case HWVersion::DCS93:
		h = "DCS audio board";
		break;

	case HWVersion::DCS95:
		h = "DCS-95 A/V board";
		break;
	}

	char buf[128];
	snprintf(buf, _countof(buf), "%s, %s", h, s);
	return buf;
}

int DCSDecoder::GetNumChannels() const
{
	// Scan the ROM for the channel track program execution loop:
	//
	//   [+00] 22 20 0F  AR = AY0 + 1
	//   [+04] 40 00 x4  AY0 = $000x     ; this is the channel count
	//   [+08] 26 E2 0F  AF = AR - AY0
	//   [+0C] 22 18 00  IF EQ AR = 0 (ALU)
	//   [+10] 9x xx xA  DM($xxxx) = AR
	//   [+14] 8x xx xA  AR = DM($xxxx)
	//   [+18] 40 0x x4  AY0 = $00xx     ; this is the channel mask
	//   [+1C] 26 E2 0F  AF = AR - AY0
	//   [+20] 18 xx x1  IF NE JUMP $xxxx
	//
	// The channel mask has a '1' bit set for each channel, so we
	// can cross-check the mask against the counter to make sure we
	// have the right instruction sequence.
	//
	// Note that we can determine the channel count from this opcode
	// sequence, but we can't *change* the channel count by patching
	// it with new numbers, because this far from the only place in
	// the program where the channel count is mentioned.  And even
	// if you tracked down every other mention of the count and
	// patched each one, the resulting code wouldn't work properly,
	// because there are several arrays in the run-time memory
	// layout whose sizes are determined by the number of channels
	// supported.  Those can't be expanded without rearranging
	// everything else to make room.
	std::unordered_map<char, uint32_t> vars;
	if (ROM[0].data != nullptr && SearchForOpcodes(
		"22200F 4000n4 26E20F 221800 9****A 8****A 400mm4 26E20F 18***1",
		ROM[0].data, 0x6000, 0, &vars) >= 0)
	{
		// get the channel count from op04 and the mask from op14
		int nChannels = static_cast<int>(vars.find('n')->second);
		uint32_t mask = vars.find('m')->second;
		if (mask == static_cast<uint32_t>((1 << nChannels) - 1))
			return nChannels;
	}

	// Not found
	return 0;
}

uint16_t DCSDecoder::ROMInfo::Checksum(const uint8_t *p, size_t size)
{
	// even-offset and odd-offset sums
	uint16_t evenSum = 0;
	uint16_t oddSum = 0;

	// run through all of the bytes
	for (size_t i = 0 ; i < size ; i += 2)
	{
		evenSum += *p++;
		oddSum += *p++;
	}

	// combine the low 8 bits of the even-offset and odd-offset sums 
	// into a 16-bit result
	return static_cast<uint16_t>(((evenSum << 8) & 0xFF00) | (oddSum & 0x00FF));
}

bool DCSDecoder::GetTrackInfo(uint16_t trackNumber, TrackInfo &ti)
{
	// clear the outputs, presuming that the track isn't valid
	ti.address = 0;
	ti.channel = 0;
	ti.type = 0;
	ti.time = 0;
	ti.looping = false;

	// validate the track number
	if (trackNumber >= catalog.nTracks)
		return false;

	// read the track program address from the catalog's track index
	auto addr = U24BE(catalog.trackIndex + trackNumber*3);

	// if the high byte of the address is 0xFF, the track isn't populated
	if ((addr & 0x00FF0000) == 0x00FF0000)
		return false;

	// get a pointer to the track data
	auto trackp = MakeROMPointer(addr);

	// read the type code and channel number
	auto type0 = *trackp++;
	auto ch0 = *trackp++;

	// the channel must be 0..7
	if (ch0 > 7)
		return false;


	// check the type
	bool trackProgramDone = false;
	int deferCode = 0xFFFF;
	switch (type0)
	{
	case 1:
		// Type 1 - the track contains a byte-code program
		break;

	case 2:
	case 3:
		// Type 2, 3 - the track contains a 16-bit deferral code, for
		// triggering via opcode 0x05 in another track's program.
		deferCode = trackp.GetU16();

		// there's no byte-code program for these track types
		trackProgramDone = true;
		break;

	default:
		// Other track types are invalid
		return false;
	}


	// Scan the program to determine the track time.  The program can
	// contain nested loops, so we need to maintain a stack to record
	// the time at each loop level.
	struct Time
	{
		uint32_t programTime = 0;
		uint32_t loopingStreamTime = 0;
		uint8_t nLoops = 1;
		bool looping = false;
	};
	std::list<Time> loopStack;
	loopStack.emplace_back();
	while (!trackProgramDone)
	{
		// read the wait counter and opcode
		uint16_t counter = trackp.GetU16();
		uint8_t opcode = trackp.GetU8();

		// If the counter is $FFFF, this is an indefinite wait, so the
		// program can't continue beyond this point.  This counts as the
		// end of the program.
		if (counter == 0xFFFF)
		{
			// The actual program running time at this level is infinity,
			// since the program spins here forever.  (It's not a "halt";
			// the channel keeps playing.)  In terms of the audible effect,
			// the loop time is the last looping stream time.
			loopStack.back().looping = true;
			loopStack.back().programTime += loopStack.back().loopingStreamTime;

			// done
			trackProgramDone = true;
			break;
		}

		// Add the wait counter to the program time
		loopStack.back().programTime += counter;

		// check the opcode
		switch (opcode)
		{
		case 0x00:
			// stop - ends the track
			trackProgramDone = true;
			break;

		case 0x01:
			// play audio stream
			{
				// get the parameters
				trackp.GetU8();  // skip the channel
				auto stream = MakeROMPointer(trackp.GetU24());
				auto repeat = trackp.GetU8();

				// the first U16 in the stream is the stream frame count
				auto streamTime = stream.GetU16();

				// If this stream loops indefinitely, note it as the current
				// looping stream time.  If the loop or enclosing program
				// goes into a spin loop at the program level, this determines
				// the effective loop iteration time.
				loopStack.back().loopingStreamTime = 0;
				if (repeat == 0)
					loopStack.back().loopingStreamTime = streamTime;
			}
			break;

		case 0x0E:
			// push a loop stack level
			loopStack.emplace_back();
			if ((loopStack.back().nLoops = trackp.GetU8()) == 0)
				loopStack.back().looping = true;
			break;

		case 0x0F:
			// pop a loop stack level
			if (loopStack.size() > 1)
			{
				// pop the top loop level
				Time level = loopStack.back();
				loopStack.pop_back();

				// add the time contribution of the inner level to the outer level
				loopStack.back().programTime += (level.looping ? 1 : level.nLoops) * level.programTime;

				// If the inner level loops forever, if effectively ends the
				// program here, since we can never move beyond this point.
				if (level.looping)
				{
					loopStack.back().looping = true;
					trackProgramDone = true;
					break;
				}
			}
			break;

		case 0x0D:
			// commands with no parameters and no timing effects
			break;

		case 0x02:
		case 0x05:
			// commands with one byte of parameters and no timing effects
			trackp.Modify(1);
			break;

		case 0x03:
		case 0x06:
		case 0x07:
		case 0x08:
		case 0x09:
		case 0x11:
		case 0x12:
			// commands with two bytes of parameters and no timing effects
			trackp.Modify(2);
			break;

		case 0x0A:
		case 0x0B:
		case 0x0C:
			// commands with four bytes of parameters and no timing effects
			trackp.Modify(4);
			break;

		case 0x04:
			// this has 1 or 3 bytes of parameters, depending on OS version
			trackp.Modify(osVersion == OSVersion::OS93a ? 3 : 1);
			break;
		}
	}

	// Pop any remaining nested levels.  We can exit early, with levels
	// still in the stack, if we reach an infinite loop within a nested
	// level.  The program can't proceed past that point, so there's no
	// need to calculate any timing beyond that point.
	while (loopStack.size() > 1)
	{
		// pop this level
		Time level = loopStack.back();
		loopStack.pop_back();

		// propagate the time to the enclosing level
		loopStack.back().programTime += (level.nLoops == 0 ? 1 : level.nLoops) * level.programTime;
		if (level.looping)
			loopStack.back().looping = true;
	}

	// it's valid - return the data
	ti.address = addr;
	ti.channel = ch0;
	ti.type = type0;
	ti.deferCode = deferCode;
	ti.time = loopStack.back().programTime;
	ti.looping = loopStack.back().looping;
	return true;
}

std::vector<DCSDecoder::Opcode> DCSDecoder::DecompileTrackProgram(uint16_t trackNumber)
{
	// create an empty vector to hold the result
	auto v = std::vector<DCSDecoder::Opcode>();
	
	// Get the track information.  Only track type 1 has a bytecode program.
	TrackInfo ti;
	if (!GetTrackInfo(trackNumber, ti) || ti.type != 1)
		return v;

	// make a pointer to the start of the track
	ROMPointer startp = MakeROMPointer(ti.address);

	// make a current working pointer, and skip the track header
	// (the channel number and type code bytes)
	ROMPointer p = startp;
	p.Modify(2);

	// loop stack
	struct LoopStackEle
	{
		int parentOffset;
	};
	std::list<LoopStackEle> loopStack;

	// Parse the program.  Each element consists of a 16-bit counter
	// prefix, followed by an 8-bit opcode, followed by parameters that
	// vary by opcode.  The track ends with counter value 0xFFFF or
	// opcode zero.
	for (bool done = false ; !done ; )
	{
		// add a new vector entry for this instruction
		auto &ele = v.emplace_back();

		// set the loop stack pointers
		ele.nestingLevel = static_cast<int>(loopStack.size());
		if (loopStack.size() != 0)
			ele.loopParent = loopStack.back().parentOffset;

		// note the instruction's byte offset from the start of the program
		ele.offset = static_cast<int>(p.p - startp.p);

		// get the counter - this represents a delay time in 7.68ms intervals
		ele.delayCount = p.GetU16();

		// a delay count of 0xFFFF is an infinite wait, so execution cannot
		// proceed beyond (or even into) this instruction; this effectively
		// ends the program, no matter what the opcode is
		if (ele.delayCount == 0xFFFFU)
			done = true;

		// get the opcode
		ele.opcode = p.GetU8();

		// start the opbytes string with the wait count and opcode
		std::string opbytes = format("%04X %02X", ele.delayCount, ele.opcode);

		// store a pointer at the start of the operand bytes
		const uint8_t *pOperands = p.p;

		// explain the opcode
		std::string instr;
		switch (ele.opcode)
		{
		case 0x00:
			// end of track
			instr += "End;";
			done = true;
			break;

		case 0x01:
			// play audio stream
			{
				// read the parameters - channel, stream offset, repeat count
				auto ch = p.GetU8();
				std::string chTag = ch == ti.channel ? "" : format("channel %d,", ch);
				auto streamPtr = p.GetU24();
				auto repeat = p.GetU8();
				opbytes += format(" %02X %06X %02X", ch, streamPtr, repeat);
				if (repeat == 0)
					instr = format("Play(%sstream $%06X, repeat forever);", chTag.c_str(), streamPtr);
				else if (repeat == 1)
					instr = format("Play(%sstream $%06X);", chTag.c_str(), streamPtr);
				else
					instr = format("Play(%sstream $%06X, repeat %d);", chTag.c_str(), streamPtr, repeat);
			}
			break;

		case 0x02:
			// stop playback in target channel
			{
				auto ch = p.GetU8();
				opbytes += format(" %02X", ch);
				instr = format("Stop(channel %d);", ch);
			}
			break;

		case 0x03:
			// queue track
			{
				auto n = p.GetU16();
				opbytes += format(" %04X", n);
				instr = format("Queue(track $%0X);", n);
			}
			break;

		case 0x04:
			// the meaning of this opcode varies by version
			if (osVersion == OSVersion::OS93a)
			{
				// OS93a -> write UINT8 to data port if non-zero, 
				// and set up channel timer
				auto b = p.GetU8();
				auto cnt = p.GetU16();
				opbytes += format(" %02X %04X", b, cnt);
				instr = format("SetChannelTimer(byte $%02X, counter $%04X);", b, cnt);
			}
			else
			{
				// all other versions -> write UINT8 to data port
				auto b = p.GetU8();
				opbytes += format(" %02X", b);
				instr = format("WriteDataPort(byte $%02X);", b);
			}
			break;

		case 0x05:
			// trigger a deferred track link
			{
				auto ch = p.GetU8();
				opbytes += format(" %02X", ch);
				instr = format("StartDeferred(channel %d);", ch);
			}
			break;

		case 0x06:
			// store variable
			{
				auto idx = p.GetU8();
				auto val = p.GetU8();
				opbytes += format(" %02X %02X", idx, val);
				instr = format("SetVariable(var $%02X, value $%02X);", idx, val);
			}
			break;

		case 0x07:
		case 0x08:
		case 0x09:
			// set channel mixing level
			{
				auto ch = p.GetU8();
				std::string chTag = ch == ti.channel ? "" : format("channel %d, ", ch);
				auto level = p.GetU8();
				opbytes += format(" %02X %02X", ch, level);
				instr = format("SetMixingLevel(%s%s %d);", chTag.c_str(),
					ele.opcode == 7 ? "level" : ele.opcode == 8 ? "increase" : "decrease", level);
			}
			break;

		case 0x0A:
		case 0x0B:
		case 0x0C:
			// fade channel mixing level
			{
				auto ch = p.GetU8();
				std::string chTag = ch == ti.channel ? "" : format("channel %d, ", ch);
				auto level = p.GetU8();
				auto steps = p.GetU16();
				opbytes += format(" %02X %02X %04X", ch, level, steps);
				instr = format("SetMixingLevel(%s%s %u, steps %u);", chTag.c_str(),
					ele.opcode == 0x0A ? "level" : ele.opcode == 0x0B ? "increase" : "decrease", level, steps);
			}
			break;

		case 0x0D:
			// NOP
			instr = "NOP;";
			break;

		case 0x0E:
			// loop start - push playback position
			{
				auto cnt = p.GetU8();
				opbytes += format(" %02X", cnt);
				if (cnt != 0)
					instr = format("Loop (%d) {", cnt);
				else
					instr = "Loop {";

				// add a loop stack entry
				loopStack.emplace_back(LoopStackEle{ static_cast<int>(v.size()) });
			}
			break;

		case 0x0F:
			// loop end - pop playback position
			instr = "}";

			// Pop the loop stack.  Note that we can't count on the track being
			// well-formed: there's at least one example (WWF Wrestlemania, track
			// 0x1204) of an 0x0F instruction with no matching 0x0E.  The original
			// decoders silently accept these, so we'll do the same.
			if (loopStack.size() != 0)
				loopStack.pop_back();
			else
				instr = "LoopEnd";
			break;

		case 0x10:
			// mystery opcode 0x10
			{
				auto b0 = p.GetU8();
				auto b1 = p.GetU8();
				opbytes += format(" %02X %02X", b0, b1);
				instr = format("Opcode$10($%02X,$%02X);", b0, b1);
			}
			break;

		case 0x11:
		case 0x12:
			// mystery opcode 0x11-0x12
			{
				auto b0 = p.GetU8();
				auto b1 = p.GetU8();
				auto w2 = p.GetU16();
				opbytes += format(" %02X %02X %04X", b0, b1, w2);
				instr = format("Opcode$%02x($%02X,$%02X,$%04X);", ele.opcode, b0, b1, w2);
			}
			break;

		default:
			instr = format("InvalidOpcode$%02X;", ele.opcode);
			done = true;
			break;
		}

		// Copy the operand bytes into the descriptor.  The bytes from the
		// saved start-of-operands pointer to the current program pointer are
		// all operands of the current instruction.
		ele.nOperandBytes = static_cast<int>(p.p - pOperands);
		for (int i = 0 ; i < ele.nOperandBytes && i < static_cast<int>(_countof(ele.operandBytes)) ; ++i)
			ele.operandBytes[i] = pOperands[i];

		// store the instruction mnemonic and hex description
		ele.desc = instr;
		ele.hexDesc = opbytes;
	}

	// return the program vector
	return v;
}

std::string DCSDecoder::ExplainTrackProgram(uint16_t trackNumber, const char *linePrefix)
{
	// get the track information
	TrackInfo ti;
	if (!GetTrackInfo(trackNumber, ti))
		return "[Invalid track]";

	// Track types 2 (deferred) and 3 (deferred indirect) don't have
	// programs - they contain only a link to another track (in different
	// formats for the two types).
	if (ti.type == 2)
	{
		// the track contains UINT16 with a link to another track number
		auto p = MakeROMPointer(ti.address);
		auto n = p.GetU16();
		return format("%Deferred ($%04x)", linePrefix, n);
	}
	else if (ti.type == 3)
	{
		// the track contains a UINT8 variable array index, and a UINT8
		// index table selector
		auto p = MakeROMPointer(ti.address);
		auto varNo = p.GetU8();
		auto tableNo = p.GetU8();
		return format("%sDeferred Indirect ($%02x[$%02x])", linePrefix, tableNo, varNo);
	}

	// decompile the track program
	auto v = DecompileTrackProgram(trackNumber);

	// build the program listing
	std::string program;
	std::string loopIndent;
	const char *perLoopIndent = "  ";
	for (auto &ele : v)
	{
		// add a newline, if there's anything in the buffer already
		if (program.size() != 0)
			program += "\n";

		// format the wait counter - this represents a delay time in 7.68ms intervals
		std::string wait;
		if (ele.delayCount == 0xFFFFU)
		{
			// infinite delay counter - this effectively ends the track, since play
			// can never proceed past this point
			wait = "Wait(Forever) ";
		}
		else if (ele.delayCount != 0)
		{
			// non-zero delay timer - this represents a pause in the track playback
			wait = format("Wait(%u) ", ele.delayCount);
		}

		// check for the end of a loop
		std::string comment = "// " + ele.hexDesc;
		if (ele.opcode == 0x0F)
		{
			// If there's a wait, show it on a separate line, at the old
			// indent, before the closing '}'.  The wait is executed within
			// the loop, so it maps better visually to show it indented with
			// the loop contents.  Show the hex opcode at this point.
			if (ele.delayCount != 0 && loopIndent.size() != 0)
			{
				// show the wait with the hex opcode comment for the loop ending
				program += linePrefix + format("%-60s    %s\n",
					(loopIndent + wait).c_str(), comment.c_str());

				// consume the wait string and comment
				wait = "";
				comment = "";
			}

			// Reduce the loop indent.  Note that we can't absolutely count
			// on the loop indent being non-zero, as there's at least one
			// production example (WWF Wrestlemania track 0x1204) of a
			// loop-end (0x0F) opcode with no matching loop-start (0x0E).
			if (loopIndent.size() != 0)
				loopIndent = loopIndent.substr(2);
			else
				comment += " Unmatched loop end opcode (0x0F)";
		}

		// construct the line
		program += linePrefix + format("%-60s    %s", 
			(loopIndent + wait + ele.desc).c_str(), comment.c_str());

		// at the start of a loop, increase the loop indent for the next instruction
		if (ele.opcode == 0x0E)
			loopIndent += perLoopIndent;
	}

	// return the program string
	return program;
}

void DCSDecoder::HardBoot()
{
	// switch to Hard Boot state
	state = State::HardBoot;

	// clear anything in the data port queue
	ClearDataPort();
	
	// reset the mode sample counter
	modeSampleCounter = 0;

	// start the host's boot timer
	host->BootTimerControl(true);
}

std::list<uint32_t> DCSDecoder::ListStreams()
{
	// While we're scanning, store the stream references in a set, so
	// that we only store one reference to each distinct stream for 
	// streams that are referenced from multiple tracks or steps.
	// The set also arranges the streams in ascending address order,
	// which isn't necessary, but is nicer aesthetically.
	std::set<uint32_t> streams;

	// scan the track programs for stream references
	// Scan all tracks for Deferred Indirect table and variable references
	for (int i = 0, nTracks = GetMaxTrackNumber() ; i <= nTracks ; ++i)
	{
		// Get the track's information.  If it's a Type 1 track,
		// scan its program steps for Play opcodes (0x01), which
		// contain stream references.
		DCSDecoder::TrackInfo ti;
		if (GetTrackInfo(i, ti) && ti.type == 1)
		{
			// scan the program steps
			for (auto &op : DecompileTrackProgram(i))
			{
				// opcode 0x01 = Play Stream
				if (op.opcode == 0x01)
				{
					// get the stream's linear ROM pointer
					uint32_t streamAddr =
						(static_cast<uint32_t>(op.operandBytes[1]) << 16)
						| (static_cast<uint32_t>(op.operandBytes[2]) << 8)
						| (static_cast<uint32_t>(op.operandBytes[3]));

					// add it to our set
					streams.emplace(streamAddr);
				}
			}
		}
	}

	// convert the set into a list for return
	std::list<uint32_t> list;
	for (auto s : streams)
		list.emplace_back(s);

	// return the list
	return list;
}

DCSDecoder::DeferredIndirectInfo DCSDecoder::GetDeferredIndirectTables()
{
	// Variable range list.  This keeps track of the maximum value assigned
	// to each opcode 0x06 variable from a program step.
	std::unordered_map<uint8_t, uint8_t> varRange;

	// Deferred Indirect table variable indexing list.  This records the
	// list of variable numbers used to index each DI table in Type 3
	// tracks.  If a Type 3 track references Table $01[Variable $07],
	// we add an entry for $07 to tableVars[0x01].
	std::unordered_set<uint8_t> tableVars[256];

	// Scan all tracks for Deferred Indirect table and variable references
	for (int i = 0, nTracks = GetMaxTrackNumber() ; i <= nTracks ; ++i)
	{
		// get the track's information
		DCSDecoder::TrackInfo ti;
		if (GetTrackInfo(i, ti))
		{
			// process the track according to its type
			switch (ti.type)
			{
			case 1:
				// Type 1 - the track contains a byte-code program
				{
					// decompile the program and scan the program steps
					for (auto &op : DecompileTrackProgram(i))
					{
						// check opcodes that need special handling
						switch (op.opcode)
						{
						case 0x06:
							// Set Deferred Indirect table lookup variable.  This
							// sets the value of one of the variables that can be
							// used in Deferred Indirect lookup.  The highest value
							// of a given variable used tells us how large a given
							// lookup table must be, by telling us which entries in
							// the table are accessible.
							{
								// get the variable ID and value from the operands
								uint8_t varId = op.operandBytes[0];
								int value = op.operandBytes[1];

								// if this variable hasn't been accessed yet, add an
								// entry for it; if we already have an entry, update
								// its maximum value if the new value is higher
								if (auto it = varRange.find(varId) ; it != varRange.end())
								{
									// an entry already exists - update the maximum value
									// if necessary
									if (value > it->second)
										it->second = value;
								}
								else
								{
									// no entry exists yet - add one
									varRange.emplace(varId, value);
								}
							}
							break;
						}
					}
				}
				break;

			case 3:
				// Type 3 - Deferred Indirect track.  The track's deferral code
				// consists of an 8-bit variable ID in the high byte and an 8-bit
				// table ID in the low byte.  
				{
					// unpack the variable ID and table ID from the deferral code
					uint8_t varId = static_cast<uint8_t>((ti.deferCode >> 8) & 0xFF);
					uint8_t tableId = static_cast<uint8_t>(ti.deferCode & 0xFF);

					// record the association between table ID and variable ID
					auto &tv = tableVars[tableId];
					if (tv.find(varId) == tv.end())
						tv.emplace(varId);

					// Ensure that there's an entry for this variable.  Even if
					// it's never assigned a value in an opcode 0x06 step, the
					// variable has still been accessed.  It's maximum value if
					// it's never written is the initial 0 value that every
					// variable in the fixed-size variable array has at run-time.
					if (varRange.find(varId) == varRange.end())
						varRange.emplace(varId, 0);
				}
				break;
			}
		}
	}

	// Figure out the size of each Deferred Indirect table, by looking at
	// the highest index value that can be applied to the table through any
	// of the variables that can be used to access the table.
	int tableSize[256];
	memset(tableSize, 0, sizeof(tableSize));
	int maxTableIndex = -1;
	for (int i = 0 ; i < 256 ; ++i)
	{
		// check to see if the table was ever referenced
		if (tableVars[i].size() != 0)
		{
			// this table is referenced - note it if it's the highest
			// numbered table we've seen so far
			if (i > maxTableIndex)
				maxTableIndex = i;

			// visit all of the variables associated with the table to 
			// determine the implied table size
			for (auto v : tableVars[i])
			{
				// get this variable's maximum value
				if (auto it = varRange.find(v); it != varRange.end())
				{
					// Figure the table size implied by the variable range: a
					// maximum index of n implies a table size of at least n+1.
					int impliedMinTableSize = it->second + 1;

					// if this is larger than the assumed size of the table 
					// so far, increase the assumed table size
					if (impliedMinTableSize > tableSize[i])
						tableSize[i] = impliedMinTableSize;
				}
			}
		}
	}

	// We now know how many Deferred Indirect tables there are, and how
	// large each table is.  Copy the old tables from the ROM.  Start
	// by getting the pointer to the Deferred Indirect index at offset
	// $0046 in the catalog.
	auto pIndex = MakeROMPointer(GetCatalogOffset() + 0x0043);
	pIndex = MakeROMPointer(pIndex.GetU24());

	// The table index can't exceed 255, since it has to be encoded
	// in a single byte in each Type 3 track body.
	if (maxTableIndex > 255)
		maxTableIndex = 255;

	// Visit each table through the maximum table index referenced from
	// opcode 0x05.  The index of tables in the ROM must have an entry 
	// for each table mentioned in opcode 0x05.  (It could conceivably
	// have additional entries that are never referenced, but there's
	// no way to tell since there's no metadata giving the size of the
	// index, as far as I can tell.  And there's no point in preserving
	// unreachable tables anyway, even if they were present, since
	// their very unreachability means that they can never be used for
	// anything.)
	DeferredIndirectInfo dii;
	for (int tableId = 0 ; tableId <= maxTableIndex ; ++tableId)
	{
		// read the next table pointer
		uint32_t tableOfs = pIndex.GetU24();

		// if the high byte is $FF, or this table wasn't referenced in our
		// scan, there's no table at this index
		if ((tableOfs & 0xFF0000) == 0xFF0000 || tableSize[tableId] == 0)
			continue;

		// get the ROM pointer to the table data in the ROM
		auto pTable = MakeROMPointer(tableOfs);

		// add an entry for the table to the return data, and copy the
		// track numbers from the ROM into the new entry
		auto &ti = dii.tables.emplace_back(tableId);
		for (int idx = 0 ; idx < tableSize[tableId] ; ++idx)
			ti.tracks.emplace_back(pTable.GetU16());

		// add all of the variables used to access this table
		for (auto tv : tableVars[tableId])
			ti.vars.emplace_back(tv);
	}

	// Pass back info on the opcode 0x06 variables
	for (auto &v : varRange)
		dii.vars.emplace_back(v.first, v.second);

	// return the result
	return dii;
}

void DCSDecoder::StartSelfTests()
{
	// this notionally exits the 250ms startup data port loop, so
	// cancel any outstanding host-side boot timer
	host->BootTimerControl(false);

	// only switch to test mode if we're doing a hard boot
	if (state == State::HardBoot)
	{
		// Hard boot mode - run the ROM checks and note the result code
		// ('post' = power-on-self test)
		uint8_t postStatus = CheckROMs();

		// Send the power-on self test status code ($79 <status>)
		// to the main board.
		host->ReceiveDataPort(0x79);
		host->ReceiveDataPort(postStatus);

		// if we're in fast boot mode, go directly to normal decoder 
		// operation; otherwise play the startup bong
		if (fastBootMode)
		{
			// fast boot mode - skip the bong and start the decoder
			SoftBoot();
		}
		else
		{
			// normal boot mode - start the bong
			startupBong.Start();

			// switch to Bong mode
			state = State::Bong;
			modeSampleCounter = 0;

			// play the bong a number of times equal to the status
			// code, as an audible indicator of the code
			bongCount = postStatus;
		}
	}
}

void DCSDecoder::SoftBoot()
{
	// cancel any pending host boot timer
	host->BootTimerControl(false);
	
	// the autobuffer is initially empty, so set the mode sample
	// counter to an impossibly high number to flag that we need
	// to refill it immediately
	modeSampleCounter = 30000;

	// If we haven't already detected the system version, do so now
	// by testing the ROMs.  This looks for the ROM index data in U2,
	// the location of which tells us which software version goes
	// with this set of ROMs.
	if (hwVersion == HWVersion::Unknown)
		CheckROMs();

	// initialize the underlying decoder
	if (Initialize())
		state = State::Running;
	else
		state = State::InitializationError;
}

void DCSDecoder::WriteDataPort(uint8_t data)
{
	// During the first 250ms of a hard boot, any data port input
	// from the host cancels the self test and soft-boots into the
	// main decoder program.
	if (state == State::HardBoot)
	{
		// soft-boot into normal decoder operation
		SoftBoot();

		// done - don't queue this byte
		return;
	}

	// queue the byte
	dataPortQueue.emplace_back(data);
}

void DCSDecoder::ClearDataPort()
{
	dataPortQueue.clear();
	lastDataPortByte = 0;
}

uint8_t DCSDecoder::ReadDataPort()
{
	// if there's anything in the queue, get and remove the oldest element
	if (dataPortQueue.size() != 0)
	{
		lastDataPortByte = dataPortQueue.front();
		dataPortQueue.pop_front();
	}

	// return the last byte
	return lastDataPortByte;
}

int16_t DCSDecoder::GetNextSample()
{
	// get samples from the appropriate source for the current decoder state
	switch (state)
	{
	case State::HardBoot:
		// We're in the hard boot state, which corresponds to a 250ms
		// wait loop in the ROM code, which monitors for an incoming
		// byte on the data port from the host WPC board.  A byte on
		// the data port instructs us to bypass the power-on tests and
		// reboot immediately in decoder mode.  If 250ms elapses with
		// no data port transmission, we're to proceed to the power-on
		// self tests.  Each audio sample represents 32us, so a 250ms
		// wait corresponds to 7812 samples.
		if (++modeSampleCounter >= 7812)
			StartSelfTests();

		// continue waiting, return silence
		return 0;

	case State::Bong:
		// Playing the startup bong.  Play this for 750ms and then
		// switch to normal decoding mode.
		if (++modeSampleCounter >= 23437)
		{
			// decrement the bong count and see if we have more remaining
			if (--bongCount <= 0)
			{
				// that was the last bong - proceed to s
				SoftBoot();
			}
			else
			{
				// more bongs remain - start a new bong cycle
				startupBong.Start();
				modeSampleCounter = 0;
			}
		}

		// return the next Bong sample
		return startupBong.GetNextSample();

	case State::Running:
		// Normal decoder operation
		{
			// process pending data port bytes
			while (dataPortQueue.size() != 0)
				IRQ2Handler();

			// If we've exhausted the autobuffer, refill it.  The decoder
			// fills half of the autobuffer at a time, so we're out of
			// samples if the sample counter is past the halfway point.
			int retries = 0;
			while (modeSampleCounter >= autobuffer.length/2)
			{
				// try fetching another half buffer
				try
				{
					// run the main decoder loop
					MainLoop();

					// Success - the autobuffer should now contain
					// length/2 samples.  Reset our sample counter
					// and stop looping.
					modeSampleCounter = 0;
					break;
				}
				catch (ResetException)
				{
					// The decoder performed a self-reset, which happens
					// if the decoder encounters a fatal error.  The reset
					// should return the decoder to the working initial
					// conditions, so we should be able to safely call it
					// again now without triggering another error.  However,
					// it's possible that a bug could cause the decoder to
					// reach a fatal error directly from the initial state,
					// in which case we'll get stuck in an infinite loop
					// here if we just keep retrying after every reset.
					// If we encounter too many resets, switch to the Fatal
					// Error state and give up.
					if (++retries > 3)
					{
						state = State::DecoderFatalError;
						errorMessage = "The decoder performed a self-reset after encountering "
							"multiple fatal errors decoding track data.  This usually indicates "
							"that the ROM image is invalid or corrupted.";
						return 0;
					}
				}
			}

			// retrieve the next sample
			auto sample = autobuffer.base[modeSampleCounter];

			// consume 'step' samples
			modeSampleCounter += autobuffer.step;

			// return the sample
			return static_cast<int16_t>(sample);
		}

	case State::DecoderFatalError:
	case State::InitializationError:
		// Error state.  The decoder is halted in this state, so just
		// return silence.
		return 0;

	default:
		// Unknown state (should never be reached)
		return 0;
	}
}

// --------------------------------------------------------------------------
// 
// Startup bong
//

void DCSDecoder::Bong::Start()
{
	cycles = 0;
	envelopeSamples = 0;
	signSamples = 0;
	level = 0x0FFF;
}

int16_t DCSDecoder::Bong::GetNextSample()
{
	// attenuate the decay envelope every 31 samples (about 1ms)
	if (envelopeSamples++ >= 31)
	{
		// figure the new attenuation level (this is a fixed-point 1.15
		// fraction calculation, based on the DCS ROM boot code - it's
		// essentially a multiply by 0.996, to form an exponential
		// attenuation envelope)
		level = static_cast<uint16_t>(((level * 0x7f80) << 1) >> 16);

		// reset the sample counter and count the cycle
		envelopeSamples = 0;
		++cycles;
	}

	// alternate negative and positive values every 80 samples
	// (about 25ms, for a crest-to-crest frequency of 50ms = 195Hz)
	if (signSamples++ >= 80)
		sign = -sign, signSamples = 0;

	// return the level times the sign
	return sign * static_cast<int16_t>(level);
}

// --------------------------------------------------------------------------
//
// Opcode search
//
int DCSDecoder::SearchForOpcodes(const char *opcodes,
	const uint8_t *romData, size_t romDataSizeBytes,
	int startingAddr, std::unordered_map<char, uint32_t> *vars)
{
	// Search.  The opcodes in a ROM image are stored as big-endian
	// 24-bit integers (3 bytes), arranged in 4-byte units.  (The
	// fourth byte is typically set to $FF, which the ROMs use for
	// uninitialized data.)  Note that this arrangement is a feature
	// of the ROMs - it's not in any way tied to the local platform's
	// integer layout.  The unit size is explicitly and universally
	// 4 bytes, NOT sizeof(uint32_t) or anything like that.  We're
	// parsing a file format here.
	int result = SearchForOpcodes(opcodes,
		[romData](int offset) { return U24BE(romData + offset*4); },
		romDataSizeBytes/4, startingAddr/4, vars);

	// if we found a match, translate the opcode index to a byte offset
	return result < 0 ? result : result * 4;
}

int DCSDecoder::SearchForOpcodes(const char *opcodes,
	const uint32_t *pm, size_t numOpcodes,
	int startingAddr, std::unordered_map<char, uint32_t> *vars)
{
	return SearchForOpcodes(opcodes,
		[pm](int offset) { return pm[offset]; },
		numOpcodes, startingAddr, vars);
}

int DCSDecoder::SearchForOpcodes(const char *opcodes, 
	std::function<uint32_t(int opcodeOffset)> fetch,
	size_t searchSpaceSizeInOpcodes, int startingAddr, 
	std::unordered_map<char, uint32_t> *vars)
{
	// convert the opcode string into a list of values and masks
	struct Op
	{
		uint32_t opcode;  // opcode to match
		uint32_t mask;    // mask to remove wildcard bits
	};
	std::list<Op> ops;

	struct Var
	{
		char name = 0;        // single-character variable name
		int ofs = 0;          // instruction offset from start of matched sequence
		int shift = 0;        // right-shift size to extract the value from matched opcode
		uint32_t mask = 0;    // mask to apply after shifting to get the final value
	};
	std::list<Var> lvars;

	int ofs = 0;
	for (const char *p = opcodes ; *p != 0 ; ++ofs)
	{
		// skip spaces
		while (*p == ' ')
			++p;

		// start a new variable accumulator
		Var curvar{ 0, 0, 0, 0 };

		// parse the next opcode - 6 hex digits, possibly with wildcards
		uint32_t opcode = 0;
		uint32_t mask = 0;
		for (int i = 0 ; i < 6 && *p != 0 && *p != ' ' ; ++p, ++i)
		{
			char c = *p;
			if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
			{
				// literal hex digit - shift it in and shift in all '1' bits in the mask
				opcode = (opcode << 4) | (c - (c >= 'a' && c <= 'f' ? 'a'-10 : c >= 'A' && c <= 'F' ? 'A'-10 : '0'));
				mask = (mask << 4) | 0xF;

				// flush any variable under construction
				if (curvar.name != 0)
				{
					lvars.emplace_back(curvar);
					curvar.name = 0;
				}
			}
			else if (c == '*')
			{
				// wildcard, no variable name attached - shift in all 0's in the mask and value
				opcode <<= 4;
				mask <<= 4;

				// flush any variable under construction
				if (curvar.name != 0)
				{
					lvars.emplace_back(curvar);
					curvar.name = 0;
				}
			}
			else
			{
				// Anything else is a variable name.  If the same variable is
				// currently under construction, add to it, otherwise flush it
				// and start a new one.
				if (curvar.name != 0 && curvar.name != c)
				{
					lvars.emplace_back(curvar);
					curvar ={ 0, 0, 0 };
				}

				// set the variable name and instruction offset
				curvar.name = c;
				curvar.ofs = ofs;

				// set the shift according to the current position
				curvar.shift = 20 - i*4;

				// shift four bits into the variable mask
				curvar.mask = (curvar.mask << 4) | 0xF;

				// shift four zero bits into the opcode mask
				opcode <<= 4;
				mask <<= 4;
			}
		}

		// flush the variable under construction
		if (curvar.name != 0)
			lvars.emplace_back(curvar);

		// add the opcode to the list
		ops.emplace_back(Op{ opcode, mask });
	}

	// set up to read from the ROM data area in DWORDs
	// search for the opcode sequence
	for (int addr = startingAddr ; addr + ops.size() < searchSpaceSizeInOpcodes ; ++addr)
	{
		// scan for a match
		bool matched = true;
		int addr2 = addr;
		for (auto &op : ops)
		{
			auto cur = fetch(addr2++);
			if ((cur & op.mask) != op.opcode)
			{
				matched = false;
				break;
			}
			else
			{
				matched = true;
			}
		}

		// if we matched it, we're done
		if (matched)
		{
			// if the caller wanted variables back, populate the table
			if (vars != nullptr)
			{
				// populate the table with the matched variables
				for (auto &v : lvars)
				{
					// extract the value from the matched code
					uint32_t val = (fetch(addr + v.ofs) >> v.shift) & v.mask;

					// add the variable name/value pair (replacing any prior value)
					vars->erase(v.name);
					vars->emplace(v.name, val);
				}
			}

			// success - return the starting address of the match
			return addr;
		}
	}

	// no match
	return -1;
}



// --------------------------------------------------------------------------
//
// String utilities
//

std::string DCSDecoder::vformat(const char *fmt, va_list va)
{
	// figure the buffer size required
	va_list va2;
	va_copy(va2, va);
	int len = vsnprintf(nullptr, 0, fmt, va2);
	va_end(va2);

	// validate the length
	if (len < 0)
		return "[Format Error]";

	// allocate the buffer and format the text into it
    std::unique_ptr<char[]> buf(new char[len + 1]);
	vsnprintf(buf.get(), len + 1, fmt, va);

	// return a std::string constructed from the buffer
	return std::string(buf.get());
}

std::string DCSDecoder::format(const char *fmt, ...)
{
	// format via vformat()
	va_list va;
	va_start(va, fmt);
	auto str = vformat(fmt, va);
	va_end(va);

	// return the result
	return str;
}


// --------------------------------------------------------------------------
//
// Subclass registration
//

DCSDecoder::Registration::Registration(
	const char *name, const char *desc, FactoryFunc factory) :
	name(name), desc(desc), factory(factory)
{
	// add this registration to the global list of available subclasses
	DCSDecoder::Register(*this);
}

void DCSDecoder::Register(const Registration &reg)
{
	// add the entry to the global map
	GetRegistrationMap().emplace(reg.name, reg);
}

DCSDecoder::RegistrationMap &DCSDecoder::GetRegistrationMap()
{
	// instantiate a map on first use
	static RegistrationMap *regMap = new RegistrationMap();
	return *regMap;
}
