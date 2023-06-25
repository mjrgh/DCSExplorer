// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder - Zip file loader
//
// This module implements a zip file loader for the DCS Decoder.  It's
// nominally part of the DCS Decoder class, but we've separated this
// part of the code into a separate compilation module so that projects
// that don't wish to use the zip loader mechanism can omit this module
// from the build, which also eliminates all dependencies on the third-
// party miniz library.  If you don't need to call the zip loader, you
// can remove this module from your build script, along with the entire
// miniz library.
//

// unzip uses old insecure CRTL functions; to enable this, we must
// #define this symbol before including the first standrad lib header
#define _CRT_SECURE_NO_WARNINGS

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <regex>
#include <list>
#include <functional>
#include <Windows.h>  // for the UINTx types required by unzip.h
#include "DCSDecoder.h"
#include "../miniz/miniz.h"
#include "../miniz/miniz_zip.h"

#pragma comment(lib, "miniz")


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
// ROM Zip file loader
//
DCSDecoder::ZipLoadStatus DCSDecoder::LoadROMFromZipFile(
	const char *zipFileName, std::list<ZipFileData> &romData, 
	const char *explicitU2, std::string *errorDetails)
{
	// return an error
	mz_zip_archive zr;
	auto Error = [errorDetails, &zr](ZipLoadStatus status, const char *fmt, ...)
	{
		// pass back the error message, if the caller is interested
		if (errorDetails != nullptr)
		{
			// format the main message
			va_list va;
			va_start(va, fmt);
			*errorDetails = vformat(fmt, va);
			va_end(va);

			// if there's an unzip error message, add that
			if (auto zerr = mz_zip_get_last_error(&zr); zerr != MZ_OK)
				*errorDetails += format("; %s", mz_zip_get_error_string(zerr));
		}

		// return the status code
		return status;
	};

	// set up the ZIP reader
	mz_zip_zero_struct(&zr);
	if (!mz_zip_reader_init_file(&zr, zipFileName, 0))
		return Error(ZipLoadStatus::OpenFileError, "Error opening ROM Zip file \"%s\"", zipFileName);

	// set up to close the ZIP control struct before returning
	std::unique_ptr<mz_zip_archive, mz_bool(*)(mz_zip_archive*)> zrCleanup(&zr, mz_zip_reader_end);

	// get the base filename with the path removed
	std::string romZipFileBase = std::regex_replace(zipFileName, std::regex("^([A-Za-z]:)?(.*[/\\\\])?"), "");

	// load all of the files in the ZIP that could possibly be ROM files
	mz_uint nZipFiles = mz_zip_reader_get_num_files(&zr);
	for (mz_uint i = 0 ; i < nZipFiles ; ++i)
	{
		// only consider regular files
		if (!mz_zip_reader_is_file_a_directory(&zr, i))
		{
			// get the entry's name
			mz_zip_archive_file_stat stat;
			if (!mz_zip_reader_file_stat(&zr, i, &stat))
				return Error(ZipLoadStatus::ExtractError, "Error reading directory entry from ZIP file \"%s\"", zipFileName);

			// create an entry for the file in our return list
			auto &rd = romData.emplace_back(stat.m_filename, static_cast<size_t>(stat.m_uncomp_size));

			// get the file contents
			if (!mz_zip_reader_extract_to_mem(&zr, i, rd.data.get(), rd.dataSize, 0))
				return Error(ZipLoadStatus::ExtractError, "Error uncompressing data from ZIP file \"%s\"", zipFileName);
		}
	}

	// Try to identify ROM file U2.  This is the file that contains the DCS decoder
	// program along with the index of the other ROMs, so it's necessarily present
	// in all versions of all games.  This file always contains loadable ADSP-2015
	// program code from the beginning of the file, and the first instruction is 
	// always a JUMP that's loaded in the ADSP-2105 reset vector at memory location
	// PM($0000).
	//
	// In addition, it's a universal convention that the file will have '2'
	// somewhere in its name, to indicate that it's an image of ROM chip U2.
	// Unfortunately, the naming convention is pretty much completely arbitrary
	// beyond that one detail, so we can't rely on the name alone.
	//
	// Because this is so heuristic, there might be cases where it fails to
	// find the right file.  For those cases, we allow the user to designate
	// the correct file manually via the -u2 command line option.
	ZipFileData *rdu2 = nullptr;
	for (auto &rd : romData)
	{
		// check if the heuristics pass, or if this file matches the name 
		// explicitly designated as U2 in the options
		if ((IsJUMP(rd.data.get()) && strchr(rd.filename.c_str(), '2') != nullptr)
			|| (explicitU2 != nullptr && _stricmp(rd.filename.c_str(), explicitU2) == 0))
		{
			// it passes the heuristics - use this as U2
			rdu2 = &rd;
			rd.chipNum = 2;
			break;
		}
	}

	// make sure we found U2
	if (rdu2 == nullptr)
		return Error(ZipLoadStatus::NoU2, "No file in %s could be identified as ROM U2", zipFileName);

	// load ROM U2
	AddROM(2, rdu2->data.get(), rdu2->dataSize);
	int nRomsLoaded = 1;
	ZipFileData *romLoaded[8] ={ rdu2, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

	// Load the other ROMs.  The file naming conventions in the PinMame ROM image
	// .zip packs are extremely idiosyncratic, so we can't rely on the name alone.
	// One thing that seems fairly consistent at first glance is that the ROM's Ux
	// number - just the digit - will appear somewhere in the name.  That often
	// narrows things down, but not far enough, because the names often also 
	// contain other digits, usually version numbers.  The feature that's more
	// consistent is that the *contents* of the U3-U9 files generally start with
	// a signature string consisting of "S" or "U" plus the chip number plus a
	// date string.  There's often other stuff mixed in, but those elements are
	// almost always present.
	for (int n = 3 ; n <= 9 ; ++n)
	{
		// search all of the loaded ROM images
		for (auto &rd : romData)
		{
			// look for the chip number in the filename, and test the signature
			char desiredDigit = '0' + n;
			if (rd.chipNum < 0 && strchr(rd.filename.c_str(), desiredDigit) != nullptr)
			{
				// match the internal signature string
				std::match_results<const char*> m;
				std::regex pat("[SU]([^\\d]*)(\\d).*?\\s+\\d\\d/\\d\\d/\\d\\d");
				bool isMatch = std::regex_match(reinterpret_cast<const char*>(rd.data.get()), m, pat);
				char signatureDigit = isMatch ? m[2].str().c_str()[0] : 0;
				bool load = (signatureDigit == desiredDigit);

				// Special case: the ROM image for Cactus Canyon U7 is marked internally
				// as U6 (apparently an error in the original ROM contents)
				if (std::regex_match(romZipFileBase, std::regex("^cc_\\d.*", std::regex_constants::icase))
					&& isMatch && desiredDigit == '7' && signatureDigit == '6')
					load = true;

				// if we matched it, add the ROM
				if (load)
				{
					AddROM(n, rd.data.get(), rd.dataSize);
					rd.chipNum = n;
					++nRomsLoaded;
					romLoaded[n-2] = &rd;

					// stop searching for this chip
					break;
				}
			}
		}
	}

	// success
	return ZipLoadStatus::Success;
}
