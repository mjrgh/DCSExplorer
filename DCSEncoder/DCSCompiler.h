// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Compiler
//
// This class is a utility for creating new DCS ROMs.  It lets you
// construct and manipulate an in-memory representation of the contents 
// of a ROM set, including the prototype ADSP-2015 program, audio track
// programs, audio streams, and deferred-indirect table structures, and
// it can then compile the in-memory representation into a set of ROM
// images, packaged in the PinMame .zip format.
// 
// This class doesn't provide a way to create new ADSP-2105 programming
// code for the ROM control program.  Instead, you provide an existing
// DCS ROM set, from any DCS title that used the same version of the
// audio boards that you intend to target, as the source of the program
// code.  The ROM builder reads the provided ROM and transfers the main
// program code to the new ROM set.  In principle, you could create your
// own ADSP-2105 program using a separate set of development tools for
// that processor, as long as your program followed the same conventions
// as the original DCS programs in terms of the ROM data format.
// 
// In addition to trasnferring the ADSP-2105 control program from an
// existing ROM set, the DCS Compiler class can also transfer the entire
// data contents of an existing ROM set, to use as the basis of the new
// ROM.  This lets you "patch" an existing ROM - replacing some of the
// sound clips while keeping others, for example.
// 
// The generated ROM set replicates the layout of the original DCS ROMs,
// so it's suitable for use with PinMame or for burning into physical
// PROM chips for installation in a physical DCS board.  

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "DCSTokenizer.h"
#include "DCSEncoder.h"
#include "../DCSDecoder/DCSDecoderNative.h"


class DCSCompiler
{
public:
	DCSCompiler();
	~DCSCompiler();

	// ROM address type.  This represents the ROM address for an in-memory
	// object, such as a stream or channel program.  ROM addresses are
	// usually indeterminate until completing the compilation process, 
	// which arranges objects within the ROM image address space and
	// assigns everything a final address.
	struct ROMAddr
	{
		// ROM chip number
		int chipNo = 0;

		// offset within the in-memory ROM image
		uint32_t imageOffset = 0;

		// Linear 24-bit ROM address in hardware format, for storage in the ROM image.
		// This uses the bit encoding format appropriate for the version of the hardware
		// platform we're targeting.
		uint32_t linearAddr = 0;
	};

	// Load the prototype ROM set.  This is the first step in compiling a
	// new ROM, and it should be completed before creating any new objects
	// (tracks, streams, etc) for the new ROM set.  The prototype ROM
	// provides the ADSP-2105 program that's trasnfered to the new ROM as
	// its main control program, so it determines the target hardware type
	// that the new ROM set will run on.
	// 
	// If 'patchMode' is true, all of the data from the prototype ROM will
	// be loaded along with the control program - track programs, audio
	// streams, etc.  This lets the old ROM serve as the starting point for
	// the new ROM.  The caller can then create new objects and replace
	// existing ones before generating the new ROM set, to create a
	// modified version of the original ROM.  This makes it possible to
	// make isolated changes in an existing game's ROM, such as replacing
	// audio clips or modifying track program steps.
	//
	// The prototype ROM must satisfy two rules.  First, it must match the
	// target hardware type - the platform that the new ROM will run on.
	// There are only two hardware types: the original DCS audio boards,
	// used in machines from 1993 to 1995, and the DCS-95 audio/video boards,
	// used from 1995 to 1998.  The ADSP-2105 control software for the two
	// boards is mutually incompatible, so the new ROM set will only run on
	// the same hardware that the prototype ROM set runs on.  Second, the
	// prototype ROM must NOT be from any of the first three DCS games from
	// 1993 (Star Trek: The Next Generation, Indiana Jones: The Pinball
	// Adventure, Judge Dredd).  The software for those games used a
	// different audio stream format from the later software, and this
	// project can only encode streams for the later format.
	bool LoadPrototypeROM(const char *romZipFile, bool patchMode, 
		std::string &errorMessage);

	// Parse a script file
	void ParseScript(const char *filename, DCSTokenizer::ErrorLogger &logger);

	// Generate the new ROM set.  This creates the ROM images based on
	// the current in-memory data structures, and writes them to the
	// specified .zip file.  All of the non-DCS files from the prototype
	// ROM set are copied into the new .zip file, so that the result can
	// be used as a replacement for the original ROM set in PinMame or
	// other programs that take PinMame ROM sets.
	// 
	// When patching a ROM set that you plan to use with PinMame, set
	// romSize to ROMSIZE_SAME_AS_PROTO and romPrefix to "*".
	//
	// 'romSize' specifies the size of the individual ROM chips, in
	// bytes.  To generate ROM images compatible with physical DCS
	// boards, ROMs must be either 512K or 1M, since the boards are
	// wired to accept only those two sizes.  The two sizes can be
	// freely mixed within a game.
	// 
	// You can also pass the magic value ROMSIZE_SAME_AS_PROTO for
	// romSize, which sets the size of each chip individually to the
	// size of the corresponding chip in the prototype ROM set.  This
	// is useful primarily if you plan to use a patched ROM set with
	// PinMame, because PinMame has a hard-coded list of known ROMs,
	// including the size of each chip, and it will only load the size
	// hard-coded into its list.
	// 
	// 'romPrefix' is a string that's used as a prefix for the ROM names
	// in the generated zip file.  The ROm files are named using the
	// pattern
	// 
	//    <prefix><u><n>.rom
	//
	// where <u> is the letter 'u' for a DCS-93 hardware target or 's'
	// for DCS_95 hardware, and <n> is the ROM chip number, 2-9.  So if
	// the prefix is "mm_", the ROM names would look like "mm_s2.rom".
	// 
	// If 'romPrefix' is "*", the generator will reuse the same names
	// for the corresponding chips from the prototype ROM.  This is
	// useful for patching a ROM set for use with PinMame, because
	// PinMame's hard-coded list of known ROMs includes the filenames
	// of the zip entries.
	//
	// If romList is non-null, it's cleared and populated with entries
	// describing the newly created ROMs on success.  This is purely
	// for information purposes, to tell the caller what's in the new
	// .zip file, mostly so that the caller can display it to the user.
	// THe list pointer can be null if the caller doesn't need the
	// information.
	static const uint32_t ROMSIZE_SAME_AS_PROTO = 0xFFFFFFFF;
	struct ROMDesc
	{
		int chipNum;           // nominal ROM chip number (2-9)
		std::string filename;  // name of file within the .zip archive
		uint32_t size;         // uncompressed ROM image size in bytes
		uint32_t bytesFree;    // number of bytes unused in ROM image space
	};
	bool GenerateROM(const char *outZipFile,
		uint32_t romSize, const char *romPrefix,
		std::string &errorMessage, std::list<ROMDesc> *romList);

	// Audio stream list entry
	struct Stream
	{
		Stream(const char *filename) : filename(filename) { }
		Stream(uint32_t protoAddr) : protoAddr(protoAddr) { }

		// Get the stream format type and subtype
		int GetStreamType() const;
		int GetStreamSubType(DCSDecoder::OSVersion osver) const;

		// Is this stream referenced from a track program?  Before we generate
		// the final ROM images, we'll go through all of the track programs and
		// mark all of the streams they reference.  Any streams that aren't
		// mentioned in track programs can be omitted from the final ROM set
		bool referenced = false;

		// Prototype ROM address, as a 24-bit linear ROM pointer, for streams
		// imported from the prototype ROM.  If a stream is imported and then
		// replaced, this will bset to zero to indicate that it's no longer an
		// imported stream.
		uint32_t protoAddr = 0;

		// descriptive name, for error messages
		std::string refName;

		// source file name, for streams imported from 
		std::string filename;

		// ROM image address.  This is assigned during the compilation process
		// when we determine where this stream will go in the final ROM layout.
		ROMAddr romAddr;

		// Store stream data.  This takes ownership of an allocated
		// stream object.
		void Store(DCSEncoder::DCSAudio &dcsObj);

		// Stream data.  This points to the internal storage object for
		// a stream created by transcoding an external file, or to the 
		// stream data in the prototype ROM if the stream came from
		// the prototype ROM.
		const uint8_t *data = nullptr;

		// number of DCS frames in the stream
		uint16_t nFrames = 0;

		// DCS binary stream data size in bytesp
		size_t nBytes = 0;

		// Storage for the stream data.
		std::unique_ptr<uint8_t> storage;
	};

	// Look up a stream by name.  This is a case-insensitive search for
	// the stream name.
	Stream *FindStream(const char *name);

	// Anonymous stream ID number.  This is a serial number used to assign
	// a unique name to each unnamed stream created via a LOAD() program
	// step that directly loads the stream from an external file.  A stream
	// created this way doesn't have a symbolic name within the script, but
	// it still needs a unique name to serve as a key for the stream map.
	// We include the sequential ID number in the name as a way to keep the
	// names unique.
	int anonStreamID = 0;

	// Track list entry
	struct Track
	{
		// does the track come from the script or from the old ROM?
		bool fromRom = false;

		// track number
		int trackNo = 0;

		// channel number
		int channel = 0;

		// Track type: 1 = play track, 2 = deferred, 3 = deferred indirect.
		// Deferred tracks don't have programs; they just have another track
		// number to queue up for deferred playback, which can be triggered
		// from another track via opcode 0x05.
		int type = 0;

		// deferred track number; only applicable to track types 2 and 3
		int deferredTrack = -1;

		// ROM image location.  This is assigned on the first pass through
		// the tracks during the compilation process, when we arrange the
		// tracks in the ROM image address space.  We use this on the second
		// pass, after resolving stream references, to re-generate the byte
		// code at the same location with all of the references fixed up.
		uint8_t *romImagePtr = nullptr;

		// program step
		struct ProgramStep
		{
			// wait time, in frames
			uint16_t wait = 0;

			// opcode
			uint8_t opcode = 0;

			// operand bytes
			int nOperandBytes = 0;
			uint8_t operandBytes[8]{ 0, 0, 0, 0, 0, 0, 0, 0 };

			// add operands
			void AddOpByte(uint32_t b) { operandBytes[nOperandBytes++] = static_cast<uint8_t>(b & 0xff); }
			void AddOpWord(uint32_t w)
			{
				AddOpByte(w >> 8);
				AddOpByte(w);
			}
			void AddOpPtr(uint32_t p)
			{
				AddOpByte(p >> 16);
				AddOpByte(p >> 8);
				AddOpByte(p);
			}

			// Stream pointer operand, if any.  This stores the pointer to
			// our internal Stream object for the referenced stream.
			Stream *stream = nullptr;

			// Stream reference, by name.  We allow tracks to reference
			// streams before they're defined, so we have to store the
			// name of the stream until the final fixup pass, when we
			// resolve the names to their stream objects.
			std::string streamName;

			// Reference location.  Opcodes that contain references that
			// need to be fixed up in the ResolveRefs() phase store the
			// tokenizer location here during the parsing pass, so that
			// any errors reported from ResolveRefs() can be shown at the
			// original source code location.  The ResolveRefs() pass is
			// run after parsing the whole file, so if we just reported
			// the "current" location during that pass, it would look
			// like all of the errors were at the end of the file, which
			// isn't very helpful in fixing them.
			DCSTokenizer::Location refLoc;
		};

		// program steps
		std::list<ProgramStep> steps;

		// Compile the program steps into the byte-code program
		void Compile(DCSCompiler *compiler);

		// Resolve references.  Scans the program steps; resolves
		// stream name references to Stream objects, and bounds-checks
		// deferred indirect table references.
		void ResolveRefs(DCSCompiler *compiler, DCSTokenizer &tokenizer);

		// compiled program byte code
		std::vector<uint8_t> byteCode;
	};

	// Deferred Indirect table.  A Type 3 track contains a deferral code
	// with two 8-bit values: the ID of one of these tables, and the ID
	// of an opcode 0x06 variable.  Opcode 0x05 triggers a pre-loaded
	// Type 3 track.  When opcode 0x05 is executed, it gets the table
	// ID and variable ID from the pre-loaded Type 3 track's deferral
	// code, gets the current value of the variable, and uses that as
	// an index into the table.  The table itself contains track 
	// numbers.  Opcode 0x05 activates the track selected via the
	// three-step table lookup.
	struct DeferredIndirectTable
	{
		DeferredIndirectTable(const char *name, int index) : index(index) { }

		// scripting name of the table; tables imported from the ROM are
		// assigned synthetic names based on the index, purely for display
		// purposes
		std::string name;

		// table index (this is the index in the ROM table reference array)
		int index;

		// Was this loaded from the prototype ROM?  If so, the script can
		// replace it without generating an error or warning.
		bool fromProto = false;

		// track numbers making up the table
		std::vector<uint16_t> trackNumbers;
	};

	// Opcode 0x06 variable.  These are used in Deferred Indirect
	// track loading.
	struct Variable
	{
		Variable(const char *name, uint8_t id) : name(name), id(id) { }

		// scripting name of the variable
		std::string name;

		// variable ID - this is the index in the runtime array of
		// opcode 6 variables
		uint8_t id;

		// highest value assigned
		int maxVal = -1;
	};

	// define a variable
	void DefineVariable(const char *name, int index);

	// look up a variable by name, case-insensitive
	const Variable *FindVariable(const char *name) const;

	// get the name of a variable by number
	const char *VariableName(int varNum) const;

	// Parse compression parameters
	void ParseCompressionParams(DCSTokenizer &tokenizer, DCSEncoder::CompressionParams &params);

	// Encode a file
	Stream *EncodeFile(Stream *replaces, 
		const char *symbolicName, const char *file, 
		const DCSEncoder::CompressionParams &params, DCSTokenizer &tokenizer);

	// DCS Decoder, containing the prototype ROM set
	DCSDecoder::MinHost decoderHostIfc;
	DCSDecoderNative decoder;

	// prototype ROM zip file contents
	std::list<DCSDecoder::ZipFileData> protoZipData;

	// Hardware platform and software version of prototype ROM software
	std::string protoRomVer;
	DCSDecoder::HWVersion protoRomHWVer = DCSDecoder::HWVersion::Unknown;
	DCSDecoder::OSVersion protoRomOSVer = DCSDecoder::OSVersion::Unknown;

	// number of channels supported by the prototype firmware
	int numChannels = 0;
	int maxChannelNumber = 0;

	// U2 ROM signature specified in the script, if any.  If this is
	// populated (with a non-empty string), it's used as the signature
	// in the generated U2 ROM image, replacing the signature from the
	// prototype U2.  If this isn't specified, the prototype signature
	// is copied into the new ROM.  Note that the length limit of 75
	// characters (plus trailing null) is deliberate: this is the limit
	// imposed by the layout of the original ADSP-2105 programs found
	// in the DCS ROMs.
	char signature[76] = "";

	// Date strings in various formats, for substitution parameters
	// in script text
	char dateStr[32];
	char shortDateStr[32];
	char longDateStr[32];
	char medDateStr[32];

	// DCS audio encoder, for encoding new audio clips
	DCSEncoder encoder;

	// Default compression parame for the encoder
	DCSEncoder::CompressionParams defaultCompressionParams;

	// track list
	std::unordered_map<int, Track> tracks;

	// highest track number used
	uint16_t maxTrackNumber = 0;

	// All stream objects.  This includes streams imported from the prototype
	// ROM and new streams created in the script by transcoding external audio
	// files.  This list is just the storage list, for managing the memory
	// used by the streams.  The streams are accessed from scripts via the
	// name and address indices.
	std::list<Stream> streams;

	// Streams by scripting name
	std::unordered_map<std::string, Stream*> streamsByName;

	// Streams by prototype ROM address.  This indexes streams imported
	// from the prototype ROM by their 24-bit linear ROM addresses.
	std::unordered_map<uint32_t, Stream*> streamsByProtoAddr;

	// Stream file search path.  This is a list of path prefixes to search
	// for stream files.
	std::list<std::string> streamFilePaths;

	// find a deferred indirect table by name
	DeferredIndirectTable *FindDITable(const char *name) const;

	// get the name of a deferred indirect table by number
	const char *DITableName(int tableNum) const;

	// Deferred-indirect tables, by name and number.  The numbered list
	// contains the objects, and the by-name map is just an index into it.
	std::unordered_map<std::string, DeferredIndirectTable*> deferredIndirectTables;
	std::unique_ptr<DeferredIndirectTable> diByNumber[256];

	// Opcode 6 variables, mapped by name
	std::unordered_map<std::string, Variable> variables;

	// Opcode 6 variables, indexed by variable number.  These are pointers
	// into the 'variables' map.
	Variable *varsByNumber[256];
};


