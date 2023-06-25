// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCSTokenizer - a simple text file tokenizer for the DCS script compiler
//
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <string>
#include "DCSTokenizer.h"
#include "DCSEncoder.h"
#include "DCSCompiler.h"


void DCSTokenizer::ErrorLogger::Log(Level level, const char *where, const char *msg)
{
	// format the message to stdout
	if (where != nullptr && where[0] != 0)
		printf("%s: %s: %s\n", where, LevelName(level), msg);
	else
		printf("%s: %s\n", LevelName(level), msg);

	// count it
	if (level == Level::Warning)
		++warnings;
	else if (level == Level::Fatal)
		++fatal, ++errors;
	else if (level == Level::Error)
		++errors;
}

void DCSTokenizer::ErrorLogger::Status(const char *msg, bool pending)
{
	printf("%s%s", msg, pending ? "" : "\n");
}

void DCSTokenizer::Error(ErrorLevel level, const char *msg, ...)
{
	// format the location
	auto where = DCSEncoder::format("%s(%d)", filename.c_str(), lineNum);

	// format the message
	va_list va;
	va_start(va, msg);
	auto formatted = DCSEncoder::vformat(msg, va);
	va_end(va);

	// log it
	logger.Log(level, where.c_str(), formatted.c_str());
}

void DCSTokenizer::Error(ErrorLevel level, const Location &location, const char *msg, ...)
{
	// format the location
	auto where = DCSEncoder::format("%s(%d)", location.filename.c_str(), location.lineNum);

	// format the message
	va_list va;
	va_start(va, msg);
	auto formatted = DCSEncoder::vformat(msg, va);
	va_end(va);

	// log it
	logger.Log(level, where.c_str(), formatted.c_str());
}

void DCSTokenizer::SkipStatement()
{
	for (;;)
	{
		// stop at end of file
		if (p == endp)
			return;

		// stop at ';' or '}'
		if (CheckPunct(";") || CheckPunct("}"))
			return;

		// skip the next token
		Read();
	}
}

void DCSTokenizer::EndStatement()
{
	Token tok = Read();
	if (tok.type != TokType::Punct || tok.text != ";")
	{
		Error(EError, "Expected ';' at end of statement, but found \"%s\"; skipping to next ';'", tok.text.c_str());
		SkipStatement();
	}
}

// load a file
bool DCSTokenizer::LoadFile(const char *filename, std::string &errorMessage)
{
	// open the file
	FILE *fp = nullptr;
	if (fopen_s(&fp, filename, "r") != 0 || fp == nullptr)
	{
		errorMessage = DCSEncoder::format("Unable to open file \"%s\" (error %d)", filename, errno);
		return false;
	}

	// get the file size
	long len;
	if (fseek(fp, 0, SEEK_END) < 0
		|| (len = ftell(fp)) < 0
		|| fseek(fp, 0, SEEK_SET) < 0)
	{
		errorMessage = DCSEncoder::format("Error getting file size for \"%s\" (error %d)", filename, errno);
		return false;
	}

	// make sure we close the file before returning
	std::unique_ptr<FILE, int(*)(FILE*)> fpHolder(fp, &fclose);

	// allocate space
	contents.reset(new (std::nothrow) char[len]);
	if (contents == nullptr)
	{
		errorMessage = DCSEncoder::format("Unable to allocate memory (%ld bytes) to load file \"%s\"", len, filename);
		return false;
	}

	// set up the read pointer at the beginning of the buffer
	p = endp = contents.get();

	// Load the contents.  Note that the read length might be less than the
	// file's on-disk size due to Windows newline translations, which replace
	// \r\n sequences in the on-disk file with \n characters in the memory
	// representation.  fseek()/ftell() should take this into account with
	// the MSVC libraries, but it seems better not to count on that.  The
	// final count returned from fread() should always be reliable.  We're
	// not allowing for any expansion in fread() translations, but as far
	// as I know, no expanding translations exist.
	contentLength = fread(contents.get(), 1, len, fp);
	if (contentLength < 0)
	{
		errorMessage = DCSEncoder::format("Error reading file \"%s\" (error %d)", filename, errno);
		return false;
	}

	// set the end pointer
	endp = p + contentLength;

	// remember the filename
	this->filename = filename;

	// success
	return true;
}

DCSTokenizer::Token DCSTokenizer::ReadSymbol()
{
	Token tok = Read();
	if (tok.type == TokType::Symbol)
		return tok;
	else
	{
		Error(EWarning, "Expected symbol name, found \"%s\"", tok.text.c_str());
		return Token(TokType::Symbol);
	}
}

DCSTokenizer::Token DCSTokenizer::ReadString()
{
	Token tok = Read();
	if (tok.type == TokType::String)
		return tok;
	else
	{
		Error(EWarning, "Expected string, found \"%s\"", tok.text.c_str());
		return Token(TokType::String);
	}
}

DCSTokenizer::Token DCSTokenizer::ReadInt()
{
	Token tok = Read();
	if (tok.type == TokType::Int)
		return tok;
	else if (tok.type == TokType::Float)
	{
		tok.type = TokType::Int;
		tok.ival = static_cast<int>(roundf(tok.fval));
		Error(EError, "Floating-point value %s rounded to integer (%d)", tok.text.c_str(), tok.ival);
		return tok;
	}
	else
	{
		Error(EWarning, "Expected integer value, found \"%s\"", tok.text.c_str());
		return Token(TokType::Int);
	}
}

DCSTokenizer::Token DCSTokenizer::ReadUInt8()
{
	Token tok = ReadInt();
	if (tok.type == TokType::Int && (tok.ival < 0 || tok.ival > 255))
		Error(EError, "Value %d ($%08x) out of range for UINT8 value (must be 0 to 255)", tok.ival, tok.ival);
	return tok;
}

DCSTokenizer::Token DCSTokenizer::ReadUInt16()
{
	Token tok = ReadInt();
	if (tok.type == TokType::Int && (tok.ival < 0 || tok.ival > 65535))
		Error(EError, "Value %d ($%08x) out of range for UINT16 value (must be 0 to 65535)", tok.ival, tok.ival);
	return tok;
}

DCSTokenizer::Token DCSTokenizer::ReadFloat()
{
	Token tok = Read();
	if (tok.type == TokType::Float)
		return tok;
	else if (tok.type == TokType::Int)
	{
		tok.type = TokType::Float;
		tok.fval = static_cast<float>(tok.ival);
		return tok;
	}
	else
	{
		Error(EWarning, "Expected floating-point value, found \"%s\"", tok.text.c_str());
		return Token(TokType::Float);
	}
}

bool DCSTokenizer::CheckPunct(const char *s)
{
	// remember where we started, in case we decide to un-get the token
	auto state = Save();

	// read the token and check for a match
	Token tok = Read();
	if (tok.type == TokType::Punct && tok.text == s)
	{
		// it's a match - keep the token
		return true;
	}
	else
	{
		// no match - unget the token
		Restore(state);
		return false;
	}
}

bool DCSTokenizer::RequirePunct(const char *s)
{
	// remember where we started, in case we decide to un-get the token
	auto state = Save();

	// read the token and check for a match
	Token tok = Read();
	if (tok.type == TokType::Punct && tok.text == s)
	{
		return true;
	}
	else
	{
		// no match - un-read the token and log the error
		Restore(state);
		Error(EError, "Expected \"%s\", found \"%s\"", s, tok.text.c_str());
		return false;
	}
}

bool DCSTokenizer::RequireSymbol(const char *s)
{
	// remember where we started, in case we decide to un-get the token
	auto state = Save();

	// read the token and check for a match
	Token tok = Read();
	if (tok.type == TokType::Symbol && _stricmp(tok.text.c_str(), s) == 0)
	{
		return true;
	}
	else
	{
		// no match - un-read the token and log the error
		Restore(state);
		Error(EError, "Expected \"%s\", found \"%s\"", s, tok.text.c_str());
		return false;
	}
}

bool DCSTokenizer::CheckKeyword(const char *s)
{
	// remember where we started, in case we decide to un-get the token
	auto state = Save();

	// read the token and check for a match
	Token tok = Read();
	if (tok.type == TokType::Symbol && _stricmp(tok.text.c_str(), s) == 0)
	{
		// it's a match - keep the token
		return true;
	}
	else
	{
		// no match - unget the token
		Restore(state);
		return false;
	}
}

// Get the next token
DCSTokenizer::Token DCSTokenizer::Read()
{
	// keep going until we find something to return
	for (;;)
	{
		// skip spaces
		for ( ; p < endp && isspace(*p) ; ++p)
		{
			if (*p == '\n')
				++lineNum;
		}

		// check what we have
		const char *start = p;
		if (p == endp)
		{
			// end of input
			return Token(TokType::End);
		}
		else if (p + 1 < endp && p[0] == '/' && p[1] == '/')
		{
			// C-style "//" comment - scan to the end of the line
			while (p < endp && *p != '\n')
				++p;

			// skip the newline
			if (p < endp && *p == '\n')
			{
				++p;
				++lineNum;
			}
		}
		else if (*p == '"')
		{
			// string
			Token tok(TokType::String);
			for (++p, ++start ; p < endp ; ++p)
			{
				// check for quotes
				if (*p == '"')
				{
					// check for stuttered quotes
					if (p + 1 < endp && p[1] == '"')
					{
						// stuttered string - copy the part from start to the first quote
						++p;
						tok.text.append(start, p - start);

						// start over after the second quote
						start = p + 1;
					}
					else
					{
						// end of the string
						break;
					}
				}
			}

			// skip the close quote
			const char *closequote = p;
			if (p < endp && *p == '"')
				++p;

			// append the rest of the string
			if (closequote > start)
				tok.text.append(start, closequote - start);

			// done
			return tok;
		}
		else if (isdigit(*p) || ((*p == '-' || *p == '+') && p + 1 < endp && (isdigit(p[1]) || p[1] == '.')))
		{
			// number

			// skip the sign, if present
			int sign = 1;
			if (*p == '-')
				sign = -1, ++p;
			else if (*p == '+')
				++p;

			// Figure the radix:  0x... is hex, otherwise it's decimal.  
			// 
			// Note that we deliberately don't use C-style octal notation
			// with a leading zero.  No one has used octal since the 1970s,
			// and as a result most people who aren't C programmers would
			// be horribly confused if numbers changed meaning when they
			// added a leading zero.  For the sake of generality, I did
			// implement octal originally, and I kept the code in case
			// anyone wants to re-enable it in the future (or use this
			// tokenizer in another project where support for octal input
			// would be more desirable).  If you want to re-enable octal,
			// just un-comment the bit bewlow that checks for a leading
			// zero.
			int radix = 10;
			if (*p == '0' && p + 1 < endp && (p[1] == 'x' || p[1] == 'X'))
			{
				radix = 16;
				p += 2;
			}
			//else if (*p == '0')
			//{
			//	// C-style octal notation
			//	radix = 8;
			//	p += 1;
			//}

			// read the digits
			int acc = 0;
			float facc = 0.0f;
			bool isFloat = false;
			bool inExp = false;
			int expAcc = 0;
			int expSign = 1;
			int fracDigits = 0;
			for (; p < endp && isdigit(*p) || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == '.' || *p == 'e' || *p == 'E' ; ++p)
			{
				// check for a float
				if (*p == '.')
				{
					// if it's already a float, we can't have another dot
					if (isFloat)
						break;

					// switch to float mode
					isFloat = true;
					facc = static_cast<float>(acc);
				}
				else if (radix != 16 && (*p == 'e' || *p == 'E'))
				{
					// 'E' not in a hex number - start a floating-point exponent
					if (!isFloat)
					{
						isFloat = true;
						facc = static_cast<float>(acc);
					}

					// only one exponent is allowed per number, obviously
					if (inExp)
						break;

					// we're now in the exponent
					inExp = true;

					// check for a sign
					if (p + 1 < endp && (p[1] == '+' || p[1] == '-'))
					{
						++p;
						expSign = (*p == '-') ? -1 : 1;
					}
				}
				else if ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
				{
					//  it's a hex digit - if we're not parsing a hex number, stop here
					if (radix != 16)
						break;

					// accumulate the digit
					acc *= 16;
					acc += *p - (*p >= 'a' && *p <= 'f' ? 'a' - 10 : 'A' - 10);
				}
				else if (radix == 8 && (*p == '8' || *p == '9'))
				{
					// non-octal digits in what we took for an octal number - invalid token

					// skip the remaining digits
					while (p < endp && isdigit(*p))
						++p;

					// return the invalid token
					Error(EWarning, "Non-octal digit found in octal number (%.*s)", static_cast<int>(p - start), start);
					return Token(TokType::Invalid, start, p);
				}
				else if (isdigit(*p))
				{
					// accumulate the digit
					if (inExp)
					{
						expAcc *= 10;
						expAcc += *p - '0';
					}
					else if (isFloat)
					{
						facc *= 10.0f;
						facc += static_cast<float>(*p - '0');
						fracDigits += 1;
					}
					else
					{
						acc *= radix;
						acc += *p - '0';
					}
				}
			}

			// return the result
			if (isFloat)
			{
				// apply the sign
				facc *= static_cast<float>(sign);

				// apply the exponent, if present
				expAcc *= expSign;
				expAcc -= fracDigits;
				if (expAcc != 0)
					facc *= powf(10.0f, static_cast<float>(expAcc));

				// return the token 
				return Token(TokType::Float, start, p, 0, facc);
			}
			else
			{
				// apply the sign and return the token
				acc *= sign;
				return Token(TokType::Int, start, p, acc, static_cast<float>(acc));
			}
		}
		else if (*p == '$')
		{
			// hex number
			int acc = 0;
			for (++p ; p < endp && (isdigit(*p) || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) ; ++p)
				acc = (acc * 16) + (*p - (*p >= 'a' && *p <= 'f' ? 'a' - 10 : *p >= 'A' && *p <= 'F' ? 'A' - 10 : '0'));

			// return the numberic token
			return Token(TokType::Int, start, p, acc, static_cast<float>(acc));
		}
		else if (strchr(".,*@!%^&*()[]{}-+=<>/?|:;", *p) != nullptr)
		{
			// punctuation mark
			++p;
			return Token(TokType::Punct, start, p);
		}
		else if (isalpha(*p) || *p == '_')
		{
			// symbol - scan for the end
			while (p < endp && (isalpha(*p) || isdigit(*p) || *p == '_'))
				++p;

			// return the new symbol
			return Token(TokType::Symbol, start, p);
		}
		else
		{
			// invalid token
			Error(EError, "Invalid character '%c'", *p);

			++p;
			return Token(TokType::Invalid, start, p);
		}
	}
}
