// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCSTokenizer - a simple tokenizer system for use with the DCS
// encoder's script compiler.  This implements a basic token reader
// mechanism to read text input using a C-like lexical structure.
//

#pragma once
#include <string>
#include <memory>

// Simple input tokenizer
class DCSTokenizer
{
public:
	// Error logger for script parsing.  The default implementation logs
	// errors to stdout.
	class ErrorLogger
	{
	public:
		// Error level
		enum class Level
		{
			Info,       // information only
			Warning,	// warning only, not necessarily an error but worth nothing
			Error,		// error; processing can proceed but the result should be considered invalid
			Fatal		// fatal; processing cannot proceed
		};

		// Error level to string name
		const char *LevelName(Level level) 
		{
			return level == Level::Info ? "note" :
				level == Level::Warning ? "warning" :
				level == Level::Fatal ? "fatal error" : "error";
		}

		// Log a warning or error.  'where' is the source location of the
		// error; when reading from a script file, this is formatted as
		// "filename(lineNumber)".  An empty string means that no source
		// location information is available.
		virtual void Log(Level level, const char *where, const char *msg);

		// Log a progress/status report.  'pending' means that the message
		// indicates the start of a long-running process, or a middle step
		// in a long-running process, and additional reports on this step
		// are forthcoming.  The client can use this to display the message
		// in a suitable style, or show a UI cue to indicate that something
		// is in progress (spinning cursor, etc).  The default implementation
		// logs messages to stdout, and pending messages are shown without
		// a trailing newline so that later updates continue on the same 
		// line.
		virtual void Status(const char *msg, bool pending);

		// Count of errors by class.  'errors' counts both regular and
		// fatal errors.
		int fatal = 0;
		int errors = 0;
		int warnings = 0;
	};

	DCSTokenizer(ErrorLogger &logger) : logger(logger) { }
	virtual ~DCSTokenizer() { }

	// error logger
	ErrorLogger &logger;

	// log an error or warning
	using ErrorLevel = ErrorLogger::Level;
	static const ErrorLevel EFatal = ErrorLevel::Fatal;
	static const ErrorLevel EError = ErrorLevel::Error;
	static const ErrorLevel EWarning = ErrorLevel::Warning;
	void Error(ErrorLevel level, const char *msg, ...);

	// current reading location, for error logging purposes
	struct Location
	{
		int lineNum = 0;
		std::string filename;
	};
	Location GetLocation() { return Location{ lineNum, filename }; }

	// log an error at a specific location
	void Error(ErrorLevel level, const Location &location, const char *msg, ...);

	// load a file
	bool LoadFile(const char *filename, std::string &errorMessage);

	enum class TokType
	{
		Symbol,		// symbol or keyword
		Int,		// integer
		Float,		// floating-point number
		String,		// string (Token::text contains the string contents, stripped of quotes and escapes)
		Punct,		// punctuation mark
		End,		// end of file
		Invalid     // invalid token
	};

	struct Token
	{
		Token(TokType type = TokType::Invalid, const char *start = "", const char *end = nullptr, int ival = 0, float fval = 0.0f) :
			type(type), text(start, end == nullptr ? 0 : end - start), ival(ival), fval(fval)
		{ }

		Token(const Token &t)
		{
			type = t.type;
			text = t.text;
			ival = t.ival;
			fval = t.fval;
		}

		TokType type;
		std::string text;   // symbol text, string contents (stripped of quotes and escapes), punctuation mark text
		int ival;           // integer value, if applicable
		float fval;         // floating-point value, if applicable

		// is this a keyword match?
		bool IsKeyword(const char *kw) { return type == TokType::Symbol && _stricmp(text.c_str(), kw) == 0; }

		// is this a punctuation match>?
		bool IsPunct(const char *s) { return type == TokType::Punct && text == s; }
	};

	// At EOF?
	bool IsEOF() const { return p >= endp; }

	// Get the next token
	Token Read();

	// Skip up to and including the next ';', to try to resynchronize after
	// a syntax error
	void SkipStatement();

	// End the current statement.  If we find ';', simply skip it and return.
	// If not, log an error, and skip until we do find a ';'.
	void EndStatement();

	// Get the next token, requiring the specified type; displays
	// an error and returns a dummy token of the required type on error.
	Token ReadSymbol();
	Token ReadString();
	Token ReadInt();
	Token ReadUInt8();     // enforces 8-bit unsigned range, returns in ival
	Token ReadUInt16();    // enforces 16-bit unsigned range, returns in ival
	Token ReadFloat();     // allows an integer or float as input, but always returns as a float

	// Check for a specific next token; if it matches, skip the token
	// and return true, otherwise return false without changing the
	// input position.
	bool CheckPunct(const char *s);
	bool CheckKeyword(const char *s);

	// require a specified token; if present, skips the token and returns
	// true; if not, shows an error and returns false
	bool RequirePunct(const char *s);
	bool RequireSymbol(const char *s);

	// filename
	std::string filename = "no file";

	// file contents, loaded into memory
	std::unique_ptr<char> contents;
	long contentLength = 0;

	// current read pointer
	const char *p = nullptr;

	// end pointer
	const char *endp = nullptr;

	// current line number
	int lineNum = 1;

	// save/restore the current token position state
	struct SavedState
	{
		const char *p;
		int lineNum;
	};
	SavedState Save() { return SavedState{ p, lineNum }; }
	void Restore(SavedState s) { p = s.p; lineNum = s.lineNum; }

	// error and warning counts
	int errorCount = 0;
	int warningCount = 0;
};
