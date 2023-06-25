# DCSDecoder Class

DCSDecoder is a C++ class that decodes and plays back the audio in a
pinball ROM set for a game that used the DCS audio system.  It's
designed as a reusable, stand-alone class that can easily be
embedded in any C++ project, with no external dependencies.


## How is this different from PinMame's DCS emulator?

PinMame runs the DCS ROMs in emulation, using Aaron Giles's ADSP-2105
CPU emulator coupled with an emulation of the memory-mapped
peripherals on the DCS boards.

In contrast, DCSDecoder offers a 100% native implementation of the
decoder, with no emulated ADSP-2105 code at all.  This is a ground-up
implementation of a compatible decoder.  It's an entirely stand-alone
class, with no dependencies on any outside projects, that can be
easily embedded in any C++ program.  It also features a relatively
simple programming interface that lets you perform DCS decoding and
playback with a few function calls.  It's designed to let the host
program drive the audio playback system timing; it just serves as
a source of PCM samples, which you can retrieve on your own schedule.

There are several benefits to this approach.  One is that it lets
you see what the decoder is actually doing inside.  PinMame by design
doesn't concern itself with what's going on inside the software; it
tackles the emulation problem by mimicking the original hardware.
You could always look at the original ADSP-2105 machine code to
see what it was doing, but PinMame doesn't provide much in the way
of tools for that, and even if it did, ADSP-2105 assembly code doesn't
make for easy reading (for me, at least) and doesn't do much to
illuminate what's going on at any level of abstraction above bits
and bytes.  So an immediate benefit to a C++ implementation is that
it's a lot easier to see what going on at the design level.

Another benefit is that the native implementation is
considerably faster than an emulator, by a factor of at least 20.
This makes it possible to use the native version on processors that
would be too slow to run the PinMame ADSP-2105 emulation, such as
microcontrollers. 

A third benefit is that the native class is easy to
embed in host applications.  It has no external dependencies (apart
from the standard C++ run-time libraries) and is designed for easy use
in just about any application architecture.  The PinMame DCS emulator,
in contrast, is so entangled with PinMame's internal application structure
that it's impractical to reuse in almost any other context.


## Alternative emulator decoder

DCSDecoder is structured into a base class that provides an abstract
interface to the decoder, and subclasses that implement specific
decoding strategies.  So far, we've been talking about the native
decoder implementation, which is a ground-up, 100% C++ implementation
of the DCS decoder and run-time system.

The project contains a second subclass that implements decoding
through ADSP-2105 machine code interpretation, much like PinMame.
This version of the decoder simply runs the original ROM code in
emulation.  Note that this subclass *still* doesn't somehow
incorporate PinMame - it uses the same ADSP-2105 machine code
interpreter that PinMame does, but it doesn't share anything
with PinMame outside of that.  The emulator subclass is just as
easy to embed in a new C++ project as the native decoder is.

The emulator decoder isn't integral to the project - it's a bonus
feature.  You can run the native decoder without even including the
emulator files in the build; the emulator is just a parallel
subclass specialization.  The main reason the emulator version is
part of the project at all is to serve as a reference implementation
to test that the native decoder is working properly.  The emulated
version is as close as we can get on a PC to the actual DCS hardware,
so it serves as our reference point for correct behavior.  (It would
be better still to test against a physical DCS board from a pinball
machine, but that would take some pretty specialized equipment.  It's
not within the realm of the practical for me.  Testing against the
emulator is the next best thing.)


## Using the decoder class in a program

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

The main decoder class definition file, DCSDecoder.h, has detailed
comments at the top of the file explaining the sequence of calls
needed to use the decoder.
