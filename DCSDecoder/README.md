# DCSDecoder Class

DCSDecoder is a C++ class that decodes and plays back the audio in a
pinball ROM set for a game that used the DCS audio system.


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
playback with a few function calls.

There are several benefits to this approach.  One is that C++ code is
much easier to read than ADSP-2105 assembly code (even if you already
know how to read ADSP-2105 assembly), so this implementation can serve
as an educational resource for those wishing to learn about how the
DCS format works.  Another is that the native implementation is
considerably faster than an emulator, by a factor of at least 20.
This makes it possible to use the native version on processors that
would be too slow to run the emulator for real-time playback, such as
microcontrollers.  A third benefit is that the native class is easy to
embed in host applications.  It has no external dependencies (apart
from the standard C++ run-time libraries) and is designed for easy use
in just about any application architecture.  The PinMame emulator, in
contrast, is so entangled with PinMame's internal interfaces and
structures that it's impractical to reuse in almost any other context.


## Alternative emulator decoder

DCSDecoder *also* features a subclass that implements decoding through
the ADSP-2105 emulator, much like PinMame.  But that's really a bonus
add-on, and isn't used at all when running the native decoder.  What's
more, it's just as stand-alone as the native decoder: you can embed it
in a C++ application using the identical abstract class interface used
with the native decoder.

The main reason the emulator version is part of the project at all is
to serve as a reference implementation to test that the native decoder
is working properly.  The emulated version is as close as we can get
on a PC to the actual DCS hardware.  That's useful because it would be
difficult to test rigorously against physical DCS boards.  The board
don't have any way to read the bit stream going into the DAC, and even
if they did, it would be hard to synchronize that with the data on the
PC side.


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
