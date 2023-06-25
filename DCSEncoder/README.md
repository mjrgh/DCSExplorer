# DCSEncoder

DCSEncoder is a compiler for DCS audio ROMs.  It lets you create
entirely new DCS ROMs with original audio material,
and it can also patch existing ones, letting
you substitute new, custom audio for selected tracks.

Compiling a new ROM set requires several inputs:

* An existing ROM set from a DCS game, to serve as a prototype for the
new ROM set.  This is provided in the form of a PinMame .zip file.

* A script describing the track programs making up the new ROM set.
When patching an existing ROM, this just specifies what you wish to
change.  The script language is described [below](#Scripting).

* Audio files for the new sounds you wish to include in the ROM.
These can be provided as MP3, Ogg, WAV, and FLAC files.  The compiler
will transcode these files to the DCS format for storage in the new
ROM set.  The script specifies how the audio files are mapped to the
pinball game's audio commands.

<style type="text/css">
indent {
    display: block;
	margin-left: 1em;
}
</style>

## Program options

DCSEncoder is a command-line program.  On Windows, you must run it
from a CMD.EXE window.

The syntax is:

*path*\dcsencoder *options* *prototypeRomFile* *scriptFile*

Options:

* -o *outputFile* : specifies the name of the output file.  The output
file name is based on the script file name if this isn't specified.

* -q : quiet mode; reduces the amount of status information displayed
during the script compilation.

* --patch : patch mode; copies the *entire* contents of the prototype
ROM to the new ROM set, including all of the track programs and audio
streams.  The script file can selectively replace tracks and streams,
and can add new ones.  This lets you "mod" an existing game's ROM with
small (or large) customizations, while still keeping the original
material you don't explicitly modify.

* --rom-prefix=*prefix* : set the name prefix for the ROM files in
the output .zip archive.  This will be combined with the chip number
to form the filename.  The default prefix is **snd_**.  Use an
asterisk (*) to use the exact same chip names as in the prototype
ROM (this is useful for creating patched ROMs for use with PinMame).

* --rom-size=*size* : sets the size of the ROM chips; this can be
either 512K or 1M, or an asterisk (*) to copy the size of each
corresponding chip in the prototype ROM (useful when creating a
patch for PinMame).

* --stream-dir=*dir* : adds a directory to the location search list
for stream files.  You can specify this as many times as necessary
to add multiple directories to search.

Note that DCS ROM sizes must be 512K or 1M.  (The Wikipedia page on
DCS incorrectly states that DCS-95 boards could accept 2M ROMs.  A
close look at the schematics shows that they're actually limited to
the same 1M limit as the original boards.  Sorry to disappoint if you
were hoping to create a mod with a gigantic amount of new material.)


## Output file

The output of a successful compilation is a .zip file, constructed in
the PinMame format, containing the new ROM images.  The file is in a
format that can be used directly with PinMame (with restrictions, as
explained [below](#UseWithPinMame)).  Each sound ROM image is
represented as a separate file within the .zip archive, as a simple
raw binary file containing the exact bytes in the ROM, and nothing
else (no headers, no metadata, just the raw ROM bytes).  These files
are suitable for burning into PROM/EPROM chips for use in an
original DCS sound board.


## Prototype ROM

One of the required inputs to the compiler is a "prototype" ROM set.
This is simply the ROM set for an existing DCS pinball machine, which
serves as a prototype for the new ROM set.  This is required even if
you're creating an entirely original ROM set, because the prototype is
the source of the ADSP-2105 control program used in the generated ROM
set.  (The DCSEncoder project doesn't include any new ADSP-2105 code.
Everything DCSEncoder generates is just audio data for use with the
well-tested and highly stable software from the original machines.)

The prototype ROM is provided in the form of a PinMame .zip file.
PinMame ROM sets for all of the DCS titles are available on
the Web.  Most of the popular pinball sites host downloads
or a least link to them; if you're not sure where to start,
try [vpforums.org](https://vpforums.org/).

The primary function of the prototype ROM set is to provide the
ADSP-2105 control program that runs on the DCS hardware.  The compiler
will read the control program from the prototype ROM you provide and
copy it into the new ROM.  This ensures that the new ROM contains a
working control program that will run on physical DCS boards, as well
as in the PinMame emulator.

For reasons that should be obvious, any new ROM set you create in this
manner is for your own personal use only.  It contains a copy of the
original DCS control program, which you can't distribute to anyone
else unless you have a license from the original copyright owner.

If you're patching an existing ROM (that is, creating a new ROM that
mostly preserves the contents of an existing ROM, with selective
changes specified in your script), the prototype ROM is simply the
original ROM you're patching.  The original ROM won't be altered in
the patching process; the "patched" copy is a whole new file written
to the output file name you specify.

If you're creating an entirely new ROM set, you can use any game's ROM
as the prototype, with one restriction: if you're planning to install
the generated ROMs in a physical pinball machine, the prototype ROM
must come from the same DCS board generation as your target machine,
where you'll be installing the ROMs.  It doesn't have to come from the
same game - just from any game that was based on the same version of
the boards, either the original 1993 audio-only boards or the 1995
audio/video boards.  If you're not sure which version your target game
uses, consult the list of games at Wikipedia:
[https://en.wikipedia.org/wiki/Digital_Compression_System](https://en.wikipedia.org/wiki/Digital_Compression_System)

If you're targeting the original 1993 audio-only boards, you should
use one of the 1994 or later games as your prototype.  The three 1993
releases (*Indiana Jones*, *Judge Dredd*, and *Star Trek: The Next
Generation*) used an early and inferior version of the audio decoder.
The DCS engineers completely overhauled the encoding format in 1994,
and all later games used that new format, which tends to yield
significantly better compression ratios at higher quality than the old
format.  The only reason to use one of the 1993 games as a prototype
is if you just want to patch that game; when creating a new ROM set,
it's better to use a 1994 or later ROM so that you can use the newer
encoding format.

## Script syntax cheat-sheet

Here's a quick list of all of the scripting language syntax, in the
form of an example or two of each construct.  To keep it brief, there
are only some terse comments by way of explanation, so this might look
baffling if this is your first time through this document.  Full details
are in the sections below, so just skip the cheat sheet if you're new
to this and go straight to the detailed documentation that follows.

```
// Two slashes start a comment

Signature "My new DCS ROM, <date>";

Default encoding parameters (
    Type=*, Subtype=*, BitRate=128000,
    PowerCut=97, MinRange=5, MaxError=5);

Stream MainTheme "theme-music.mp3" (BitRate=96000);
Stream AltTheme replaces $2010CA "alt-music.ogg";   // replaces imported stream, for --patch use

Var X;
Var Y : 2;   // sets variable ID, for use with --patch

Deferred Indirect table Countdown ($0071, $0072, $0073);
Deferred Indirect table NewTable : 2     // sets table ID to 2, for --patch use
    ($00A0, $0102, $0207, $00A1, $00A4);

Track $0005 channel 0 Defer($0007);
Track $0006 channel 0 Defer Indirect(Countdown[X]);

Track $0000 channel 0 {
   Wait(1) Stop(*);   // stop all of the OTHER channels, waiting 1 frame before each one
   Stop(0);           // stop channel 0 (the current channel, so also immediately ends this track program)
}

Track $0007 channel 0 {
   Wait(10);       // wait 10 DCS frames
   Wait(1 sec);    // wait 1 second
   Wait(16ms);     // wait 16 milliseconds
   Wait(forever);  // infinite wait - stops the script here

   SetMixingLevel($70);  // set the mixing level to almost full volume
   Play(MainTheme);      // plays a stream loaded previously with STREAM
   Wait(stream);         // waits for most recently played stream to finish

   Play("trolls.mp3" (bitrate=96000));  // use custom encoding parameters for this audio clip
   Loop (3) {                           // loop three times over the enclosed block of instructions
      Wait(1 sec) WriteDataPort($80);   // send byte $80 to the data port once per second on each loop

      Loop (2) {
        // loops can be nested within other loops
      }
   }
   Wait(stream);

   Queue($000A);   // queue track $000A for playback as though the WPC host sent command code $00 $0A

   SetVariable(Var X, Value 3);   // set variable X = 3
   StartDeferred(Channel 1);      // start the pending deferred track on channel 1, if any
};

Track $0120 channel 1 {
   SetMixingLevel(channel 0, decrease $40, steps 0.5 sec); // reduce main music mixing level by $40 over 1/2 second
   SetMixingLevel($70);                                    // set full volume on my own channel
   Play(Stream "extra-ball.wav");                          // play the stream
   Wait(stream - 0.5 sec);                                 // wait until 1/2 second before end of stream
   SetMixingLevel(channel 0, increase $40, steps 0.5 sec); // fade the main music mixing level back to where it started
};
```


## <a name="Scripting"></a> Script file

The instructions for creating a new ROM are listed in a script file,
which is a text file containing programming instructions for the new
ROM set.  The script lets you specify what each "track command" should
do in terms of audio playback.

First off, any scripting language needs a way to write comments.
Our little scripting language uses `//` comments in the style of C++.

```
// A line starting with two slashes is a comment

track $0001 channel 0 defer($0010);  // Comments can also follow script code on the same line
```

Whenever you need to specify a number, the default is to treat it as
an ordinary decimal value.  It's also sometimes convenient to use
hexadecimal numbers, which you can write by starting the number with a
dollar sign, or with the C/C++ `0x` prefix.  (C-style octal numbers
are deliberately **not** included; I don't think anyone has used octal
since the 1970s, and C's octal notation could actively create
confusion by changing the interpretation of what would look like an
ordinary decimal number to anyone who's not steeped in C syntax.)

```
10    // this is the decimal value 10
$10   // hexadecimal 10, equal to decimal 16
0x1F  // hex 1F = decimal 31
```

In some contexts, you can also specify fractional values.  These are
always in decimal, using the same format as in C++, which is also
pretty much the ordinary way you'd write it on paper.  (It's ordinary
if you ignore the C++ format's scientific notation with E, anyway.
The E notation is accepted here, too, but I doubt anyone will ever
have occasion to use.)

```
1.25   // decimal value one-and-a-quarter
```

Names and keywords in the scripting language are all case-insensitive,
meaning you can freely mix upper and lower case, and the compiler will
ignore all that and treat everything as though it had been written in
all upper-case.  For example, the STREAM keyword can be written as
STREAM, Stream, stream, or sTrEaM.

Apart from comments and blank space, everything in a script is a
"command" that specifies something that goes into the ROM.  Every
command ends with a semicolon, `;`.  Spaces and line breaks within
a command are ignored, so you can split a command up over several
lines when that helps readability.

The commands you can use in a script are listed below.
We use some common syntax conventions in the command listings:

* When a portion of a command is enclosed in [square brackets], it
means that that part is optional.  The one exception is in the
Deferred Indirect statement, where the square brackets are entered
literally; they're shown in boldface in that context to so indicate.

* A comma followed by "..." indicates a list of items with any number
of entries.  **Do** enter the comma literally, to separate one element
from the next, but **don't** enter the "..." literally - that's just
meant as a placeholder for "add more entries here as needed".

* Most other punctuation should be entered literally, including
parentheses, commas, curly braces, semicolons, and quote marks.

Here are the commands...

**Default encoding parameters (** *name*=*value*, ... **);**

<indent>
Changes the default settings for audio file compression.  The
default settings are used in subsequent STREAM commands and PLAY
program steps that import audio files.  See [Encoding Parameters](#EncodingParams).
Use commas to separate parameters if you're specifying more than one.
(Don't enter the "..." at the end of the list literally - that's just
a placeholder meaning "add more *name=-value* pairs as needed".)
</indent>

<indent>
Example: `Default encoding parameters (type=*, BitRate=96000);`
</indent>


**Deferred indirect table** *symbolic-name* [**:** *index*] **(** *track-number*, ... **);**

<indent>
Defines a [Deferred Indirect table](#DeferredIndirect).  The
symbolic name is an arbitrary label you assign to the table,
which you can use to refer to the table in a **Defer Indirect()**
track definition.  As with all symbolic names, it must start
with a letter of the alphabet or an underscore, and can contain
any mix of letters and numbers and underscores after that.
Upper and lower case can be used interchangeably, and case
differences are ignored when matching the name.
</indent>

<indent>
The list of track numbers lists
the tracks that the table selects, in order of index number,
starting at index 0.
</indent>

<indent>
The optional index assignment lets you specify the numeric index
(starting at 0) assigned to the table.  If you leave this out, the
compiler assigns the table's index number automatically.  The DCS
ROM program itself doesn't know about the symbolic name - everything
just has a number as far as it's concerned - so this lets you specify
the exact number that the DCS ROM uses for the table.  The only reason
you'd ever want to use this is if you're using the `--patch` feature
to modify an existing ROM, and that ROM already has its own
Deferred Indirect tables, and you want to replace one of the existing
tables with your own.  Assigning an index explicitly tells the compiler
to discard the old table imported from the old ROM and replace it with
your shiny new table.
</indent>

<indent>
Suppose you define a table like so:
</indent>

<indent>
`Deferred indirect table Countdown ($0070, $0071, $0072, $0073);`
</indent>

<indent>
This defines a table named **Countdown** with four track entries:
track number $0070 at index 0 (the "$" prefix means that the number is
expressed in hexadecimal, base-16), track $0071 at index 1, track $0072
at index 2, and track $0073 at index 3.  You can now refer to the table
in a track definition:
</indent>

<indent>
`Track 5 channel 0 Defer Indirect(Countdown[Timer]);`
</indent>

<indent>
That Track 5 definition tells the decoder that a $0005 command
from the WPC host doesn't play anything immediately, but just
queues up **Countdown[Timer]** as the deferred track on channel 0.
That stashes `Countdown[Timer]` in a secret internal memory
location in the decoder, awaiting a **StartDeferred(channel 0)**
command.  As soon as that command is executed in an active track program,
the decoder goes back to the secret memory location, finds
the stashed deferral information, and selects a track from the
table.  The track selected depends upon the current value of
the variable **Timer**, as set by a **SetVariable(var Timer)**
command in a track program.  Whatever the last setting was,
it's used to select a track from the table.
</indent>


**Signature** "*string*"**;**

<indent>
Sets the signature string for the first ROM chip (the one labeled
U2 or S2 on the physical DCS circuit board).  The original DCS ROM
contents included a block of text at the start of the first ROM
that informally identifies the ROM.  For example, the *Medieval Madness*
sound ROM starts with the text `Medieval Madness AV (c) 1997 Williams - DWF`.
The **signature** command lets you specify the text to use in that
location for the generated ROM set.  The text can contain any
ordinary ASCII characters.  The length limit is 75 characters.
(This is the space allowed by the structure of the DCS ADSP-2105
boot program, which is also stored at the beginning of the U2
ROM chip).
</indent>

<indent>
If you include the literal text **&lt;date&gt;** in the string,
it's replaced with the build date (your computer's system date
at the time you run the compiler) in MM/DD/YYYY format, as in
01/31/2023.
</indent>

<indent>
Example: `Signature "My new DCS ROM, <date>";`
</indent>

**Stream** *symbolic-name* "*filename*" [**replaces** *address*] [**(** *param=value*, ... **)**]**;**

<indent>
Reads an audio file and encodes it into the DCS format for storage
in the generated ROM set.   The stream is assigned the given symbolic
name, which you can then use in a **Play()** command within a track
program to load and play the stream.  A **Play()** command can also
load a stream directly from a file, without bothering with a separate **Stream**
command.  The point of the **Stream** command is that it lets you load a
stream once and then use it in several **Play()** commands without
having to specify the filename and encoding parameters again each time.
</indent>

<indent>
The source file can be in any of the supported
mainstream audio formats (MP3, Ogg Vorbis, WAV, FLAC), or in DCS Explorer's
raw exported DCS stream format.  An exported DCS stream lets you reuse an
audio clip from an existing DCS ROM in a newly generated ROM set without any
trans-coding losses, as long as the source game and target ROM set are
based on the same DCS format version.
</indent>

<indent>
The optional section in parentheses at the end lets you specify special
encoding parameters just for this stream, overriding the default
encoding parameters currently in effect.
</indent>

<indent>
The **replaces** clause applies only if you're using the `--patch` mode to
create a patched version of an existing game's sound ROMs.  This option
replaces the selected stream from the prototype ROM with the audio clip
from the file.  All of the original ROM's track programs that reference
the stream will use the new stream instead.  DCS ROMs don't contain any
metadata that assigns symbolic names to streams, so we have to resort
to identifying the stream by its numeric address.  The best way to find
the address of a given stream is with DCS Explorer's `--programs` option,
which lists all of the track programs in a ROM.  The track programs
correspond to the command numbers that the WPC host sends to trigger
sound effect playback, so as long as you know the command number that
plays the sound you want to replace, you can look it up in the
`--programs` list, and find the **Play()** command within that
track program.  (There might be more than one, since many tracks play
back music or effects that are divided into multiple stream objects.)
The **Play()** command contains the address of the stream it plays,
which is the same address you use in the **replaces** clause.  DCS
Explorer shows track addresses in hexadecimal (base-16), which you
can enter in the script with a **$** (dollar sign) prefix.  For
example, `stream MainTheme " replace $102A7E;`.
</indent>

<indent>
Example: `Stream MainTheme "main-theme.mp3" (BitRate=96000);`
</indent>

**Track** *track-number* **channel** *channel-number* **{** *program-steps* **};**

<indent>
Defines a track program.  When the WPC host sends the same command
number as the *track-number* to the sound board, the sound board
immediately loads the program steps into the specified channel
and starts executing them.
</indent>

<indent>
Note that the {curly braces} around the program steps are entered
literally.
</indent>

<indent>
See [Track Programss](#TrackPrograms) for details on the
contents of the track program.
</indent>

<indent>
Example:
</indent>

<indent>
```
Track $0021 channel 0 {
   SetMixingLevel($70);
   Play("beep.mp3");
   Wait(stream);
};
```
</indent>

**Track** *track-number* **channel** *channel-number* **Defer(** *deferred-track-number* **);**

<indent>
Defines a deferred track command.  When the WPC host sends the
same command number as the *track-number* to the sound board, the
sound board stashes the *deferred-track-number* into a special
memory location that records the pending track for the channel.
This then sits dormant until some other active track program
executes a **StartDeferred** command targeting the same channel.
That retrieves the pending deferred track information from the
special memory location, and starts that track running.
</indent>

<indent>
Example: `Track $0051 channel 0 defer($0080);`
</indent>

**Track** *track-number* **channel** *channel-number* **Defer Indirect(** <i>table-name</i><b>[</b><i>var-name</i><b>]</b> **);**

<indent>
Note that the [square brackets] after the table name are entered literally.
</indent>

<indent>
Defines a [Deferred Indirect](#DeferredIndirect) track.  When the
WPC host sends the same command number as the *track-number* to the
sound board, the sound board stashes the <i>table-name</i><b>[</b><i>var-name</i><b>]</b>
information in a special memory location recording the pending
track for the channel.  This sits dormant until some other active
track program executes a **StartDeferred** command targeting the
same channel.  That retrieves the pending deferral information
from the special memory location, and starts the selected
track executing.
</indent>

<indent>
We call this type of deferral "indirect" because
it doesn't specify the new track number directly.  Instead,
it specifies a Deferred Indirect table, which must have been
previously defined with the **Deferred Indirect Table** command,
<i>and</i> the name of a variable, which must have been previously
defined with a **Var** command.  When a **StartDeferred** command
eventually triggers the deferred track, the ROM program gets the
current value of the variable, as set by the latest **SetVariable**
track program command that affected the variable, and uses the
number stored in the variable as the index into the named table.
The two pieces of information combine to determine the track
that's finally loaded.
</indent>

<indent>
Example: `Track $0050 channel 0 Defer Indirect(Countdown[timer]);`
</indent>

**Var** *variable-name* [**:** *index*] ,... **;**

<indent>
Defines one or more variable names for use in [Deferred Indirect track loading](#DeferredIndirect).
The names are arbitrary labels that you assign to
the variables.  In the final ROM set, variables are simply numbered,
but it's easier for human readers to keep track of these things if
you give them a meaningful name.
</indent>

<indent>
A variable name has to start with a letter of the alphabet or an
underscore character (_), and can contain any mix of letters and
numbers and underscores after that.  As with almost everything else
in the scripting language, upper and lower case are interchangeable;
if you name a variable **X**, you can refer to it later as **X**
or **x** equally well.
</indent>

<indent>
The optional **:**<i>index</i> part lets you specify the numeric
ID that the variable uses.  The DCS software itself doesn't use
variable names at all; every variable just has a numeric ID in the ROM.
This clause lets you assign a specific number.  If you don't
assign an index explicitly, the compiler will assign one for
you automatically.  The only time you'd ever want to assign an
index  yourself is when you're using the `--patch` option to modify an
existing ROM, and that ROM already has one or more Deferred
Indirect tables with associated variables.  In that case, you
might want to refer to the same variables that are already
used in the original ROM.  The index assignment lets you refer
to these existing variables in the script, using names you
assign as aliases for the original numbering.
</indent>

<indent>
Note that *var : index* does **not** assign a **value** to the
variable.  Variables in a DCS program always start with the
initial value 0, and there's no way to change that.
It's just the way the original DCS ADSP-2105 control program works.
This syntax only sets the variable's internal ID number that's
used within the final ROM.
</indent>

<indent>
Exaample:  `Var Timer;`
</indent>


## <a name="TrackPrograms"></a> Track Programs

DCS was designed from the start as a sound controller for video games and
pinball machines.  This makes it a little different from mainstream audio
formats like MP3.

In a regular audio format, the basic unit of functionality is simply a
chunk of audio, such as a song from a CD, that plays back linearly
from start to finish.

DCS does have simple audio clips like that, which we call "streams",
but that's not its main functional unit.  The main unit that DCS works
with is what we call a "track program".  A track program is a
miniature procedural computer program, written in a special-purpose
language that the DCS designers invented, that can carry out simple
operations such as playing audio clips, pausing, looping, and sending
data to the WPC host.  When the WPC host sends a command to the sound
board, the sound board finds the track program whose track number
matches the command code, and starts executing the track.  For
example, if the WPC host sends the sound board the two-byte sequence
$01 $40, the sound board loads track $0140.  (Commands that load tracks are
always two bytes long like that, and they're always interpreted
in that order.)

The track programming language as implemented in the DCS ROM software
is similar to machine code, in that it's a series of byte codes that
carry out simple operations.  The compiler doesn't require you to
learn the numeric codes, though; instead, it provides a symbolic
language that looks more like a regular programming language.

In the script, a track program is defined by a **Track** command:

```
Track $0020 channel 0 {
   // the program steps go here
};
```

When the track is executed, the program steps within the braces are
carried out sequentially.  After the last step has been carried out,
the track program ends, and any audio playing on the channel is
stopped.

### Waits

By default, the DCS software runs through all of the steps as fast as
it can, but you can control the timing by adding a **Wait()** prefix
before any instruction.  You can specify a wait time in frames,
seconds, or milliseconds:

```
Wait(10);           // waits for 10 DCS frames
Wait(1 sec);        // waits for 1 second of real time
Wait(1.5 sec);      // waits for a second and a half (you can use fractional seconds)
Wait(20ms);         // waits for 20 milliseconds
```

The fundamental time unit in DCS is the "frame", which is exactly
7.68 milliseconds of real time.  That's how long it takes to play back 240 PCM
samples at 31,250 samples per second, which is the clock rate that DCS
uses for its audio output, and which controls all aspects of timing in
the DCS boards.  If you specify a wait in terms of seconds or
milliseconds, it's rounded to nearest increment of 7.68ms, since DCS
can't do anything at finer time divisions than one frame.

In addition to a timed wait, you can tell DCS to simply never move
past a particular point:

```
Wait(forever);
```

This sets up an infinite wait, which prevents the track program from
progressing past this point.  This is useful when you want to set up
an infinite music loop.  Note that an infinite wait doesn't mean the
channel is stuck forever playing this one track, because the old
track will be removed as soon as the next WPC command comes along
that loads a new track into the same channel.  `Wait(forever)`
only stops the track program itself from proceeding - it doesn't
block new tracks from being loaded into the channel.

Finally, you can tell DCS to wait for the most recently played audio
stream to finish, or to come within a specified interval of finishing:

```
Wait(stream);          // waits for the stream from the last Play() to finish
Wait(stream - 1 sec);  // waits for the stream to get within 1 second of finishing
```

This form of wait lets you time the next event to happen exactly at
the end of the last played stream.  This is useful for stringing
together multiple streams that were created as fragments of a larger
whole, since it lets you time the start of the next fragment at
exactly the moment when the previous fragment ends.  Stream waits
are also useful for simple track programs that do nothing more than
play back an audio clip; for such tracks, you should always use
an idiom like this:

```
Track $0021 channel 0 {
    SetMixingLevel($70);   // always set the mixing level for a new track
    Play(MainTheme);       // start an audio clip
    Wait(Stream);          // ...and wait for it to finish before exiting
};
```

Always remember to use a `Wait(Stream)` at the end of a simple
single-clip playback track like this, because if you don't include the
Wait, the track program will stop immediately before any audio from
the clip gets played.  Remember, the DCS program executes each step in
the program immediately in the absence of Wait directives, and it
clears the track program *and* its audio stream as soon as the last
instruction in the track finishes.  The `Wait(stream)` ensures that
DCS keeps the track around until the clip has played in full.

Note that DCS itself doesn't have any concept of `Wait(stream)`.  The
byte code language that the ROM uses internally can only express
concrete wait times in terms of DCS frame counts.  The compiler
automatically translates stream waits into frame counts, based
on the size of the stream in the last `Play()` command.

You can think of `Wait` as a separate programming statement, but in
the DCS byte code language, it's actually a prefix that's applied to
every instruction.  The scripting language syntax reflects this,
by letting you specify `Wait()` as a prefix to any other statement,
with no intervening semicolon:

```
Track $0022 channel 1 {
   Stop(0);
   Wait(1) Stop(2);
};
```

It doesn't make any practical difference whether you look at Wait as a
prefix or as a separate statement in its own right, but as far as the
byte code language is concerned, every step has a Wait attached to it.
The compiler lets you ignore this, though, by automatically assuming
`Wait(0)` for any step that doesn't have an explicit wait attached.

Another internal detail that the compiler hides for your convenience
is that the byte code language has limits on how long a Wait can be,
but the compiler lets you specify any wait time you want, by splitting
a jumbo Wait into multiple instructions as needed.  The byte code
language only allows an individual Wait to be up to 65534 frames, but
you don't have to worry about this limit when using the compiler.

### Loops

Track programs can include looping sections.  Loops are quite simple:
they can repeat a specified number of times, or can repeat forever.
(As with infinite waits, infinite loops aren't *truly* infinite; they
only last until the track program is interrupted by some outside
event, such as a command from the WPC host that loads a new track
into the same channel.)

To write a loop, use the keyword `Loop` followed by the number
of repeats in parentheses.  If you don't include a repeat count,
you create an infinite loop.  The contents of the loop go in
curly braces after the `Loop` keyword and repeat count.

```
Loop (5) {   // repeats the contents of the loop 5 times
   // one or more statements go here
}

Loop {       // infinite loop (because it has no repeat count)
   // statements go here
}
```

The loop can contain any series of program statements, including
nested loops.  The statements within the loop can have Wait times
attached as normal.


### Program statements

**End;**

<indent>
Marks the end of the track program.  When execution reaches this
point, the track program exits, and any audio playing on the same
channel is stopped.
</indent>

<indent>
The `End;` statement is optional.  If you don't include one in
a track program, the compiler automatically acts as though one
is placed immediately before the program's closing curly brace.
If you do supply an `End;` statement, it must be the last step of
the track program, since nothing past that point can ever be
executed.
</indent>

**Play(Channel** *channel-number*, **Stream** *stream-name*, **Repeat** *count* **);**

<indent>
Plays an audio stream on the specified channel, for the specified
number of repeats.  The channel can be omitted if you want to play
the stream on the same channel that the track program itself uses.
The repeat count can also be omitted if you only want to play the
track once:
</indent>

```
Play(stream MainTheme);  // play one time on the track program's channel
Play(MainTheme);         // you can leave out the STREAM keyword when it's the only parameter
```

<indent>
The stream can be the symbolic name of a stream previously defined
with the **STREAM** command, or it can be a string giving the name
of a sound file to play.  If you specify a sound file, the compiler
reads the file and transcodes it to the DCS format.  This is more
convenient than using a separate **STREAM** command if you're only
using a particular audio file in one **Play()** in one track
program.  If you're using the same audio file more than once,
though, you should import it with a **STREAM** command so that
the same memory can be reused in each reference.
</indent>

```
Play("main-theme.mp3");
```

<indent>
When you specify the stream as a sound file, you can
optionally include custom encoding parameters that
override the defaults for this one stream.  Place the new encoding
parameters in parentheses after the filename. Use the same *name=value*
pairs as in a **Default encoding parameters** or **STREAM** command.
See [Encoding Parameters](#EncodingParams) for a list of settings.
</indent>

```
Play("main-theme.mp3" (bitrate=96000));
```


**Queue(Track** *track-number* **)**

<indent>
Queue a track for execution.  This essentially pretends the the WPC
board just sent a command matching the specified track number.  It
loads the track into its channel and starts it executing.  If the
track uses one of the Deferred modes, DCS sets up the deferred
track the same way it would if the WPC board had sent the command.
</indent>

<indent>
The compiler lets you omit the **Track** keyword, since it's the
only parameter for this command.  `Queue($0004);` means the same
thing as `Queue(Track $0004);`.
</indent>

```
Queue($0004);   // pretend that the WPC host just send command code $0004` <br>
Queue(Track $0102);
```

<indent>
We call this command **Queue()** because it doesn't actually start
the new track program instantaneously.  Instead, it inserts the
track number into a memory location where incoming commands from
the WPC board are stored as they arrive and await processing.
That's what we mean by "queue"; the new track is lined up for
processing the next time DCS goes through its inbox.  This does
have one important practical implication: even if the new track
takes over the current track program's channel, the current track
program isn't interrupted immediately.  It continues executing
subsequent program steps until either reaching the end of the
program, or reaching a **Wait()**.  A **Wait()** gives DCS a
chance to process incoming commands, at which point the queued
track will take effect and will interrupt the current track
program if it's on the same channel.  If the new track is on
a different channel, or uses one of the Deferred modes, the
current program will go on running even after a **Wait()**.
</indent>

**SetChannelTimer(Byte** *number*, **Interval** *frame-count* **);**

<indent>
This command can only be used with the 1993a software shipped with
<i>Indiana Jones</i> and <i>Judge Dredd</i>.  It can't be used
with any of the later ROM versions.
</indent>

<indent>
This command has two effects.  First, it immediately sends
the given byte value (a number from 1 to 255, or $FF in hex) to
the data port, the same as **WriteDataPort()**.  Second, it sets
up the track program's "channel timer" to *repeatedly* send
the same data byte at the specified interval, measured in
DCS frames (one frame equals 7.68 milliseconds).
</indent>

<indent>
If the byte value is zero, **or** the interval is zero, **or**
you simply omit the interval, the command **clears** the timer
instead of setting the timer.  This stops any repeating data
send that was previously scheduled for the channel.  If the
byte value is non-zero and the interval is zero, the command
clears the timer but still sends the byte value once,
immediately when the command is executed.
</indent>

<indent>
We call this feature a "channel timer" because there's a
separate timer like this for each channel, and all of them
can be programmed independently.  The command can only set
the timer for its own channel (the one that the current
track program occupies).
</indent>

<indent>
Curiously, the channel timer mechanism is implemented in every
version of the DCS software, but this command to set it up is
only available in the 1993a software.  In the later software,
the command is replaced with **WriteDataPort()**,
so you can no longer access the channel timers.  As a rule,
programmers don't usually bother deleting old code even after
it's no longer usable, and we seem to have a case of that
pathology at work here.
</indent>


**SetMixingLevel(Channel** *channel-number*, **Level** *number*, **Steps** *interval* **);**<br>
**SetMixingLevel(Channel** *channel-number*, **Increase** *number*, **Steps** *interval* **);**<br>
**SetMixingLevel(Channel** *channel-number*, **Decrease** *number*, **Steps** *interval* **);**

<indent>
Sets the mixing level on the specified channel.  You can set the
level to a specific value with the **Level** keyword, or you
can adjust it relative to its current level with the **Increase**
and **Decrease** keywords.  The **Channel** can be omitted
if you want to set the level for the current channel (the one
that contains the track program).
</indent>

<indent>
The level is a number from -128 to +127.  Anything zero or
below is silence; +127 is the full reference volume.
In most cases, it's better not to turn the volume all
the way up to +127, since that doesn't leave any room to add
in other sounds that might be playing at the same time in
other channels.  A setting around $70 seems to be a good
"full volume" level in most cases.
</indent>

<indent>
The reason that negative settings are available is that
multiple channels can all change the level in *another*
channel.  When this happens, the contributions from the
different channels are combined by adding them together.
That makes negative values useful in some cases, since
they can cancel out positive values added by other channels.
</indent>


<indent>
The level scale is roughly logarithmic, which makes it map
pretty well to the way humans intuitively perceive audio volume.
In other words, the level sounds like it's adjusting linearly as
you move across the 0-127 scale.
You can think of the 0-127 scale as a volume knob with notches
marked from 0 to 127 rather than the standard 0 to 10.
</indent>

<indent>
The optional **Steps** parameter lets you specify a "fade" time
for the change.  If this is included, the level is smoothly adjusted
from the current level to the new level over the given interval.
The **Steps** interval can be specified as a number of DCS
frames (one frame is equal to 7.68 millisecond), or as a time
parameter with the suffix **sec** for seconds or **ms** for
milliseconds.
</indent>

```
// this does a 1-second fade-in from silence
SetMixingLevel(Level 0);                 // set to silence, then...
SetMixingLevel(Level $70, Steps 1 sec);  // adjust to $70 over 1 second

// now play a stream and wait for it to finish, leaving 1 second at the end
Play("main-theme.mp3");
Wait(stream - 1 sec);

// and finally fade back out to silence at the end
SetMixingLevel(Level 0, Steps 1 sec);
Wait(stream);
```

<indent>
It's important to note that the **Steps** interval doesn't
trigger a pause in the track program.  The fade is handled
asynchronously by the DCS audio mixer, so the timing has no
impact on the track program execution.  In the example above,
note how we set up a fade first, and then start playing the
track.  The **SetMixingLevel()** command schedules the fade
with the audio mixer, but program execution immediately
proceeds to the **Play()** command, so the audio track starts
playing.  As playback proceeds, the audio mixer gradually
adjusts the mixing level according to the scheduled fade.
</indent>

<indent>
**Setting the mixing level on other channels:**  The most
common way to use the mixing level is on your own channel,
as in the example above, to control the mixing level for
an audio clip managed by the same channel program.  But
a track program can also adjust the mixing level in other
channels.  The original DCS ROMs often use this feature to
bring a sound effect to the "foreground" by temporarily
turning down the volume on the main music track:
</indent>

```
Track $0140 channel 2 {
    // turn down the main music track while the effect plays
    SetMixingLevel(channel 0, decrease $50, Steps 0.5 sec);

    // set our own level to roughly full volume and play the clip
    SetMixingLevel(level $70);
    Play("extra-ball.mp3");

    // restore the main music track volume
    Wait(stream - 0.5 sec);
    SetMixingLevel(channel 0, increase $50, Steps 0.5 sec);

    // let our clip finish before we exit
    Wait(stream);
};
```

<indent>
The DCS mixer has a nice scheme for handling these
cross-channel level adjustments.  When the Channel 2 programs
sets the mixing level on Channel 0, it doesn't just go in and
squash the original Channel 0 level.  Instead, the mixer keeps
track of a whole matrix of channel-by-channel adjustments,
and combines them together to get the final level for each
channel.  That's very useful in a real-time, event-driven
environment like a pinball game, because it frees us from
worrying about weird interactions if Channel 3 also tries
to adjust the main music track at the same time that our
Channel 2 program is running.  The mixer keeps track of
the two adjustment separately, and makes sure that
everything is set back to normal at the end.
</indent>


**SetVariable(Var** *name*, **Value** *number* **);**

<indent>
Note: this command is only allowed in the 1994 and later
software. 
</indent>

<indent>
Sets the value of a variable, previously defined with a **VAR**
statement, to the given numeric value, from 0 to 255 ($FF hex).
The variable can also be specified as a numeric ID, from 0
to 79, but it's better to use a symbolic name for readability.
</indent>

```
SetVariable(Timer, 3);
```

<indent>
This is used with the Deferred Indirect mechanism to select
which table entry will be used when a deferred track based on
the specified variable is triggered.
See [Deferred and Deferred Indirect Tracks](#Deferred) for more
information on how to use this feature.
</indent>


**StartDeferred(Channel** *channel-number* **);**

<indent>
Triggers the pending deferred track on the given channel.  If
the channel doesn't have a pending deferred track, the command
has no effect.
</indent>

<indent>
The channel number must be specified, but you can leave out the
**Channel** keyword if you prefer.
</indent>

<indent>
If the deferred track is on the same channel as the current
track program, the new (deferred) track program immediately
replaces the current one, so execution of the current program
doesn't proceed beyond the **StartDeferred()** command.
</indent>

<indent>
See [Deferred and Deferred Indirect Tracks](#Deferred) for more information
on how to use this feature.
</indent>


**Stop(Channel** *channel-number* **);**

<indent>
Stops all activity on the specified channel number and resets the
channel.  Stops and clears any track program current executing on
the channel, and stops any audio stream playback in progress.
</indent>

<indent>
The compiler lets you omit the keyword **Channel**, since it's
the only parameter for this statement.  So `Stop(2);` means
the same thing as `Stop(channel 2);`.
</indent>

<indent>
When this is used on the track program's own channel, it immediately
ends the track program.  Nothing past this point will be executed.
</indent>

<indent>
Most of the original DCS ROMs define Track $0000 as an "All Stop"
program that executes a **Stop()** on every channel.  It's worth
noting that most of the DCS ROMs got it wrong, by coding it like
this:
</indent>

```
Track $0000 channel 0 {
   Stop(0);
   Wait(1) Stop(1);
   Wait(1) Stop(2);
   Wait(1) Stop(3);
   Wait(1) Stop(4);
   Wait(1) Stop(5);
};
```

<indent>
You can see this pattern repeated in most of the original DCS ROMs
by using DCS Explorer's `--programs` option to show program listings.
Do you see the error?  Note how the program itself runs on channel 0,
and what it does in its first step: it stops channel 0!  That has the
effect of immediately ending the track program, so it doesn't get
a chance to stop any of the other tracks.  I suppose they included
this little program out of habit but didn't actually use it enough
to notice that it doesn't work.
</indent>

<indent>
To simplify the *correct* coding of a similar "All Stop" command,
the compiler lets you use the special syntax `Stop(*)` to
mean "stop all of the *other* channels".  With that syntax, you
could write the program above correctly, and more simply, as:
</indent>

```
Track $0000 channel 0 {
    Wait(1) Stop(*);
};
```

<indent>
Note that the compiler expands `Stop(*)` into a series of Stop()
commands for the individual channels, since the DCS byte code language
doesn't have a "stop all" command of its own.
</indent>


**WriteDataPort(Byte** *number* **);**

<indent>
Writes the given byte value - a number from 0 to 255 ($FF in hex) -
to the data port.  This sends the byte to the WPC host board.  This
allows the sound program to let the main pinball program know about
timed events in the sound playback, or pass requested information
back to the WPC host.
</indent>

<indent>
The **BYTE** keyword can be omitted if you prefer, since it's the
only parameter this statement uses.  `WriteDataPort($05)` is the
same as `WriteDataPort(Byte $05);`.

<indent>
The byte values sent by this command are completely arbitrary.
They don't mean anything by themselves; their only meaning is whatever
meaning the main pinball program running on the host board assigns
to them.  Each DCS game has its own set of ad hoc codes it uses.
Most of the examples in the original DCS ROMs seem to be "events",
letting the WPC program know that a certain part of the music
just played, so that the pinball program can coordinate the timing
of the light show or solenoid action to match the music beats.
In a few cases, the ROMs use a question-and-answer format, where
the WPC host sends a track command that simply replies back with
a byte value.  One use appears to be version queries, where the WPC
board asks the sound ROM for its version number and the
sound board sends back a byte or two in reply.
</indent>

<indent>
If you're sending multiple bytes in a row, you should wait a little
while between bytes to allow the WPC board to process the input.
I'm not sure if there's a fixed rate limit; it probably depends on
the game.  A wait of perhaps 25 milliseconds between consecutive
bytes is probably adequate for any game.
</indent>

<indent>
If you're using one of the first two 1993 games for your prototype
ROM (*Indiana Jones* or *Judge Dredd*), the byte value cannot be
zero.
</indent>


## <a name="Channels"></a> Channels

Almost everything that happens on the DCS sound board happens in the
context of a "channel".  A channel is just an arbitrary unit of memory
on the sound board that can hold a track program and an audio stream,
keeping track of the current playback status of each.

Note that DCS channels **aren't** used for stereo or surround sound!
The DCS pinball boards are strictly monophonic.  The purpose of the
channels is simply to let the game play multiple, independent,
overlapping audio cues at once, such as playing a voice cue at the
same time that the main music is playing.

When audio streams are playing in two or more channels at the same
time, the audio from the active streams is mixed together to form the
monophonic output signal.  The mixing is simply additive.  In
addition, track programs can adjust the relative mixing levels
of their own output or that of other channels, using the **SetMixingLevel()**
program command.  This lets a foreground effect temporarily reduce
the loudness of the background music, for example.

Each DCS program version has its own fixed number of channels
available:

* The early software versions used in the three 1993 releases
support 4 channels, numbered 0 through 3

* The later games have 6 channels, numbered 0 through 5

Each channel can hold one track program at a time, and simultaneously
can hold one audio stream.  The structure of the ROM program suggests
that the DCS engineers thought of the channels this way, as holders of
these two objects, but the two functions could just as well be thought
of as separate.  That is, you could think of there being 4-6 track
program channels, and separately, 4-6 audio stream channels.  The two
functions are really pretty independent.  Even so, I find it easier to
think of each channel as a combined object that holds one track and one
stream, since that seems to map better to the original conception.

Every channel is identical to the rest.  No channel has any special
purpose or special capabilities that's different from any other
channel.

Each channel is independent.  Whatever's going on in one channel has
no effect on any other channel.

If you're creating an original DCS ROM set, you can use channels
in any way you see fit.  If you're patching an existing ROM, you'll
want to examine the way it uses the channels to make sure that your
modifications coexist peacefully with the original channel usage.
The original DCS ROMs almost all use a simple convention:

* Channel 0 is used for the main music track that plays continuously
in the background

* The remaining channels are used for momentary sound effects and
brief music cues that play in response to events in the game action


## Track numbers

Pinball machines from the DCS era have a main control board, known as
the WPC board, and a sound board, which is of course the DCS board
that we're so concerned with here.

The main WPC board is responsible for most of the game control
functions.  It keeps track of the score, displays graphics on the
plasma panel, and controls all of the lights and solenoids.  The
DCS sound board is only responsible for playing music and sound
effects through the speakers.  The sound board doesn't have any
knowledge of the game action; its only job is to play back the
sound effects that the WPC board tells it to, when the WPC board
tells it to.

The two boards communicate with each other through what we call a data
port, which is essentially a primitive network connection.  When the
main pinball controller wants to play back a sound effect, it sends a
command code to the sound board.  When the sound board receives a
command code, it executes the track program that matches the command
code.

In technical terms, a command code is a sequence of two bytes.  For
convenience, we write these as four-digit hexadecimal numbers,
because that's an easy way to show the correspondence between the
numbers and the bytes.  Those hex numbers are what we call the track
numbers, and they're how you identify the tracks in your script file.
When you write a script command defining "Track 1", you're specifying
the sound effects that play when the pinball controller sends command
code $0001 to the sound board.

The tracks in a DCS ROM are numbered from $0000 to some upper limit
that varies by game.  On most games, the upper limit is somewhere
around $07FF, but that can vary a lot.  And on most games, there aren't
nearly as many tracks as that upper limit would imply.  The track
numbers actually used are typically just randomly scattered around
between $0000 and the upper limit.

The track numbers are arbitrary.  They don't have any intrinsic
meaning.  They're just arbitrary ID numbers that the WPC program and
the sound ROM agree upon as a way to identify the various sound
effects.  Looking through a sampling of the original ROMs, I'd guess
that the Williams audio design people had a few informal conventions
for how they assigned track numbers, but they mostly just assigned
numbers as needed.  One convention that's used in most games
is that the lowest-numbered tracks ($0001 through $0010 or so)
are usually used to identify the main music
cues.  Another convention is that related sound effects are
often grouped together in a block of adjacent track numbers.
Another is that Track $0000 was almost always an
"all stop" track that just stopped anything playing on any channels.
Track $0001 was usually the music that would play while the ball was
sitting in the plunger lane waiting to launch, and track $0002 was the
main music theme, but sometimes those two are reversed.  Beyond that,
it's hard to find any rhyme or reason to the layout.  And in any case,
there aren't any rules imposed by the design of the DCS software - it
lets you assign any meaning to any track number.

If you're creating a ROM set for an existing DCS game - for example,
if you want to create a new soundtrack for <i>Medieval Madness</i> -
then the meanings of the track numbers are set in stone by the WPC
control software for that particular game.  The <i>Medieval
Madness</i> WPC program always sends command $0001 to play the
pre-launch-ball music, $0002 to play the main theme, $0004 for the
Trolls music, etc.  Short of reprogramming the whole WPC ROM as well,
you'll need to assign the same track numbers to your versions of all
of the cues.  How do you figure out which track number corresponds to
which music or effect?  Unfortunately, the the DCS ROM format doesn't include
any sort of metadata that assigns names to the tracks, so there's no
systematic way to find out what's in the ROMs.  The DCS Explorer program
is probably your best bet; it lets you see exactly which track numbers contain
anything at all, and lets you experiment with playing back tracks
interactively to find out what each one sounds like.

On the other hand, if you're creating an entirely original game with
its own control program and its own music and sound, you're completely
free to assign track numbers any way you want.


## <a name="EncodingParams"></a> Encoding parameters

One of DCSEncoder's main tricks is that it can create new DCS audio
streams from files stored in standard audio formats like WAV, MP3, and
Ogg Vorbis.

DCS is a "lossy" digital compression format that's similar in design
to mainstream lossy formats like MP3 and Vorbis.  Like those formats,
it achieves a lot of its data size reduction by discarding some of the
detail from the audio signal - that's the "lossy" part.

The format allows for a little bit of variability in how much detail
is discarded, to allow tuning the trade-off between compression size
and signal fidelity on a track-by-track basis.  That's where the
encoding parameters come in: they let you provide some hinting to the
encoder about how much detail it should attempt to keep in the
compressed stream.

The encoder always starts up with a default set of parameters that
have been chosen to work pretty well across a wide range of material,
so you don't have to bother fiddling with the parameters at all if you
don't want to.  If you find that some streams come out too large, you
can adjust the parameters for those streams to reduce the detail level
and hopefully make the compressed results smaller; or if some streams
don't sound clear enough after compression, you can try increasing the
detail level for those streams.

There are two places you can set the encoding parameters.  The first
is the **default encoding parameters** command, which sets the
parameters that will be used for all subsequent streams in the script
that don't specify their own settings.  The second place is in any
command that loads an audio file, where you can specify special
settings that apply just to that single file.  Any parameters you
don't override when loading a file are taken from the current
defaults.

The encoding parameters in either context (**default encoding parameters**
commands or stream loading commands) are specified the same way, with
a list of NAME=VALUE pairs that set the various parameters to the
desired values.  You can specify any combination of the allowed
parameters in any given list; any that you don't specify are simply
inherited from the current defaults.

For example:

`default encoding parameters (powercut=98, maxerror=2);`

Here are the parameters you can set:

* **TYPE**: The stream format type.  The default is '*', which
tries each available option for each stream, and selects the one that
yields the smallest result for that stream.  See [Stream Types](#StreamTypes).

* **SUBTYPE**: The stream format subtype.  The default is '*', which
tries each available option for each stream, and selects the one that
yields the smallest result for that stream.  See [Stream Types](#StreamTypes).

* **BITRATE**: Target bit rate; default 128000.  This is a hint to the
encoder about the upper limit for the bit rate.  It's just a hint, in
that the encoder doesn't make any attempt to maintain a constant bit
rate to match the setting, nor does it guarantee that the bit rate
won't exceed the setting.  The DCS formats are defined in such a way
that the actual bit rate varies from moment to moment, according to
the information content of the audio material.  In most cases, the
actual average bit rate end up lower than this setting, but it depends
on the material; "noisy" material (like radio static or drum solos or
electronic distortion) generally requires more bits.  The DCS formats
tend to sound acceptable at around 100 kpbs, with fairly good quality
at 120 kbps and higher.  The default setting of 128 kbps should result
in good quality for most material.  Lower values will produce smaller
stored streams, at the cost of reduced audio quality.

* **POWERCUT**: The audio power cutoff percentage, 0 to 100, default 97.
You can specify fractional values here (e.g., 97.5).  This
adjusts how much of the "power" of the audio signal the encoder tries
to keep.  The highest frequency portion of the signal above the cutoff
is discarded.  Because this is expressed as a percentage of the total
power, it will automatically keep more high-frequency information for
audio tracks that are biased towards the higher frequencies.  But
if you find that a track is too muted at the high end, you can try
increasing this value.

* **MINRANGE**: The minimum dynamic range required to retain an audio
band; default 10.  This is expressed in the 16-bit integer units used
in the encoding, which range from -32768 to +32767, so each unit
represents one part in 65536 or about 0.0015%.  An "audio band" is a
block of frequency ranges in the signal.  The dynamic range of a band
is the numerical difference between the highest and lowest sample
within the band.  The higher the dynamic range, the more audible the
information in the band is; so a very small dynamic range is
effectively inaudible and can be discarded.  This is applied on a
moment-by-moment (technically, frame-by-frame) basis, not to the whole
stream, so it doesn't mute out a frequency range across the whole clip.
It just throws out bands in sections of the track where they don't
meet the minimum range requirement, in which case they shouldn't
be audible in those sections anyway because they contain so little
sound energy.

* **MAXERROR**: The maximum quantization error; default 10.  This is
in the same 16-bit integer units as MINRANGE.  The DCS encoding stores
each 16-bit number from the original data using a varying number of
bits, from 1 to 16.  When it tries to store a 16-bit number with, say,
only 5 bits, the actual number stored has to be rounded to fit into
the smaller field, so it's only an approximation of the original
value.  It's exactly as though you were asked to write the value of
Pi, but you're only allowed to use 4 digits: so you'd round it to
3.142, knowing that this is a little bit off from the true value.  The
"error" in the rounded value is the difference between the rounded
value and the true original value - in our Pi example, 3.142 minus Pi
is about 0.0004, so the error is about one part in 8,000 of the
original value.  MAXERROR specifies the corresponding tolerance for
error when the encoder "rounds" values from 16 bits to a smaller
number of bits.

The parameters are all "hints" to the encoder.  The DCS format imposes
limits on what the encoding can contain, so it's not always possible
for the encoder to meet the exact numbers specified in the parameters.
When the encoder can't reach a desired threshold, it picks the nearest
available alternative.

The `*` options for TYPE and SUBTYPE slow down the encoding process
slightly, since they require the encoder to compress the stream
multiple times to try each selected format.  The time difference isn't
much, though; the bulk of the computing work in encoding a clip
consists of converting it to the DCS sample rate and translating it
into frequency-domain frames, which only has to be done once even when
multiple format types are tested.


### <a name="StreamTypes"></a> Stream Types

DCS uses several different formats to encode its streams.  The
earliest software version, used for the three DCS pinball titles
released in 1993 (*Indiana Jones: The Pinball Adventure*, *Judge
Dredd*, and *Star Trek: The Next Generation*) use one set of formats,
and all of the later games use a wholly different set.  I call these
the "1993 formats" and the "1994+ formats".  The basic design of the two
systems is similar, but the details of the binary data encoding are
significantly different.  My guess is that the 1993 set was more or
less an early prototype that the engineers threw away and replaced
with something better based on what they learned from that first
attempt.

DCS was designed as a purely embedded system, so they didn't make any
attempt at forward or backward compatibility.  The 1994+ software
doesn't understand tracks recorded in the 1993 formats, and vice versa.
They didn't even bother to provide a way to identify which version a
particular stream uses.  There was simply no need, since the software
and data were always bundled together as a set.  DCSEncoder recognizes
this limitation, and always encodes streams in a format that matches
the software in your prototype ROM set.

In addition to the major schism between the 1993 and 1994+ versions,
each of those versions has a couple of its own sub-formats.  We refer
to these variations as the **Stream Type**.  The internal details are
beyond our scope here, but the point of the sub-formats is to provide
the encoder with some variability in the choice of compression
strategy for each track.  This is useful because compression
algorithms tend to be optimized for particular types of material, so
you can often achieve better results if you have a couple of
alternative algorithms to choose from for each track.

The compiler lets you specify which Stream Type to use via the
encoding parameters.  There are actually two parameters: **TYPE** and
**SUBTYPE**.  In most cases, it's best to leave these set to the
default setting of `*`, which instructs the encoder to try all of the
options for each stream, and pick the combination that yields the
smallest stored stream.  The test is performed on a stream-by-stream
basis, so you'll usually see a heterogeneous collection of stream
types in the final ROM when you let the encoder pick the type.

Probably the only reason to specify an explicit stream type is if you
think the automatic choice based on size resulted in poor audio
quality for a particular track.  In that case, you can manually try
the other format options to see if one of them sounds better.

Here are the available formats by software version:

* 1993a (*Indiana Jones* and *Judge Dredd*)
  * Type 0

* 1993b (*Star Trek: The Next Generation*)
  * Type 0
  * Type 1

* All later games
  * Type 0
  * Type 1 / Subtype 0 (1.0)
  * Type 1 / Subtype 3 (1.3)

"Type 0" in the 1993 software is a completely different format from
"Type 0" in the 1994+ software.  The type codes are always relative to
the software generation, except that the 1993a and 1993b Type 0
formats are identical.

The 1993a DCS ROM software also has its own Type 1 format, which is a
wholly separate format from the 1993b Type 1.  It's not listed above
because DCSEncoder doesn't currently include an encoder for it.  The
DCS engineers apparently didn't think much of this format; it was only
used in one game, *Judge Dredd*, and only for about 10% of its
material, and most damingly, it was purged entirely from the decoder
by the time *STTNG* shipped.  I don't think it's worth the effort to
build an encoder for it given its limited support in the decoders.
The only possible use case for it is to patch *IJ:TPA* and *JD* ROMs,
and it's not even required for that task, since you can use the 1993
Type 0 format instead.

You might wonder what happened to Subtypes 1 and 2 in the 1994+ Type 1
streams.  So do I!  An examination of the code suggests that the DCS
engineers originally intended to implement distinct Subtypes 1 and 2,
but when they wrote the ADSP-2105 program, they made an arithmetic
error that ended up treating Subtypes 1 and 2 the same as Subtype 3.
In fact, a couple of later DCS games even contain streams marked as
Type 1.1 and 1.2, so someone was thinking about it.  DCSEncoder
accepts 1.1 and 1.2 designations, but generates them as type 1.3
streams, since it knows that's how the ROM software will interpret
them.


## Patching an existing ROM

One of the things DCSEncoder can do is "patch" an existing DCS ROM
set, which means that you selectively replace some of the old material
with new material, but keep the rest intact.  For example, you could
replace a game's main theme music track but leave all of the other
sound effects as-is.

The term "patch" might be a little misleading, especially if you're a
computer person, because the customary meaning is to alter a file in
place.  That's not the case here - the original prototype ROM file
isn't modified at all.  The new patched version is written as a whole
new file, to the file you specify as the output file.  We're using the
term simply to mean that you're mostly keeping the original contents
and just making small changes, as opposed to creating a whole new ROM
from scratch.

To patch a ROM, specify the original ROM that you want to patch as the
prototype ROM, and include the `--patch` option on the command line.
Patch mode starts by loading the entire ROM, then compiles the ROM
definitions script.  The ROM definitions can add to the existing
material and selectively replace old material.

* **Track** definitions in the script that assign track numbers already
defined in the ROM replace the corresponding ROM tracks.

* **Track** definitions in the script that assign track numbers not
already contained in the ROM add new tracks.

* A **Stream** definition in the script can replace a specific ROM
stream by using the **replaces** clause, with the numeric address of
the stream in the original ROM.  You can use DCS Explorer to get
a listing of all of the streams in the source ROM; the numeric
stream addresses that DCS Explorer displays are the same ones
to use here.

`Stream MainTheme "mainTheme.mp3" replaces $207C8;`

* You can export an audio stream from one DCS ROM and import it into
another without any transcoding losses by using the DCS Explorer's
"raw" export format.  That creates ".dcs" files, which use a custom
format that stores the raw DCS audio stream as it appears in the ROM.
DCSEncoder can import these via the normal **stream** commands, and
automatically recognizes them as pre-encoded DCS audio that can be
stored back in the new ROM directly without any re-encoding.
(This only works when the source ROM and patch ROM use compatible
versions of the DCS software - either the 1993 software or the
1994+ software.  The stream formats are completely different
between the two versions, so a stream exported from one has to
be re-encoded if imported into the other.  The compiler will
automatically re-encode an imported raw DCS stream if needed.)

* Deferred-indirect tables that don't include explicit index number
assignments will be *added* to the existing ROM.  When an explicit
index number assignment is included, however, the new table will
replace any existing table at the same index.  (As far as patching
original DCS pinball ROMs goes, this doesn't matter one way or the
other, since none of the original DCS ROMs contain any of these
tables.  The feature was never used even once.  But it might become
relevant in the future if you want to patch a ROM that was created in
the first place with DCSEncoder.)

* Variables that don't include explicit index number assignments will
be added as new variables alongside the ones used in the ROM.  An
explicit index assignment can be used to assign a symbolic name to a
specific variable index.  (As with deferred indirect tables, the DCS
ROMs never used this feature, but it could be relevant if you patch a
DCSEncoder-generated ROM.)


## Using generated ROMs with DCSExplorer

The DCS Explorer program can directly load the .zip file that the
compiler produces.  You can check that the file is working as you
intended by opening it in DCSExplorer's interactive mode and trying
the various track commands you programmed with the script.


## <a name="UseWithPinMame"></a> Using generated ROMs with PinMame

The output of the compiler is a .zip file that's constructed using
PinMame's .zip format, so in principle, you can use it directly with
PinMame.  There are some significant restrictions, though.  The
big two are:

* The prototype ROM set must be one that PinMame can load (it has
to be on PinMame's list of known ROMs)

* The new ROM set must have the same number of ROM chips, with
the same filenames and the same sizes as in the prototype

These restrictions apply because PinMame has a hard-coded list of
ROMs it knows about, and it can only load ROMs from that list.
The list includes information on the chip sizes, which have to
match exactly.

If you plan to use your new ROM set with PinMame, you can force it to
use the same chip layout as the original, to the extent possible, by
specifying `--rom-size=* --rom-prefix=*` when you run the compiler.
This is only "to the extent possible", though, because the compiler
might have to create more ROM chips than in the original set, if you
add more audio material than will fit into the same number of ROMs.

If you're comfortable building a custom version of PinMame from its
source code, you can work around these limitations by modifying
PinMame's hard-coded list of ROMs.  The list is part of PinMame's
source code, so the only way to modify it is to download a copy of the
whole source code base and compile a custom build.  The ROM list
entries are scattered all over the code, and are somewhat complex, so
it's beyond what I can do here to explain how to change them.


## Installing DCSEncoder ROMs in a pinball machine

**WARNING!!! Installing DCSEncoder-generated ROMs in a pinball machine
could potentially damage the machine.  INSTALL THEM AT YOUR OWN
RISK.** Remember that this is free software that comes with **no
warranty**.  The author has never used it to create physical pinball
ROMs, and can't offer any assurances that such ROMs will work.

If you want to give it a try anyway, you'll need programmable ROM
chips that are electronically and physically compatible with the DCS
boards, and a device that can program those chips with data files from
a PC.  All of that is outside of my expertise, so I can't offer
instructions here, but it's easy to find information on the Web:
search for **How to program pinball EPROMs**.

Once you have the necessary blank chips and programming equipment, it
should be easy to transfer the DCSEncoder-generated ROM images into
the chips.  Just follow the equipment's procedures to download the
sound ROM image files from the output .zip file to the chips.  The
.zip file contains one file entry per DCS ROM chip, and each file is a
simple raw byte file containing a complete image of the 512K or 1M of
data to store in the ROM.  It should be possible to copy the ROM data
files from the .zip into the physical ROM chips verbatim.  No data
conversions or translations should be necessary.

Note that the .zip file contains other files besides the the DCS audio
ROM files.  It contains unmodified copies of any non-DCS files that
were in the prototype .zip file.  For the DCS board, you can ignore
the non-DCS files - you only need the sound board chips U2 through U9.
(Not all of those chips will necessarily be populated - the encoder
will only generate as many chips as are needed to store all of the
audio data.)  The encoder shows a list of the sound files and
corresponding chip numbers when it finishes generating the .zip file.


## <a name="Deferred"></a> Deferred and Deferred Indirect tracks

Like all of the other DCS internals, I have no idea what the original
in-house terminology at Williams was for this feature, so I made up my
own name for it based on what it appears to do.

As mentioned earlier with the Track command, there are three different
types of tracks:

* A regular program track, which carries out a series of program steps

* A Deferred track, which doesn't play any sounds immediately, but
sets up *another* track command that can be triggered later

* A Deferred Indirect track, which sets up deferred playback using
a table with several alternative tracks to play when triggered

A plain "Deferred" track is simple enough: it just specifies another
track number to load for later playback.  Suppose you program Track
$0123 like so:

`Track $0123 channel 1 Defer($0345);`

When the WPC board sends command code $0123, the sound board sees the
"Defer" code at track $0123, and simply prepares track $0345 for
future playback.  Track $0345 will remain dormant until some other
track program executes a `StartDeferred(channel 1)` command.  At that
point, track $0345 will start playing, just as though the WPC host had
sent a $0345 command across the wire.

The point of the deferred playback mechanism is that it lets you
arrange track changes so that they occur at specially selected points
in the music playback, rather than immediately interrupting the
current music.  This allows for more natural-sounding transitions,
without forcing the WPC host to carefully time each command so that it
occurs at the right point in the music.  The WPC host can send the
deferred command whenever it's convenient, knowing that the sound
board will schedule it at exactly the right point that's coded into
the track program.

Here's an example of how this is used in <i>Medieval Madness</i> (this
isn't just *an* example from <i>MM</i>, but the *only* example from
<i>MM</i>):

```
Track $0001 Channel 0 {     // Pre-launch music (ball in shooter lane)
    SetMixingLevel(level 100);
    Loop {
      StartDeferred(channel 0);
      Play(stream $871C0A);
      Wait(498)
    }
    Wait(Forever) End;
};
Track $0046 Channel 0 Defer ($0002);  // Track $0002 is the main theme music
```

When a new ball is served into the shooter lane, the WPC host sends a
$0001 command to play the "get ready" music while waiting for the
player to launch the ball.  As you can see, this track is an infinite
loop that'll run forever, or until the WPC host sends a new command to
replace it.  Each time through the loop, it tries starting the
deferred track on channel 0, via the `StartDeferred` command.  Now, if
there *isn't* a deferred track pending, the `StartDeferred` command
does nothing, so the Track $0001 program proceeds to the `Play()`,
which plays a little 4-second-long "get ready" music clip.  The
program then sits in a Wait for 498 frames (the exact length of the
"get ready" clip, about 4 seconds), then goes back to the top of the
loop.  Again, it checks for a deferred track, and if there still isn't
one, it plays the 4-second "get ready" clip again.  (The clip is
composed in such a way that it loops seamlessly, so it sounds like the
orchestra is playing from a long, continuous score that happens to be
a bit repetitive.)  This goes on for as long as the player lets the
ball sit in the plunger lane.  But when the player finally does press
the Launch button, the WPC board sends a $0046 command.  Track $0046
is a Deferred track, so we set the pending deferred track code to
$0002.  That's all that the $0046 command does - it just records that
we now have a pending deferred track, $0002, on channel 0.  Meanwhile,
our Track $0001 loop is still playing.  When it finishes the next
round of the 4-second "get ready" music, it'll loop back to the
`StartDeferred` command - and finally something interesting happens
there!  The `StartDeferred` command sees that a deferred track is
pending this time, so it immediately starts the pending track, Track
$0002, running.  Track $0002 is the main theme music, and since it's
also on channel 0, it replaces the "get ready" loop.  So we seamlessly
switch from "get ready" to the main theme as soon as the next "get
ready" loop completes.

The music designers' goal here was obviously to make the music
transition always happen at the exact end of a full cycle of the "get
ready" music, rather than interrupting it in the middle, the moment
you press Launch.  The transition waits until the "get ready" clip
finishes playing through its whole cycle, and only then do we switch
to the main theme music.  Note how this all works without the WPC host
needing to be aware of the music timing - the timing is all handled
within the sound board.  The WPC host has only sent two commands in
all this time: the $0001 command when the ball was served to the
shooter lane, and $0046 when the player pressed Launch.  The music
looping and synchronization was all handled by the sound board without
communicating anything back to the WPC host.

### <a name="DeferredIndirect"></a> Deferred Indirect

That's the "plain" Deferred track system.  What about this "Deferred
Indirect" business?  I'm afraid I can't give you an example from an
original DCS ROM, because there aren't any.  The Williams pinball
sound designers apparently never found a reason to use this feature.
I haven't been able to think up a good use case for it myself, but we
can at least look at how the mechanism works.

Deferred Indirect extends the plain Deferred track idea by letting you
specify several alternative tracks to play when a `StartDeferred`
is executed, rather than just specifying a single track.  That's why
it's "indirect": the track that sets up the deferred playback doesn't
specify the exact track to play, but rather provides a list of
options.  The running track program that triggers the deferral selects
which one of those options to play.

The list of alternative tracks to play is specified through what I
call a Deferred Indirect Table.  A DI Table is simply a list of track
numbers.

```
Deferred Indirect Table MyTable ($0070, $0071, $0072, $0073);
```

A Deferred Indirect track specifies which table to use.  It also
specifies a "variable", which is used to select which of the table's
alternatives will be activated.

```
Var X;
Track $0007 Channel 0 Defer Indirect(MyTable[X]);
```

Finally, the track program that triggers the deferred track selects the
table option to activate by setting variable X to the index of the table
selection it wants to activate, via a `SetVariable` command in the
track program, then executing a `StartDeferred` command, as it would
in the ordinary Deferred track case.

```
Track $0120 Channel 0 {
    SetVariable(Var X, Value 2);
    StartDeferred(Channel 0);
}
```

When the `StartDeferred` is executed, the program finds the deferred
track for the channel is set to `MyTable[X]`, so it puts the pieces
together to select the track.  It looks at variable X and sees that
it's currently set to 2, from the `SetVariable` just before the
`StartDeferred`.  Table indexing starts at element zero (MyTable[0]),
so MyTable[2] is the *third* element, which is Track $0072.  So the
result is that Track $0072 starts playing.

The whole point of using a variable to index the table is that the
value can *vary*, so a more realistic example would require some
reason to change the variable at different points in the track
program.  So imagine that we have a "countdown" sequence, with a music
track playing while the user has to complete some goal under time
pressure.  If the user completes the goal, we'll switch to a "victory"
track.  But we want the choice of victory music to depend on how far
along we were in the countdown.  To do this, we could use a
Deferred Indirect table, and increment the indexing variable
every couple of seconds while playing the countdown music:

```
Track $0121 Channel 0 {
    Play(Countdown);  // countdown music track
    Wait(2 sec);
    SetVariable(Var X, Value 0);
    StartDeferred(Channel 0);
    Wait(2 sec);
    SetVariable(Var X, Value 1);
    StartDeferred(Channel 0);
    Wait(2 sec);
    SetVariable(Var X, Value 2);
    StartDeferred(Channel 0);
    Wait(2 sec);
    SetVariable(Var X, Value 3);
    StartDeferred(Channel 0);
};
```

I didn't originally plan to implement syntax for Deferred Indirect
tracks in the compiler, since I'm just not convinced it's a useful
feature.  The original Williams sound designers apparently never came
up with a use for it, even though it was implemented in every DCS ROM
software version from the very start.  But ultimately, I gave in to my
baser, completist instincts, and included it in the compiler.  Maybe
someone using the compiler to create a 21st-century DCS soundtrack
will find a creative use for it.

A warning if you're using the 1993 software: Deferred Indirect
is essentially the same as plain Deferred with the 1993 DCS OS.
The 1993 software implements *most* of the mechanism, but omits
the crucial `SetVariable` command.  That means that you can never
select anything but the first track in a Deferred Indirect table,
since every variable has a fixed value of 0 that you can never change.
The compiler will warn you of this by letting you know that
`SetVariable` has no effect if you're using a 1993 game as the
prototype ROM.
