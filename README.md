# DCS Explorer

This project is the result of some pinball archaeology, an attempt to
better understand the DCS audio format.  DCS is the name of the audio
system used in Williams pinball machines, including those from
sub-brands Bally and Midway, from 1993 through 1998.  DCS stores the
audio data using a proprietary compressed digital audio format that's
similar in design to mainstream formats like MP3, AAC, and Vorbis, and
the player system has some special features tailored to the
event-driven environment of an arcade game.

In the pinball machines, DCS was physically implemented with a
purpose-built circuit board based on the Analog Devices ADSP-2105
processor.  But the hardware is only incidental; DCS is really a
software platform that can be implemented on any CPU, as long as it
can perform the decoding work fast enough for real-time playback.
Today's PCs are plenty fast enough, and in fact, even a higher-end
microcontroller like a Raspberry Pi can easily handle the job.

The internal details of the DCS format have never been published
(until now), so it's always been difficult to find out exactly what's
in a DCS pinball's sound ROMs, and quite impossible to create new
ones.  The only way to examine the contents of a DCS ROM was to run
PinMame in its debugging mode, and just try all of the possible
command codes to see what each one did.  That wasn't an entirely
satisfactory solution, in part because the PinMame UI is awkward, and
in part because some of the commands in a DCS ROM don't just trigger
simple audio playback; some have side effects that aren't apparent
when you just run through them all one by one.

DCS Explorer is my name for the overall project, which consists
of three main pieces:

* The DCS Explorer program, a simple command-line tool that lets you examine
the contents of a DCS ROM in detail, and interactively play back
the audio it contains.

* DCSDecoder, a portable C++ class that implements a fully native decoder for
DCS ROMs, without any ADSP-2105 emulation.  It's a standalone class
with no dependencies on PinMame or any other external libraries, and
no dependencies on any system audio interfaces or OS services.
It's easy to incorporate into any C++ project, and its programming
interface is easy to use.  It works with all of the DCS pinball
titles released from 1993 to 1998.  I've tested
representative ROMs for every DCS title and validated that they
produce PCM output that's bit-for-bit identical to the PinMame
emulator's output.  (In fact, my work on this project turned up two
errors in PinMame's emulation that have since been corrected in the
PinMame mainline.)  The code is written in a readable style
and extensively commented, in the hopes that it can serve as an
informational resource to DCS internals for people who can
read C++ code, and as a reference implementation for developing
new DCS-related software.

* DCS Encoder, a program that lets you create your own DCS ROMs.
It not only transcodes audio files into the DCS format, but also
builds entire ROM images that you could install in a DCS pinball
machine.  You can use it both to create wholly original DCS ROMs, and
to make minor changes ("patches") to existing ones, such as
replacing just a few selected audio tracks with original material.
You can encode original DCS audio from mainstream sources like
MP3, WAV, and Ogg Vorbis files.  The project's C++ code is
structured into reusable modules that could be incorporated into
other projects as well, with services to encode audio into the
DCS formats and generate ROM images.

Refer to the README.md files in the project folders for details
on the individual projects.



## DCS Technical Reference

A major by-product of this project is my [DCS Audio Format Technical Reference](http://mjrnet.org/pinscape/dcsref/DCS_format_reference.html).
I tried to capture everything I learned from this project, including
details of the ROM layouts, the platform's run-time behavior, and
(most importantly) the internal data format of the compressed
audio streams.  The technical reference describes the formats in
enough depth to make it possible to create new DCS ROMs
containing original audio material.


## Building

The git repository includes all dependencies.  Building on Windows
should just be a matter of cloning the git repository, opening the
solution (.sln) file in Visual Studio, and executing a Build Solution
command.

The DCS Explorer program has some dependencies on Windows for audio
playback, so it will require additional work to port to Linux or
any other non-Windows platforms.  Most of the rest of the code should
be readily portable, although I haven't created build scripts for any
other platforms or attempted building it anywhere but Windows.

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


## Origins and goals of the project

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

I thought about calling the project DCS Exploder, based on the jokey
name a friend always uses for certain Microsoft applications.  It was
tempting especially because of the nice symmetry that would have obtained
from calling the encoder the DCS Imploder.  But it seemed a little too
silly.


## Links to other DCS documentation

My new [DCS Audio Format Technical Reference](http://mjrnet.org/pinscape/dcsref/DCS_format_reference.html),
which documents what I learned in the course of this project,
is probably the only source of information on DCS internals.

Very little information was previously published on the DCS
format.  That shouldn't be entirely surprising, given that it's a
proprietary format that was only ever used in embedded systems for a
niche industry that mostly disappeared over 20 years ago.  And yet, living
as we do in the age of total information awareness, I'm
always shocked and amazed when I encounter *any* subject, no matter how obscure,
that isn't exhaustively documented somewhere on the Web.  Especially when
it's something at the intersection of technology and popular art.  DCS seems to
be of those rare subjects that time forgot.  In all of the Web, there are only a few
mentions of the technology, and those few mentions are really skimpy.
There's a skeletal [Wikipedia page](https://en.wikipedia.org/wiki/Digital_Compression_System),
and a sort of [marketing-highlights article](https://web.archive.org/web/20070929205008/http://pinballhq.com/willy/willy3.htm)
from a long-defunct pinball 'zine (remember 'zines?) that's only still accessible at all thanks to
[the Wayback Machine](https://web.archive.org/), and that's about it.

[PinMame](https://github.com/vpinball/pinmame) is another place to
look for a certain amount technical information, but that will only
tell you about the hardware.  PinMame includes a software emulator
that can run the original ROM images from the DCS boards, and the
emulator's source code contains a lot of fine-grained detail about how
the original circuit boards work.  The emulator has details that you
can't learn from reading the published schematics of the boards,
because the boards use proprietary PLAs (programmable logic arrays)
whose programming isn't published anywhere.  The PLAs implement most
of the external addressing logic that connects the CPU to the on-board
peripherals, so you can't infer from the schematics alone how the
software is meant to access the peripherals or what side effects are
triggered in the hardware when it does.  That's something you have to
know to understand how the ROM software works.  The PinMame code also
helps nail down some of the details of the various chips on the board,
such as the ADSP-2105 CPU and AD1851 DAC, and how they're used in that
particular application.  But PinMame will only tell you about the DCS
hardware environment; PinMame's whole philosophy is that the software
is a black box that you trick into thinking it's still running in a
dark corner of an arcade in a mall in the 1980s.  It doesn't try to
peek inside to see what the software is made of.  The present project
takes the exact opposite approach: forget the hardware, let's figure
out everything the software is doing.


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

Please see the individual library source folders for the full license text
for each project.

DCS is a trademark of Williams Electronic Games, Inc., and is used here
for information and identification purposes only.  This project isn't
endorsed by or connected in any way to the commercial entities who
created or own the original hardware/software platform.
