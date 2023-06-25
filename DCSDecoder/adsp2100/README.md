# ADSP-21xx Interpreter

This is a slightly modified version of Aaron Giles's ADSP-21xx
machine-code interpreter, by way of PinMame.  This version has
some custom modifications for this project, so if you're looking
for a reusable ADSP-21xx emulator, the one in PinMame is probably
closer to what you're looking for (although that also has some
custom modifications for PinMame's sake).

The main modifications in this version:

* An added "OS trap" feature, which causes the interpreter to return
control to the host program (effectively suspending the simulated CPU)
when a certain opcode bit pattern is executed as an instruction.  The
special opcode is one of the unused opcode bit patterns that would
otherwise form an invalid ADSP-21xx instruction.  This allows the host
program to insert the trap instruction at key places where it wants to
hook into the ADSP-21xx code.  The emulator-decoder class uses this to
convert parts of the original ROM code into subroutines that it can
call, by forcing the ADSP-21xx code to return to the host at certain
points.

* Removal of PinMame's cycle counter, which constrains the length of
time the CPU runs before suspending the CPU and returning to the host.
PinMame uses this for what you might call "cooperative
multiprocessing", where each simulated CPU is allowed to execute for a
brief time before switching to the next.  This is a good general
emulation strategy for PinMame, which aims to be a universal emulator
for all of the solid-state pinball platforms and thus has to work with
many combinations of emulated CPUs running in simulated real time.
But it's often inefficient in practice, because the real-time programs
on those emulated CPUs frequently go into spin loops where they just
burn up CPU cycles waiting for an external event.  This is the case
with the DCS control program, which spends a lot of its time waiting
for the asynchronous audio buffer reader to catch up.  The OS trap
approach is much more efficient, because it suspends the simulated CPU
before it can ever enter the wait loop.  Since the DCS Decoder is a
one-trick pony, it can afford to make a special case of the DCS
control program, inserting the traps at the needed locations.

* Added some hooks that allow the host program to implement a simple
assembly-level interactive debugger, which is useful if you're
trying to trace what the ADSP-21xx software is doing internally.
