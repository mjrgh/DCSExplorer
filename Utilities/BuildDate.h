// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Build Date Utilities
//

#pragma once

// -----------------------------------------------------------------------
//
// Miscellaneous date/time utilities
//
class DateUtils
{
public:
	// Calculate the Julian Day Number (JDN) for a given calendar date
	// The date is in terms of normal calendar notation, with the full
	// year (e.g., 2022), numeric month (1..12), and nominal day of the
	// month (1..31).  
	// 
	// Note that even though the result is the Julian day number, the
	// input date is specified in terms of the modern Gregorian calendar.
	// 
	// The main use of the JDN is for figuring days between dates.  The
	// JDN for a given date is the number of days between that date and
	// a fixed epoch (January 1, 4713 BCE on the proleptic Julian calendar,
	// which corresponds to November 24, 4714 BCE on the proleptic
	// Gregorian calendar).  To calculate the number of days between any
	// two dates, simply take the difference between their JDNs.
	//
	// Note that this calculation doesn't take the time of day into
	// account.  Technically, each Julian Day starts at 12:00:00 (noon)
	// UTC.  CalcJDN() assumes a time after noon, so if you want the
	// correct JDN for a time in the morning on the given date, subtract
	// one from the result.
	static int CalcJDN(int yyyy, int mm, int dd);

	// Calculate the Julian Date (JD) for a given calendar date and time
	// of day.  The time is UTC, expressed using 24-hour clock notation
	// (hh 0..59, mi 0..59, ss 0..59).
	static double CalcJD(int yyyy, int mm, int dd, int hh, int mi, int ss);
};


// -----------------------------------------------------------------------
//
// Program build date/time.  This parses the compiler-defined __DATE__ 
// and __TIME__ macros into a struct.  This can be used to display the
// timestamp for the current build in any desired format, or to generate
// date-based build numbers.
//
struct ProgramBuildDate
{
	// date
	int dd;      // day of the month, 1..31
	int mm;      // month, 1..12
	int yyyy;    // year

	// day of the week, day of the year
	int wday;    // 0 = Sunday
	int yday;    // 0 = Jan 1 of year YYYY

	// Julian Day Number for the date
	int jdn;

	// time
	int hh;      // hour on 24-hour clock, 0 for midnight to 23 for 11:00 pm
	int mi;      // minute of the hour, 0..59
	int ss;      // second of the minute, 0..59

	// construct from __DATE__ and __TIME__
	ProgramBuildDate() { Init(__DATE__, __TIME__); }

	// construct from an arbitrary date and time, using the same formats
	// as the compiler's __DATE__ and __TIME__ macros
	ProgramBuildDate(const char *date, const char *time) { Init(date, time); }

	// Initialize from a date string in the compiler's __DATE__ format
	// and a time string in the compiler's __TIME__ format.  
	void Init(const char *date, const char *time);

	// Return a build date string
	std::string YYYYMMDD() const;

	// Generate a copyright date string of the form "<base year>-<current year>",
	// or just "<current year>" if the two are the same.  This can be used to
	// build a copyright message banner that automatically updates to include
	// the year of the current build, with the range starting at the initial
	// creation time.
	std::string CopyrightYears(int startingYear) const;
};
