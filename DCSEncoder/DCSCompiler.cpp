// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Compiler
//
// This implements a script compiler for a special-purpose mini-language
// defining the layout of a new DCS audio ROM.  DCS Encoder uses the
// script compiler to load the user's definitions for the new ROM.

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <algorithm>
#include <string>
#include <regex>
#include <filesystem>
#include "DCSCompiler.h"
#include "DCSTokenizer.h"
#include "OSSpecific.h"
#include "../DCSDecoder/DCSDecoder.h"
#include "../miniz/miniz.h"
#include "../miniz/miniz_zip.h"

#pragma comment(lib, "dcsdecoder")
#pragma comment(lib, "miniz")

DCSCompiler::DCSCompiler() : decoder(&decoderHostIfc)
{
	// get the local time
	time_t t;
	time(&t);
	struct tm tm;
	localtime_s(&tm, &t);

	// format date strings in various formats, for substitutions in script text
	static const char *month[] ={ "January", "February", "March", "April", "May", "June",
		"July", "Auguest", "September", "October", "November", "December" };
	sprintf_s(dateStr, "%02d/%02d/%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
	sprintf_s(shortDateStr, "%02d/%02d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
	sprintf_s(longDateStr, "%s %d, %04d", month[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
	sprintf_s(medDateStr, "%02d-%.3s-%04d", tm.tm_mday, month[tm.tm_mon], tm.tm_year + 1900);

	// clear the variables-by-number and deferred-indirect-tables-by-number lists
	memset(varsByNumber, 0, sizeof(varsByNumber));
	memset(diByNumber, 0, sizeof(diByNumber));

	// Set the desired stream types to "any" in the default compression parameters.
	// This tells the encoder to try each supported stream type for the prototype
	// ROM's OS version and pick the one that results in a smaller compressed
	// stream.  (Space is at a premium given the 8MB upper limit for the DCS 
	// boards, so the default optimization settings should favor minimizing the
	// generated ROM size.)
	defaultCompressionParams.streamFormatType = -1;
	defaultCompressionParams.streamFormatSubType = -1;
}

DCSCompiler::~DCSCompiler()
{
}

bool DCSCompiler::LoadPrototypeROM(const char *romZipName, bool patchMode, std::string &errMsg)
{
	// load the prototype ROM set
	auto loadStatus = decoder.LoadROMFromZipFile(romZipName, protoZipData, nullptr, &errMsg);
	if (loadStatus != DCSDecoder::ZipLoadStatus::Success)
		return false;

	// initialize the decoder
	decoder.SoftBoot();

	// note the prototype ROM version data
	protoRomVer = decoder.GetVersionInfo(&protoRomHWVer, &protoRomOSVer);

	// Set the target format version in the default compression parameters
	// to match the prototype ROM version.  We have to generate to the same
	// stream format as the prototype ROM because the prototype ROM supplies
	// the ADSP-2105 program that will decode the streams at run-time, and
	// each ADSP-2105 program can only process the one specific format it
	// was built for.  The ADSP-2105 programs don't even have the ability
	// to detect different formats, let alone process them - it would just
	// crash the decoder to supply it with a different format from the one
	// it's hard-coded to accept.
	switch (protoRomOSVer)
	{
	case DCSDecoder::OSVersion::OS93a:
		defaultCompressionParams.formatVersion = 0x9301;
		break;

	case DCSDecoder::OSVersion::OS93b:
		defaultCompressionParams.formatVersion = 0x9302;
		break;

	case DCSDecoder::OSVersion::OS94:
	case DCSDecoder::OSVersion::OS95:
		defaultCompressionParams.formatVersion = 0x9400;
		break;

	default:
		errMsg = "The prototype ROM contains an unrecognized or unsupported "
			"version of the DCS ADSP-2105 control program.  The encoder can't "
			"create a new ROM set from this prototype.";
		return false;
	}

	// note the number of channels supported in the prototype firmware
	numChannels = decoder.GetNumChannels();
	maxChannelNumber = numChannels - 1;

	// if we're in patch mode, load all of the tracks and streams from the old ROM
	if (patchMode)
	{
		// Build our internal representation of the track program listing
		// for each Type 1 track
		for (int i = 0, nTracks = decoder.GetMaxTrackNumber() ; i <= nTracks ; ++i)
		{
			// get the track's information
			DCSDecoder::TrackInfo ti;
			if (decoder.GetTrackInfo(i, ti))
			{
				// this track is populated - add it to our track list
				auto &track = tracks.emplace(std::piecewise_construct, std::forward_as_tuple(i), std::forward_as_tuple()).first->second;

				// mark is as not coming from the script, so that the script can replace it
				track.fromRom = false;

				// store the basic information
				track.trackNo = i;
				track.channel = ti.channel;
				track.type = ti.type;
				track.deferredTrack = ti.deferCode;

				// note the highest populated track number
				if (i > maxTrackNumber)
					maxTrackNumber = i;

				// scan the program steps if it's a Type 1 track
				if (track.type == 1)
				{
					// decompile the program
					for (auto &op : decoder.DecompileTrackProgram(i))
					{
						// add a step and copy the opcode and operands
						auto &step = track.steps.emplace_back();
						step.wait = op.delayCount;
						step.opcode = op.opcode;
						step.nOperandBytes = op.nOperandBytes;
						memcpy(step.operandBytes, op.operandBytes, step.nOperandBytes);

						// check for a Play Stream opcode
						if (step.opcode == 0x01)
						{
								// get the stream's linear ROM pointer
							uint32_t streamAddr =
								(static_cast<uint32_t>(step.operandBytes[1]) << 16)
								| (static_cast<uint32_t>(step.operandBytes[2]) << 8)
								| (static_cast<uint32_t>(step.operandBytes[3]));

							// get a ROM pointer to the stream data
							auto streamPtr = decoder.MakeROMPointer(streamAddr);

							// get the stream size
							auto streamInfo = decoder.GetStreamInfo(streamPtr);

							// look for an existing entry for the stream
							if (auto it = streamsByProtoAddr.find(streamAddr) ; it != streamsByProtoAddr.end())
							{
								// This stream has already been imported.  Simply store
								// the existing object reference.
								step.stream = it->second;
							}
							else
							{
								// This stream hasn't been imported into our stream list
								// yet.  Create a new stream object.
								auto &stream = streams.emplace_back(streamAddr);

								// store a direct pointer to the stream data in the prototype ROM
								stream.refName = DCSEncoder::format("%s($07x)", romZipName, streamAddr);
								stream.data = streamPtr.p;
								stream.nBytes = streamInfo.nBytes;
								stream.nFrames = streamInfo.nFrames;

								// store the stream reference
								step.stream = &stream;
							}
						}
					}
				}
			}
		}

		// Build an internal list of Deferred Indirect tables in the ROM
		auto dii = decoder.GetDeferredIndirectTables();
		for (auto &table : dii.tables)
		{
			// Create a scripting object for the table.  The ROM doesn't have
			// any metadata naming the tables, so synthesize a name based on
			// the index for display purposes.
			auto *dit = new DeferredIndirectTable(DCSEncoder::format("$%02x", table.id).c_str(), table.id);

			// add it to the numeric index
			diByNumber[table.id].reset(dit);

			// mark it as coming from the prototype ROM, so that the script can
			// replace it if desired
			dit->fromProto = true;

			// copy the table entries (track numbers) from the ROM table to
			// our scripting object representing the table
			auto &tn = dit->trackNumbers;
			tn.reserve(table.tracks.size());
			for (auto track : table.tracks)
				tn.emplace_back(track);
		}

		// Create script variable entries for the referenced variables
		for (auto &v : dii.vars)
		{
			// Add it under the synthesized name "$<id in hex>".  If the
			// script wants to refer to the same variable, it can give it
			// a proper name by declaring it with an explicit index.
			char name[10];
			sprintf_s(name, "$%02x", v.id);
			DefineVariable(name, v.id);
		}
	}

	// success
	return true;
}

DCSCompiler::Stream *DCSCompiler::EncodeFile(Stream *replaces, 
	const char *symbolicName, const char *filename,
	const DCSEncoder::CompressionParams &params, 
	DCSTokenizer &tokenizer)
{
	using ErrorLevel = DCSTokenizer::ErrorLevel;
	const ErrorLevel EError = ErrorLevel::Error;
	const ErrorLevel EFatal = ErrorLevel::Fatal;

	// log a status report
	auto Status = [&tokenizer](bool pending, const char *msg, ...)
	{
		// format the message arguments
		va_list va;
		va_start(va, msg);
		auto fmt = DCSEncoder::vformat(msg, va);
		va_end(va);

		// log it
		tokenizer.logger.Status(fmt.c_str(), pending);
	};

	// If the file doesn't exist with the name exactly as given, and the name
	// is a relative path, search for the file in each stream folder.
	std::string streamFile = filename;
	if (!std::filesystem::exists(filename) && std::filesystem::path(filename).is_relative())
	{
		// try each path prefix
		for (auto &path : streamFilePaths)
		{
			// combine this path with the filename and see if we have a winner
			auto test = std::filesystem::path(path) / filename;
			if (std::filesystem::exists(test))
			{
				// found it - use this file
				streamFile = test.string();
				break;
			}
		}
	}

	// establish the new encoding parameters
	encoder.compressionParams = params;

	// log progress
	Status(true, "Encoding %s", streamFile.c_str());

	// DCS-encode the audio file
	DCSEncoder::DCSAudio dcsObj;
	std::string encodingErrMsg;
	DCSEncoder::OpenStreamStatus status;
	if (encoder.EncodeFile(streamFile.c_str(), dcsObj, encodingErrMsg, &status))
	{
		// Success.  If we're replacing an existing stream, simply
		// reuse that stream object, overwrite its existing stream data.
		// Otherwise, create a new stream object
		Stream *stream = replaces != nullptr ? replaces : &streams.emplace_back(streamFile.c_str());

		// set the filename, and forget any prototype address (in case we're
		// replacing a prototype stream)
		if (symbolicName != nullptr)
			stream->refName = DCSEncoder::format("%s(%s)", symbolicName, streamFile.c_str());
		else
			stream->refName = streamFile;
		stream->filename = streamFile;
		stream->protoAddr = 0;

		// hand ownership of the new DCS object to the new stream entry
		stream->Store(dcsObj);

		// log progress
		int uncompressedBytes = dcsObj.nFrames * 240 * 2;
		float ratio = static_cast<float>(uncompressedBytes) / static_cast<float>(dcsObj.nBytes);
		Status(false, " : OK, %u DCS frames, %lu bytes in ROM (%.1f:1 compression, %.0f avg bps)",
			dcsObj.nFrames, dcsObj.nBytes, uncompressedBytes != 0 ? ratio : 1.0f,
			(static_cast<float>(dcsObj.nBytes) * 8.0f) / (static_cast<float>(dcsObj.nFrames) * 0.00768f));

		// return the new stream object
		return stream;
	}
	else
	{
		// encoding failed - if we left a progress report open, close it
		Status(false, "");

		// Check for an "unsupported format version" error.  The file encoder
		// will fill in a perfectly fine error message in that case, but we want
		// to override the default message with more details anyway, because we
		// want to explain that the choice of target format is a function of the
		// prototype ROM.  Just explaining it in terms of the version selection
		// might create the misleading impression that the target format is 
		// something you can configure separately, which could lead to a lot of
		// wasted time as the user searches in vain for an option setting where
		// you can select the target format.  We want to explain that the target
		// format is inalterably fixed by the choice of prototype ROM.  We also
		// want to mention that we can still patch this ROM as long as we don't
		// have to encode new audio streams for it.
		if (status == DCSEncoder::OpenStreamStatus::UnsupportedFormat)
		{
			tokenizer.logger.Log(EFatal, "",
				"The selected prototype ROM contains the 1993 version of the "
				"DCS ADSP-2105 software.  DCSEncoder can't create new audio streams "
				"in the 1993 software's encoding format.  You can still create a "
				"ROM set for this pinball machine, but if you want to include any new "
				"audio material, you'll have to use a prototype ROM from one of the "
				"1994 games instead.  That will produce a ROM that will work in your "
				"1993 pinball machine, even though it came from a later game.  You "
				"can also still patch a 1993 ROM with this program, as long as the "
				"patch doesn't require encoding any new audio material.  For example, "
				"you can alter the track programs, or insert new audio tracks that "
				"were extracted in the \"raw\" format from a DCS ROM based on the "
				"same software version.");
			exit(2);
		}

		// log the error
		tokenizer.Error(EError, "Error encoding audio file: %s", encodingErrMsg.c_str());

		// return a null stream
		return nullptr;
	}
}

void DCSCompiler::ParseScript(const char *filename, DCSTokenizer::ErrorLogger &logger)
{
	// convenience definitions for the error logger
	using ErrorLevel = DCSTokenizer::ErrorLevel;
	const ErrorLevel EFatal = ErrorLevel::Fatal;
	const ErrorLevel EError = ErrorLevel::Error;
	const ErrorLevel EWarning = ErrorLevel::Warning;
	const ErrorLevel EInfo = ErrorLevel::Info;

	// log a status report
	auto Status = [&logger](bool pending, const char *msg, ...)
	{
		// format the message arguments
		va_list va;
		va_start(va, msg);
		auto fmt = DCSEncoder::format(msg, va);
		va_end(va);

		// log it
		logger.Status(fmt.c_str(), pending);
	};

	// load the ROM definitions script file
	DCSTokenizer tokenizer(logger);
	std::string errMsg;
	if (!tokenizer.LoadFile(filename, errMsg))
	{
		logger.Log(EError, "", errMsg.c_str());
		return;
	}

	// flag for script parsing: we've run out of variable slots below 0x50
	bool varsOver0x50 = false;

	// Parse the ROM definitions file
	for (;;)
	{
		// read a token
		auto tok = tokenizer.Read();

		// stop at EOF
		if (tok.type == DCSTokenizer::TokType::End)
			break;

		// see what we have
		if (tok.IsKeyword("signature"))
		{
			// get the signature string
			std::string sigTok = tokenizer.ReadString().text;

			// expand date strings
			sigTok = std::regex_replace(sigTok, std::regex("<date>"), dateStr);

			// Check the signature length, and truncate if it's too long.  The
			// length limit is a function of the layout of the original ROM boot
			// code, since that first ROM page is actually the program image that
			// gets loaded when the processor resets.  All of the DCS boot loader
			// programs are similar, with the actual program starting at
			// PM($001C) or PM($001D), which translates to byte offset 80-84
			// (decimal) in the ROM image.  Taking out the four bytes of the JUMP
			// opcode at offset 0, that leaves us 76-80 bytes for the signature
			// text and the null byte terminator, so 75-79 bytes of text.  So a
			// 75-byte maximum seems to be safe in all cases.
			//
			// Note that most of the ROMs don't actually zero out all of the 
			// space between the end of the signature and ROM offset 80 (decimal).
			// Some of them have recognizable opcodes there, usually repeated 
			// 0A001F (RTI) codes.  I'm pretty sure from examining the boot code
			// that nothing before offset 80 ever matters to the operation of the
			// program, but if you wanted to be really cautious, you could limit
			// the signature to the longest of the original ones, say 60 bytes.
			// If in doubt about a longer signature, just test it and see if it
			// boots (in PinMame, for example).  The boot program is simple and
			// deterministic enough that if it boots once, it should boot every
			// time.
			const int MAX_SIG_LEN = 75;
			if (sigTok.length() > MAX_SIG_LEN)
			{
				sigTok.resize(MAX_SIG_LEN);
				tokenizer.Error(EWarning, "The signature string is too long; it will "
					"be truncated to %d bytes (this limit is imposed by the original "
					"DCS ROM structure)", MAX_SIG_LEN);
			}

			// store the truncated signature
			strcpy_s(signature, sigTok.c_str());
			signature[_countof(signature)-1] = 0;

			// end the statement
			tokenizer.EndStatement();
		}
		else if (tok.IsKeyword("default"))
		{
			// Set the default encoding parameters:
			// 
			//   default encoding parameters ( name=value, ... );
			//
			if (tokenizer.RequireSymbol("encoding") 
				&& tokenizer.RequireSymbol("parameters")
				&& tokenizer.RequirePunct("("))
				ParseCompressionParams(tokenizer, defaultCompressionParams);

			// end of statement
			tokenizer.EndStatement();
		}
		else if (tok.IsKeyword("stream"))
		{
			// stream <symbolic-stream-name> "sound-file-name" [replaces <address>] (param=value ...) ;
			std::string streamName = tokenizer.ReadSymbol().text;
			std::string streamFile = tokenizer.ReadString().text;

			// check for a 'replaces' clause
			Stream *replaces = nullptr;
			uint32_t replacesAddr = 0;
			if (tokenizer.CheckKeyword("replaces"))
			{
				// skip 'replaces' and read the original stream address
				replacesAddr = static_cast<uint32_t>(tokenizer.ReadInt().ival);

				// Search for an imported stream at the specified address
				if (auto it = streamsByProtoAddr.find(replacesAddr); it != streamsByProtoAddr.end())
				{
					// note the replacement stream
					replaces = it->second;

					// it's an error if the stream has already been replaced
					if (replaces->protoAddr == 0)
						tokenizer.Error(EError,
							"Prototype stream $%08lx in REPLACES clause has already been replaced", replacesAddr);
				}
				else
				{
					// No such stream exists - this is an error
					tokenizer.Error(EError,
						"Prototype stream $%08lx in REPLACES clause doesn't exist", replacesAddr);
				}
			}

			// assume our current default compression parameters
			auto curParams = defaultCompressionParams;

			// check for compression options
			if (tokenizer.CheckPunct("("))
				ParseCompressionParams(tokenizer, curParams);

			// get the upper-case version of the scripting to use as the stream
			// map key, for case-insensitive matching
			std::string key = streamName;
			std::transform(key.begin(), key.end(), key.begin(), ::toupper);
		
			// make sure this name isn't already used
			if (auto it = streamsByName.find(key); it != streamsByName.end())
				tokenizer.Error(EError, "Stream '%s' has already been defined", streamName.c_str());

			// encode the file
			Stream *stream = EncodeFile(replaces, streamName.c_str(), streamFile.c_str(), curParams, tokenizer);
			if (stream != nullptr)
			{
				// add it to the by-scripting-name map
				streamsByName.emplace(key, stream);
			}

			// end the statement
			tokenizer.EndStatement();
		}
		else if (tok.IsKeyword("var"))
		{
			// Variable name definition(s)
			// 
			//  var <name> [=<index>] [,...] ;
			do
			{
				// get the name
				std::string name = tokenizer.ReadSymbol().text;

				// make sure the name isn't already taken
				if (FindVariable(name.c_str()) != nullptr)
				{
					tokenizer.Error(EError, "Variable '%s' has already been defined", name.c_str());
					break;
				}

				// check for an explicit index assignment
				int index;
				if (tokenizer.CheckPunct(":"))
				{
					// Explicit index assignment.  Limit the index to the 8-bit
					// range (unsigned), and warn if it's 0x50 or over.  The
					// original ROM code appears to reserve only 0x50 slots for
					// the array where the variable values are stored, but it
					// doesn't do any bounds checking, so values 0x50 and above
					// could potentially cause memory corruptions when running
					// the original code.  Our native decoder implementation
					// doesn't have this limitation - it'll safely accept any
					// 8-bit index - so we'll allow the higher indices, but
					// it's dangerous to use them because the resulting track
					// program might crash if used on the original boards.
					index = tokenizer.ReadInt().ival;
					if (index < 0 || index > 0xff)
					{
						tokenizer.Error(EError, "Variable index out of bounds - must be 0 to 255");
						break;
					}
					if (index >= 0x50)
					{
						tokenizer.Error(EWarning, "Variable index %d is unsafe when "
							"used in a physical DCS board or with the PinMame emulator.  The "
							"original ADSP-2105 software might become unstable if variable "
							"indices above 79 are used.", index);
					}

					// it's an error if the index is already taken
					if (varsByNumber[index] != nullptr)
					{
						tokenizer.Error(EError, "Variable index %d has already been assigned to variable '%s'",
							index, varsByNumber[index]->name.c_str());
						break;
					}
				}
				else
				{
					// no '=', so automatically assign the next available index
					for (index = 0 ; index < 255 ; ++index)
					{
						if (varsByNumber[index] == nullptr)
							break;
					}

					// make sure we found an index
					if (index >= 255)
					{
						tokenizer.Error(EError, "No more variable slots are available");
						break;
					}

					// warn the first time we have to assign a variable number over 0x50
					if (index >= 0x50 && !varsOver0x50)
					{
						tokenizer.Error(EWarning, "More than 80 variables have been defined, which makes "
							"this program unsafe to run on physical DCS boards or the PinMame emulator. "
							"The original DCS ADSP-2105 software might behave unpredictably when variable "
							"indices above 79 are used.");

						// only generate this warning once - if there are any more variables
						// after this one, the same problem will apply to them, but that's
						// so obvious from first warning itself that there's no point in
						// repeating it for every additional variable
						varsOver0x50 = true;
					}
				}

				// add the variable
				DefineVariable(name.c_str(), index);

			} while (tokenizer.CheckPunct(","));

			// end the statement
			tokenizer.EndStatement();
		}
		else if (tok.IsKeyword("deferred"))
		{
			// Deferred indirect table definition:
			//
			//  deferred indirect table <name> [=<index>] { <trackNum>, ... } ;
			if (tokenizer.RequireSymbol("indirect") && tokenizer.RequireSymbol("table"))
			{
				if (auto nameTok = tokenizer.ReadSymbol(); nameTok.type == DCSTokenizer::TokType::Symbol)
				{
					// make sure the table name isn't a repeat
					if (FindDITable(nameTok.text.c_str()) != nullptr)
						tokenizer.Error(EError, "Deferred indirect table '%s' has already been defined", nameTok.text.c_str());

					// check for a table index
					int index = -1;
					if (tokenizer.CheckPunct(":"))
					{
						// explicit table index assignment
						index = tokenizer.ReadInt().ival;
						if (index < 0 || index > 255)
							tokenizer.Error(EError, "Deferred indirect table index %d out of range (must be 0 to 255)", index);
					}
					else
					{
						// no explicit table index specified - assign the next available index
						for (int i = 0 ; i < 255 ; ++i)
						{
							if (diByNumber[i] == nullptr)
							{
								index = i;
								break;
							}
						}
					}

					// if the index is valid, create the new table, or get the existing one
					DeferredIndirectTable *table = nullptr;
					if (index >= 0 && index < 255)
					{
						// check for an existing table
						if (diByNumber[index] != nullptr)
						{
							// The slot is occupied.  If the table was loaded from the prototype
							// ROM, the new definition simply replaces the prototype table.  If
							// the table was created explicitly in the script, it's an error.
							table = diByNumber[index].get();
							if (!table->fromProto)
								tokenizer.Error(EError, "Deferred indirect table at index %d has already been defined", index);

							// the table is now script-defined
							table->fromProto = false;

							// clear any existing entries
							table->trackNumbers.clear();
						}
						else
						{
							// the slot is empty - create a new table
							table = new DeferredIndirectTable(nameTok.text.c_str(), index);
							diByNumber[index].reset(table);

							// add it to the name index, using the upper-case name as the key,
							// for case-insensitive lookup
							std::string key = nameTok.text;
							std::transform(key.begin(), key.end(), key.begin(), ::toupper);
							deferredIndirectTables.emplace(key, table);
						}
					}

					// read the track number entries
					if (tokenizer.RequirePunct("("))
					{
						for (;;)
						{
							// stop at the closing ')'
							if (tokenizer.CheckPunct(")"))
								break;

							// read the track number and add it to the list
							auto trackTok = tokenizer.ReadInt();
							if (table != nullptr)
								table->trackNumbers.emplace_back(trackTok.ival);

							// require a comma or ')'
							if (!tokenizer.CheckPunct(","))
							{
								tokenizer.RequirePunct(")");
								break;
							}
						}
					}
				}
			}

			// end the statement
			tokenizer.EndStatement();
		}
		else if (tok.IsKeyword("track"))
		{
			// get the track number and channel number
			int trackNum = tokenizer.ReadInt().ival;
			int channel = tokenizer.RequireSymbol("channel") ? tokenizer.ReadInt().ival : 0;

			// it's an error if it's out of range
			if (channel < 0 || channel >= maxChannelNumber)
			{
				tokenizer.Error(EError, "Channel number %d is invalid "
					"(the prototype firmware version supports channels 0-%d)", channel, maxChannelNumber);
			}

			// check if the track is already populated
			Track *track = nullptr;
			if (auto it = tracks.find(trackNum); it != tracks.end())
			{
				// it already exists - if it came from the ROM, that's fine, we
				// can just override it; if not, it's replacing a track already
				// defined in this script, which is an error
				track = &it->second;
				if (track->fromRom)
				{
					// It's from the old ROM, so we can replace the old one.  Clear
					// out the old data and change its source to the script.
					track->fromRom = false;
					track->steps.clear();
				}
				else
				{
					// it's from the script, so redefining it is an error
					tokenizer.Error(EError, "Track #%d has already been defined in this script; the original definition will be discarded", trackNum);
				}
			}
			else
			{
				// create the new track entry
				it = tracks.emplace(std::piecewise_construct,
					std::forward_as_tuple(trackNum),
					std::forward_as_tuple()).first;

				// note the new high-water mark
				if (trackNum > maxTrackNumber)
					maxTrackNumber = trackNum;

				// get the track pointer
				track = &it->second;
			}

			// set the channel number
			track->channel = channel;

			// check for "defer"
			auto tok = tokenizer.Read();
			if (tok.IsKeyword("defer"))
			{
				// defererd or deferred indierct - these track types don't have any program steps
				track->steps.clear();

				// check for DEFER INDIRECT
				if (tokenizer.CheckKeyword("indirect"))
				{
					// DEFER INDIRECT ( <tableName|id> [ <varName|id> ] )
					// Track type 3
					int tableId = -1;
					int varId = -1;
					if (tokenizer.RequirePunct("("))
					{
						auto tableTok = tokenizer.Read();
						if (tableTok.type == DCSTokenizer::TokType::Symbol)
						{
							// deferred indirect table name
							auto *table = FindDITable(tableTok.text.c_str());
							if (table != nullptr)
								tableId = table->index;
							else
								tokenizer.Error(EError, "Undefined DEFER INDIRECT table '%s'", tableTok.text.c_str());
						}
						else if (tableTok.type == DCSTokenizer::TokType::Int)
						{
							// deferred indirect table number
							tableId = tok.ival;
							if (tableId < 0 || tableId > 255)
								tokenizer.Error(EError, "Invalid DEFER INDIRECT table ID %d (must be 0 to 255)", tableId);
						}
						else
						{
							tokenizer.Error(EError,
								"Expected deferred indirect table name or number in DEFER INDIRECT statement, found '%s'",
								tableTok.text.c_str());
						}

						// get the [variable] section
						if (tokenizer.RequirePunct("["))
						{
							auto varTok = tokenizer.Read();
							if (varTok.type == DCSTokenizer::TokType::Symbol)
							{
								// variable name
								if (auto *v = FindVariable(varTok.text.c_str()) ; v != nullptr)
									varId = v->id;
								else
									tokenizer.Error(EError, "Undefined variable name '%s'", varTok.text.c_str());
							}
							else if (tableTok.type == DCSTokenizer::TokType::Int)
							{
								// variable number
								varId = tok.ival;
								if (varId < 0 || varId > 255)
									tokenizer.Error(EError, "Invalid DEFER INDIRECT variable ID %d (must be 0 to 255)", varId);
							}
							else
							{
								tokenizer.Error(EError,
									"Expected deferred indirect variable name or number in DEFER INDIRECT statement, found '%s'",
									varTok.text.c_str());
							}

							// end the [variable] section
							tokenizer.RequirePunct("]");
						}

						// end the (table[variable]) section
						tokenizer.RequirePunct(")");
					}

					// Set the deferral code, if we successfully parsed it.  The
					// variable ID goes in the high byte and the table number goes
					// in the low byte.
					if (tableId >= 0 && varId >= 0)
					{
						track->type = 3;
						track->deferredTrack = (varId << 8) | tableId;
					}
				}
				else
				{
					// DEFER (<trackNumber>)
					// Basic deferred load - track Type 2
					if (tokenizer.RequirePunct("("))
					{
						track->type = 2;
						track->deferredTrack = tokenizer.ReadInt().ival;
						tokenizer.RequirePunct(")");
					}
				}
			}
			else if (tok.IsPunct("{"))
			{
				// Track program
				track->type = 1;

				// Parse the program contents until we reach the closing '}'
				struct LoopLevel
				{
					LoopLevel(int iters, int streamTime) : 
						iters(iters), startingStreamTime(streamTime) { }

					// loop iteration count
					int iters = 0;

					// wait time within the loop, in units of DCS frames
					int64_t waitTime = 0;

					// Remaining stream time going into the loop.  When we
					// leave the loop, if the loop didn't start a new stream,
					// the time remaining on the stream going into the loop
					// will be reduced after the loop by the total time spent
					// in the loop, which equals the time per iteration times
					// the number of loop iterations.
					int startingStreamTime = 0;
				};
				std::list<LoopLevel> loopStack;
				bool hasEndOp = false;
				bool hasWaitForever = false;
				bool opsPastEnd = false;
				int lastStreamTimeRemaining = -1;
				size_t lastStreamLoopLevel = 0;
				for (bool done = false; !done && !tokenizer.IsEOF() ; )
				{
					// Each statement is of the form:
					//
					//  [Wait(<numFrames>)|<time> second|<time> ms|forever|stream] <opname> [( <param> <value> [, ...] )] ;
					// 
					// - Wait(<numFrames>) waits for the specified number of DCS frames (7.68ms intervals)
					// - Wait(<time> sec) waits for the specified time in seconds, which may have a fractional part
					// - Wait(<time> ms) waits for the specified time in milliseconds (with minimum resolution of one frame, 7.68ms)
					// - Wait(forever) sets an infinite wait
					// - Wait(stream) waits for the last played stream to finish
					//
					// The Loop instruction has the special format:
					//
					//  Loop [(<count>)] {
					//     <statements>
					//  }
					//
					// Opcode names and parameter formats:
					//
					//   End
					//   Stop(channel <channel>)
					//   Play(stream <name|address>, channel <channel>)
					//   Queue(track <trackNum>)
					//   WriteDataPort(byte <byteVal>)
					//   StartDeferred(channel <channel>)
					//   StoreVariable(id <name|number>, value <number>)
					//   SetMixingLevel(channel <channel>, {level|increase|decrease} <number>)
					//   FadeMixingLevel(channel <channel>, {level|increase|decrease} <number>, steps <number>)

					// single-iteration loop, to allow breaking out to end the statement on error
					bool needSemicolon = true;
					do
					{
						// Add a new step.  Set it up initially as a NOP (0x0D), in case we run
						// into a problem parsing it.  If the last step is already a NOP, and
						// it isn't already set to an infinite wait, we can just overwrite the
						// NOP with the new step, combining the two waits.
						Track::ProgramStep *step;
						if (track->steps.size() != 0 && track->steps.back().opcode == 0x0D
							&& track->steps.back().wait != 0xFFFF)
						{
							// the previous step is a NOP - overwrite it with the new instruction
							step = &track->steps.back();
						}
						else
						{
							// add a new step
							step = &track->steps.emplace_back();
							step->opcode = 0x0D;
						}

						// parse a time unit
						auto ParseTime = [&lastStreamTimeRemaining, &tokenizer, EError, EWarning](DCSTokenizer::Token *pNumTok = nullptr)
						{
							// read the numeric value
							auto numTok = tokenizer.Read();

							// if the caller wants the leading number token back, pass it back
							if (pNumTok != nullptr)
								*pNumTok = numTok;

							// handle time-based units
							auto ParseUnit = [&tokenizer, &numTok, EError, EWarning](const char *unit, float msValue, int64_t &result)
							{
								// match the keyword
								if (tokenizer.CheckKeyword(unit))
								{
									// figure the time in 7.68ms DCS frames
									int64_t t = static_cast<int64_t>(roundf(numTok.fval * msValue / 7.68f));
									if (numTok.type != DCSTokenizer::TokType::Int && numTok.type != DCSTokenizer::TokType::Float)
										tokenizer.Error(EError, "Time value \"<n> %s\" must be a number, found '%s'", unit, numTok.text.c_str());
									else if (t < 0)
										tokenizer.Error(EError, "Invalid negative time value");
									else if (t == 0)
										tokenizer.Error(EWarning, "Time value \"%.f %s\" is a zero-length wait, because "
											"the minimum wait is the DCS frame time of 7.68ms", numTok.fval, unit);

									// return the time value
									result = t;
									return true;
								}

								// not matched
								return false;
							};

							// try unit suffixes
							int64_t result;
							if (ParseUnit("sec", 1000.0f, result))
								return result;
							else if (ParseUnit("ms", 1.0f, result))
								return result;

							// no time units, so it's a frame counter; get the integer value
							result = numTok.ival;
							if (numTok.type != DCSTokenizer::TokType::Int)
								tokenizer.Error(EError, "Time value in frame units must be an integer, found '%s'", numTok.text.c_str());

							// reject negative frame counts
							if (result < 0)
								tokenizer.Error(EError, "Invalid negative time value", static_cast<int32_t>(result));
							
							// return the result
							return result;
						};

						// Unrolled repeat wait.  We define some special commands that act
						// like macros to carry out multiple byte-code steps, such as STOP(*)
						// to stop multiple channels.  Since we have to unroll these into
						// multiple steps, we have to decide how to translate a WAIT prefix
						// for the macro into a WAIT prefix for the individual steps.  This
						// variable keeps track of the explicit time WAIT specified for the
						// current instruction, so that we can decide how to apply it if the
						// current command turns out to be a macro.  The unrolled repeat
						// wait is always the current wait only, not any accumulated wait
						// from prior steps, and is only an explicit time/frame wait, never
						// a Wait(Stream).  We don't want to apply Wait(Stream) to repeated
						// steps because the stream wait is inherently used up in its first
						// iteration, as the stream it was waiting for ends at that point.
						// We also can't unroll Wait(forever) for the obvious reason that
						// we can never get to any of the steps of an unrolled sequence if
						// we have to wait forever before the first step.
						int64_t unrolledRepeatWait = 0;

						// Update container bookkeeping counters with a new wait time
						auto WaitBookkeeping = [&lastStreamTimeRemaining, &loopStack](int64_t t)
						{
							// Deduct the new wait from the remaining stream wait
							if (lastStreamTimeRemaining > 0)
							{
								if (t > lastStreamTimeRemaining)
									lastStreamTimeRemaining = 0;
								else
									lastStreamTimeRemaining -= static_cast<int>(t);
							}

							// add it in the current loop level wait time
							if (loopStack.size() != 0)
								loopStack.back().waitTime += t;
						};

						// Apply a wait to the curent step, adding NOP steps as needed for
						// a wait that exceeds the 16-bit capacity of the opcode wait prefix
						auto ApplyWait = [this, &step, &track](int64_t t)
						{
							// If the new wait is over 0xFFFE, we have to break the wait
							// into multiple program steps, since the maximum wait we can
							// encode in a single step is 0xFFFE.  0xFFFF is out of bounds
							// because it's the special magic value for "forever".  A finite
							// wait of 0xFFFF is not out of the question, but it takes two
							// steps to encode it.
							while (t > 0xFFFE)
							{
								// set the current step to the maximum 0xFFFE
								step->wait = 0xFFFE;

								// deduct this from the remaining total wait time
								t -= 0xFFFE;

								// add a new NOP step to hold as much of the remaining 
								// wait as possible
								step = &track->steps.emplace_back();
								step->opcode = 0x0D;
							}

							// allocate the remaining wait time to the (possibly new) last step
							step->wait = static_cast<uint16_t>(t);
						};

						// check for a Wait prefix
						if (tokenizer.CheckKeyword("wait"))
						{
							// require the '('
							if (!tokenizer.RequirePunct("("))
								break;

							// the wait parameter can be "forever", "stream", "stream - <time>", or
							// a time value
							int64_t curWait = 0;
							if (tokenizer.CheckKeyword("forever"))
							{
								// the magic value 0xFFFF means wait(forever)
								step->wait = 0xFFFF;
							}
							else if (tokenizer.CheckKeyword("stream"))
							{
								// Wait for the remaining time on the most recently played stream. 
								// Conditions of use:
								//
								// - The last stream played must have a known length (so it can't be a
								//   forward reference to a stream that hasn't been played yet)
								// 
								// - If we're in a loop, the stream must have been started at the same
								//   loop level, or within a nested loop
								// 
								if (lastStreamTimeRemaining <= 0)
								{
									tokenizer.Error(EError, "Wait(stream) can only be used after Play() with a stream "
										"processed earlier in the script.");
								}
								else if (loopStack.size() != 0 && lastStreamLoopLevel != loopStack.size())
								{
									tokenizer.Error(EError, "Wait(stream) can only be used within a loop with a "
										"stream started earlier within the same loop, or within a nested loop.");
								}
								else
								{
									// set the wait time to the stream time remaining
									curWait = lastStreamTimeRemaining;
								}

								// Check for a deduction, for timing an event in advance of the
								// end of the stream
								if (tokenizer.CheckPunct("-"))
								{
									// read the deduction
									int64_t deduction = ParseTime();

									// deduct it from the wait, but don't go below zero
									curWait -= deduction;
									if (curWait < 0)
									{
										tokenizer.Error(EWarning, "This is a zero-length wait, because the deduction "
											"exceeds the remaining stream time.");
										curWait = 0;
									}
								}
							}
							else
							{
								// parse a literal time value
								curWait = ParseTime();

								// an explicit time wauit can be applied to the unrolled
								// steps of a multi-step macro
								unrolledRepeatWait = curWait;
							}

							// Update the container bookkeeping for the new wait
							WaitBookkeeping(curWait);

							// If the previous wait wasn't infinite, add the previous wait
							// already carried forward into this step.  We can fold consecutive 
							// waits ttogether as long as there are no actual operations between
							// them.
							if (step->wait != 0xFFFF)
								curWait += step->wait;

							// apply the wait, adding NOPs as needed
							ApplyWait(curWait);

							// require the ')'
							if (!tokenizer.RequirePunct(")"))
								break;
						}

						// check for an empty statement - a NOP
						if (tokenizer.CheckPunct(";"))
						{
							needSemicolon = false;
							break;
						}

						// check if we're at the end of a block
						if (tokenizer.CheckPunct("}"))
						{
							// if we're not inside a loop, this ends the track program
							if (loopStack.size() == 0)
							{
								// not in a loop - this ends the track program
								done = true;
							}
							else
							{
								// emit an End Loop opcode
								step->opcode = 0x0F;

								// get the loop's wait time
								auto &l = loopStack.back();
								int64_t loopTime = l.waitTime * l.iters;

								// check if the last stream started within this loop
								if (lastStreamLoopLevel == loopStack.size())
								{
									// The stream started in the loop, so the remaining time
									// on the stream flows to the enclosing loop level.  Treat
									// the stream as though it started in the enclosing level,
									// and leave the remaining time as it is.
									lastStreamLoopLevel -= 1;
								}
								else
								{
									// The stream started in an enclosing level, so the stream's
									// remaining time after the loop is the remaining time going
									// into the loop minus the time spent in the loop times the
									// number of loop iterations.  Note that we've already counted
									// one iteration against the stream time, since we count each
									// wait against the current stream as we encounter it, so we
									// just have to deduct N-1 iterations at this point.
									int64_t addedTime = loopTime - l.waitTime;
									if (addedTime > lastStreamTimeRemaining)
										lastStreamTimeRemaining = 0;
									else
										lastStreamTimeRemaining -= static_cast<int>(addedTime);
								}

								// pop the loop stack
								loopStack.pop_back();

								// add the loop's total wait time to the enclosing loop's wait time
								if (loopStack.size() != 0)
									loopStack.back().waitTime += loopTime;
							}

							// this ends the statement, and doesn't need a ';'
							needSemicolon = false;
							break;
						}

						// We have a full opcode statement - read the opcode name
						DCSTokenizer::Token opname = tokenizer.ReadSymbol();

						// "Loop" has special syntax
						if (opname.IsKeyword("loop"))
						{
							// set the opcode
							step->opcode = 0x0E;

							// check for a counter
							uint8_t cnt = 0;
							if (tokenizer.CheckPunct("("))
							{
								// read the count
								cnt = static_cast<uint8_t>(tokenizer.ReadUInt8().ival);
								if (cnt == 0)
									tokenizer.Error(EWarning, "Loop(0) creates an infinite loop");

								// require the ')' 
								tokenizer.RequirePunct(")");
							}
							step->AddOpByte(cnt);

							// read the '{'
							tokenizer.RequirePunct("{");

							// push the loop stack level
							loopStack.emplace_back(cnt, lastStreamTimeRemaining);

							// this statement doesn't end with a semicolon
							needSemicolon = false;

							// done with this statement
							break;
						}

						// parameter table
						class Params
						{
						public:
							Params(DCSCompiler *compiler, DCSTokenizer &tokenizer) : compiler(compiler), tokenizer(tokenizer) { }
							DCSCompiler *compiler;
							DCSTokenizer &tokenizer;

							// parameter value
							struct Value
							{
								Value() { }
								Value(const DCSTokenizer::Token &tok) : tok(tok) { }

								// parameter token
								DCSTokenizer::Token tok;

								// parsed time value, for parameters that specify times
								int64_t timeVal = 0;
							};

							// parameter map
							std::unordered_map<std::string, Value> params;

							// Set the default parameter.  If there's an unnamed parameter, set
							// its name to the given name.
							void SetDefaultParam(const char *name)
							{
								if (auto it = params.find(""); it != params.end())
								{
									// re-list the parameter under the new name
									Add(name, it->second.tok);
									params.erase("");
								}
							}

							// Check for extraneous parameters.  Pass a nullptr-terminated list of
							// the allowed parameters.
							void Check(const char *stmt, ...)
							{
								// make a std::list of the names
								std::list<std::string> names;
								va_list va;
								va_start(va, stmt);
								for (auto name = va_arg(va, const char *); name != nullptr; name = va_arg(va, const char*))
									names.push_back(name);

								// check each map entry for a corresponding name list entry
								for (auto &p : params)
								{
									// make sure this parameter name is in the list
									if (std::find(names.begin(), names.end(), p.first) == names.end())
										tokenizer.Error(EError, "Invalid parameter '%s' in %s statement", p.first.c_str(), stmt);
								}
							}

							// add a parameter name/value pair
							Value dummyVal;
							Value &Add(const std::string &name, DCSTokenizer::Token &val)
							{
								// store the token with an uppercase key name for case-insensitive compares
								std::string key = name;
								std::transform(key.begin(), key.end(), key.begin(), ::toupper);

								// check that it's not already present
								if (params.find(key) != params.end())
								{
									tokenizer.Error(EError, "Parameter %s is already defined for this statement", key.c_str());
									return dummyVal;
								}

								// store it and return the new Value object reference
								return params.emplace(key, val).first->second;
							}

							// check if a parameter exists
							bool Has(const char *name) const
							{
								return params.find(name) != params.end();
							}

							// get a parameter token value
							DCSTokenizer::Token missingTok{ DCSTokenizer::TokType::Invalid, "<missing>" };
							const DCSTokenizer::Token& GetTok(const char *name, const char *stmt) const
							{
								if (auto it = params.find(name); it != params.end())
									return it->second.tok;
								else
								{
									tokenizer.Error(EError, "Missing %s parameter in %s statement", name, stmt);
									return missingTok;
								}
							}

							// get an optional parameter token value
							const DCSTokenizer::Token *GetTokOpt(const char *name) const
							{
								if (auto it = params.find(name); it != params.end())
									return &it->second.tok;
								else
									return nullptr;
							}

							// get the parameter as a time value
							int64_t GetTime(const char *name, const char *stmt) const
							{
								if (auto it = params.find(name); it != params.end())
								{
									auto &tok = it->second.tok;
									if (tok.type != DCSTokenizer::TokType::Int && tok.type != DCSTokenizer::TokType::Float)
										tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected frame count or time value", name, stmt);
									return it->second.timeVal;
								}
								else
								{
									tokenizer.Error(EError, "Missing %s parameter in %s statement", name, stmt);
									return 0;
								}
							}

							// get a parameter as a symbol
							const char *GetSym(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Symbol)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected a name", name, stmt);
								return tok.text.c_str();
							}

							// get a parameter as a string
							const char *GetStr(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::String)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected a string value", name, stmt);
								return tok.text.c_str();
							}

							// get a parameter as a number
							int GetInt(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Int)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected an integer value", name, stmt);
								return tok.ival;
							}

							// get a channel number parameter
							int GetChannel(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Int)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected an integer value", name, stmt);
								else if (tok.ival < 0 || tok.ival > compiler->maxChannelNumber)
									tokenizer.Error(EError, "Invalid %s parameter value %d in %s statement; the prototype firmware version "
										"only supports channel numbers 0 to %d", name, tok.ival, stmt, compiler->maxChannelNumber);
								return tok.ival;
							}

							// get a parameter as a signed INT8
							int GetInt8(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Int)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected an integer value", name, stmt);
								else if (tok.ival < -128 || tok.ival > 127)
									tokenizer.Error(EError, "Value out of range for %s parameter in %s statement (valid range is -128 to 127)", name, stmt);
								return tok.ival;
							}

							// get a parameter as a UINT8
							int GetUInt8(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Int)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected an integer value", name, stmt);
								else if (tok.ival < 0 || tok.ival > 255)
									tokenizer.Error(EError, "Value out of range for %s parameter in %s statement (valid range is 0 to 255)", name, stmt);
								return tok.ival;
							}

							// get a parameter as a UINT16
							int GetUInt16(const char *name, const char *stmt) const
							{
								auto &tok = GetTok(name, stmt);
								if (tok.type != DCSTokenizer::TokType::Invalid && tok.type != DCSTokenizer::TokType::Int)
									tokenizer.Error(EError, "Wrong type for %s parameter in %s statement; expected an integer value", name, stmt);
								else if (tok.ival < 0 || tok.ival > 65535)
									tokenizer.Error(EError, "Value out of range for %s parameter in %s statement (valid range is 0 to 65535)", name, stmt);
								return tok.ival;
							}
						};
						Params params(this, tokenizer);

						// A STREAM parameter that specifies a filename can include compression 
						// parameters - set up to use the default compression parameters
						DCSEncoder::CompressionParams compressionParams = defaultCompressionParams;

						// check for parameters
						if (tokenizer.CheckPunct("("))
						{
							// keep going until we run out of parameters
							while (!tokenizer.IsEOF())
							{
								// read the first token - usually a parameter name
								DCSTokenizer::Token paramName = tokenizer.Read();

								// If this is the first parameter, and the next token is
								// ')', it's a "default" parameter with no name.  This is
								// allowed for opcodes that only have one operand; it's
								// more intuitive (and more concise) in these cases to just
								// give the operand without bothering to include the name.
								if (params.params.size() == 0 && tokenizer.CheckPunct(")"))
								{
									// enter it as the default parameter, with no name
									params.Add("", paramName);

									// stop here
									break;
								}

								// For PLAY("filename"), with the STREAM param name implied,
								// allow compression parameters in parens after the filename.
								if (params.params.size() == 0 && opname.IsKeyword("PLAY")
									&& paramName.type == DCSTokenizer::TokType::String
									&& tokenizer.CheckPunct("("))
								{
									// enter the string as the default parameter
									params.Add("", paramName);

									// parse the compression parameters
									ParseCompressionParams(tokenizer, compressionParams);

									// make sure we have the closing ')' for the statement
									tokenizer.RequirePunct(")");
								
									// stop here
									break;
								}


								// Check for parameters with special types:
								//
								//   SetMixingLevel(STEPS) -> time value
								//
								bool valueDone = false;
								if (opname.IsKeyword("SetMixingLevel") && paramName.IsKeyword("STEPS"))
								{
									// parse a time value
									DCSTokenizer::Token leadTok;
									int64_t t = ParseTime(&leadTok);

									// add the parameter and set the time value
									auto &v = params.Add(paramName.text, leadTok);
									v.timeVal = t;

									// value processed
									valueDone = true;
								}

								// if we didn't do special value-type parsing, parse a generic token value
								if (!valueDone)
								{
									// read the parameter value, and add a map entry
									DCSTokenizer::Token paramVal = tokenizer.Read();
									params.Add(paramName.text, paramVal);

									// Special case: for a PLAY(STREAM) parameter, we can have a
									// list of compression parameters in parens when the stream is
									// specified as a filename string
									if (opname.IsKeyword("PLAY") && paramName.IsKeyword("STREAM")
										&& paramVal.type == DCSTokenizer::TokType::String
										&& tokenizer.CheckPunct("("))
									{
										// parse the compression parameters
										ParseCompressionParams(tokenizer, compressionParams);
									}
								}

								// check for a comma or close paren
								if (tokenizer.CheckPunct(","))
								{
									// another parameter follows; just keep looping
								}
								else if (tokenizer.CheckPunct(")"))
								{
									// end of the parameters
									break;
								}
								else
								{
									// error
									auto tok = tokenizer.Read();
									tokenizer.Error(EError, "Expected ',' or ')' in opcode parameter list, found '%s'", tok.text.c_str());
									break;
								}
							}
						}

						// set the opcode and operand bytes the code for the program step
						if (opname.IsKeyword("End"))
						{
							// end of track
							hasEndOp = true;
							step->opcode = 0x00;
						}
						else if (opname.IsKeyword("Play"))
						{
							// Play(channel <channel>, stream <stream>, repeat <count>) - load and play an audio stream
							params.SetDefaultParam("STREAM");
							params.Check("End", "CHANNEL", "STREAM", "REPEAT", nullptr);

							// Optional parameter: channel
							int targetChannel = channel;
							if (params.Has("CHANNEL"))
								targetChannel = params.GetChannel("CHANNEL", "Play");

							// Optional parameter: repeat
							int repeat = 1;
							if (params.Has("REPEAT"))
								repeat = params.GetUInt8("REPEAT", "Play");

							// set up the new stream time counter, presuming we don't know the new
							// stream's play time yet
							lastStreamTimeRemaining = -1;
							lastStreamLoopLevel = loopStack.size();

							// Required parameter: stream <name>|<number>
							auto &streamTok = params.GetTok("STREAM", "Play");
							if (streamTok.type == DCSTokenizer::TokType::Symbol)
							{
								// Stream by symbolic name.  This selects a stream defined with
								// a STREAM statement, which can come before or after Play()
								// references.  If we've already loaded the stream, we can get
								// its Stream object directly; otherwise we have to keep the
								// reference in the form of a name until later.
								if (Stream *stream = FindStream(streamTok.text.c_str()) ; stream != nullptr)
								{
									// it's already defined - set the direct stream reference
									step->stream = stream;

									// set the remaining play time counter for Wait(stream) to the
									// new stream's full length
									lastStreamTimeRemaining = stream->nFrames;
								}
								else
								{
									// it's not defined yet - set the reference by name
									step->streamName = streamTok.text;

									// remember the file location, in case we need to report an error
									// later on when resolving the stream name symbol
									step->refLoc = tokenizer.GetLocation();
								}
							}
							else if (streamTok.type == DCSTokenizer::TokType::Int)
							{
								// Stream reference by number.  This selects a stream loaded
								// in the prototype file, by address.
								if (auto it = streamsByProtoAddr.find(streamTok.ival); it != streamsByProtoAddr.end())
								{
									// set the reference
									step->stream = it->second;

									// set the last stream time
									lastStreamTimeRemaining = it->second->nFrames;
								}
								else
								{
									// stream not found
									tokenizer.Error(EError, "Stream $%08lx doesn't exist in the prototype ROM set", streamTok.ival);
								}
							}
							else if (streamTok.type == DCSTokenizer::TokType::String)
							{
								// Encode the stream and store the result in the program step.  This
								// type of stream has no name, so there's no need to add a name map
								// entry for it - it's only reachable from this script step.
								step->stream = EncodeFile(nullptr, nullptr, streamTok.text.c_str(),
									compressionParams, tokenizer);

								// set the stream time
								if (step->stream != nullptr)
									lastStreamTimeRemaining = step->stream->nFrames;
							}
							else if (streamTok.type != DCSTokenizer::TokType::Invalid)
							{
								tokenizer.Error(EError, "Invalid STREAM parameter value '%s' in Play statement; "
									"the stream must be specified as a stream name, a numeric stream address for "
									"a stream imported from the prototype file (only for --patch mode), or the name "
									"of an external audio file to encode as the stream contents", streamTok.text.c_str());
							}

							// Encode the instruction.  Use zero as placeholder for the 
							// stream pointer.  The stream pointer will be fixed up in the
							// "link" phase, where when we lay out the contents of the ROM
							// images and assign everything a final location.
							step->opcode = 0x01;
							step->AddOpByte(targetChannel);
							step->AddOpPtr(0);
							step->AddOpByte(repeat);
						}
						else if (opname.IsKeyword("Stop"))
						{
							// Stop(channel <channel>)
							params.SetDefaultParam("CHANNEL");
							params.Check("Stop", "CHANNEL", nullptr);

							// CHANNEL can be '*' for "stop all OTHER channels", '**' for 
							// "stop ALL channels", or specified channel number
							if (params.Has("CHANNEL") && params.GetTok("CHANNEL", "Stop").text == "*")
							{
								// It's one of the special '*' or '**' wildcards.  Generate
								// a Stop for each channel, excluding the current channel.
								for (int i = 0 ; i <= maxChannelNumber ; ++i)
								{
									// Skip the current channel for now, since stopping the
									// current channel cancels the current track program,
									// hence we wouldn't get a chance to stop the remaining
									// channels later in the loop.  (Many of the original
									// DCS ROMs contain exactly this error in their Track 
									// $0000 "All Stop" programs!)
									if (i != channel)
									{
										// If we've already populated the current step, add a new one
										if (step->opcode == 0x02)
										{
											// add the step
											step = &track->steps.emplace_back();

											// apply the unrolled wait to the step
											ApplyWait(unrolledRepeatWait);
											WaitBookkeeping(unrolledRepeatWait);
										}

										// encode the STOP instruction
										step->opcode = 0x02;
										step->AddOpByte(i);
									}
								}
							}
							else
							{
								// not a wildcard, so it must be a channel number
								int targetChannel = params.GetChannel("CHANNEL", "Stop");

								// encode the instruction
								step->opcode = 0x02;
								step->AddOpByte(targetChannel);
							}
						}
						else if (opname.IsKeyword("Queue"))
						{
							// Queue(track <trackNum>)
							params.SetDefaultParam("TRACK");
							params.Check("Queue", "TRACK", nullptr);
							int track = params.GetUInt16("TRACK", "Queue");

							// encode it
							step->opcode = 0x03;
							step->AddOpWord(track);
						}
						else if (opname.IsKeyword("WriteDataPort"))
						{
							// WriteDataPort(byte <val>)
							params.SetDefaultParam("BYTE");
							params.Check("WriteDataPort", "BYTE", nullptr);
							int b = params.GetUInt8("BYTE", "WriteDataPort");

							// encode it
							step->opcode = 0x04;
							step->AddOpByte(b);

							// If this is OS93a, opcode 0x04 has an additional UINT16
							// operand setting the channel timer counter.  When the
							// counter operand is zero, the opcode has exactly the same
							// effect as the simplified opcode 0x04 in later OS versions,
							// so WriteDataPort() is a perfectly good alias for the
							// opcode with the UINT16 operand set to zero.
							if (protoRomOSVer == DCSDecoder::OSVersion::OS93a)
							{
								// Add a zero UINT16 operand to mean "clear the timer"
								step->AddOpWord(0);

								// If the data port byte is zero, it's an error, because 
								// OS93a won't actually send anything to the data port in 
								// this (whereas the later OSes will: thus the error,
								// since the meaning is different in this case).
								if (b == 0)
								{
									tokenizer.Error(EError, "OS93a won't send a zero byte to the data port with this instruction; "
										"if you only intended to clear the channel timer, use SetChannelTimer() instead, to make that "
										"intention explicit");
								}
							}
						}
						else if (opname.IsKeyword("SetChannelTimer"))
						{
							// SetTimer(byte <val>, counter <count>)
							params.SetDefaultParam("BYTE");
							params.Check("SetChannelTimer", "BYTE", "INTERVAL");
							int b = params.GetUInt8("BYTE", "SetChannelTimer");
							int interval = params.Has("INTERVAL") ? params.GetUInt16("INTERVAL", "SetChannelTimer") : 0;

							// encode the instruction
							step->opcode = 0x04;
							step->AddOpByte(b);
							step->AddOpWord(interval);

							// this form of the opcode only works on OS93a
							if (protoRomOSVer != DCSDecoder::OSVersion::OS93a)
							{
								tokenizer.Error(EError, "SetChannelTimer() is only implemented in the OS93a software "
									"(from Indiana Jones or Judge Dredd); the closest equivalent in the later "
									"OS versions is WriteDataPort()");
							}
						}
						else if (opname.IsKeyword("StartDeferred"))
						{
							// trigger a deferred command on a channel
							params.SetDefaultParam("CHANNEL");
							params.Check("StartDeferred", "CHANNEL", nullptr);
							int targetChannel = params.GetChannel("CHANNEL", "StartDeferred");

							// encode it
							step->opcode = 0x05;
							step->AddOpByte(targetChannel);
						}
						else if (opname.IsKeyword("SetVariable"))
						{
							// SetVariable(var <name|number>, value <byteval>)
							params.Check("SetVariable", "VAR", "VALUE", nullptr);
							auto &varTok = params.GetTok("VAR", "SetVariable");
							int val = params.GetUInt8("VALUE", "SetVariable");

							// interrpet the variable: if it's a symbol, it has to be a previously
							// defined variable name; otherwise, it's a number giving the variable
							// ID (the index into the run-time array of opcode 0x06 variables)
							int varIndex = 0;
							if (varTok.type == DCSTokenizer::TokType::Symbol)
							{
								// look up the variable
								if (auto *v = FindVariable(varTok.text.c_str()); v != nullptr)
									varIndex = v->id;
								else
									tokenizer.Error(EError, "Undefined variable name '%s' used in SetVariable VAR parameter "
										"; variables must be defined before use with VAR <name> statements in global scope",
										varTok.text.c_str());
							}
							else if (varTok.type == DCSTokenizer::TokType::Int)
							{
								// make sure it's in range
								varIndex = varTok.ival;
								if (varIndex < 0 || varIndex > 255)
									tokenizer.Error(EError, "Variable index %d is out of range (0 to 255 allowed, 0 to 79 recommended)", varIndex);
								else if (varIndex > 79)
									tokenizer.Error(EWarning, "Variable index %d is unsafe for use with original DCS ROM "
										"firmware; indices higher than 79 might cause memory corruptions at run-time when "
										"running on physical DCS boards or under the PinMame emulator", varIndex);
							}
							else
							{
								tokenizer.Error(EError,
									"SetVariable VAR parameter must be a variable name or numeric index, found '%s'",
									varTok.text.c_str());
							}

							// encode the instruction
							step->opcode = 0x06;
						
							// this has no effect on the 1993 games
							if (protoRomOSVer == DCSDecoder::OSVersion::OS93a || protoRomOSVer == DCSDecoder::OSVersion::OS93b)
							{
								// Warn that this is a no op on this OS, and generate the
								// instruction with no operand bytes.  OS93 reads the
								// instruction and treats it as a zero-operand no-op.
								tokenizer.Error(EWarning, "SetVariable has no effect with the 1993 operating system");
							}
							else
							{
								// add the operand bytes
								step->AddOpByte(varIndex);
								step->AddOpByte(val);

								// This instruction will be scanned in the reference checking
								// pass at the end of the parsing pass, to ensure that the
								// variable value reference is within bounds for all associated
								// deferred indirect tables indexed through the variable. 
								// Remember the source location for reporting in any errors
								// detected during that scan.
								step->refLoc = tokenizer.GetLocation();
							}
						}
						else if (opname.IsKeyword("SetMixingLevel"))
						{
							// SetMixingLevel([channel <channel>], level|increase|decrease <val>, [steps <steps>])

							// This operation comprises 6 opcodes:
							//
							//   0x07 Set level immediate
							//   0x08 Increase level immediate
							//   0x09 Decrease level immediate
							//   0x0A Set level gradually over N steps
							//   0x0B Increase level over N steps
							//   0x0C Decrease level over N steps
							//
							// The grouping is formulaic: start with base opcode 0x07, then
							// add 3 if the STEPS parameter is present to specify the fade
							// time, and then also add 1 if the INCREASE parameter is present
							// or 2 if DECREASE is present.
							params.SetDefaultParam("LEVEL");
							params.Check("SetMixingLevel", "LEVEL", "INCREASE", "DECREASE", "CHANNEL", "STEPS", nullptr);
							step->opcode = 0x07;

							// get the optional channel spec, using the track's channel as the default
							int targetChannel = channel;
							if (params.Has("CHANNEL"))
								targetChannel = params.GetChannel("CHANNEL", "SetMixingLevel");

							// we need one of LEVEL, INCREASE, or DECREASE
							int val = 0;
							int nLevelParams = 0;
							if (params.Has("LEVEL"))
								++nLevelParams, val = params.GetInt8("LEVEL", "SetMixingLevel");
							if (params.Has("INCREASE"))
								++nLevelParams, step->opcode += 1, val = params.GetInt8("INCREASE", "SetMixingLevel");
							if (params.Has("DECREASE"))
								++nLevelParams, step->opcode += 2, val = params.GetInt8("DECREASE", "SetMixingLevel");

							// make sure we have exactly one level parameter
							if (nLevelParams == 0)
								tokenizer.Error(EError, "LEVEL, INCREASE, or DECREASE must be specified in SetMixingLevel");
							else if (nLevelParams > 1)
								tokenizer.Error(EError, "Contradictory SetMixingLevel parameters - only one of LEVEL, INCREASE, or DECREASE may be specified");

							// encode the instruction
							step->AddOpByte(targetChannel);
							step->AddOpByte(val);

							// add the steps parameter for the Fade variant
							if (params.Has("STEPS"))
							{
								step->opcode += 3;
								int64_t t = params.GetTime("STEPS", "SetMixingLevel");
								if (t < 0 || t > 65535)
									tokenizer.Error(EError, "STEPS value is out of range; must be 0 to 65535 frames (503 seconds)");
								step->AddOpWord(static_cast<uint16_t>(t));
							}
						}
						else
						{
							// invalid opcode name
							tokenizer.Error(EError, "Invalid track program step command '%s'", opname.text.c_str());
						}

						// If we have an infinite wait with an opcode other
						// than End, warn that this (and everything after it)
						// is unreachable.
						if (step->wait == 0xFFFF)
						{
							// note that we've encountered an infinite wait
							hasWaitForever = true;

							// warn if anything other than End follows
							if (!opname.IsKeyword("end") && !opsPastEnd)
							{
								// warn
								tokenizer.Error(EWarning, "Everything after wait(forever) is unreachable");

								// only generate this warning once
								opsPastEnd = true;
							}
						}
						else if ((hasWaitForever || hasEndOp) && !opsPastEnd)
						{
							// this statement follows an End or an earlier infinite wait -
							// warn that it (and everything past it) is unreachable
							tokenizer.Error(EWarning, "This statement (and everything after it) is unreachable "
								"due to an earlier Wait(forever) and/or End");
							opsPastEnd = true;
						}

					} while (false);

					// end the statement if required
					if (needSemicolon)
						tokenizer.EndStatement();
				}

				// if there's no END or WAIT FOREVER, add an end op
				if (!(hasEndOp || hasWaitForever))
				{
					// If the last step is a NOP, we can take it over as the
					// end op.  Otherwise we need to allocate a new one.
					if (track->steps.size() != 0 && track->steps.back().opcode == 0x0D)
					{
						// the last step is a NOP; simply change it to END
						track->steps.back().opcode = 0x00;
					}
					else
					{
						// add a new END step
						auto &endStep = track->steps.emplace_back();
						endStep.opcode = 0x00;
					}
				}
			}

			// end the statement
			tokenizer.EndStatement();
		}
	}

	// Run post-compile checks and fixups: resolve stream name references, check
	// deferred indirect table reference bounds.
	for (auto &track : tracks)
		track.second.ResolveRefs(this, tokenizer);
}

void DCSCompiler::ParseCompressionParams(DCSTokenizer &tokenizer, DCSEncoder::CompressionParams &params)
{
	// convenience definitions for the error logger
	using ErrorLevel = DCSTokenizer::ErrorLevel;
	const ErrorLevel EFatal = ErrorLevel::Fatal;
	const ErrorLevel EError = ErrorLevel::Error;
	const ErrorLevel EWarning = ErrorLevel::Warning;

	// keep going until we reach the ")"
	while (!tokenizer.CheckPunct(")"))
	{
		// if we're at a ';', assume the ')' was missing
		auto save = tokenizer.Save();
		if (tokenizer.CheckPunct(";"))
		{
			tokenizer.Restore(save);
			tokenizer.Error(EError, "Found ';' within a compression parameters list; check for a missing ')' at the end of the parameters");
			return;
		}

		// Read the name, and convert to upper-case for case-insensitive matching
		std::string param = tokenizer.ReadSymbol().text;
		std::transform(param.begin(), param.end(), param.begin(), ::toupper);

		// Read the value.  Some parameters allow '*' to indicate a special option.
		tokenizer.RequirePunct("=");
		float fval = 0.0f;
		if ((param == "TYPE" || param == "SUBTYPE") && tokenizer.CheckPunct("*"))
		{
			// TYPE=*, SUBTYPE=* - use the magic value -1, which means that we
			// should try all format options and pick the one that yields the 
			// smallest stream
			fval = -1.0f;
		}
		else
		{
			// everything else needs an int or float value; parse it as a float
			// for more generality
			fval = tokenizer.ReadFloat().fval;
		}

		// get the integer equivalent, for params that only take ints
		int ival = static_cast<int>(fval);

		// apply the parameter
		if (param == "TYPE")
		{
			params.streamFormatType = ival;
			if (!(ival == -1 || ival == 0 || ival == 1))
				tokenizer.Error(EError, "Invalid TYPE parameter; must be 0, 1, or *");
		}
		else if (param == "SUBTYPE")
		{
			params.streamFormatSubType = ival;
			if (!(ival == -1 || ival == 0 || ival == 1 || ival == 2 || ival == 3))
				tokenizer.Error(EError, "Invalid SUBTYPE parameter; must be 0, 1, 2, 3, or *");
		}
		else if (param == "POWERCUT")
		{
			params.powerBandCutoff = fval / 100.0f;
			if (fval < 0.0f || fval > 100.0f)
				tokenizer.Error(EError, "Invalid POWERCUT parameter; must be 0.0 to 100.0");
		}
		else if (param == "MINRANGE")
		{
			params.minimumDynamicRange = fval / 32768.0f;
			if (fval < 0.0f || fval > 65536.0f)
				tokenizer.Error(EError, "Invalid MINRANGE parameter; must be 0 to 65536");
		}
		else if (param == "MAXERROR")
		{
			params.maximumQuantizationError = fval / 32768.0f;
			if (fval < 0.0f || fval > 65536.0f)
				tokenizer.Error(EError, "Invalid MAXERROR parameter; must be 0 to 65536");
		}
		else if (param == "BITRATE")
		{
			params.targetBitRate = ival;
			if (ival < 48000 || ival > 256000)
				tokenizer.Error(EError, "BITRATE parameter is out of range; must be 48000 to 256000");
		}
		else
		{
			tokenizer.Error(EError, "Invalid compression parameter name \"%s\"", param.c_str());
		}

		// heck for ',' or ')' - stop looping at ')'
		if (!tokenizer.CheckPunct(","))
		{
			tokenizer.RequirePunct(")");
			break;
		}
	}
}

bool DCSCompiler::GenerateROM(const char *outZipFile,
	uint32_t romSize, const char *romPrefix,
	std::string &errorMessage, std::list<ROMDesc> *romList)
{
	// 
	// Construct the new in-memory ROM images
	//

	// In-memory ROM image list
	struct ROMImage
	{
		ROMImage(int chipNum, uint32_t size) :
			chipNum(chipNum), size(size), data(new uint8_t[size])
		{
			// Clear the image data memory to all $FF bytes.  The choice of fill is 
			// more or less arbitrary, but $FF is what th original ROMs use in most
			// unpopulated areas, and the DCS ROM software checks for $FF bytes in
			// a lot of places to detect end markers and empty slots.  They're not
			// perfectly consistent about it - sometimes the end/empty markers are
			// $00 bytes - but it's the most common convention, and it's especially
			// consistent to pad out the unused portion at the top end of each ROM's
			// address space.  $FF is probably the conventional "empty" because it's
			// what the ADSP-2105 hardware will generally read when accessing bus
			// locations that aren't physically populated.
			memset(data.get(), 0xFF, size);

			// set the next free byte pointer to the start of the data - the
			// entire thing is uncommited so far
			p = data.get();
		}

		// chip number
		int chipNum;

		// Size, in bytes
		uint32_t size;

		// filename within the .zip archive (assigned during the .zip
		// file build)
		std::string filename;

		// Image data
		std::unique_ptr<uint8_t> data;

		// Next free byte pointer
		uint8_t *p;

		// free space available
		uint32_t BytesFree() const { return static_cast<uint32_t>(size - (p - data.get())); }

		// Force the next free pointer to be even-aligned.  If it's
		// currently on an odd boundary, we'll bump it by one byte.
		void EvenAlignFreePointer() 
		{
			if (((p - data.get()) & 1) != 0)
				++p;
		}

		// Force the next free pointer to be DWORD-aligned
		void DwordAlignFreePointer()
		{
			while (((p - data.get()) & 3) != 0)
				++p;
		}
	};

	std::list<ROMImage> newRoms;

	// Helper functions
	// Write a big-endian UINT16, advancing the pointer
	auto WriteU16 = [](uint8_t* &p, uint16_t val)
	{
		*p++ = static_cast<uint8_t>((val >> 8) & 0xFF);
		*p++ = static_cast<uint8_t>(val & 0xFF);
	};

	// write a bit-endian UINT32
	auto WriteU32 = [](uint8_t* &p, uint32_t val)
	{
		*p++ = static_cast<uint8_t>((val >> 24) & 0xFF);
		*p++ = static_cast<uint8_t>((val >> 16) & 0xFF);
		*p++ = static_cast<uint8_t>((val >> 8) & 0xFF);
		*p++ = static_cast<uint8_t>(val & 0xFF);
	};

	// Write a UINT24 ROM pointer, advancing the byte pointer.  This
	// writes the given UINT24 value literally, with no translation 
	// for the different pointer formats for DCS vs DCS-95 hardware.
	auto WriteU24 = [](uint8_t* &p, uint32_t val)
	{
		*p++ = static_cast<uint8_t>((val >> 16) & 0xFF);
		*p++ = static_cast<uint8_t>((val >> 8) & 0xFF);
		*p++ = static_cast<uint8_t>(val & 0xFF);
	};

	// Convert a ROM image pointer to a linear UINT24 address for
	// ROM storage
	auto LinearROMPointer = [this](const uint8_t *ptr, const ROMImage *rom)
	{
		// figure the offset from the base of the ROM
		uint32_t ofs = static_cast<uint32_t>(ptr - rom->data.get());

		// add the chip select - this goes in bits 21-24 for DCS-95
		// audio/video boards, or bits 20-23 for the original 
		// audio-only boards
		int chipSelectShift = (protoRomHWVer == DCSDecoder::HWVersion::DCS95) ? 21 : 20;

		// combine the chip select and the offset to form the linear address
		return ofs | ((rom->chipNum - 2) << chipSelectShift);
	};

	// Write a UINT24 ROM pointer.  This forms the correct linear
	// ROM pointer value for the prototype ROM's hardware platform.
	// Advances the write pointer.
	auto WriteROMPtr = [&LinearROMPointer](uint8_t* &p, const uint8_t *romPtr, const ROMImage *rom)
	{
		// figure the linear ROM pointer representation for storage
		auto addr = LinearROMPointer(romPtr, rom);

		// write the linear pointer value
		*p++ = static_cast<uint8_t>((addr >> 16) & 0xFF);
		*p++ = static_cast<uint8_t>((addr >> 8) & 0xFF);
		*p++ = static_cast<uint8_t>(addr & 0xFF);
	};

	// Figure the ROM size for a given chip number
	auto NewRomSize = [this, romSize](int chipNum) -> uint32_t
	{
		// start with the ROM size setting
		uint32_t newSize = romSize;

		// if that's set to SAME AS PROTO, we need to find the corresponding
		// prototype chip, so that we can create the new chip at the same size
		if (romSize == ROMSIZE_SAME_AS_PROTO)
		{
			// search for the new chip number in the prototype set
			for (auto &z : protoZipData)
			{
				if (z.chipNum == chipNum)
				{
					newSize = static_cast<uint32_t>(z.dataSize);
					break;
				}
			}

			// If we didn't find it, we must need more chips than
			// the original set.  Add the new chip at the maximum
			// 1M.  Note that there's no reason to match any of
			// the other chips in this case, since the DCS boards
			// are perfectly happy with heterogeneous sizes, and
			// PinMame won't be able to use the new set no matter
			// what size we choose, simply because we've added a
			// new chip that wasn't part of the original set and
			// thus isn't in PinMame's hard-coded list.
			if (newSize == ROMSIZE_SAME_AS_PROTO)
				newSize = 1024*1024;
		}

		// return the new size
		return newSize;
	};

	// Figure the largest ROM size that we'll have available.  This
	// sets the upper bound for the largest possible stream size,
	// since a stream has to fit entirely within a single ROM.
	size_t maxRomSize = NewRomSize(2);
	for (int chip = 3 ; chip <= 9 ; ++chip)
	{
		if (size_t curSize = NewRomSize(chip); curSize > maxRomSize)
			maxRomSize = curSize;
	}

	// Figure the maximum size allowed for a contiguous object (which is 
	// really the same as saying the maximum size for a stream, since
	// those are the only large contiguous objects we need to store).
	// This is the size of the largest ROM chip allowed in the current
	// configuration, minus some space for overhead (specifically, the
	// signature string at the start of each ROM.)
	const size_t maxContiguousObjectSize = maxRomSize - 16;

	// Add a new ROM image
	auto AddChip = [this, romSize, &newRoms, &errorMessage, &NewRomSize](int chipNum) -> ROMImage*
	{
		// Make sure we're not out of chip slots (the DCS boards can
		// only accept 8 ROM chips, U2-U9).
		if (newRoms.size() == 8)
		{
			// explain the problem
			errorMessage = DCSEncoder::format(
				"Out of space in the ROM layout.  The DCS pinball "
				"sound boards are limited to 8 ROM chips, but this build "
				"requires at least 9 chips at the current size setting of "
				"%dK bytes per chip.%s",
				romSize / 1024,
				(romSize == 1024*1024 ? "" :
					"  You could try increasing the generated ROM "
					"size setting to the maximum of 1M per chip."));

			// return failure
			return nullptr;
		}

		// Add a new ROM image to the list, and return the descriptor
		ROMImage *newRom = &newRoms.emplace_back(chipNum, NewRomSize(chipNum));

		// add the signature
		sprintf_s(reinterpret_cast<char*>(newRom->data.get()), newRom->size, "%c%d %s",
			protoRomHWVer == DCSDecoder::HWVersion::DCS95 ? 'S' : 'U',
			chipNum, shortDateStr);

		// set the free pointer to the end of the signature
		newRom->p = newRom->data.get() + strlen(reinterpret_cast<const char*>(newRom->data.get())) + 1;

		// return the new chip pointer
		return newRom;
	};

	// Reserve space for a given number of bytes.  Adds a new ROM
	// if necessary, and updates the pointers to the new space.
	auto Reserve = [this, &AddChip, &newRoms, &errorMessage](uint8_t* &p, ROMImage* &rom, size_t nBytes)
	{
		// check if we're out of space in the current ROM
		int ofs = static_cast<int>(p - rom->data.get());
		int rem = rom->size - ofs;
		if (rem < 0 || static_cast<size_t>(rem) < nBytes)
		{
			// We're out of space, so we need to create a new ROM image
			int chipNum = newRoms.back().chipNum + 1;
			auto newRom = AddChip(chipNum);
			if (newRom == nullptr)
				return false;
		}

		// success
		return true;
	};

	// Create the U2 image.  U2 is special because it contains a copy
	// of the ADSP-2105 program from the prototype ROM, and it also 
	// contains the "catalog" - the index of ROM chips and the track
	// program index.
	auto *u2 = AddChip(2);

	// find the prototype U2
	for (auto &proto : protoZipData)
	{
		if (proto.chipNum == 2)
		{
			// copy everything from the prototype U2 up to the start of the catalog
			memcpy(u2->data.get(), proto.data.get(), decoder.GetCatalogOffset());

			// we can stop searching for the prototype U2
			break;
		}
	}

	// Store the U2 signature and trailing null, if a new one was specified
	// in the script.  (Leave the prototype signature as-is if not.)
	if (signature[0] != 0)
	{
		// Before copying the new signature, zero out all of the bytes
		// occupied by the old one.  This will avoid leaving cruft if
		// the new signature is shorter than the old one.
		uint8_t *u2p = u2->data.get();
		for (int i = 4 ; i < 80 && u2p[i] != 0 ; ++i)
			u2p[i] = 0;

		// copy the new signature string
		strcpy_s(reinterpret_cast<char*>(&u2p[4]), 76, signature);
	}

	// Write the U2 track catalog pointer, deferred-indirect index pointer,
	// and track count.  The track index starts immediately after these
	// entries, and the deferred catalog starts immediately after that.
	const int nTracks = maxTrackNumber + 1;
	const uint32_t catalogOfs = decoder.GetCatalogOffset();
	const uint32_t trackIndexOfs = catalogOfs + 0x48;
	const uint32_t trackIndexSize = nTracks * 3;
	const uint32_t deferredIndexOfs = trackIndexOfs + trackIndexSize;

	// set up a working pointer to the U2 ROM image in memory
	ROMImage *pRom = u2;

	// Reserve space for the checksum fixup bytes (two bytes).  The
	// purpose of the fixup bytes is to make the checksum come out at a
	// chosen value (which happens to be zero, but is arbitrary and could
	// have been chosen to be any other value), by setting their values
	// such that the sum of all of the bytes comes out to the chosen
	// result.
	// 
	// Put the placeholder bytes immediately after the ROM catalog data.
	// The catalog is 8*6+2 = 50 ($32) bytes long (8 entries of 6 bytes 
	// each for the ROM slots, plus space for a 2-byte end marker).  The
	// track index starts 64 ($40) bytes after the start of the catlog.
	// The space between the ROM catalog and the track index is unused
	// in the original ROMs, as far as I can tell, and even if I've
	// missed something that's stored there in the originals, this
	// encoder doesn't otherwise use the space.  So we have 14 bytes
	// of free space.
	// 
	// The checksum is calculated separately over all of the even-offset
	// and odd-offset bytes in the ROM, so we need one even-offset slot
	// and one odd-offset slot for the balancer bytes.  So before we set
	// the location, make sure we're on an even boundary.
	//
	// Remember the location, and write zeroes as placeholders.  The
	// checksum is a linear sum of the bytes over the whole image, so
	// setting the placeholders to zero will effectively exclude these
	// bytes from the sum during the first pass to determine the
	// tentative checksum, which in turn determines the final values
	// we have to place in these bytes to get the checksum to come out
	// at a chosen value.
	pRom->p = u2->data.get() + catalogOfs + 0x32;
	pRom->EvenAlignFreePointer();
	uint8_t *pChecksumFixup = pRom->p;
	WriteU16(pRom->p, 0);

	// set up our working pointer at the start of the catalog data
	pRom->p = u2->data.get() + catalogOfs + 0x0040;
	WriteU24(pRom->p, trackIndexOfs);
	WriteU24(pRom->p, deferredIndexOfs);
	WriteU16(pRom->p, nTracks);

	// Reserve space for the track index.  The track index consists
	// of three bytes per track, for all tracks from 0 to the maximum
	// track number.  The index is a simple array, so every track number
	// gets a slot whether or not the track actually exists.  This makes
	// it easy to calculate the space required.
	uint8_t *pTrackIndex = pRom->p;
	pRom->p += nTracks * 3;

	// find the maximum populated deferred indirect table index
	int maxDITableIndex = -1;
	for (auto &dit : deferredIndirectTables)
	{
		auto index = dit.second->index;
		if (index > maxDITableIndex)
			maxDITableIndex = index;
	}

	// Figure the pointer to the end of the DI table pointer index.
	// The table contents will start immediately after the index.
	uint8_t *pDITIndex = pRom->p;
	pRom->p += (maxDITableIndex + 1) * 3;

	// populate the DI table index and table contents
	for (int tableIdx = 0 ; tableIdx <= maxDITableIndex ; ++tableIdx)
	{
		// if the table is populated, store it
		if (diByNumber[tableIdx] != nullptr)
		{
			// store a pointer to the current table
			WriteROMPtr(pDITIndex, pRom->p, pRom);

			// store the table entries
			for (auto tn : diByNumber[tableIdx]->trackNumbers)
				WriteU16(pRom->p, tn);
		}
		else
		{
			// there's no table at this index - store the conventional "empty"
			// pointer value of $FFFFFF
			WriteU24(pDITIndex, 0xFFFFFF);
		}
	}

	// Write the track programs, populating the track index as we go
	for (int trackNum = 0 ; trackNum < nTracks ; ++trackNum)
	{
		// get the track
		if (auto it = tracks.find(trackNum) ; it != tracks.end())
		{
			// Compile the track.  Note that this compilation pass is
			// tentative, because we haven't assigned the stream addresses
			// yet for any referenced streams.  But this will still generate
			// the right byte code size, which will let us assign the space.
			auto &track = it->second;
			track.Compile(this);

			// Reserve space for track.  In practice it's almost inconceivable
			// that the track programs could ever overflow U2, since they're
			// only on the order of tens of bytes each and there are typically
			// only around a hundred populated tracks.  But the format allows
			// for the track programs to be located anywhere, so let's take
			// advantage of that.  Note that the format doesn't allow the
			// track index to span ROMs, so there's no point in reserving
			// space for the next index entry, and it simply couldn't overflow
			// anyway since the track counter is only 16 bits (64K * 3 bytes
			// per entry = 192KB, which is way less than the minimum ROM size
			// of 512K - and that's only if every possible track were
			// populated, which is never true anyway).
			size_t trackSize = 2 + (track.type == 1 ? track.byteCode.size() : 2);
			if (!Reserve(pRom->p, pRom, trackSize))
				return false;

			// remember the track's assigned ROM image location in the track 
			// object, so that we can come back and re-write the byte code
			// program after resolving all of the stream references
			track.romImagePtr = pRom->p;

			// write the index entry to point to the track's reserved location
			WriteROMPtr(pTrackIndex, pRom->p, pRom);

			// write the track header: BYTE track type, BYTE channel number
			*pRom->p++ = track.type;
			*pRom->p++ = track.channel;

			// write the track program or deferred link, according to
			// the track type
			if (track.type == 1)
			{
				// Skip the space assigned to the track program.  There's
				// no point in actually copying the track on this pass,
				// because we have to recompile the track later, on the
				// second pass, in order to resolve any stream references.
				// The first pass compiles the tracks to get their size,
				// but they only contain placeholders for the track
				// addresses at this point.
				pRom->p += track.byteCode.size();
			}
			else if (track.type == 2 || track.type == 3)
			{
				// types 2 and 3 are deferred links; there's no track
				// program, just a 16-bit link code (another track
				// number for type 2, or an indirect index for type 3)
				WriteU16(pRom->p, track.deferredTrack);
			}
		}
		else
		{
			// This track isn't populated - write $FFFFFF to the track
			// index slot to tell the decoder program that this track
			// doesn't exist.
			WriteU24(pTrackIndex, 0xFFFFFF);
		}
	}

	// The streams come next.  They're essentially all that's left, and
	// they can be in arbitrary order; the only constraint is that each
	// one must be stored as a contiguous block within one ROM image (a
	// stream can't span a ROM boundary).
	// 
	// Stream sizes are variable, and we want to use the available ROM
	// image space efficiently by packing each populated ROM as near to
	// full as possible, which means we have to solve what's known as a
	// "bin packing" problem.  Optimal bin packing is computationally
	// intractable for large collections, but there are several good
	// heuristic algorithms that yield efficient packings without too
	// much work.  We'll use an algorithm known as "best fit", which
	// visits the items one at a time and assigns each one to the bin
	// with the least remaining free space that's sufficient to hold
	// the item.  This algorithm yields better results with the "best
	// fit decreasing" variant, where the elements are scanned in
	// descending order of size.  That selects the placement for the
	// largest elements first, which tends to improve the results
	// because the large items are easier to pack earlier, while most
	// of the space is still uncommitted.
	std::unique_ptr<Stream*> streamIndex(new Stream*[streams.size()]);
	Stream **pStreamIndex = streamIndex.get();
	std::list<Stream*> oversizedStreams;
	for (auto &s : streams)
	{
		// add it to the index
		*pStreamIndex++ = &s;

		// note if this stream is oversized
		if (s.referenced && s.nBytes > maxContiguousObjectSize)
			oversizedStreams.emplace_back(&s);
	}

	// If we found any oversized streams, generate an error message
	if (oversizedStreams.size() != 0)
	{
		errorMessage = DCSEncoder::format("One or more audio streams are too large.  Each "
			"compressed stream must fully fit within a single ROM image; the "
			"maximum ROM size for your current configuration is %s.  Consider breaking "
			"the long stream(s) into multiple parts and playing them back sequentially.\n"
			"The following streams are too large:\n",
			maxRomSize == 512*1024 ? "512K" : maxRomSize == 1024*1024 ? "1M" : 
			DCSEncoder::format("%u bytes", static_cast<int>(maxRomSize)).c_str());

		for (auto s : oversizedStreams)
		{
			errorMessage += DCSEncoder::format("   %s (%d bytes compressed size)",
				s->refName.c_str(), s->filename.c_str(), s->nBytes);
		}

		return false;
	}

	// Sort in descending order of size, for the Best Fit Decreasing
	// bin layout algorithm.
	std::sort(streamIndex.get(), streamIndex.get() + streams.size(),
		[](Stream* const &a, Stream* const &b) { return a->nBytes > b->nBytes; });

	// The packed-bit-stream portion of an audio stream must always be 
	// WORD aligned (so, at an even address offset in the ROM).  This
	// is a consequence of the structure of the DCS ADSP-2105 code,
	// which uses the ADSP-2105 circular buffer addressing mode with a
	// 2-byte length modulus as it reads back the bit stream.  A 2-byte
	// circular buffer in this mode has to be even-aligned.  To satisfy
	// this restriction, the packed-bit-stream section must start at
	// an even address offset.
	// 
	// With one exception, all of the DCS stream formats start with
	// a two-byte frame count prefix and a 16-byte header, followed by
	// the packed-bit-stream section.  Since the preamble is always 18
	// bytes, which is an even length, the packed-bit section will be
	// even-aligned if the start of the stream is even-aligned.
	// 
	// The exception is OS93a Type 1 streams, which have a two-byte
	// frame count prefix and a ONE-byte header, followed by the
	// packed bits.  Since the preamble for this type of stream has
	// an odd length (3 bytes), even-aligning the packed-bit section
	// requires that the start of the stream be ODD aligned.
	//
	// To simplify even alignment, maintain even alignment for the
	// next free byte in every chip as we allocate streams.  Start
	// off with even alignment for the first stream.
	for (auto &r : newRoms)
		r.EvenAlignFreePointer();

	// Now place the streams.  Iterate over the streams using the
	// size-ordered index we build earlier.
	pStreamIndex = streamIndex.get();
	for (size_t i = 0 ; i < streams.size() ; ++i)
	{
		// If the stream hasn't been referenced, skip it - we only
		// need to include streams that are reachable from track
		// programs.
		Stream *s = *pStreamIndex++;
		if (!s->referenced)
			continue;

		// Figure the allocation size.  This is simply the stream
		// size plus any extra padding bytes needed for alignment.
		// All stream types except OS93a Type 1 require EVEN
		// alignment, which we'll get by default, since we leave
		// the free pointer even-aligned after placing each stream.
		// OS93a Type 1 streams require ODD alignment, which means
		// we have to add one byte of padding before the stream.
		size_t sizeNeeded = s->nBytes;
		bool oddAligned = false;
		if (protoRomOSVer == DCSDecoder::OSVersion::OS93a && s->GetStreamType() == 1)
		{
			sizeNeeded += 1;
			oddAligned = false;
		}

		// search for the best fit for this stream
		uint32_t minFree = 0;
		ROMImage *bestChip = nullptr;
		for (auto &r : newRoms)
		{
			// If there's space in this ROM, and it's either the
			// first match or the best fit so far, select it.  The
			// best fit is the one with the smallest free space
			// available - we want the tightest possible fit.
			uint32_t curFree = r.BytesFree();
			if (curFree >= sizeNeeded
				&& (bestChip == nullptr || curFree < minFree))
			{
				// this is the best fit we've seen so far
				minFree = curFree;
				bestChip = &r;
			}
		}

		// If we didn't find a chip with enough free space, allocate
		// a new chip.  Allocate new chips repeatedly if necessary:
		// it might be necessary to try more than once when the "use
		// same size as prototype" option is set, because that might
		// result in varying-size ROM chips.  In that case, the first
		// one we allocate might not be the largest size available,
		// hence even if the new stream doesn't fit in the first
		// allocated chip, it might still fit in a larger chip that
		// we allocate subsequently.  This would only ever happen
		// with a stream that's between 512K and 1M in byte length,
		// so it seems unlikely, but it's certainly possible.
		while (bestChip == nullptr)
		{
			// Allocate a new chip if possible.  If that fails,
			// we're out of room, so the whole build fails.
			bestChip = AddChip(newRoms.back().chipNum + 1);
			if (bestChip == nullptr)
				return false;

			// make sure the next free byte in the new chip is
			// even-aligned
			bestChip->EvenAlignFreePointer();

			// if the new stream doesn't fit in the new chip,
			// forget it and keep looping
			if (sizeNeeded > bestChip->BytesFree())
				bestChip = nullptr;
		}

		// Note that we definitely have a place to put the stream
		// at this point.  bestChip can't be null, because we can't
		// exit the loop (other than by returning an error) until
		// it's not null.

		// We keep the free pointer even-aligned up to this point,
		// so if this stream type requires odd alignment, add a byte
		// of padding before placing the stream.
		if (oddAligned)
			bestChip->p += 1;

		// record the stream's newly assigned address
		s->romAddr.chipNo = bestChip->chipNum;
		s->romAddr.imageOffset = static_cast<uint32_t>(bestChip->p - bestChip->data.get());
		s->romAddr.linearAddr = LinearROMPointer(bestChip->p, bestChip);
			
		// Copy the stream into the chosen chip's free area
		memcpy(bestChip->p, s->data, s->nBytes);

		// consume the space, and ensure the free pointer is set
		// to even alignment for the next stream
		bestChip->p += s->nBytes;
		bestChip->EvenAlignFreePointer();
	}

	// Now we have to go back and re-compile and re-write all of the
	// track programs, since the stream storage addresses weren't
	// resolved on the first pass.  We had to do the first pass over
	// the tracks before we could assign the stream addresses, but
	// we have to do it all again now that the addresses are known.
	for (int trackNum = 0 ; trackNum < nTracks ; ++trackNum)
	{
		// get the track
		if (auto it = tracks.find(trackNum) ; it != tracks.end())
		{
			// we only have to rewrite Type 1 tracks, since those are
			// the only ones with byte code programs
			auto &track = it->second;
			if (track.type == 1)
			{
				// Recompile the track with the stream references resolved
				track.Compile(this);

				// copy the updated track into the ROM image
				memcpy(track.romImagePtr + 2, track.byteCode.data(), track.byteCode.size());
			}
		}
	}

	// Build the ROM index in the catalog.  The ROM index always
	// has 8 entries, even if there are fewer actual chips installed.
	auto curRom = newRoms.begin();
	uint8_t *pRomIndex = u2->data.get() + catalogOfs;
	const int chipSelectShift = (protoRomHWVer == DCSDecoder::HWVersion::DCS95) ? 9 : 8;
	for (int i = 0 ; i < 8 ; ++i)
	{
		if (curRom != newRoms.end())
		{
			// This ROM is populated - write its index entry

			// Calculate the checksum.  For ROM U2, the checksum is necessarily
			// set by fiat, because the checksum bytes are themselves included
			// in the checksum calculation, which makes them overconstrained.
			// This means that we have to *choose* the checksum bytes, and then
			// set a pair of unused bytes elsewhere in the ROM image to whatever
			// values they need to have to make the calculated checksum come out
			// at the chosen value.  This is fortunately very easy with the DCS
			// checksum algorithm: it's just a linear sum of all of the bytes.
			// To make the calculated sum come out as X, we simply add up all of
			// the other bytes, and then add a byte with value X - S, where S is
			// the calculated sum of all of the other bytes.  All of the DCS
			// ROMs use $0000 as the chosen-by-fiat U2 checksum, so we'll do the
			// same.
			uint16_t checksum = (i == 0) ? 0x0000 :
				DCSDecoder::ROMInfo::Checksum(curRom->data.get(), curRom->size);

			// Write the index entry:
			//   UINT16 ROM size in 4K byte units
			//   UINT16 chip select code
			//   UINT16 checksum
			WriteU16(pRomIndex, curRom->size / 4096);
			WriteU16(pRomIndex, (curRom->chipNum - 2) << chipSelectShift);
			WriteU16(pRomIndex, checksum);

			// advance to the next one
			++curRom;
		}
		else
		{
			// This ROM isn't populated - fill the index entry with zeroes
			WriteU16(pRomIndex, 0x0000);
			WriteU16(pRomIndex, 0x0000);
			WriteU16(pRomIndex, 0x0000);
		}
	}
	
	// add a zero terminator at the end of the ROM index
	WriteU16(pRomIndex, 0x0000);

	// U2 has been fully populated now, so we can finally set the
	// checksum balancer bytes to make the checksum come out at the
	// chosen-by-fiat value of zero.  First, compute a tentative
	// checksum of the U2 image as it stands now.  The balancer byte
	// slots are currently filled with placeholder zeroes, so they
	// won't contribute to the overall calculation - it's effectively
	// like excluding those bytes from the checksum.
	uint16_t checksum = DCSDecoder::ROMInfo::Checksum(u2->data.get(), u2->size);

	// Figure the required balancer bytes.  The high byte of checksum
	// if the sum of the even-numbered bytes in the ROM, and the low
	// byte is the sum of the odd-numbered bytes, both mod 0xFF.  We
	// want both checksums to come out as zero after adding the 
	// balancer bytes, so the balancer byte values are simply the
	// 8-bit 2's-complement negative of the tentative check bytes.
	int checkEven = static_cast<int>((checksum >> 8) & 0xFF);
	int checkOdd = static_cast<int>(checksum & 0xFF);
	pChecksumFixup[0] = static_cast<uint8_t>(-checkEven);
	pChecksumFixup[1] = static_cast<uint8_t>(-checkOdd);

	// 
	// Create the ZIP file
	//

	// Helper function to display a ZIP error and exit
	mz_zip_archive zw;
	auto ZipError = [&zw, outZipFile, &errorMessage](const char *activity)
	{
		// pass the error details back to the caller's error string
		const char *zipErrMsg = mz_zip_get_error_string(mz_zip_get_last_error(&zw));
		errorMessage = DCSEncoder::format(
			"Error %s ROM output file %s: %s\n", activity, outZipFile, zipErrMsg);

		// return false to indicate failure
		return false;
	};

	// open the ZIP file
	mz_zip_zero_struct(&zw);
	if (!mz_zip_writer_init_file(&zw, outZipFile, 0))
		return ZipError("opening");

	// before returning, close out the zip file structs
	std::unique_ptr<mz_zip_archive, void(*)(mz_zip_archive*)> zwHolder(&zw, [](mz_zip_archive *zw) {
		mz_zip_writer_finalize_archive(zw);
		mz_zip_writer_end(zw);
	});

	// make sure we have a non-null ROM prefix string
	if (romPrefix == nullptr)
		romPrefix = "";

	// Figure the ROM chip name designator: 'u' for the original DCS
	// board, 's' for the DCS-95 A/V board.  This doesn't really
	// matter for any technical reason; it's just a matter of the
	// conventions that Williams used for labeling the ROM chips.
	char romChipDesignator = (protoRomHWVer == DCSDecoder::HWVersion::DCS95) ? 's' : 'u';

	// write the ROM images
	char prevFilename[64] = "";
	for (auto &rom : newRoms)
	{
		// figure the filename
		char filename[64] = "";
		if (romPrefix[0] == '*')
		{
			// prefix "*" means that we should use the same name as
			// the corresponding prototype chip, if there is one
			const char *prevName = nullptr;
			for (auto &r : protoZipData)
			{
				if (r.chipNum == rom.chipNum)
				{
					// this is the chip we're looking for - copy its name
					strcpy_s(filename, r.filename.c_str());
					break;
				}
			}

			// If we didn't find a match, we'll have to synthesize a
			// name.  If we have a name from a previous chip, use that
			// as the template, otherwise just generate a name from
			// whole cloth.
			if (filename[0] == 0)
			{
				if (prevFilename[0] != 0)
				{
					// copy the previous name
					strcpy_s(filename, prevFilename);

					// Look for a digit in the name matching the previous
					// chip number.  If we find it, change it to this chip
					// number.  This will change a name like "xyz_s2.rom"
					// to "xyz_s3.rom", on the theory that the digit is
					// the chip number.  This doesn't always work, since
					// the base name sometimes contains other digits that
					// represent the version number or something else.
					// There's very little consistency in the naming.  But
					// the name is mostly for the user's benefit anyway in
					// sorting out which file is which, so it doesn't
					// matter that much if the heuristic goes wrong here.
					bool found = false;
					for (int i = 0 ; filename[i] != 0 ; ++i)
					{
						if (filename[i] == '0' + rom.chipNum - 1)
						{
							found = true;
							filename[i] += 1;
							break;
						}
					}

					// if we didn't find a digit to modify, just append
					// a designator-digit string
					if (!found)
						sprintf_s(filename, "%s.%c%d", prevFilename, romChipDesignator, rom.chipNum);
				}
				else 
				{
					// synthesize a name
					sprintf_s(filename, "snd_%c%d.rom", romChipDesignator, rom.chipNum);
				}
			}

			// Remember this generated name, so that we can use it as
			// the template for the next chip, in case it's past the
			// end of the prototype ROM set.
			strcpy_s(prevFilename, filename);
		}
		else
		{
			// generate a ROM name based on the prefix and chip number
			sprintf_s(filename, "%s%c%d.rom", romPrefix, romChipDesignator, rom.chipNum);
		}

		// remember the assigned filename
		rom.filename = filename;

		// write it out
		if (!mz_zip_writer_add_mem(&zw, filename, rom.data.get(), rom.size, MZ_DEFAULT_COMPRESSION))
			return ZipError("writing");
	}

	// copy the non-DCS files from the prototype ROM
	for (auto &f : protoZipData)
	{
		if (f.chipNum < 2
			&&!mz_zip_writer_add_mem(&zw, f.filename.c_str(), f.data.get(), f.dataSize, MZ_DEFAULT_COMPRESSION))
			return ZipError("writing");
	}

	// if desired, pass back the ROM image descriptions
	if (romList != nullptr)
	{
		// clear any old list entries
		romList->clear();

		// populate the list
		for (auto &r : newRoms)
			romList->emplace_back(ROMDesc{ r.chipNum, r.filename, r.size, r.BytesFree() });
	}

	// success
	return true;
}

DCSCompiler::Stream *DCSCompiler::FindStream(const char *name)
{
	// get the key, as the name in upper-case, for case-insensitive matching
	std::string key = name;
	std::transform(key.begin(), key.end(), key.begin(), ::toupper);

	// look it up and return the stream, if found, or null if not
	if (auto it = streamsByName.find(key); it != streamsByName.end())
		return it->second;
	else
		return nullptr;
}

void DCSCompiler::DefineVariable(const char *name, int index)
{
	if (index >= 0 && index < 255)
	{
		// get the key, as the name in upper-case, for case-insensitive matching
		std::string key = name;
		std::transform(key.begin(), key.end(), key.begin(), ::toupper);

		// add an entry for the variable
		auto &v = variables.emplace(std::piecewise_construct,
			std::forward_as_tuple(key), std::forward_as_tuple(name, index)).first->second;

		// add it to the numeric index
		varsByNumber[index] = &v;
	}
}


const DCSCompiler::Variable *DCSCompiler::FindVariable(const char *name) const
{
	// get the key as the name in upper-case, for case-insensitive matching
	std::string key = name;
	std::transform(key.begin(), key.end(), key.begin(), ::toupper);

	// look it up and return the result if found, or null if not
	if (auto it = variables.find(key) ; it != variables.end())
		return &it->second;
	else
		return nullptr;
}

const char *DCSCompiler::VariableName(int varNum) const
{
	if (varNum < 0 || varNum >= static_cast<int>(_countof(varsByNumber)))
		return "<invalid-variable-number>";
	else if (varsByNumber[varNum] == nullptr)
		return "<unnamed>";
	else
		return varsByNumber[varNum]->name.c_str();
}

DCSCompiler::DeferredIndirectTable *DCSCompiler::FindDITable(const char *name) const
{
	// get the key as the name in upper-case, for case-insensitive matching
	std::string key = name;
	std::transform(key.begin(), key.end(), key.begin(), ::toupper);

	// look it up and return the result if found, or null if not
	if (auto it = deferredIndirectTables.find(key) ; it != deferredIndirectTables.end())
		return it->second;
	else
		return nullptr;
}

const char *DCSCompiler::DITableName(int tableNum) const
{
	if (tableNum < 0 || tableNum >= static_cast<int>(_countof(diByNumber)))
		return "<invalid-table-number>";
	else if (diByNumber[tableNum] == nullptr)
		return "<unnamed>";
	else
		return diByNumber[tableNum]->name.c_str();
}

// --------------------------------------------------------------------------
//
// Streams
//

void DCSCompiler::Stream::Store(DCSEncoder::DCSAudio &dcsObj)
{
	// take ownership of the memory
	this->storage.reset(dcsObj.data.release());

	// set the data pointer to our storage object
	data = this->storage.get();

	// remember the size in bytes and frames
	this->nFrames = dcsObj.nFrames;
	this->nBytes = dcsObj.nBytes;
}

int DCSCompiler::Stream::GetStreamType() const
{
	// The stream type is given by the high bit ($80) of the first
	// header byte: Type 0 if the bit is zero, type 1 if the bit is set.
	// The header starts at the third byte of the stream (following
	// the 2-byte frame count prefix).
	return (data[2] & 0x80) != 0 ? 1 : 0;
}

int DCSCompiler::Stream::GetStreamSubType(DCSDecoder::OSVersion osver) const
{
	// Check the OS version
	if (osver == DCSDecoder::OSVersion::OS93a || osver == DCSDecoder::OSVersion::OS93b)
	{
		// The OS93a and OS93b formats don't have sub-types - return 0
		return 0;
	}
	else
	{
		// OS94 streams encode the subtype in the high bits of the second and
		// third header bytes
		return ((data[3] & 0x80) >> 6) | ((data[4] & 0x80) >> 7);
	}
}


// --------------------------------------------------------------------------
//
// Tracks
//
void DCSCompiler::Track::Compile(DCSCompiler *compiler)
{
	// discard any previous program
	byteCode.clear();

	// reserve a (somewhat oversized) guess at the space we'll need
	byteCode.reserve(steps.size() * 8);

	// add each step
	for (auto &step : steps)
	{
		// add the delay count
		byteCode.push_back(static_cast<uint8_t>((step.wait >> 8) & 0xFF));
		byteCode.push_back(static_cast<uint8_t>(step.wait & 0xFF));

		// add the opcode
		byteCode.push_back(step.opcode);

		// resolve operands
		switch (step.opcode)
		{
		case 0x01:
			// Play Stream - if there's a stream, fix up the address
			if (step.stream != nullptr)
			{
				// store the 24-bit linear address of the stream pointer 
				// in the operand bytes
				auto addr = step.stream->romAddr.linearAddr;
				step.operandBytes[1] = static_cast<uint8_t>((addr >> 16) & 0xFF);
				step.operandBytes[2] = static_cast<uint8_t>((addr >> 8) & 0xFF);
				step.operandBytes[3] = static_cast<uint8_t>(addr & 0xFF);

				// mark the stream as referenced
				step.stream->referenced = true;
			}
			break;
		}

		// add the operand bytes
		for (int i = 0 ; i < step.nOperandBytes ; ++i)
			byteCode.push_back(step.operandBytes[i]);
	}
}

void DCSCompiler::Track::ResolveRefs(DCSCompiler *compiler, DCSTokenizer &tokenizer)
{
	// scan the program steps
	for (auto &step : steps)
	{
		switch (step.opcode)
		{
		case 0x01:
			// Play Stream - resolve stream name references
			if (step.streamName.size() != 0 && step.stream == nullptr)
			{
				// look up the stream by name
				step.stream = compiler->FindStream(step.streamName.c_str());

				// if we couldn't find it, log an error
				if (step.stream == nullptr)
				{
					tokenizer.Error(DCSTokenizer::ErrorLevel::Error, step.refLoc,
						"Stream '%s' is not defined", step.streamName.c_str());
				}
			}
			break;

		case 0x06:
			// Store variable.  Check that this variable value is within
			// the bounds of all defererd indirect tables indexed with the
			// variable.  The association between table and variable is
			// formed by Type 3 tracks, which are entirely composed of
			// Table[Variable] references.
			//
			// This instruction is a no-op for 1993 games.
			if (compiler->protoRomOSVer != DCSDecoder::OSVersion::OS93a
				&& compiler->protoRomOSVer != DCSDecoder::OSVersion::OS93b)
			{
				for (auto &t : compiler->tracks)
				{
					Track *track = &t.second;
					if (track->type == 3)
					{
						// get the table number and variable number from the deferral code
						int tableNum = (track->deferredTrack & 0xFF);
						int varNum = ((track->deferredTrack >> 8) & 0xFF);

						// check to see if it's the same variable number (first operand byte)
						if (varNum == step.operandBytes[0])
						{
							// It's the same variable, so this track can index the target
							// table by the variable value set by this opcode (the second
							// operand byte).  Check that this index is valid for the table.
							int varVal = step.operandBytes[1];
							DeferredIndirectTable *table = compiler->diByNumber[tableNum].get();
							int maxIndex = table != nullptr ? static_cast<int>(table->trackNumbers.size()) : -1;
							if (table == nullptr || varVal >= maxIndex)
							{
								tokenizer.Error(DCSTokenizer::EError, step.refLoc,
									"Track $%04x references deferred indirect table %d (%s) entry [%d] through variable %d (%s); "
									"the maximum index for this table is %d",
									track->trackNo, tableNum, compiler->DITableName(tableNum),
									varVal, varNum, compiler->VariableName(varNum), maxIndex);
							}
						}
					}
				}
			}
			break;
		}
	}
}
