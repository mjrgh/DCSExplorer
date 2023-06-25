// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder - main program entrypoint
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <string>
#include <list>
#include <vector>
#include <unordered_map>
#include <regex>
#include <memory>
#include "DCSCompiler.h"
#include "OSSpecific.h"
#include "../Utilities/BuildDate.h"


//
// Msin program entrypoint
//
int main(int argc, char **argv)
{
	// OS-specific setup
	OSInit();

	// set up a compiler
	DCSCompiler compiler;

	// parse options
	int argi = 1;
	const char *outFile = nullptr;
	uint32_t romSize = 1024*1024;
	bool patchMode = false;
	bool quietMode = false;
	const char *romPrefix = nullptr;
	for (; argi < argc && argv[argi][0] == '-' ; ++argi)
	{
		const char *argp = argv[argi];
		if (strcmp(argp, "--") == 0)
		{
			// explicit last option
			break;
		}
		else if (strcmp(argp, "-o") == 0 && argi + 1 < argc)
		{
			// set the output file
			outFile = argv[++argi];
		}
		else if (strcmp(argp, "-q") == 0)
		{
			// quiet mode
			quietMode = true;
		}
		else if (strcmp(argp, "--patch") == 0)
		{
			// enable patch mode
			patchMode = true;
		}
		else if (strncmp(argp, "--rom-size=", 11) == 0)
		{
			// set the ROM size - must be 512K, 1M, or '*' for 'same as proto'
			if (strcmp(&argp[11], "*") == 0)
			{
				// "*" -> same as proto
				romSize = DCSCompiler::ROMSIZE_SAME_AS_PROTO;
			}
			else
			{
				// parse a numeric size, with an optional 'K' or 'M' suffix
				romSize = atoi(argp + 11);
				for (const char *p = argp + 11 ; *p != 0 ; ++p)
				{
					if (*p == 'k' || *p == 'K')
					{
						romSize *= 1024;
						break;
					}
					else if (*p == 'm' || *p == 'M')
					{
						romSize *= 1024*1024;
						break;
					}
					else if (!isdigit(*p))
						break;
				}

				if (!(romSize == 512*1024 || romSize == 1024*1024))
				{
					printf("Invalid ROM size specified (%d) - must be 512k or 1M\n", romSize);
					exit(1);
				}
			}
		}
		else if (strncmp(argp, "--rom-prefix=", 13) == 0)
		{
			// set the ROM prefix
			romPrefix = argp + 13;
		}
		else if (strncmp(argp, "--stream-dir=", 13) == 0)
		{
			// add the stream directory prefix
			compiler.streamFilePaths.emplace_back(argp + 13);
		}
		else
		{
			// unrecognized option - consume remaining arguments
			argi = argc;
			break;
		}
	}

	// check usage
	if (argi + 2 != argc)
	{
		ProgramBuildDate buildDate;
		printf(
			"DCS Encoder   Version 1.0, Build %s\n"
			"(c)%s Michael J Roberts / BSD 3-clause license / NO WARRANTY\n"
			"\n"
			"Usage: dcsencoder [options] <prototypeRom> <scriptFile>\n"
			"\n"
			"Reads a ROM definitions script file, which contains a script that defines\n"
			"the contents of a new DCS ROM set, and generates a ROM file set according\n"
			"to the script's instructions.  The prototype ROM is a PinMame ROM set (as\n"
			"a .zip file) that serves as the prototype for the new ROM set.  The ADSP-2105\n"
			"program code and decoder data structures are copied from the prototype ROM.\n"
			"The prototype ROM determines the hardware platform (original DCS or DCS-95\n"
			"A/V) that the target ROM will run on, so you must choose a prototype ROM\n"
			"that runs on the same hardware platform you're targeting for the new ROM.\n"
			"\n"
			"Options:\n"
			"   -o <file>            set the output file name (default is <romDefFile>.zip)\n"
			"   -q                   quiet mode (suppress updates on stream encoding)\n"
			"   --patch              patch mode (copies all tracks and streams from the prototype ROM)\n"
			"   --rom-prefix=<x>     set the prefix string for the sound files generated in the zip\n"
			"   --rom-size=<size>    set the ROM size to x bytes; valid sizes are 512K and 1M (default),\n"
			"                        or * to use the same sizes as the corresponding prototype ROMs\n"
			"   --stream-dir=<dir>   search in directory <dir> when looking for a stream file that's\n"
			"                        not found in the current directory (this can be specified any\n"
			"                        number of times, to search in multiple locations)\n",
			buildDate.YYYYMMDD().c_str(), buildDate.CopyrightYears(2023).c_str());
		exit(1);
	}

	// get the prototype ROM set
	const char *protoRomFile = argv[argi++];

	// get the ROM definer file name
	const char *romDefFile = argv[argi++];

	// apply a default output file name, if the -o option wasn't specified
	std::string outFileBuf;
	if (outFile == nullptr)
	{
		// use the track file as a template, replacing or adding a default extension
		outFileBuf = std::regex_replace(romDefFile, std::regex("\\.[^./\\\\:]+$"), "") + ".zip";
		outFile = outFileBuf.c_str();
	}

	// string to receive error message text from various interfaces we call
	std::string errMsg;

	// Show progress if we're not in quiet mode
	if (!quietMode)
		printf("Loading prototype ROM set from %s\n", protoRomFile);

		// load the prototype ROM set
	if (!compiler.LoadPrototypeROM(protoRomFile, patchMode, errMsg))
	{
		printf("Error loading prototype ROM file %s: %s\n", protoRomFile, errMsg.c_str());
		exit(2);
	}

	// Show progress
	if (!quietMode)
		printf("Compiling script from %s\n", romDefFile);
	
	// parse the script
	class Logger : public DCSTokenizer::ErrorLogger 
	{
	public:
		Logger(bool quietMode) : quietMode(quietMode) { }
		bool quietMode;

		void Status(const char *msg, bool pending)
		{
			// suppress status messages in quiet mode
			if (!quietMode)
				__super::Status(msg, pending);
		}
	};
	Logger logger(quietMode);
	compiler.ParseScript(romDefFile, logger);

	// stop if there were errors
	if (logger.errors != 0 || logger.fatal != 0)
	{
		int nErrors = logger.errors + logger.fatal;
		printf("\nScript compilation failed with %d error%s, %d warning%s\n"
			"No ROM images generated\n", 
			nErrors, nErrors != 1 ? "s" : "",
			logger.warnings, logger.warnings != 1 ? "s" : "");
		exit(2);
	}
	
	// note warnings
	if (logger.warnings != 0)
	{
		printf("\nScript compilation succeeded, but note that %d warning%s generated\n",
			logger.warnings, logger.warnings == 1 ? " was" : "s were");
	}

	// Set the default ROM prefix, if the user didn't specify one.  If we're
	// in patch mode, copy the original filenames by default; otherwise use
	// "snd_".
	if (romPrefix == nullptr)
		romPrefix = patchMode ? "*" : "snd_";

	// show progress
	if (!quietMode)
		printf("\nGenerating new ROM set to %s\n", outFile);

	// generate the ROMs
	std::list<DCSCompiler::ROMDesc> romList;
	if (compiler.GenerateROM(outFile, romSize, romPrefix, errMsg, &romList))
	{
		// success
		printf("ROM .zip file layout:\n"
			"Chip  Size   Bytes Used  Unused  Filename\n");
		for (auto &r : romList)
		{
			printf("U%d    %-6s %-10d  %-6d  %s\n",
				r.chipNum,
				r.size == 512*1024 ? "512k" : r.size == 1024*1024 ? "1M" : DCSEncoder::format("%dK", r.size/1024).c_str(),
				r.size - r.bytesFree, r.bytesFree, r.filename.c_str());
		}
		printf("\nROM creation succeeded\n");
	}
	else
	{
		printf("ROM creation failed: %s\n", errMsg.c_str());
		exit(2);
	}

	// exit with success status
	return 0;
}
