# DCS Explorer demonstration program

I started writing the DCS Explorer program as a demonstration of the DCS
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
works the same way, letting you play the role of the WPC host board
by typing in the command numbers manually.  Each track
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
format.  The content is inherently dynamic, so merely trying all
of the track numbers one at a time doesn't necessarily tell you
everything that's in the ROM.

A "stream" contains the actual audio, as compressed digital data.
DCS streams use a proprietary lossy compression format based on the
same basic DSP math as more widely used formats like MP3 and Vorbis.

The full details of the stream format can be found in my
[DCS Audio Format Technical Reference](http://mjrnet.org/pinscape/dcsref/DCS_format_reference.html).
The quick overview is that a stream is s sequence of frames, where a frame
represents a 7.68ms time window, containing 240 consecutive PCM
samples at 31250 samples per second.  The binary data stream doesn't
contain the PCM samples directly; instead, a stream stores a
frequency-domain transformation of the PCM data over a 256-sample time
window (256, not 240, because consecutive frames overlap by 16
samples).  The transformed samples are then stored in the stream using
a Zip-like bit-compression encoding.  That part of the coding is lossless,
but it only accounts for a small part of the compression.  Most of the
compression comes from the "lossy" part, which saves space by reducing the
precision of the stored information.  For example, if an audio sample in the
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

