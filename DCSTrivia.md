# DCS Trivia

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
byte.  (In the command list below, we use the C++ tilde notation for a
bitwise NOT, ~x, to denote the bit-inverted byte.)  That inverted
fourth byte is just there as a sort of checksum to validate that the
sequence was received correctly.  I guess when the messages get up to
a whopping four bytes long, there's too much of a chance of
transmission error in such a primitive network setup, so they had to
add some error detection.

Here are the special command sequences:

* $55 $AA *volume* ~*volume*: Sets the master audio volume, 0 to 255,
mute to maximum.  The volume on the DCS boards is set purely in
software, by scaling the PCM data going into the DAC.   The volume
control is figured into the PCM data by multiplying the PCM amplitudes
by a scaling factor that's derived from the 0 to 255 setting, using a
log curve that makes the scale sound roughly linear to a human ear.
At 255, the scaling factor is 1.0, so you get the reference level of
the recordings; lower volumes attenuate the PCM amplitude along that
log curve.  The 0-255 scale corresponds straightforwardly to the
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
timers.  But the code never does anything with the data structures
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
Windows.  And the reason you don't hear 255 startup "bongs" every time you
enter the operator menu is that the WPC software uses that feature of
the boot loader we mentioned earlier, where the boot loader bypasses
the startup tests if it receives a byte on the data port within 250ms
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


