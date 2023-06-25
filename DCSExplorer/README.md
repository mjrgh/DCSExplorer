# DCS Explorer

This project is the result of some pinball archaeology, an attempt to
better understand the DCS audio format that was used in the Williams
pinball machines of the 1990s.

DCS is the name of the audio system used in Williams pinball machines
(including those from sub-brands Bally and Midway) from 1993 through 1998.
The system was physically implemented with a circuit board based
on the Analog Devices ADSP-2105 processor, and used a proprietary
compressed digital audio format that's similar in design to
mainstream formats like MP3, AAC, and Vorbis.

The details of the DCS format have never been published, so it's
always been difficult to find out exactly what's in a DCS pinball's
sound ROMs, and quite impossible to create new ones.  The only way to
examine the contents of a DCS ROM up until now was to run PinMame in
its debugging mode, and just try all of the possible command codes to
see what each one did.  That wasn't an entirely satisfactory solution,
in part because the PinMame UI isn't very friendly, and in part because
some commands have side effects that aren't apparent when you just run
through them all one by one.

This project has three main pieces:

* DCS Explorer, a simple command-line tool that lets you examine
the contents of a DCS ROM in detail, and interactively play back
the audio it contains.

* DCSDecoder, a C++ class that implements a fully native decoder for
DCS ROMs, without any ADSP-2105 emulation.  It's a standalone class
with no dependencies on PinMame or any other external libraries, so
it's easy to incorporate into any C++ project, and its programming
interface is easy to use.  It works with all of the DCS pinball
titles released from 1993 to 1998.  I've tested
representative ROMs for every DCS title and validated that they
produce PCM output that's bit-for-bit identical to the PinMame
emulator's output.  (In fact, my work on this project turned up two
errors in PinMame's emulation that have since been corrected in the
PinMame mainline.)  It's also written in a readable coding style
and extensively commented, so it can serve as an extremely
detailed technical reference to DCS internals for people who
can read C++ code.

* DCS Encoder, a program that lets you create your own DCS ROMs.
It not only transcodes audio files into the DCS format, but also
builds entire ROM images that you could install in a DCS pinball
machine.  You can use it both to create wholly original DCS ROMs, and
to make minor changes ("patches") to existing ones, such as
replacing just a few selected audio tracks with original material.
You can encode original DCS audio from mainstream sources like
MP3, WAV, and Ogg Vorbis files.


### DCS Explorer demonstration program

I started writing DCS Explorer as a demonstration of the DCS
Decoder class, but it's become pretty useful in its own right.

At the most basic level, DCS Explorer lets you load a ROM set and play
back its contents interactively.  In this mode, it does the same thing
that PinMame does when you run a DCS game, playing back audio tracks
in response to numeric commands that you send to the simulated "data
port".  In a physical pinball machine, the main pinball controller
board sends commands to the sound board via a ribbon cable to make it
play sounds in sync with the game action.  With the interactive
demonstration program, you can simply type in commands directly.  This
is why I named it "Explorer" - it lets you try commands manually to
find out what they do.

PinMame also has an interface that lets you type in sound commands
directly, but I've always found it pretty awkward to use.  DCS
Explorer is centered around this function and makes it (in my
estimation, anyway) a lot more usable.  DCS Explorer also works
very differently from PinMame internally.  Whereas PinMame runs the
original ROM code in emulation, DCS Explorer uses an entirely new
implementation of the DCS decoder that runs entirely as native C++
code on your PC.  Not that this is really important on a modern
PC; PinMame runs adequately fast on a mid-range PC despite its
use of emulation.  But if you were interested in decoding DCS on
a slower machine, it might make a difference to run the decoder
natively.

The demonstration program does a lot more as well.  In the course of
writing the native decoder, I had to figure out a lot about the data
format of the ROMs.  The Explorer program takes advantage of what I
learned to display some interesting things about what's inside the
ROMs.  The most important of these is that you can get a complete
listing of the audio commands the ROM will accept, including
information on exactly what each command actually does.  It can also
show you the full program listing for the ADSP-2105 assembly code.


#### How to use DCS Explorer

DCS Explorer lets you see the contents of a DCS ROM set and
interactively play back its audio tracks.

* Interactive mode: Without any of the analysis options (such as
`--tracks` or `--dasm`), the program runs interactively, emulating a
DCS sound board (without the pinball machine that goes with it).  On
the original machines, the pinball controller commanded the sound
board by sending it the track numbers to play.  The interactive mode
works the same way, letting you type in the track numbers. Each track
number is a four-digit hexadecimal number; use the `--tracks` option
to get a listing of the available tracks.

* Set the initial volume: `--vol=n` sets the initial volume, to a
number from 0 (mute) to 255 (reference volume).  Setting the volume to
255 plays back the recordings at their recorded reference level, and lower
settings attenuate the digital audio on a roughly logarithmic scale.
This uses the same volume curve as the original pinball machines,
although the nominal scale on the pinball machines was shown in
the operator menus as 0-31 rather than 0-255; the mapping is
linear, with each increment on the pinball machine's 0-31 scale
corresponding to 8 units on the internal 0-255 scale.

* Showing a ROM overview:  The `--info` option shows some basic
information on the contents of the ROMs and exits (without running
the interactive mode).

* Displaying a track list:  Run the program with the `--tracks` option
to get a list of the tracks.  Use the `--programs` option to include
a full explanation of each track's control program.

* Getting a machine code program list: The `--dasm` option generates
an assembly language listing of the ADSP-2105 machine code contained
in the ROM.  The disassembler takes advantage of what we know about
the higher-level structure of the program to organize the sections
somewhat, for improved readability.

* Extracting tracks and streams: `--extract-tracks=prefix` extracts
all of the audio tracks in the ROM to individual WAV files, and
`--extract-streams=prefix` extracts the individual streams.  (The
tracks are complete audio programs, corresponding to the track
number commands you can enter interactively.  The streams are the
individual audio recordings, which the tracks play back, sometimes
sequencing several streams together.)  The extracted WAV files use
the native sample rate and bit format (31250 samples per second,
16 bit PCM, monophonic), so they record exactly what you hear on
playback, with no loss of detail.  The filenames are formed by
appending the track and stream numbers to the prefix you provide.
When recording streams, each stream is only recorded once, even if
it's referenced multiple times from multiple tracks.  When recording
tracks that loop forever, one iteration of the overall loop is
recorded.


### Validation mode

The DCS Explorer program has a special testing mode that runs the
native-code version of the decoder in parallel with an emulator that
runs the original ADSP-2105 machine code contained in the ROM.  When
running in test mode, the program plays the output of the native
decoder in the left stereo channel, and plays the output from the
emulated version in the right stereo channel.  The idea is that this
should let you easily hear any significant discrepancies between the
outputs of the two decoders.  When everything's working correctly, it
should sound just like normal monophonic playback, since the two
streams should decode to exactly the same PCM data, hence identical
audio should be playing in each channel.

The listening test isn't very rigorous, of course; it's just a sanity
check to assure us that the test is actually happening.  The real
testing happens inside the program, where it numerically compares
every pair of samples from the two decoders to ensure they're exactly
equal.  Any numerical differences will be reported on the console, and
can also be logged to a file for review.  The comparison is *exact*.
The DCS decoder generates 16-bit integer PCM samples as output, and
the validation mode tests that the native decoder and reference
decoder outputs are the identical 16-bit numbers.

The validation mode also compares data written from the DCS program to
the simulated data port.  (In the original pinball installation, this
lets the DCS program send data back to the WPC main control program.)
This must also match exactly for the validation test to pass.

To run the validator, use the `--validate` option.  If you specify
a filename, such as `--validate=test.txt`, the program will create
a file logging any validation errors detected.

For automated testing, the `--autoplay` option plays through all of
the playable tracks sequentially.  If a track has an infinite loop,
it's allowed to run through one full iteration of the loop, which
presumably will play back all of its audio information.

You can speed up an automated test with the `--silent` option.  This
can only be used in `--autoplay` mode, because it disables interactive
command entry.  In silent autoplay validation mode, the program
decodes the tracks from both decoders (native and emulated) and
compares the outputs, just as in normal validation mode, but it
doesn't actually play back any sound through the speakers.  This
allows the program to read samples from the decoders as fast as the
decoders are able to supply them, without having to wait for the
audio player to catch up.


### DCSDecoder C++ class

DCSDecoder is a standalone C++ class that lets you work with DCS audio
ROMs in any C++-based program.  It's designed to be easy to
incorporate into any C++ project, and easy to use, from a programming
perspective.  The class has most of the
same features as the DCS Explorer program - that program is mostly
just an interactive demonstration of the class's capabilities.

The decoder class is entirely standalone.  In particular, you don't
have to drag all of PinMame into your build, or even any small part of
PinMame.  In fact, PinMame is so *not* involved that the class doesn't
even use ADSP-2105 emulation.  It's a 100% C++ re-implementation of
the DCS decoder algorithms that runs entirely as native code on your
host machine.  As a result, it's extremely fast; it makes a barely
noticeable dent in my CPU usage percentage in Windows Task Manager,
only a fraction of a percent.  (The class actually *can* run the ROM
code in emulation as well, but that's just a bonus feature that's
included specifically for the sake of rigorous testing against the
reference implementation.  DCS Explorer uses this in its "validation
mode".  The emulator version is an alternative subclass that you can
instantiate if you want that form of implementation, but if you don't,
you can just ignore it and use the native class exclusively.)

Although the class makes it easy to perform real-time audio playback
from the ROMs, it doesn't have any hard-coded assumptions that you're
actually going to be doing that, and it doesn't have any dependencies
on any hardware audio interfaces or OS audio APIs.  Rather, it has a
simple mechanism that you can use with whatever audio API you wish to
use in your application.  If you want to play back live audio in your
program, you first set up whatever audio player system you want to use
(DCSDecoder doesn't get involved in that at all) and then you call the
decoder class to ask it to provide more audio samples whenever you
need to refill your system audio buffer.  That's basically all there
is to it.  The class doesn't need any hardware audio buffers or timers
or anything else; it just fills your requests for more samples
whenever you're ready to make them.  This lets you drive the timing
entirely from your audio player system; the decoder class will keep in
sync with real time automatically, by virtue of generating a fixed
number of samples (31,250) per second of real time.


## Building

The source repository should include all dependencies.  Building
should just be a matter of cloning the git repository, opening the
solution (.sln) file in Visual Studio, and executing a Build Solution
command.

If you want to use DCSDecoder in your own project, you can either use
the static library (dcsdecoder.lib) that the DCSDecoder sub-project
builds, or you can manually import the source files into your own
project tree.  The static library is usually the simplest approach as
long as you're using identical build settings, but if not, it's
probably a lot less work to import the source files directly.  You'll
also need the miniz library if you want to use the ZIP file loader
feature (in DCSDecoderZipLoader.cpp) - but that whole module can be
omitted if you don't need it, which will also remove the dependency on
miniz.  miniz is also included in the solution as a static library
project, so you'll get that automatically if you're building against
dcsdecoder.lib; if not, you'll also have to import the miniz source,
but I didn't encounter any complications getting that building, so
hopefully you won't either.



## What's in a DCS ROM

A DCS ROM contains a set of "tracks" and "streams" - these are my
terms, not anything official from DCS's creators, so I'll explain.

A "track" is a miniature procedural program that controls playback of a music cue
or sound effect.  A track doesn't contain audio directly; it just has
the instructions to play a sequence of audio clips, along with timing
information and simple looping instructions to play back sections
repeatedly.  A track program can trigger another track program, and it
can specify transition points so that tracks change on selected music
beats.

The track design is the root reason that it's always been so difficult
to extract all of the audio in a DCS ROM for conversion to another
format.  It's simply not possible in principle to extract *everything*,
because the content is inherently dynamic.

A "stream" contains the actual audio, as compressed digital data.
DCS streams use a proprietary lossy compression format based on the
same basic DSP math as more widely used formats like MP3 and Vorbis.

I'll document the stream format in detail separately.  The quick
overview is that a stream is s sequence of frames, where a frame
represents a 7.68ms time window, containing 240 consecutive PCM
samples at 31250 samples per second.  The binary data stream doesn't
contain the PCM samples directly; instead, a stream stores a
frequency-domain transformation of the PCM data over a 256-sample time
window (256, not 240, because consecutive frames overlap by 16
samples).  The transformed samples are then stored in the stream using
a ZIP-like bit-compression encoding that tries to squeeze the bits
into a smaller space by using short bit sequences in the coding to
represent common bit patterns in the data, similar to how Morse Code
assigns the shortest dot-and-dash patterns to the most frequently used
letters.  That part of the coding is lossless, but it only accounts
for a small part of the compression.  Most of the compression comes
from the "lossy" part, which saves space by reducing the precision of
the stored information.  For example, if an audio sample in the
original frequency-domain frame has the value 1.234567, the encoder
can save some space by rounding that to 1.235, 1.24, or even just 1.2,
depending on how the sample relates to other samples within the frame.
All modern lossy compression formats use this same approach.

I have no idea what the original Williams encoder's rules were like
for implementing the lossy compression steps.  We can only guess that
they used something like the techniques you'd see in a modern MP3
encoder.  My own encoder's compression algorithm is much more
simple-minded.  It doesn't use any human perceptual modeling, the way
that MP3 and Vorbis encoders do, but rather simply tries to keep the
the round-trip quantization errors below a chosen threshold.  This
works better than I would have expected; it seems to produce decent
fidelity at compression rates similar to what we see in the original
DCS ROMs.  Even so, I'm sure it's possible to do better, so perhaps
someone with an interest in this area will want to investigate it
further at some point.


## "Known" pinball machines

The decoder includes special recognition for the 29 commercial pinball
titles released from 1993 to 1998 that used DCS audio.  DCS Explorer
displays the "Known pinball machine" title that it recognizes as part
of its description of the loaded ROM data.

The recognition is based on matching the U2 ROM signature against a
collection of text fragments from the various games' signatures that
distinguish the 29 games from one another.  It doesn't match the
*entire* U2 signatures, so that it can match different versions of the
same game without having to exhaustively list every version's
signature.  This approach is reliable at recognizing and
distinguishing all of the **original** DCS ROMs, but it can easily be
fooled by new and modded ROM sets that use signature strings that
happen to contain any of the special text fragments.  If a false
positive match occurs with a modded ROM, you can either ignore the
match or fix the ROM (if it's your own ROM to fix) so that it's
signature doesn't trigger the false match.

You can usually just ignore false positives, because the recognition
has almost no practical effect on the operation of the decoder.
Currently, there's only one special case in the decoder for one game
(<i>Tales of the Arabian Nights</i>) that handles a peculiarity in
that game's original ROM programming.  The title recognition doesn't
affect the decoder's understanding of the audio format or other
version-specific features - that's purely based on an analysis of the
ADSP-2105 code in the ROM.


## The raw DCS stream file format (.dcs)

One of DCS Explorer's capabilities is stream extraction.  It can
extract all of the audio streams in a ROM set to individual audio
files.  The default is to extract to uncompressed WAV files.  WAV is
the default because it's lossless and practically every audio tool can
work with it, so it's a perfect intermediate step along the way to
whatever you want to use the extracted files for.

If you're keeping the extracted stream within the DCS space, though,
you'll want to use the other extraction format option: the "raw"
format.  The raw format extracts streams from the ROMs exactly as they're
stored there, without any decoding or conversion.  This is the right
format to use if you're going to use an extracted stream as part of a
new DCS ROM set, because it preserves the exact DCS stream data across
the whole process.  This is better than making a round trip through
WAV format because, even though WAV itself is lossless, the re-encoding
step from WAV back into DCS is lossy.  It's the same thing that
happens if you keep feeding an MP3 file into an MP3 encoder over and
over: each re-encoding pass degrades the signal a little more.  The
raw format keeps the data in its original DCS-encoded form from end to
end, so there's no need to re-encode it to store it back in a new ROM.

There's no such thing as a "DCS audio file" in the real world, of
course, so I had to invent a brand new file type.  I came up with a very
simple container format that wraps the raw DCS stream data - it's
really just a small header followed by the DCS stream bytes.  Nearly
the entire purpose of the header is format recognition, to allow DCS
Explorer-related projects to recognize instances of this new file
type, and to allow mainstream audio programs to recognize that they're
*not* standard audio files.  The header also crucially allows DCS
Explorer-related projects to detect the specific DCS stream format contained
in the file.  There are three separate DCS stream formats,
corresponding to the major generations of the DCS firmware, and the
streams themselves don't contain any information that would help you figure out which
format they're encoded with.  You just have to know which version of the
ROM firmware they were embedded with.  That's fine when you're working
with an actual ROM, but once you export a stream as a separate object
detached from its ROM, it would be impossible to figure out where it
came from if we didn't include this information explicitly.  The
header thus contains a version marker that records this
information, so that readers will know how to interpret the stream
contents properly.

The file format consists of a 32-byte header, followed by a "stream
data chunk" consisting of a 4-byte length marker (giving the length in
bytes of the stream data) and then the raw DCS stream bytes, extracted
verbatim from the ROM image.  All integer types are stored in
big-endian format, honoring the ubiquitous use of big-endians
throughout the DCS ROM data.

| Offset | Field | Type | Description |
| ------ | ----- | ---- | ----------- |
|   0    | Signature | BYTE[4] | "DCSa", 4 literal bytes, to identify the file type to other software |
|   4    | FormatVer | UINT16  | The format version: 0x9301 for DCS-93a (Indiana Jones, Judge Dredd); 0x9302 for DCS-93b (ST:TNG); 0x9400 for all later games |
|   6    | Channels  | UINT16  | Number of audio channels; always 0x0001 (mono) |
|   8    | SampleRate | UINT16 | Samples per second; always 0x7A12 (31250 samples per second, the invariant DCS audio rate) |
|   10   | Reserved | BYTE[22] | Reserved for future use; all bytes set to zero currently |
|   32   | DataSize | UINT32   | Size of the data stream in bytes
|   36   | Data     | BYTE[DataSize]   | Raw DCS data stream bytes

The information on sample rate and channel count is utterly
unnecessary, since it's always the same for every DCS stream.  I
included it anyway, mostly for the sake of appearances, but also because the
additional redundancy improves the accuracy of mechanical format
recognition.  The only header information that can every really vary
is the format type, and that does contain vital information that a
reader needs in order to properly interpret the stream data.




## Origins and goals of this project

I got started on this project because I wanted a DCS decoder that I
could use on an embedded platform, for another project.  PinMame's
emulator is great for running Visual Pinball on a fast modern PC, but
its approach - interpreting the original ADSP-2105 machine code, one
instruction at a time - is a little too slow for real-time playback on
the slower CPUs typically found on microcontrollers and embedded
platforms.  So I wanted to look into a fully native version of the
PinMame player, without any of that instruction-at-a-time
interpretation.

One way to do that would have been to mechanically translate the ROMs
to native machine code on the new target platforms, using either a JIT
cross-compiler at run-time, or translating the ROM code in bulk ahead
of time.  PinMame successfully uses the JIT approach to run the ARM
code for some of the early 2000s Stern sound boards.  I decided to try
the ahead-of-time approach instead, translating to portable C++.  That
proved a quite workable approach, but as I got further into it, I
started to get glimpses of how DCS worked on the inside.  That
aroused my curiosity, and started me down the road of
writing a fully native decoder based on an understand of the format
itself, not just a rote translation of old machine code.

Once I understood the format well enough to decode it fully with an
original program, I realized that I also understood it well enough to
*encode* new material in the format.  That led to the final piece of
this project, my DCS Encoder, which can create original DCS ROMs with
new audio material.


## Other DCS trivia

Here are some other miscellaneous details about the DCS software that
I've observed working on this project.

### Special data port command codes

Most of the command codes sent from the WPC host to the DCS board are
two-byte sequences that load track programs.  The sound board interprets
each such sequence as a 16-bit number, with the first byte as the high
8 bits, and looks up the track matching that number in the track index.
For example, if the WPC board sends the sequence $01 $40, the sound
board combines the bytes to form the 16-bit number $0140, and looks
in the track index for track $0140.  If such a track exists, it loads
the track into its channel and starts it executing.

There are also a few special command codes that the software handles
directly, without looking for a matching track program.  These command
sequences are always either two bytes or four bytes long.  The first
byte is always $55, and the second byte is a code $AA to $FF that
specifies which special command it is.

For the four-byte commands, the third byte is a parameter, and the
fourth byte is always the bitwise NOT (all bits inverted) of the third
byte.  The fourth byte is just there as a sort of checksum to validate
that the sequence was received correctly.  I guess when the messages
get up to a whopping four bytes long, there's too much of a chance of
transmission error in such a primitive network setup, so they had
to add some error detection.

Here are the special command sequences:

* $55 $AA *volume* ~*volume*: Sets the master audio volume, 0 to 255,
mute to maximum.  The volume on the DCS boards is set purely in
software, by scaling the PCM data going into the DAC.   The volume
control is figured into the PCM data by multiplying the PCM amplitudes
by a scaling factor that's derived from the 0 to 255 setting, using a
log curve that makes the scale sound roughly linear to a human ear.
At 255, the scaling factor is 1.0, so you get the reference level of
the recordings; lower volumes attenuate the PCM amplitude along that
log curve.  The 0-255 scale corresponds very straightforwardly to the
0-31 scale displayed in the operator menus when you use the volume
controls inside the coin door, by multiplying the displayed value
by 8.  When the display shows 31, the software sends 31*8 = 248 ($F8)
to the DCS board.

* $55 $AB-$B0 *level* ~*level*: Sets the per-channel mixing level.
The second byte selects which channel this affects: $AB is channel 0,
$AC is channel 1, and so forth through $B0 for channel 5.  The level
is a byte value, 0 to 255, that sets the mixing level for the
channel corresponding to the second byte value.  The per-channel
mixing level is combined with the internal mixing levels set by
the track programs to determine the relative volume of the channel
in the overall mix-<zero-width space>down into the final mono signal, and this is
then further modified by the global volume level.  So many volume
adjustments!  In practice, none of the WPC games seem to ever send
these commands, so this is one of those hidden features
that the DCS engineers implemented for their own amusement, and
never got around to mentioning to the soundtrack designers.

* $55 $C2: Get the DCS software's major version number.  The usual
convention with software version numbers is to use a "dot" notation
with a series of number parts, as in 1.0 or 2.1.7.  The first number
is conventionally known as the "major" version number, and the second
number is the "minor" number.  This command causes the sound board to
write one byte to the data port containing the DCS software's major
version number, which is always $01 in all of the pinball titles that
support this command.  The command is only implemented in DCS-95 games
starting around 1996; earlier games didn't implement this command at
all.  Note that this is separate from the "audio data version number"
that the WPC software usually displays right after you enter the
operator menu - that doesn't come from DCS, but rather from a track
program.  Most of the DCS ROMs have a couple of track programs
somewhere in the mid to high numbered tracks that return the
audio data version number when asked.  Run a `--programs` dump
and look for a `WriteDataPort()` command with a value around $10
or $11, which WPC interprets as 1.0 or 1.1.  For example,
*Medieval Madness* uses track $03E7 for this.

* $55 $C3: Get the DCS software's minor version number.  This
command causes the sound board to write one byte to the data
port containing the minor version number - the part after the
dot in the "1.03" format.  The reply is $03, $04, or $05, depending
upon the vintage of the game.  As with $55 $C2 (query the major
version number), this command is only implemented in DCS titles
released starting around 1996.

* $55 $BA-$C1 *param* ~*param*: This is a mystery command, implemented
only in some of the later DCS-95 games, that doesn't actually do
anything.  It seems to be scaffolding for a feature that was
started and abandoned before it was finished, either because Williams
quit the pinball business before the engineers could finish it, or
because the engineers decided it wasn't a very good idea after all and
decided to cancel the feature, but didn't delete the code.  Whatever
the story behind it, the code suggests that this command was meant to
do something parallel to the $55 $AB series of commands, in that
it's similarly arrayed across the audio channels, and it seems to have
similar data structures for something like a volume level and some
timers.  But the code never does anything with the data structure
apart from populate them when one of these commands is received, so I
have no idea what the command was meant to do.  It's another of
those never-used features lying dormant in the code.


### Data exchange at power-on

Whenever the DCS board receives a hardware reset signal, it loads its
"hard boot" program from ROM.  The first thing the boot program does
is sit in a spin loop for 250ms monitoring the data port.  If a
byte - any byte - arrives on the data port during that time, the
boot program discards the byte read and performs a "soft" CPU reset,
which loads the full DCS software and goes into the normal decoder
operation mode.

If the 250ms interval elapses without anything arriving on the data
port, the boot program proceeds to run its power-on self test, which
scans all of the ROMs to make sure they match the checksums stored in
the "catalog" in U2, and runs several cycles of reads and writes on
RAM memory to check for hardware faults.

When the self-test completes, the DCS board writes two bytes to the
data port: $79, followed by a status code.  The status code byte is
$01 if the self-tests were successful, $02 through $09 if ROM U2
through U9 didn't match its checksum ($02 if U2 failed, $03 if U3
failed, etc), or $0A if the RAM test failed.  The board also plays the
distinctive startup "bong" tone the same number of times as the second
status byte - that's why you always get one "bong" when things are
working properly at startup.  The repeated bongs when an error occurs
are meant to provide the operator with some minimal diagnostic
feedback - an audio rendition of the status code that the sound board
sends to the main board - in case the main board is also screwed up so
badly that it can't display the status code sent back in the $79
message.  If the main board *is* working, it will normally intervene
before the sound board can play the error bongs by sending a reset
signal to the sound board, so you'll only hear the extra bongs if
the main board is also having problems booting up.

The WPC software can reset the DCS board at any time by writing to a
special port known in PinMame as the control port.  The only function
of this port is to reset the sound board under software control from
the main board.  The WPC software exercises this authority whenever
you enter or exit the operator menu; in fact, it exercises it quite
aggressively, resetting the sound board 255 times in a row on each
transition.  I'm not sure what the point is of repeating the reset so
many times, but the point of doing it at all is probably just to kick
the sound board out of any wedged state it might have gotten itself
into due to software faults, the way that the first troubleshooting
step that your Technical Support Telephone Representative insists upon
is always to reboot all of your PCs and network routers and cable TV
boxes, even if you're calling about the fridge.  At any rate, those
255 consecutive resets aren't noticeable because a reset only takes a
few milliseconds.  This is ancient embedded software, thankfully, not
Windows.  And the reason you don't 255 startup "bongs" every time you
enter the operator menu is that the WPC software uses that feature of
the boot loader we mentioned earlier, where the boot loader bypasses
the startup tests if it receives a byte on the data port within 256ms
after a reset.  The WPC software knows about this (obviously; the two
systems were designed together as a set), and it sends an otherwise
meaningless byte to the sound board after these soft resets to let it
know that it should just go straight into a fresh decoder session.

The other information that the WPC board has to send to the sound
board after every reset is a master volume command - the $55 $AA
sequence described above.  The WPC board has to send a volume command
after every sound board reset because the sound board itself has no
access to any sort of non-volatile storage.  Every time the sound
board resets, either at power on or one of those soft resets when
entering or exiting the operator menu, its entire memory is wiped,
including its memory of the volume setting.  The WPC board is the only
part of the system with non-volatile storage, so it's responsible for
saving the volume across power cycles, and it's also responsible for
initializing the volume in the sound board on every reset.  The sound
board doesn't have any way to "ask" for the volume; it just has to
count on the WPC software sending an update shortly after every reset.
The WPC software is good about fulfilling this obligation.

The WPC software will also usually send a track program command
shortly after startup to ask the sound ROM for its "audio data version
number".  If you're watching the data port, look for an exchange where
the WPC board sends a track command and the sound board immediately
replies with a byte value around $10 or $11.  The WPC board interprets
that kind of value as a two-part version number: $10 is version 1.0,
$11 is version 1.1, etc.  The command in this exchange is just a track
number, not one of the special $55 commands, which makes it utterly
idiosyncratic per game.


## Links to other DCS documentation

Up until now, very little information has been published on the DCS
format.  That shouldn't be entirely surprising, given that it's a
proprietary format that was only ever used in embedded systems for a
niche industry that mostly disappeared over 20 years ago.  And yet, living
as we do in the age of total information awareness, I'm
always shocked and amazed when I encounter *any* subject, no matter how obscure,
that isn't exhaustively documented somewhere on the Web.  DCS seems to
be of those rare subjects that time forgot.  In all of the Web, there are only a few
mentions of the technology, and those few mentions are really skimpy.
There's a skeletal [Wikipedia page](https://en.wikipedia.org/wiki/Digital_Compression_System),
and a sort of [marketing-highlights article](https://web.archive.org/web/20070929205008/http://pinballhq.com/willy/willy3.htm)
from a long-defunct pinball 'zine (remember 'zines?) that's only still accessible at all thanks to
[the Wayback Machine](https://web.archive.org/), and that's about it.

If you want more than generalities, the only source I know of that
has any detail at all is the source code for
[PinMame](https://github.com/vpinball/pinmame).  PinMame includes a
software emulator that can run the original ROM images from the DCS
boards, and the emulator's source code contains a lot of fine-grained
detail about how the original circuit boards work.  The emulator has
details that you can't learn from reading the published schematics of
the boards, because the boards use proprietary PLAs (programmable
logic arrays) whose programming isn't published anywhere.  The PLAs
implement most of the external addressing logic that connects the
CPU to the on-board peripherals, so it's not clear from the schematics
alone how the software is meant to access the peripherals or what side
effects are triggered in the hardware when it does.  That's something you
have to know to understand how the ROM software works.  The PinMame
code also helps nail down some of the details of the various chips on
the board, such as the ADSP-2105 CPU and AD1851 DAC, and how they're
used in that particular application.

When I say that the PinMame source code "contains" this information, I
don't mean to suggest that it's explicitly documented there.  I just
mean that the information is embodied in the structure of the C code.
If you can read the C code, you can work backwards from the code to
infer things about how the hardware being emulated must have worked.


## Third-party software credits

This project uses the several open-source libraries, all of which have
license terms at least as permissive as this project's.  My thanks to
the authors for their work in creating these excellent components and
their generosity in sharing them.

* ADSP-21xx series CPU emulator, by Aaron Giles, by way of PinMame.  Released
under a BSD 3-clause license.

* libnyquist, Copyright (c) 2019, Dimitri Diakopoulos All rights reserved.
Released under a BSD license.

* libsamplerate, Copyright (c) 2012-2016, Erik de Castro Lopo <erikd@mega-nerd.com>.
Released under a BSD license.

* miniz, Copyright 2013-2014 RAD Game Tools and Valve Software, and
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC.
Released under an MIT license.

Please see the individual source folders for these libraries for their
full license text.

DCS is a trademark of Williams Electronic Games, Inc., and is used here
for information and identification purposes only.  This project isn't
endorsed by or connected in any way to the commercial entities who
created or own the original hardware/software platform.
