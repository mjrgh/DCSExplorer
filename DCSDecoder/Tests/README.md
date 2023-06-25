# DCSDecoder Tests

test-all.bat is a Windows batch file (CMD.EXE script) that runs a
basic validation test on the native C++ DCS decoder implementation.
It tests the native decoder's output against the output from the
ADSP-2105 emulator version, testing playback of each track in a
collection of DCS ROM images.  

The test is set up to use one representative ROM version (usually the
final production release) from each of the 29 DCS commercial titles.
You can easily add more ROM images, by adding the names of the new ROM
.zip files to the "for" list.

The ROM images aren't part of the repository, but they can be found
online from various sources, such as vpforums.org.  Download them from
the site of your choice and save them in the roms/ subdirectory.  The
ROMs must be bundled as .zip files, using the PinMame conventions.
Zip files that were prepared for use with Visual Pinball are suitable.

To run the test, open a CMD window, CD to this folder, and type
`test-all`.  This will run all of the tests and record the outcomes in
the results/ subdirectory.  If a test passes, a .success file will be
created in the results/ folder.  If a test fails with diffs, a .diff
file will appear in the results/ folder.  In addition, each tests
produces a .log file, which logs any differences found (capturing the
contents of each non-matching audio frame).  These can be helpful to
track down the source of a validation failure.

A validation test simply runs through every track in the ROM once, in
sequential order of track number.  This isn't a truly exhaustive test,
since DCS allows simultaneous playback of from four to eight tracks
(depending the software version), and tracks that play together can
have interactions that change the way they play as compared to playing
back individually.  The validation test doesn't attempt to search for
or exercise such combinations.
