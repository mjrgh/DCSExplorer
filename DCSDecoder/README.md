# DCSDecoder Class

DCSDecoder is a C++ class that decodes and plays back the audio in a
pinball ROM set for a game that used the DCS audio system.



## How is this different from PinMame's DCS emulator?

PinMame runs the DCS ROMs in emulation, using Aaron Giles's ADSP-2105
CPU emulator coupled with an emulation of the memory-mapped
peripherals on the DCS boards.  In contrast, DCSDecoder offers a 100%
native implementation of the decoder, with no emulated ADSP-2105 code
at all.  This is a ground-up implementation of a compatible decoder.

There are several benefits to this approach.  One is that C++ code is
much easier to read than ADSP-2105 assembly code (even if you already
know how to read ADSP-2105 assembly), so this implementation can serve
as an educational resource for those wishing to learn about how the
DCS format works.  Another is that the native implementation is
considerably faster than an emulator, by a factor of at least 20.
This makes it possible to use the native version on processors that
would be too slow to run the emulator for real-time playback, such as
embedded controllers or microcontrollers.  A third benefit is that the
native class is easy to embed in host applications.  It has no
external dependencies (apart from the standard C++ run-time libraries)
and is designed for easy use in just about any application
architecture.  The PinMame emulator, in contrast, is so entangled with
PinMame's internal interfaces and structures that it would be
impractical to reuse in almost any other context.

(DCSDecoder actually *also* has a subclass that implements decoding
through the ADSP-2105 emulator, much like PinMame.  But that's really
a bonus add-on.  Its primary purpose is to serve as a reference
implementation to test that the native decoder is working properly.
The emulated version is as close as we can get on a PC to the actual
DCS hardware.  That's useful because it would be difficult to test
rigorously against physical DCS boards.  The board don't have any way
to read the bit stream going into the DAC, and even if they did, it
would be hard to synchronize that with the data on the PC side.)

