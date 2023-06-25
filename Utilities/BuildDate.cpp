// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// Build Date Utilities
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <list>
#include <memory>
#include "BuildDate.h"


// -----------------------------------------------------------------------
//
// CompileDate utilities
//

// Initialize from a date string in the compiler's __DATE__ format
// and a time string in the compiler's __TIME__ format.  
void ProgramBuildDate::Init(const char *date, const char *time)
{
    // Magic hash table for decoding the month from the __DATE__ macro
    // forma into a month number 1..12.
    //
    // __DATE__ always uses the format "Mon DD YYYY", where Mon is in
    // [Jan, Feb, Mar, Apr, May, Jun, Jul, Aug, Sep, Oct, Nov, Dec].
    // The day-of-the-month and year fields are easy to parse, since
    // they're just numbers.  The month is by name, though, and we
    // need the numeric month 1..12.  How to convert it?  We *could*
    // simply compare the month field to a series of fixed strings
    // ("Jan", "Feb", etc) until we find a match, but that's inefficient.
    // Or, we could construct a std::map keyed by month name, but
    // that's overkill for such a small lookup table.  What we'd really
    // like to do is come up with a simple arithmetic formula that we
    // can apply to the ASCII character values.  That seems beyond
    // reach, but what we can do is create an ad hoc "perfect hash" for
    // this particular set of strings.  There are many ways to do this;
    // the one here takes advantage of the near-uniqueness of the third
    // letter of the month names: there are only two letters that aren't
    // unique ('n' for Jan/Jun, and 'r' for Mar/Apr).  Both of those
    // cases can be distinguished via the *second* letter, so we don't
    // even need to consider the first letter.  A little experimentation
    // reveals that if you take the sum of the upper-case ASCII values
    // of the second and third letters, and then take that mod 17, you
    // get a value that's different for every month - a perfect hash key
    // into a 17-bucket hash table.  
    //
    // Note that this doesn't reliably reject invalid month names, since
    // we're discarding a lot of information to get such a compact hash
    // code; many other two-letter sequences that aren't valid month
    // names will map to non-zero 'mm' values with this hash formula. 
    // But that's okay, since __DATE__ comes from the compiler, not user
    // input.  It's safe to assume it's well-formed.  If we wanted to
    // adapt this approach to parse potentially invalid date strings as
    // well, a simple improvement would be to use the 'mm' result as an
    // index into an array of strings with the month names in order, and
    // test that against the input to make sure they match, rejecting
    // the input as invalid if not.
    static const int month[] ={ 12, 5, 0, 8, 0, 0, 0, 1, 7, 4, 6, 3, 11, 9, 0, 10, 2 };

    // Note: the & ~0x20 clears the "lower-case" bit, converting lower to
    // upper and leaving upper unchanged, for case insensitivity
    mm = month[((date[1] & ~0x20) + (date[2] & ~0x20)) % 17];

    // The rest of the fields in __DATE__ and __TIME__ are numeric,
    // so we can simply pull them out via atoi()
    yyyy = atoi(&date[7]);
    dd = atoi(&date[4]);
    hh = atoi(&time[0]);
    mi = atoi(&time[3]);
    ss = atoi(&time[5]);

    // store the Julian date
    jdn = DateUtils::CalcJDN(yyyy, mm, dd);

    // store days since Jan 1 of the same year
    yday = jdn - DateUtils::CalcJDN(yyyy, 1, 1);

    // Calculate and store the weekday.  Since we've already gone to
    // the trouble of calculating the JDN, the weekday can be easily
    // figured as the number of days since an arbitrary Sunday in
    // the past, mod 7.  Julian Day Number 0 happens to be a Monday,
    // so JDN -1 is a Sunday, hence (jdn - -1) == (jdn + 1) is the
    // number of days since a known Sunday.  The reason to pick one
    // so far in the past is simply to ensure that we're operating
    // on a positive value with the %7 calculation, since the C++ '%'
    // is only a proper 'mod' operator with positive inputs.
    wday = (jdn + 1) % 7;
}

std::string ProgramBuildDate::YYYYMMDD() const
{
    char buf[16];
    sprintf_s(buf, "%04d%02d%02d", yyyy, mm, dd);
    return buf;
}

std::string ProgramBuildDate::CopyrightYears(int startingYear) const
{
    char buf[16];
    if (yyyy == startingYear)
        sprintf_s(buf, "%4d", startingYear);
    else
        sprintf_s(buf, "%4d-%4d", startingYear, yyyy);

    return buf;
}


// -----------------------------------------------------------------------
//
// Date utilities
//

int DateUtils::CalcJDN(int yyyy, int mm, int dd)
{
    int a = (14 - mm) / 12;
    int m = (mm + 12 * a - 3);
    int y = yyyy + 4800 - a;
    return dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

double DateUtils::CalcJD(int yyyy, int mm, int dd, int hh, int mi, int ss)
{
    int jdn = CalcJDN(yyyy, mm, dd);
    int s = hh*60*60 + mi*60 + ss;
    if (hh < 12)
        return (jdn - 1) + (s + 43200)/84600.0;
    else
        return jdn + (s - 43200)/84600.0;
}

