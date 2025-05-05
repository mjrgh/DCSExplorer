// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Universal Decoder
//
// This is a C++ implementation of the DCS decoder algorithm originally
// implemented in the DCS sound boards used in Williams/Bally/Midway
// pinball machines of the 1990s.  This version of the decoder is
// designed to work with all of the pinball ROMs for the DCS boards.
//

#include <string.h>
#include <memory>
#include <list>
#include "DCSDecoderNative.h"

// subclass registration
static DCSDecoder::Registration registration("native", "Universal native decoder",
    [](DCSDecoder::Host *host) { return new DCSDecoderNative(host); });

// construction
DCSDecoderNative::DCSDecoderNative(Host *host) : DCSDecoder(host)
{
    // clear buffers
    memset(trackProgramVariables, 0, sizeof(trackProgramVariables));
    memset(frameBuffer, 0, sizeof(frameBuffer));
    memset(outputBuffer, 0, sizeof(outputBuffer));
    memset(overlapBuffer, 0, sizeof(overlapBuffer));
}

// initialize in standalone mode, with no ROMs loaded
void DCSDecoderNative::InitStandalone(OSVersion osVersion)
{
    // save the OS version
    this->osVersion = osVersion;

    // infer the matching hardware version - each OS version is only
    // compatible with one hardware platform
    switch (osVersion) 
    {
    case OSVersion::OS93a:
    case OSVersion::OS93b:
    case OSVersion::OS94:
        hwVersion = HWVersion::DCS93;
        break;

    case OSVersion::OS95:
        hwVersion = HWVersion::DCS95;
        break;

    case OSVersion::Invalid:
        hwVersion = HWVersion::Invalid;
        break;

    case OSVersion::Unknown:
        hwVersion = HWVersion::Unknown;
        break;
    }
}

// Main Decoder Loop
// 
// This routine corresponds to the main loop of the original ROM
// implementation.  The ROM version spends all of its time in this
// main loop, where it refills the PCM sample buffer while the
// hardware autobuffer DMA mechanism asynchronously clocks samples
// out to the DAC through the serial port. 
// 
// For this software reimplementation, the main loop isn't really
// a loop at all.  It simply decodes one buffer-full and then returns.
// This allows the host program to handle the details of sending
// the samples to the hardware audio playback system.  This still
// essentially replicates the overall control flow of the original
// system, since the host system will still need to call MainLoop()
// in a never-ending loop to continually refill its own hardware
// audio output buffers.  It's just a matter of where the loop
// control and hardware synchronization occurs.  In the original
// ROM version, the ROM code had direct access to the hardware, so
// it was in control of the timing.  In this software version, the
// DCS decoder doesn't know anything about the hardware and doesn't
// handle the synchronization - it simply decodes into a memory
// buffer when the host system asks it to.
// 
// Note that this routine is itself an internal detail that isn't
// directly exposed to the host program.  The host program calls
// this indirectly by calling the higher-level sample buffering
// mechanism exposed through GetNextSample().
// 
void DCSDecoderNative::MainLoop()
{
    // Clear the frame buffer
    memset(frameBuffer, 0, sizeof(frameBuffer));

    // Check for channels with forced-stop flags
    for (int ch = 0 ; ch < MAX_CHANNELS ; ++ch)
    {
        // check the stop flag
        if (channel[ch].stop)
        {
            // stop flag is set - reset it
            channel[ch].stop = false;

            // if there's an audio stream, reset it and clear the decoding buffers
            if (!channel[ch].audioStream.playbackBitPtr.IsNull())
            {
                channel[ch].audioStream.playbackBitPtr.Clear();
                ResetMixingLevels(ch);
            }

            // clear the host event timer
            channel[ch].hostEventTimer.Clear();

            // clear the track byte-code program
            channel[ch].trackPtr.Clear();
        }
    }
    
    // Process pending commands.  A "command" is really an array index
    // into the track list in the catalog, and issuing the command causes
    // track to be loaded into a channel for execution.  A "track" doesn't
    // directly contain audio data; it's actually a little byte-code
    // program that executes a series of events when activated.  Byte-code
    // operations in the track program can in turn start playback of
    // audio clips, as well as perform operations like looping and
    // issuing new commands.  Commands in the queue can come from the
    // host system (on a physical pinball machine, the WPC MPU board), and
    // can also be added by track programs as part of their execution.
    while (commandQueue.size() != 0)
    {
        // retrieve the next command
        uint16_t cmd = commandQueue.front();
        commandQueue.pop_front();

        // The command code is simply an index into the track index.  Make
        // sure it's in range, and discard it if not.
        if (cmd >= catalog.nTracks)
            continue;

        // Get the track pointer from the index
        uint32_t trackOfs = U24BE(catalog.trackIndex + cmd*3);

        // ignore it if it's invalid (high byte 0xFF)
        if ((trackOfs & 0xFF0000) == 0xFF0000)
            continue;

        // get the track pointer
        ROMPointer trackPtr = MakeROMPointer(trackOfs);

        // Read the type and channel number
        uint8_t type = *trackPtr++;
        uint8_t ch = *trackPtr++;
        if (type == 1)
        {
            // first byte is 1 -> load track
            LoadTrack(ch, trackPtr);
        }
        else if (type <= 3)
        {
            // deferred track -> store the type and deferred track link, but
            // don't start playback yet
            channel[ch].nextTrackType = type;
            channel[ch].nextTrackLink = trackPtr.GetU16();
        }
        else
        {
            // codes > 3 are invalid
            throw ResetException();
        }
    }

    // Process active track byte-code programs in all channels.  This 
    // executes each currently active track programs until it either 
    // terminates or pauses to wait for stream playback. 
    // 
    // Note that we can't simply make one pass through all of the channels.
    // Instead, we have to keep repeating the loop over channels until all
    // channels are marked as "done".  The reason is that executing the
    // track program in one channel can create new work in another channel.
    // A channel that we already visited and marked as done can become
    // un-done again by the time we reach the end of the loop over channels.
    const int ALL_DONE_MASK = (1 << MAX_CHANNELS) - 1;
    channelMask = 0;
    for (int ch = 0 ; channelMask != ALL_DONE_MASK ; ch = (ch + 1) % MAX_CHANNELS)
    {
        // if the channel isn't marked as done, process it
        if ((channelMask & (1 << ch)) == 0)
        {
            // execute the track program
            ExecTrack(ch);

            // set the channel's "done" bit
            channelMask |= (1 << ch);
        }
    }

    // Figure the sum of the effective volume level for all channels with active
    // audio streams.  The effective volume level is the mixing level multiplied
    // by the master volume level.  Note that both numbers are in the "1.15"
    // fixed-point fraction format, where the mathematical value is the 2's
    // complement value divided by 32768.  Multiplying two numbers in this format
    // yields a 2.30 number, and the sum of 6 of these fits into a 4.30 number.
    // Since that's 35 bits total, we have to use a 64-bit int for the running 
    // total.
    //
    // The point of this sum is to determine the combined maximum PCM level that
    // we'll get when we add together all of the channels in the final mixing
    // step.  This tells us the fixed-point scale we need to use for the decoded
    // samples throughout the frame decompression and transform steps such that
    // we maximize precision without ever overflowing.  We'll adjust the per-
    // channel mixing multiplier to that scale, and then we'll adjust the final
    // PCM results back to full-scale.  In other words, all of the values in the
    // decompression and transform steps will be stored as 1.15 mantissas with
    // an implied scale of "times 2^volumeShift", where volumeShift is a negative
    // number.  In the final step where we calculate the INT16 values, we'll
    // actually multiply the 1.15 values by 2^volumeShift to get the integer 
    // representation.
    //
    // Note that this wouldn't be necessary if we were to use floating-point
    // types through the decoding process.  Floating-point types would keep the
    // scale for each value individually, so we wouldn't have to per-calculate
    // a compromise scale that we use for all intermediate values.  The ADSP-2105
    // doesn't have native floating-point types, though, so the reference decoder
    // uses this fixed-point approach.  We need to use the same arithmetic to
    // get identical PCM results; if we used floating-point types, they'd keep
    // more precision in the intermediate steps, and we'd get slightly different
    // final PCM results after rounding to INT16.
    uint64_t mixingSum = 0;
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        // check the special "max mixing level" flag (this applies even if the
        // channel isn't playing)
        if (channel[i].maxMixingLevelOverride)
        {
            mixingSum += static_cast<uint64_t>(static_cast<uint64_t>(channel[i].mixingMultiplier) * 0x7FFE);
            continue;
        }

        // if the channel is in use, add its mixing level
        if (!channel[i].audioStream.playbackBitPtr.IsNull())
            mixingSum += static_cast<uint64_t>(static_cast<uint64_t>(channel[i].mixingMultiplier) * volumeMultiplier);
    }

    // We now have a 5.30 fractional value.  Shift right 2 bits to get it into
    // 4.28 format so that it fits into 32 bits.
    mixingSum >>= 2;

    // Get the floating point scale of the result, adding 3 to adjust from 
    // 4.28 to 1.31 format.  If we were to express this value with a binary
    // floating-point type (e.g., a C++ float or double), this is the
    // exponent portion of the value after normailzing the mantissa.  The
    // normalizing exponent for a whole number is always negative, but since
    // we're going to use it in bit shifts, negate it to get the shift count.
    int volShift = -(CalcExp32(static_cast<uint32_t>(mixingSum)) + 3);

    // Limit the result to the range 0..8.  Since we only have 16 bits of
    // precision in the intermediate values, the final INT16 PCM results
    // will only have (16 - volShift) bits of precision remaining.  The
    // cap of 8 bits ensures that we have at least 8 bits of precision
    // left in the final PCM results.
    volShift = volShift < 0 ? 0 : volShift > 8 ? 8 : volShift;

    // Calculate the per-channel mixing multiplier, working in the scale
    // selected in volShift.
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        uint16_t v = channel[i].maxMixingLevelOverride ? 0x7FFE : volumeMultiplier;
        auto m = (static_cast<uint64_t>(static_cast<uint64_t>(channel[i].mixingMultiplier) * v) << 1);
        channel[i].mixingMultiplier = static_cast<uint16_t>((m << volShift) >> 16);
    }

    // Decompress the next frame from each active stream
    for (int ch = 0 ; ch < MAX_CHANNELS ; ++ch)
        DecodeStream(ch);

    // We now have all of the decompressed frame data mixed into the frame
    // buffer.  This is expressed as frequency-domain data for the current
    // frame's time window.  Transform it into PCM samples.
    decoderImpl->TransformFrame(volShift);

    // Update the per-channel mixing levels
    UpdateMixingLevels();

    // Increment the data port timeout.  This counts the time since the last
    // data port input was received in units of the main loop processing time,
    // which is fixed at 7.68ms.  (That's the amount of time it takes to play
    // back 240 samples at 31250 samples per second.  Assuming real-time
    // playback of the samples we generate, the main loop must be called at
    // least every 7.68ms to refill the hardware playback buffer.  In the
    // original ROM code, this was guaranteed, because the code syncs up with
    // the hardware DMA read pointer on every main loop pass.  For this C++
    // version, it's up to the host program to synchronize with real-time
    // audio playback, so the counter only corresponds to real-time units if
    // the host is actually performing synchronized playback.)
    // 
    // The data port timeout in the original ROM code is 13 ticks on this
    // counter, or about 100ms.  That's the maximum time that can elapse
    // between data port reception of the bytes of a multi-byte command
    // sequence.  Cap the counter at 13 since anything higher means the
    // same thing as 13, and we don't want to allow the counter to
    // overflow (as that would anomalously allow short periods where
    // the counter says we're not in the timeout state even though we
    // should be).
    dataPortTimeout += 1;
    if (dataPortTimeout > 13)
        dataPortTimeout = 13;
}

// --------------------------------------------------------------------------
//
// Static data tables for the frame transformations
//

// Overlap buffer coefficients
const static uint16_t overlapCoefficients[] = {
    0x013c, 0x0734, 0x1090, 0x1cec, 0x2bf6, 0x3d07, 0x4ef6, 0x6029,
    0x6eec, 0x79fa, 0x80df, 0x8405, 0x8463, 0x8326, 0x816e, 0x8030
};

// Bit-reverse indexing table for a 9-bit (512-element) address space
const static int bitRev9[] ={
    0x000, 0x100, 0x080, 0x180, 0x040, 0x140, 0x0c0, 0x1c0, 0x020, 0x120, 0x0a0, 0x1a0, 0x060, 0x160, 0x0e0, 0x1e0,     // 000..00f
    0x010, 0x110, 0x090, 0x190, 0x050, 0x150, 0x0d0, 0x1d0, 0x030, 0x130, 0x0b0, 0x1b0, 0x070, 0x170, 0x0f0, 0x1f0,     // 010..01f
    0x008, 0x108, 0x088, 0x188, 0x048, 0x148, 0x0c8, 0x1c8, 0x028, 0x128, 0x0a8, 0x1a8, 0x068, 0x168, 0x0e8, 0x1e8,     // 020..02f
    0x018, 0x118, 0x098, 0x198, 0x058, 0x158, 0x0d8, 0x1d8, 0x038, 0x138, 0x0b8, 0x1b8, 0x078, 0x178, 0x0f8, 0x1f8,     // 030..03f
    0x004, 0x104, 0x084, 0x184, 0x044, 0x144, 0x0c4, 0x1c4, 0x024, 0x124, 0x0a4, 0x1a4, 0x064, 0x164, 0x0e4, 0x1e4,     // 040..04f
    0x014, 0x114, 0x094, 0x194, 0x054, 0x154, 0x0d4, 0x1d4, 0x034, 0x134, 0x0b4, 0x1b4, 0x074, 0x174, 0x0f4, 0x1f4,     // 050..05f
    0x00c, 0x10c, 0x08c, 0x18c, 0x04c, 0x14c, 0x0cc, 0x1cc, 0x02c, 0x12c, 0x0ac, 0x1ac, 0x06c, 0x16c, 0x0ec, 0x1ec,     // 060..06f
    0x01c, 0x11c, 0x09c, 0x19c, 0x05c, 0x15c, 0x0dc, 0x1dc, 0x03c, 0x13c, 0x0bc, 0x1bc, 0x07c, 0x17c, 0x0fc, 0x1fc,     // 070..07f
    0x002, 0x102, 0x082, 0x182, 0x042, 0x142, 0x0c2, 0x1c2, 0x022, 0x122, 0x0a2, 0x1a2, 0x062, 0x162, 0x0e2, 0x1e2,     // 080..08f
    0x012, 0x112, 0x092, 0x192, 0x052, 0x152, 0x0d2, 0x1d2, 0x032, 0x132, 0x0b2, 0x1b2, 0x072, 0x172, 0x0f2, 0x1f2,     // 090..09f
    0x00a, 0x10a, 0x08a, 0x18a, 0x04a, 0x14a, 0x0ca, 0x1ca, 0x02a, 0x12a, 0x0aa, 0x1aa, 0x06a, 0x16a, 0x0ea, 0x1ea,     // 0a0..0af
    0x01a, 0x11a, 0x09a, 0x19a, 0x05a, 0x15a, 0x0da, 0x1da, 0x03a, 0x13a, 0x0ba, 0x1ba, 0x07a, 0x17a, 0x0fa, 0x1fa,     // 0b0..0bf
    0x006, 0x106, 0x086, 0x186, 0x046, 0x146, 0x0c6, 0x1c6, 0x026, 0x126, 0x0a6, 0x1a6, 0x066, 0x166, 0x0e6, 0x1e6,     // 0c0..0cf
    0x016, 0x116, 0x096, 0x196, 0x056, 0x156, 0x0d6, 0x1d6, 0x036, 0x136, 0x0b6, 0x1b6, 0x076, 0x176, 0x0f6, 0x1f6,     // 0d0..0df
    0x00e, 0x10e, 0x08e, 0x18e, 0x04e, 0x14e, 0x0ce, 0x1ce, 0x02e, 0x12e, 0x0ae, 0x1ae, 0x06e, 0x16e, 0x0ee, 0x1ee,     // 0e0..0ef
    0x01e, 0x11e, 0x09e, 0x19e, 0x05e, 0x15e, 0x0de, 0x1de, 0x03e, 0x13e, 0x0be, 0x1be, 0x07e, 0x17e, 0x0fe, 0x1fe,     // 0f0..0ff
    0x001, 0x101, 0x081, 0x181, 0x041, 0x141, 0x0c1, 0x1c1, 0x021, 0x121, 0x0a1, 0x1a1, 0x061, 0x161, 0x0e1, 0x1e1,     // 100..10f
    0x011, 0x111, 0x091, 0x191, 0x051, 0x151, 0x0d1, 0x1d1, 0x031, 0x131, 0x0b1, 0x1b1, 0x071, 0x171, 0x0f1, 0x1f1,     // 110..11f
    0x009, 0x109, 0x089, 0x189, 0x049, 0x149, 0x0c9, 0x1c9, 0x029, 0x129, 0x0a9, 0x1a9, 0x069, 0x169, 0x0e9, 0x1e9,     // 120..12f
    0x019, 0x119, 0x099, 0x199, 0x059, 0x159, 0x0d9, 0x1d9, 0x039, 0x139, 0x0b9, 0x1b9, 0x079, 0x179, 0x0f9, 0x1f9,     // 130..13f
    0x005, 0x105, 0x085, 0x185, 0x045, 0x145, 0x0c5, 0x1c5, 0x025, 0x125, 0x0a5, 0x1a5, 0x065, 0x165, 0x0e5, 0x1e5,     // 140..14f
    0x015, 0x115, 0x095, 0x195, 0x055, 0x155, 0x0d5, 0x1d5, 0x035, 0x135, 0x0b5, 0x1b5, 0x075, 0x175, 0x0f5, 0x1f5,     // 150..15f
    0x00d, 0x10d, 0x08d, 0x18d, 0x04d, 0x14d, 0x0cd, 0x1cd, 0x02d, 0x12d, 0x0ad, 0x1ad, 0x06d, 0x16d, 0x0ed, 0x1ed,     // 160..16f
    0x01d, 0x11d, 0x09d, 0x19d, 0x05d, 0x15d, 0x0dd, 0x1dd, 0x03d, 0x13d, 0x0bd, 0x1bd, 0x07d, 0x17d, 0x0fd, 0x1fd,     // 170..17f
    0x003, 0x103, 0x083, 0x183, 0x043, 0x143, 0x0c3, 0x1c3, 0x023, 0x123, 0x0a3, 0x1a3, 0x063, 0x163, 0x0e3, 0x1e3,     // 180..18f
    0x013, 0x113, 0x093, 0x193, 0x053, 0x153, 0x0d3, 0x1d3, 0x033, 0x133, 0x0b3, 0x1b3, 0x073, 0x173, 0x0f3, 0x1f3,     // 190..19f
    0x00b, 0x10b, 0x08b, 0x18b, 0x04b, 0x14b, 0x0cb, 0x1cb, 0x02b, 0x12b, 0x0ab, 0x1ab, 0x06b, 0x16b, 0x0eb, 0x1eb,     // 1a0..1af
    0x01b, 0x11b, 0x09b, 0x19b, 0x05b, 0x15b, 0x0db, 0x1db, 0x03b, 0x13b, 0x0bb, 0x1bb, 0x07b, 0x17b, 0x0fb, 0x1fb,     // 1b0..1bf
    0x007, 0x107, 0x087, 0x187, 0x047, 0x147, 0x0c7, 0x1c7, 0x027, 0x127, 0x0a7, 0x1a7, 0x067, 0x167, 0x0e7, 0x1e7,     // 1c0..1cf
    0x017, 0x117, 0x097, 0x197, 0x057, 0x157, 0x0d7, 0x1d7, 0x037, 0x137, 0x0b7, 0x1b7, 0x077, 0x177, 0x0f7, 0x1f7,     // 1d0..1df
    0x00f, 0x10f, 0x08f, 0x18f, 0x04f, 0x14f, 0x0cf, 0x1cf, 0x02f, 0x12f, 0x0af, 0x1af, 0x06f, 0x16f, 0x0ef, 0x1ef,     // 1e0..1ef
    0x01f, 0x11f, 0x09f, 0x19f, 0x05f, 0x15f, 0x0df, 0x1df, 0x03f, 0x13f, 0x0bf, 0x1bf, 0x07f, 0x17f, 0x0ff, 0x1ff,     // 1f0..1ff
};

// Transform coefficients table.  These are the pre-computed cos() and sin() 
// values used in the frequency domain to time domain transforms.  These were
// pre-computed for a fixed set size of 128 real/imaginary pairs.  Elements
// are in the 1.15 fixed-point fraction format, as signed values.  To recover
// the mathematical value of a 1.15-formatted number, cast to int16_t (signed)
// and divide by 32768.0f.
// 
// The values are in the bit-reversed access order used in the transform
// algorithm, so the pattern of theta angles is a little wacky.  See the loop
// below where they're used for the access pattern.
//
static const uint16_t ifftCoefficients[] ={
    0x0000, 0x8000, 0xa57e, 0xa57e, 0xcf04, 0x89be, 0x89be, 0xcf04, 0xe707, 0x8276, 0x9592, 0xb8e3, 0xb8e3, 0x9592, 0x8276, 0xe707,
    0xf374, 0x809e, 0x9d0e, 0xaecc, 0xc3a9, 0x8f1d, 0x8583, 0xdad8, 0xdad8, 0x8583, 0x8f1d, 0xc3a9, 0xaecc, 0x9d0e, 0x809e, 0xf374,
    0xf9b8, 0x8027, 0xa129, 0xaa0a, 0xc946, 0x8c4a, 0x877b, 0xd4e1, 0xe0e6, 0x83d6, 0x9236, 0xbe32, 0xb3c0, 0x9930, 0x8163, 0xed38,
    0xed38, 0x8163, 0x9930, 0xb3c0, 0xbe32, 0x9236, 0x83d6, 0xe0e6, 0xd4e1, 0x877b, 0x8c4a, 0xc946, 0xaa0a, 0xa129, 0x8027, 0xf9b8,
    0xfcdc, 0x800a, 0xa34c, 0xa7bd, 0xcc21, 0x8afb, 0x8894, 0xd1ef, 0xe3f4, 0x831c, 0x93dc, 0xbb85, 0xb64c, 0x9759, 0x81e2, 0xea1e,
    0xf055, 0x80f6, 0x9b17, 0xb140, 0xc0e9, 0x90a1, 0x84a3, 0xdddc, 0xd7d9, 0x8676, 0x8dab, 0xc673, 0xac65, 0x9f14, 0x8059, 0xf695,
    0xf695, 0x8059, 0x9f14, 0xac65, 0xc673, 0x8dab, 0x8676, 0xd7d9, 0xdddc, 0x84a3, 0x90a1, 0xc0e9, 0xb140, 0x9b17, 0x80f6, 0xf055,
    0xea1e, 0x81e2, 0x9759, 0xb64c, 0xbb85, 0x93dc, 0x831c, 0xe3f4, 0xd1ef, 0x8894, 0x8afb, 0xcc21, 0xa7bd, 0xa34c, 0x800a, 0xfcdc,
    0x8000, 0x0000, 0xa57e, 0x5a82, 0x89be, 0x30fc, 0xcf04, 0x7642, 0x8276, 0x18f9, 0xb8e3, 0x6a6e, 0x9592, 0x471d, 0xe707, 0x7d8a,
    0x809e, 0x0c8c, 0xaecc, 0x62f2, 0x8f1d, 0x3c57, 0xdad8, 0x7a7d, 0x8583, 0x2528, 0xc3a9, 0x70e3, 0x9d0e, 0x5134, 0xf374, 0x7f62,
    0x8027, 0x0648, 0xaa0a, 0x5ed7, 0x8c4a, 0x36ba, 0xd4e1, 0x7885, 0x83d6, 0x1f1a, 0xbe32, 0x6dca, 0x9930, 0x4c40, 0xed38, 0x7e9d,
    0x8163, 0x12c8, 0xb3c0, 0x66d0, 0x9236, 0x41ce, 0xe0e6, 0x7c2a, 0x877b, 0x2b1f, 0xc946, 0x73b6, 0xa129, 0x55f6, 0xf9b8, 0x7fd9,
    0x800a, 0x0324, 0xa7bd, 0x5cb4, 0x8afb, 0x33df, 0xd1ef, 0x776c, 0x831c, 0x1c0c, 0xbb85, 0x6c24, 0x9759, 0x49b4, 0xea1e, 0x7e1e,
    0x80f6, 0x0fab, 0xb140, 0x64e9, 0x90a1, 0x3f17, 0xdddc, 0x7b5d, 0x8676, 0x2827, 0xc673, 0x7255, 0x9f14, 0x539b, 0xf695, 0x7fa7,
    0x8059, 0x096b, 0xac65, 0x60ec, 0x8dab, 0x398d, 0xd7d9, 0x798a, 0x84a3, 0x2224, 0xc0e9, 0x6f5f, 0x9b17, 0x4ec0, 0xf055, 0x7f0a,
    0x81e2, 0x15e2, 0xb64c, 0x68a7, 0x93dc, 0x447b, 0xe3f4, 0x7ce4, 0x8894, 0x2e11, 0xcc21, 0x7505, 0xa34c, 0x5843, 0xfcdc, 0x7ff6
};

// --------------------------------------------------------------------------
//
// Transform samples from frequency domain to time domain - 1994-1998
// algorithm.  This is used for all games except the three titles released 
// in 1993.  (The 1993 games perform the same underlying transform, but they
// use a different algorithm to do it, which results in slightly different
// numerical results due to the different amounts of accumulated rounding
// errors in the respective intermediate calculations.)
// 
// The basic transform that the algorithm implements is a real-valued
// discrete Fourier transform (RDFT).
//
void DCSDecoderNative::DecoderImpl94x::TransformFrame(int volShift)
{
    // Pre-processing steps.  These rearrange the RDFT samples
    // into the corresponding complex pairs to use as IFFT inputs.

    auto *frameBuf = decoder->frameBuffer;
    frameBuf[0x80] = MulSS(frameBuf[0x80], 0x8000);
    frameBuf[0x81] = MulSS(-SIGNED(frameBuf[0x81]), 0x8000);
    uint16_t *p0 = frameBuf;
    uint16_t *p1 = frameBuf + 0x100;
    for (int i = 0 ; i < 0x0040 ; ++i, p0 += 2, p1 -= 2)
    {
        int32_t x0 = SIGNED(p0[0]);
        int32_t y0 = SIGNED(p1[0]);
        int32_t x1 = SIGNED(p0[1]);
        int32_t y1 = SIGNED(p1[1]);

        p0[0] = MulSS(SaturateInt16(x0 + y0), 0x8000);
        p1[0] = MulSS(SaturateInt16(x0 - y0), 0x8000);
        p0[1] = MulSS(SaturateInt16(x1 - y1), 0x8000);
        p1[1] = MulSS(SaturateInt16(x1 + y1), 0x8000);
    }

    uint16_t I0 = 2;
    uint16_t I1 = 0;
    uint16_t *p4 = frameBuf;
    uint16_t *p5 = frameBuf + 0x100;
    for (int i = 0 ; i < 0x0040 ; ++i, p4 += 2, p5 -= 2, I0 += 4, I1 += 4)
    {
        // theta = 2*PI*(128 - i)/256
        // c0 = cos(theta), c1 = sin(theta)
        auto c0 = ifftCoefficients[bitRev9[I0]];
        auto c1 = ifftCoefficients[bitRev9[I1]];

        int32_t x0 = SIGNED(p4[0]);   // f[2i]
        int32_t x1 = SIGNED(p4[1]);   // f[2i+1]
        auto xn0 = p5[0];             // f[N-2i]
        auto xn1 = p5[1];             // f[N-2i+1]

        // prod0 = f[N-2i+1]*c1 - f[N-2i]*c0
        uint64_t MR;
        MulSS(MR, xn1, c1);
        int32_t prod0 = SIGNED(MultiplyRoundSub(MR, xn0, c0));

        // prod1 = f[N-2i+1]*c0 + f[N-2i]*c1
        MulSS(MR, xn1, c0);
        int32_t prod1 = SIGNED(MultiplyRoundAdd(MR, xn0, c1));

        // f[2i] = f[N-2i+1]*c0 + f[N-2i]*c1 + f[2i]
        p4[0] = SaturateInt16(prod1 + x0);

        // f[2i+1] = f[N-2i+1]*c1 - f[N-2i]*c0 + f[2i+1]
        p4[1] = SaturateInt16(prod0 + x1);

        // f[N-2i] = f[2i] - f[N-2i+1]*c0 - f[N-2i]*c1
        p5[0] = SaturateInt16(x0 - prod1);

        // F[N-2i+1] = f[N-2i+1]*c1 - f[N-2i]*c0 - f[2i+1]
        p5[1] = SaturateInt16(prod0 - x1);
    }

    p0 = frameBuf;
    p1 = frameBuf + 0x80;
    for (int i = 0 ; i < 0x0040 ; ++i)
    {
        int32_t x0 = SIGNED(p0[0]);
        int32_t y0 = SIGNED(p1[0]);
        int32_t x1 = SIGNED(p0[1]);
        int32_t y1 = SIGNED(p1[1]);

        *p0++ = SaturateInt16(x0 + y0);
        *p1++ = SaturateInt16(x0 - y0);
        *p0++ = SaturateInt16(x1 + y1);
        *p1++ = SaturateInt16(x1 - y1);
    }

    // Inverse FFT - this is the iterative in-place Cooley-Tukey IFFT
    // algorithm.  The main loop is abbreviated by one iteration from
    // usual, which would run for log2(128) = 7 iterations.  Stopping
    // one iteration short leaves the results partitioned into the two
    // halves of the main set, with a/ separate FFT completed on each 
    // partition.  The inputs are arranged in standard frequency-domain
    // order; the outputs are arranged in bit-reversed-indexing order.
    int nPartitions = 2;
    int partitionSize = 0x40;
    for (int i = 0 ; i < 6 ; ++i)
    {
        const uint16_t *pSin = ifftCoefficients;
        const uint16_t *pCos = ifftCoefficients + 0x80;
        p0 = frameBuf;
        p1 = frameBuf + partitionSize;

        for (int partitionNum = 0 ; partitionNum < nPartitions ; ++partitionNum, p0 += partitionSize, p1 += partitionSize)
        {
            auto cSin = *pSin++;
            auto cCos = *pCos++;
            for (int j = partitionSize/2 ; j != 0 ; --j)
            {
                // real/imaginary a = *p1
                auto aReal = p1[0];
                auto aImag = p1[1];
                
                // real part of a*exp(theta)
                uint64_t prod;
                MulSS(prod, aReal, cCos);
                int32_t tReal = SIGNED(MultiplyRoundSub(prod, aImag, cSin));

                // imaginary part of a*exp(theta)
                MulSS(prod, aImag, cCos);
                int32_t tImag = SIGNED(MultiplyRoundAdd(prod, aReal, cSin));

                // real/imaginary u = *p0
                int32_t uReal = SIGNED(p0[0]);
                int32_t uImag = SIGNED(p0[1]);

                // store u' = *p0++ = u - t
                *p0++ = SaturateInt16(uReal - tReal);
                *p0++ = SaturateInt16(uImag - tImag);

                // store t' = *p1++ = u + t
                *p1++ = SaturateInt16(uReal + tReal);
                *p1++ = SaturateInt16(uImag + tImag);

            }
        }
        nPartitions *= 2;
        partitionSize /= 2;
    }

    // Apply volume normalization.  The intermediate calculations were
    // all done at a fixed-point scale chosen in the main loop such that
    // the maximum sample value would just fill the 1.15 format, so in
    // effect, the sample values are all 1.15 fractional mantissas with
    // an implied exponent of 2^-volShift.  To translate to the final
    // INT16 representation for PCM output, apply that fixed-point scale.
    p0 = frameBuf;
    for (int i = 0 ; i < 0x0100 ; ++i, ++p0)
        *p0 = static_cast<uint16_t>(static_cast<int32_t>(SIGNED(*p0)) >> volShift);

    // Mix the previous frame's overlap buffer into the first 16 elements
    // of the new frame.
    const uint16_t *co0 = overlapCoefficients;
    const uint16_t *coN = overlapCoefficients + 0x000F;
    uint16_t *ovp = decoder->overlapBuffer;
    for (int i = 0 ; i < 16 ; i += 2)
    {
        int bi = bitRev9[i];

        uint64_t a, b;
        MulSU(a, frameBuf[bi], *co0++);
        MulSU(b, *ovp++, *coN--);
        a += b;
        frameBuf[bi++] = RoundMultiplyResult(a, 0);

        MulSU(a, frameBuf[bi], *co0++);
        MulSU(b, *ovp++, *coN--);
        a += b;
        frameBuf[bi++] = RoundMultiplyResult(a, 0);
    }

    // Fetch the 240 output samples for this frame, rearranging them into time 
    // order via the bit-reversed-indexing permutation
    uint16_t *outbufp = decoder->outputBuffer;
    for (int i = 0 ; i < 240 ; i += 2)
    {
        int bi = bitRev9[i];
        *outbufp++ = frameBuf[bi++];
        *outbufp++ = frameBuf[bi];
    }

    // Save the last 16 output samples into the overlap buffer, to mix into
    // the next frame
    ovp = decoder->overlapBuffer;
    for (int i = 240 ; i < 256 ; i += 2)
    {
        int bi = bitRev9[i];
        *ovp++ = frameBuf[bi++];
        *ovp++ = frameBuf[bi];
    }
}


// --------------------------------------------------------------------------
//
// Transform samples from frequency domain to time domain - 1993 algorithm.
// This is used for the three games released in 1993 (STTNG, IJTPA, JD).
// 
// The frequency-domain frame format is actually identical in every DCS
// game across the entire time line.  In all cases, it's a collection of
// 255 INT16 frequency amplitude samples, the output of a specific
// discrete integral transform applied to the PCM data contained in a
// 256-sample time window, with some rearrangement and normalization.
// 
// Even though the format is the same, the 1993 and 1994+ games use
// slightly different algorithms to perform the inverse transform to
// convert the frequency-domain samples back to time-domain PCM samples.
// The two algorithms are calculating the same mathematical formula, but
// the differences in the way they express the intermediate calculations
// result in some slight numerical differences in the final outputs for
// a given set of inputs, due to differences in the accumulated rounding
// errors in the intermediate steps.  If we want to get bit-for-bit
// identical output compared to the respective ADSP-2105 implementations,
// which we do, we have to use the 1993 algorithm for 1993 ROMs and the
// 1994-1998 algorithm for 1994-1998 ROMs.
// 
// The 1993 algorithm is simpler but less efficient than the 1994-1998
// version.  The 1993 version expands the 256 sample set to 512 derived
// samples, and performs an FFT on the larger set.  The 1994-1998
// algorithm accomplishes the same thing with some in-place twiddling
// to the 256 sample set before and after a 256-sample FFT.  The 
// smaller input set to the FFT reduces the computational work by more
// than a factor of 2 (which is offset slightly by the extra twiddling
// work, but that's very small in comparison).
// 
// The transform that the algorithm implements is the real-valued
// discrete Fourier transform (RDFT).
//
void DCSDecoderNative::DecoderImpl93::TransformFrame(int volShift)
{
    // get the frame buffer pointer into a local for convenience
    auto &frameBuf = decoder->frameBuffer;

    // The 1993 decoder starts off by treating the first pair of
    // elements as a complex number (frameBuf[0] + frameBuf[1]*i),
    // and rewriting that complex number as an entirely real
    // number with the combined magnitude of the complex value
    // moved into its real component.  In other words, it computes 
    // sqrt([0]^2 + [1]^2) and stores that in [0], setting [1] to 
    // zero.  The 1994-1998 decoder doesn't have this step, which 
    // appears to be because it was never necessary in the first 
    // place: the encoder seems to guarantee that [1] is always
    // zero.  I think it's best to keep the calculation in place
    // just in case there's an example lurking somewhere that
    // I've missed, but as far as I can tell, this code is never
    // exercised by any of the extant DCS ROMs.

    // get abs(frameBuf[0]), and note the sign for later, so that
    // we can restore it in the final real-number result
    uint16_t AR = frameBuf[0];
    bool ASFlag = (SIGNED(AR) < 0);
    if (ASFlag)
        AR = static_cast<uint16_t>(-SIGNED(AR));

    // figure f0 = frameBuf[0]^2 + frameBuf[1]^2
    uint64_t MR, prod2;
    MulSS(MR, frameBuf[1], frameBuf[1]);
    MulSS(prod2, AR, AR);
    MR += prod2;

    // normalize f0 to 1.15 format and get the normalizing shift count
    uint32_t SR = static_cast<uint32_t>(MR & 0xFFFFFFFF);
    int exponent = SIGNED(Normalize32(SR));
    AR = MR1(SR);

    // If it's non-zero, figure sqrt(f0) = |f0|, and replace the first
    // buffer pair with (|f0| + 0i) - that is, replace the imaginary number
    // formed by the first buffer pair with a real number of the same
    // magnitude and real sign, with the imaginary component set to zero.
    if (AR != 0)
    {
        // This is calculating the Taylor series for sqrt(f0), using the
        // 1.15-format primitives available on the ADSP-2105.  This is an
        // incredibly tedious way to do it - we could just call sqrt() to
        // get the same result, if we didn't mind some rounding differences:
        // the C++ library version would have much less rounding error than
        // the 1.15 calculation below.  That might sound better, but it's
        // more important that we get bit-for-bit identical results to the
        // original reference implementation, since that's the only way to
        // validate that our decoder is working properly.

        // MR = 0.10379
        MR = 0x0D490000;

        // MR = 0.10379 + 0.72745*f0
        MR += (static_cast<uint64_t>(static_cast<int64_t>(0x5D1D) * static_cast<int64_t>(SIGNED(AR))) << 1);

        // MR = 0.10379 + 0.72745*f0 - 0.67245*f0^2
        uint16_t MF = MultiplyAndRound(AR, AR);
        MR += (static_cast<uint64_t>(static_cast<int64_t>(-22035) * static_cast<int64_t>(SIGNED(MF))) << 1);

        // MR = 0.10379 + 0.72745*f0 - 0.67245*f0^2 + 0.5534*f0^3
        MF = MultiplyAndRound(AR, MF);
        MR += (static_cast<uint64_t>(static_cast<int64_t>(0x46D6) * static_cast<int64_t>(SIGNED(MF))) << 1);

        // MR = 0.10379 + 0.72745*f0 - 0.67245*f0^2 + 0.5534*f0^3 - 0.26825*f0^4
        MF = MultiplyAndRound(AR, MF);
        MR += (static_cast<uint64_t>(static_cast<int64_t>(-8790) * static_cast<int64_t>(SIGNED(MF))) << 1);

        // MR = 0.10379 + 0.72745*f0 - 0.67245*f0^2 + 0.5534*f0^3 - 0.26825*f0^4 + 0.05606*f0^5
        MF = MultiplyAndRound(AR, MF);
        MR += (static_cast<uint64_t>(static_cast<int64_t>(0x072D) * static_cast<int64_t>(SIGNED(MF))) << 1);

        // check for odd exponent
        if ((exponent & 0x0001) != 0)
        {
            // odd exponent - multiply by sqrt(2)/2 and make even (0x5A82 = 0.70709 [1.15])
            MultiplyAndRound(MR, MR1(MR), 0x5A82);
            exponent += 1;
        }

        // convert the result back to 1.15 format, applying the normalizing
        // exponent, and restore the sign
        exponent = exponent/2 + 1;
        SR = BitShiftSigned32(static_cast<uint32_t>(MR & 0xFFFFFFFF), exponent);
        AR = MR1(SR);
        if (ASFlag)
            AR = static_cast<uint16_t>(-static_cast<int32_t>(SIGNED(AR)));
    }

    // store the adjusted real component back in the first frame buffer 
    // element, and set the imaginary component at [1] to zero.  Copy 
    // these to the phantom wrap-around element at [0x100] as well.
    frameBuf[0x0000] = frameBuf[0x0100] = AR;
    frameBuf[0x0001] = frameBuf[0x0101] = 0x0000;

    // Expand the 256 samples from the frame buffer into 512 samples for
    // the integration step.
    uint16_t *i0 = frameBuf + 0x0002;
    uint16_t *i1 = frameBuf + 0x00FE;
    uint16_t *i2 = frameBuf + 0x0102;
    uint16_t *i3 = frameBuf + 0x01FE;
    for (int i = 0 ; i < 0x0040 ; ++i, i0 += 2, i1 -= 2, i2 += 2, i3 -= 2)
    {
        int32_t xr = SIGNED(i0[0]);
        int32_t xi = SIGNED(i0[1]);
        int32_t yr = SIGNED(i1[0]);
        int32_t yi = SIGNED(i1[1]);

        i0[0] = i1[0] = static_cast<uint16_t>(xr + yr);
        i2[0] = static_cast<uint16_t>(xr - yr);
        i3[0] = static_cast<uint16_t>(yr - xr);

        i2[1] = i3[1] = static_cast<uint16_t>(xi + yi);
        i0[1] = static_cast<uint16_t>(xi - yi);
        i1[1] = static_cast<uint16_t>(yi - xi);
    }

    // IFFT.  This the Cooley-Tukey iterative in-place IFFT algorithm.  The
    // outer loop stops one iteration short of the full IFFT, leaving the data
    // arranged in two partitions, with the even-numbered outputs representing
    // the IFFT of the lower half of the inputs, and the odd-numbered outputs
    // representing the IFFT of the high half of the inputs.  Note also that
    // this works on a collection of 512 samples, but half of the samples are
    // redundant mirror images; the final PCM data consists only of the lower
    // half (the first 256 elements) of the frame buffer.
    uint16_t nPartitions = 2;
    uint16_t partitionSize = 0x80;
    for (int i = 0 ; i < 7 ; ++i)
    {
        const uint16_t *pSin = ifftCoefficients;
        const uint16_t *pCos = ifftCoefficients + 0x80;
        i0 = frameBuf;
        i1 = frameBuf + partitionSize;
        for (uint16_t j = 0 ; j < nPartitions ; ++j)
        {
            auto cSin = *pSin++;
            auto cCos = *pCos++;
            for (int k = partitionSize/2 ; k > 0 ; --k)
            {
                auto a0 = i1[0];
                auto a1 = i1[1];
                auto y0 = SIGNED(i0[0]);
                auto y1 = SIGNED(i0[1]);
                
                MulSS(MR, a0, cCos);
                auto x0 = SIGNED(MultiplyRoundSub(MR, a1, cSin));

                MulSS(MR, a1, cCos);
                auto x1 = SIGNED(MultiplyRoundAdd(MR, a0, cSin));

                *i0++ = static_cast<uint16_t>(y0 - x0);
                *i0++ = static_cast<uint16_t>(y1 - x1);

                *i1++ = static_cast<uint16_t>(x0 + y0);
                *i1++ = static_cast<uint16_t>(x1 + y1);
            }
            i1 += partitionSize;
            i0 += partitionSize;
        }
        nPartitions *= 2;
        partitionSize /= 2;
    }

    // Apply volume normalization, and extract the samples in time order (applying
    // the bit-reversal indexing permutation)
    uint16_t I1 = 0;
    uint16_t I4 = 1;
    for (int i = 0 ; i < 0x100 ; ++i, I4 += 2)
        frameBuf[I4] = static_cast<uint16_t>(static_cast<int32_t>(SIGNED(frameBuf[bitRev9[I1++]])) >> volShift);

    // The first 16 PCM samples of output are formed from combining the new
    // decompressed frame with overlapping samples from the previous frame.
    uint16_t *outp = decoder->outputBuffer;
    uint16_t *ovp = decoder->overlapBuffer;
    const uint16_t *cp1 = overlapCoefficients;
    const uint16_t *cp2 = overlapCoefficients + 0x00F;
    i3 = frameBuf + 1;
    for (int i = 0 ; i < 0x10 ; ++i, i3 += 2)
    {
        // output = (overlapBuffer[i] * coefficients[15-i]) + (frameBuffer[i] * coefficients[i])
        uint64_t a, b;
        MulSU(a, *ovp++, *cp2--);
        MulSU(b, *i3, *cp1++);
        a += b;
        *outp++ = RoundMultiplyResult(a, 0);
    }

    // The remaining 224 PCM samples come directly from the decompressed frame
    for (int i = 0 ; i < 0xE0 ; ++i, i3 += 2)
        *outp++ = *i3;

    // The last 16 samples of the new decompressed frame go into the overlap
    // buffer for the next frame
    ovp = decoder->overlapBuffer;
    for (int i = 0 ; i < 0x10 ; ++i, i3 += 2)
        *ovp++ = *i3;
}

// --------------------------------------------------------------------------
//
// Track programs
//
// A "track" is a miniature byte-code program that controls playback on
// a channel.  A track program can load audio streams, do simple looping,
// control the volume across tracks, and coordinate transitions between
// tracks (e.g., so that transitions occur on music beats).
//

// Load a track
void DCSDecoderNative::LoadTrack(int ch, const ROMPointer &trackPtr)
{
    // store the new program pointer
    channel[ch].trackPtr = trackPtr;

    // reset the audio stream pointer
    channel[ch].audioStream.playbackBitPtr.Clear();

    // reset all of the track counters
    channel[ch].trackCounter = 0;
    channel[ch].hostEventTimer.Clear();
    channel[ch].loopStack.clear();

    // the channel now has pending work
    channelMask &= ~(1 << ch);

    // clear the decoding buffers for the channel
    ResetMixingLevels(ch);
}

// Execute an active track.  This interprets the byte-code program for
// an active track.
void DCSDecoderNative::ExecTrack(int curChannel)
{
    // get the track program pointer; if it's null, there's nothing to do here
    ROMPointer p = channel[curChannel].trackPtr;
    if (p.IsNull())
        return;

    // process the track's opcode sequence from the current location
    for ( ;; )
    {
        // Read the next opcode's count prefix.  If the channel's
        // track counter hasn't yet reached the count prefix, pause
        // execution of the track, leaving the track read pointer at
        // the current position.
        uint16_t countPrefix = p.GetU16();
        if (countPrefix == 0xFFFF || channel[curChannel].trackCounter != countPrefix)
        {
            // Un-get the last U16, and update the channel with the
            // next track read position
            p.Modify(-2);
            channel[curChannel].trackPtr = p;
            return;
        }

        // clear the iteration counter
        channel[curChannel].trackCounter = 0;

        // read and interpret the next opcode
        uint16_t opcode = p.GetU8();
        switch (opcode)
        {
        case 0x00:
            // Opcode 0x00 - Stop.  This stops playback on the track and
            // clears its track program and audio stream.
            channel[curChannel].trackPtr.Clear();
            channel[curChannel].audioStream.playbackBitPtr.Clear();
            channel[curChannel].loopStack.clear();
            channel[curChannel].hostEventTimer.Clear();
            ResetMixingLevels(curChannel);
            return;

        case 0x01:
            // Opcode 0x01 - Load audio stream
            {
                // read the channel where the stream will play (this might be
                // a different channel - one channel's program can load audio
                // streams into other channels)
                auto streamChannel = p.GetU8();

                // if we're loading channel 5, clear the "max mixing level" flag
                if (streamChannel == 5)
                    channel[5].maxMixingLevelOverride = false;

                // read the pointer to the start of the audio stream's binary data
                ROMPointer audioStreamPtr = MakeROMPointer(p.GetU24());

                // get the loop counter
                uint8_t loopCounter = p.GetU8();

                // load the track
                LoadAudioStream(streamChannel, curChannel, loopCounter, audioStreamPtr);
            }
            break;

        case 0x02:
            // Code 0x02 - Stop playback in a specified channel (UINT8 operand)
            {
                // read the channel number
                uint16_t targetChannel = p.GetU8();

                // clear the audio stream on the target channel
                if (!channel[targetChannel].audioStream.playbackBitPtr.IsNull())
                {
                    channel[targetChannel].audioStream.playbackBitPtr.Clear();
                    ResetMixingLevels(targetChannel);
                }

                // clear the track program on the target channel
                channel[targetChannel].trackPtr.Clear();

                // clear the host event timer
                channel[targetChannel].hostEventTimer.Clear();

                // if the target channel was the same as the current channel,
                // the current program has been terminated
                if (channel[curChannel].trackPtr.IsNull())
                    return;
            }
            break;

        case 0x03:
            // Opcode 0x03 - Queue audio command in UINT16 operand.  This queues
            // a command code as though it had been sent on the data port.
            QueueCommand(p.GetU16());
            break;

        case 0x04:
            // Opcode 0x04.  The meaning of this opcode varies by OS version.
            if (osVersion == OSVersion::OS93a)
            {
                // OS93a (IJTPA, JD) only: Set up the channel timer with
                // the UINT8 command byte and the UINT16 counter value.
                // A command byte of zero clears the timer without sending
                // anything to the host; any other command byte is sent
                // immediately.
                uint8_t cmdByte = p.GetU8();
                uint16_t counter = p.GetU16();
                auto &timer = channel[curChannel].hostEventTimer;
                if (cmdByte == 0)
                {
                    // command byte 0 -> clear the timer
                    timer.Clear();
                }
                else
                {
                    // non-zero command byte -> send immediately to the host
                    host->ReceiveDataPort(cmdByte); 

                    // if the counter is non-zero, set up the channel timer,
                    // otherwise clear the timer
                    if (counter != 0)
                        timer.Set(cmdByte, counter);
                    else
                        timer.Clear();
                }
            }
            else
            {
                // All other versions -> Write UINT8 operand to data port
                uint8_t byteVal = p.GetU8();
                host->ReceiveDataPort(byteVal);

                // In the 1.05 ROM software, byte values $69 and $6A also have
                // a special meaning: they respectively set and clear the channel 
                // 5 "max mixing level override" flag.  This is a truly bizarre 
                // special case, but we have to implement it to be true to the
                // original decoders.
                if (nominalVersion == 0x0105)
                {
                    if (byteVal == 0x69)
                        channel[5].maxMixingLevelOverride = true;
                    else if (byteVal == 0x6A)
                        channel[5].maxMixingLevelOverride = false;

                }
            }
            break;

        case 0x05:
            {
                // Opcode 0x05 - Trigger a deferred track link in another 
                // channel.  If the target channel has a deferred track queued
                // up, this starts the deferred track.  This lets a track
                // program start a related sound effect at a chosen point in
                // the source track's playback, which might be useful to
                // synchronize effects with music beats, for example.
                // 
                // A deferred track is a track with type code 2 or 3.  When
                // the main controller sends a command that refers to a track
                // of type 2 or 3, the track is loaded into the channel, but
                // playback isn't started.  Playback is only started when
                // another track program executes on opcode 0x05 targeting
                // the channel with the deferred track.

                // read the target channel number
                uint16_t targetChannel = p.GetU8();

                // Get the track type in the other channel, to determine
                // what kind of pending linked track information it's
                // storing.  If the type is zero, do nothing - this means
                // that there's no new track programmed on the incoming
                // channel, so the old track (the current track) just keeps
                // playing for now.
                auto targetTrackType = channel[targetChannel].nextTrackType;
                if (targetTrackType == 0)
                    break;

                // set the target track's new type to zero - this operation consumes
                // the pending status in the target track
                channel[targetChannel].nextTrackType = 0;

                // load the channel's next track link
                if (targetTrackType == 2)
                {
                    // Type 2 - nextTrackLink is simply a command code
                    QueueCommand(channel[targetChannel].nextTrackLink);
                }
                else if (targetTrackType == 3)
                {
                    // Type 3 - nextTrackLink contains a convoluted indirect index value
                    // that combines a "variable" previously stored with opcode 0x06 and
                    // an index into a two-level table in the ROM catalog.  The 16-bit
                    // track link value is actually two 8-bit values packed into the
                    // UINT16: an index into the track program variable array in the high
                    // byte, and an index into the first level ROM catalog table in 
                    // the low byte.
                    //
                    // There are so many levels of indirection that I can't figure out
                    // how this was meant to be used.  And, apparently, neither did any
                    // of the Williams sound designers: there are no examples of type 3
                    // tracks in any of the 29 DCS pinball titles.  As with a couple of
                    // other implemented-but-never-used-features, this must have been
                    // either planned for future use that never happened (probably not,
                    // since this is implemented in the very first 1993 ROMs), or it was
                    // implemented for the sake of one of the DCS video games rather than
                    // for the pinball side.
                    auto linkVal = channel[targetChannel].nextTrackLink;
                    uint16_t lo =  linkVal & 0x00FF;
                    uint16_t hi = (linkVal >> 8) & 0x00FF;

                    // get the track program variable array value - this is a value 
                    // stored earlier by an 0x06 opcode
                    auto variableVal = trackProgramVariables[hi];

                    // Get the table pointer.  Catalog[$0043] points to an array
                    // of table pointers, and the low byte of the link is an index
                    // into that array.  Indexing the table pointer array by the
                    // low byte yields a pointer to the base of the table.  The
                    // table is itself an array of UINT16 track numbers.
                    // 
                    // The high byte is an index into the opcode 0x06 variable
                    // array.  The value of that variable is the index into the
                    // UINT16 track number array.
                    // 
                    // Catalog[$0043] -> array of UINT24 { <address of table 0>, <address of table 1>, ... }
                    // Index with low byte of track link to get table
                    // 
                    // Variables -> array of BYTE
                    // Index with high byte of track link to get variable value
                    // 
                    // <address of table 0> -> array of UINT16 { <track number 0>, <track number 1>, ... }
                    // Index with variable value to get new track number
                    //
                    // So the final track is:   Catalog[$0043][lo][variableValue[hi]]
                    //
                    ROMPointer tablePtr = MakeROMPointer(U24BE(catalog.indirectTrackIndex + lo*3));
                    tablePtr.Modify(variableVal * 2);
                    QueueCommand(tablePtr.GetU16());
                }
            }
            break;

        case 0x06:
            // Opcode 0x06 - Store variables[UINT index] = UINT8 value.  Writes a
            // byte value to a selected slot in the track program variable.  These 
            // variables are read back when opcode 0x05 executes and the next track
            // link has type code 3.
            //
            // Exception: This is a no-op for the 1993 software.
            //
            if (osVersion == OSVersion::OS93a || osVersion == OSVersion::OS93b)
            {
                // 1993 software - no-op, no operand bytes
            }
            else
            {
                // 1994+ software - Set Variable opcode
                uint8_t varIndex = p.GetU8();
                trackProgramVariables[varIndex] = p.GetU8();
            }
            break;

        case 0x07:
        case 0x08:
        case 0x09:
            // Opcodes 0x7-0x09 - Mixing level control, immediate change
            MixingLevelOp(curChannel, p, opcode - 0x07, false);
            break;

        case 0x0A:
        case 0x0B:
        case 0x0C:
            // Opcodes 0x0A-0x0C - Mixing level control, with fade
            MixingLevelOp(curChannel, p, opcode - 0x0A, true);
            break;

        case 0x0D:
            // Opcode 0x0D - NOP
            break;

        case 0x0E:
            // Opcode 0x0E - Push the current position onto the loop stack.  The
            // byte parameter is the loop counter; if this is non-zero, the loop
            // repeats this number of times, and zero means loop forever.
            {
                uint8_t loopCounter = p.GetU8();
                channel[curChannel].PushPos(loopCounter, p);
            }
            break;

        case 0x0F:
            // Opcode 0x0F - Jump back to a loop stack save point set with 0x0E
            channel[curChannel].PopPos(p);
            break;

        case 0x10:
            // Opcode 0x10 (BYTE channel, BYTE value_parameter)
            // Mystery op - set mystery parameters with immediate effect
            //
            // Opcodes 0x10-0x12 were added in the 1994 software.  They appear
            // to be an abortive start at a feature that was never finished.
            // There are no instances of these opcodes in any of the 29 DCS
            // pinball titles, and even if there were, they'd apparently have no
            // effect anyway, since the memory locations that the opcode handlers
            // write to are never referenced anywhere else where they'd have any
            // effect.  (The affected memory all appears to be in ordinary DM()
            // RAM space, not anywhere mapped to external peripherals.)
            // 
            // What we can infer from the opcode handlers is that these opcodes
            // are structured just like the mixing level control opcodes.  They
            // have three variations, for set/increase/decrease, and there's a
            // timer that ramps the setting from a current value to a target
            // value over a given number of frames.  The mystery is: what else
            // apart from the volume would you want to control via levels and
            // fades like this?  Bass/treble?  Special effects like reverb?  
            // None of that seems likely, but I don't have any better ideas.
            {
                // get the parameters
                auto ch = p.GetU8();
                auto val = p.GetU8();

                // set the parameters in the channel
                if (ch >= 0 && ch < MAX_CHANNELS)
                    channel[ch].mysteryOpParams.Set(val);
            }
            break;

        case 0x11:
        case 0x12:
            // Opcode 0x11/0x12 (BYTE channel, BYTE delta, WORD stepCounter)
            // These are related to opcode 0x10 above - these are the increase/
            // decrease variations.
            {
                // get the parameters
                auto ch = p.GetU8();
                int delta = static_cast<int>(p.GetU8());
                uint16_t stepCounter = p.GetU16();

                // if the channel is invalid, ignore it
                if (ch >= 6)
                    break;

                // get the channel struct
                auto &params = channel[ch].mysteryOpParams;

                // for opcode $12, the delta is a decrement
                if (opcode == 0x12)
                    delta = -delta;

                // apply the delta to the target
                int newVal = static_cast<int>(params.target) + delta;

                // cap it to 00..FF
                newVal = (newVal < 0 ? 0 : newVal > 0xFF ? 0xFF : newVal);

                // store the new target
                params.target = static_cast<uint16_t>(newVal);

                // If the current value already matches the target, or if the new step
                // counter is zero, make the update immediate.
                if (params.current == params.target || stepCounter == 0)
                {
                    // we're already there - make the change immediate
                    params.Set(params.current);
                }
                else
                {
                    // the change is to take place over the give number of steps - store
                    // the new step counter, and calculate the step size
                    params.stepCounter = stepCounter;
                    params.stepSize = static_cast<float>(delta) / static_cast<float>(stepCounter);
                }
            }
            break;

        default:
            // invalid opcode - reset the decoder
            throw ResetException();
        }
    }
}


// --------------------------------------------------------------------------
//
// Track program looping operations
//

// Push the current track position.  This processes track opcode 0x0E,
// which sets the start of a looping section.  This pushes a new entry
// onto the looping stack, recording the current track playback position
// and initializing the new stack element's loop counter to the value
// specified in the opcode.
void DCSDecoderNative::Channel::PushPos(uint16_t counter, const ROMPointer &pos)
{
    loopStack.emplace_back(counter, pos);
}

// Pop the track loop stack.  This processes track opcode 0x0F, which
// marks the end of a looping section.  This returns the track playback
// position to the most recent opcode 0x0E and decrements the stacked
// loop counter.  If the loop counter reaches zero, the loop ends and
// the loop stack element is discarded.
void DCSDecoderNative::Channel::PopPos(ROMPointer &pos)
{
    // if there's an element on the loop stack, loop back to the
    // start of the looping section
    if (loopStack.size() != 0)
    {
        // Check the current loop counter value.  If it's already zero,
        // it means that this is an infinite loop, so leave it at zero,
        // reset the track position to the start of the loop, and leave
        // the stack element in place.  
        // 
        // If the counter is non-zero, it specifies the number of times
        // to repeat the loop.  If it's 
        // 0 means loop forever; otherwise, loop until the counter reaches 1
        if (auto c = loopStack.back().counter ; c == 0)
        {
            // counter value 0 -> infinite loop - go back for another
            // iteration, leaving the counter at zero
            pos = loopStack.back().pos;
        }
        else if (c == 1)
        {
            // Finite loop, and the counter has reached 1, so we've finished
            // the last iteration.  Pop the stack and let the track continue
            // from the next opcode.
            loopStack.pop_back();
        }
        else
        {
            // It's a finite loop, and the counter is still greater than 1, so
            // we have more iterations left to do.  Decrement the counter and
            // loop back to the starting point for another round.
            loopStack.back().counter -= 1;
            pos = loopStack.back().pos;
        }
    }
}


// --------------------------------------------------------------------------
//
// Mixing level control operations (track program opcodes 0x07-0x0C)
//
void DCSDecoderNative::MixingLevelOp(int curChannel, ROMPointer &p, int mode, bool fade)
{
    // read the target channel (BYTE operand)
    uint16_t targetChannel = p.GetU8();

    // read the target level/delta parameter (signed BYTE operand; the
    // actual parameter value is 64x the BYTE value)
    int param = (static_cast<int>(static_cast<int8_t>(p.GetU8()))) << 6;

    // if it's a fade, get the number of steps
    int steps = fade ? static_cast<int>(p.GetU16()) : 0;

    // Get the mixing array entry.  The mixing level applies to the
    // mixer array for the target channel, and goes into the slot
    // for the current channel.
    auto &mixer = channel[targetChannel].mixer[curChannel];

    // save the fade step counter
    mixer.fadeSteps = steps;

    // Get the old level.  Note that we use the CURRENT level as the
    // starting point for a delta and/or fade ramp, because that's
    // what the original DCS code does, and we have to do the same
    // thing to get matching output.  (It probably makes more sense
    // in the abstract to use the TARGET level instead, since the
    // current level might be in flux due to a fade in progress,
    // but that would cause slightly different results compared to
    // the original decoders.)
    int oldLevel = mixer.curLevel;

    // interpret the new level/delta parameter according to the mode
    int newLevel = oldLevel;
    switch (mode)
    {
    case 0:
        // parameter is new absolute level
        newLevel = param;
        break;

    case 1:
        // increase by parameter
        newLevel = oldLevel + param;
        break;

    case 2:
        // decrease by parameter
        newLevel = oldLevel - param;
        break;
    }

    // Figure the delta.  Note that we have to figure the delta BEFORE applying
    // the range limit.  It's probably debatable whether or not this makes more 
    // sense than using the range-limited value, but that's moot because this
    // is the way the original DCS implementations do it.  We have to use the
    // same algorithm to get matching outputs.
    int delta = newLevel - oldLevel;

    // limit the new level to +/- 8191 (0xE001..$1FFF)
    if (newLevel > 8191)
        newLevel = 8191;
    else if (newLevel < -8191)
        newLevel = -8191;

    // set the new target level
    mixer.fadeTargetLevel = newLevel;

    // set the immediate change or fade ramp
    if (steps != 0)
    {
        // fade - set the per-step delta
        mixer.fadeDelta = delta / steps;
    }
    else
    {
        // immediate - set the new level directly
        mixer.curLevel = newLevel;
    }
}

// --------------------------------------------------------------------------
// 
// Is a stream playing in the given channel?
//
bool DCSDecoderNative::IsStreamPlaying(int channelNo)
{
    return !channel[channelNo].audioStream.playbackBitPtr.IsNull();
}


// --------------------------------------------------------------------------
//
// Directly load an audio stream into a channel
//
void DCSDecoderNative::LoadAudioStream(int streamChannelNum, const ROMPointer &streamPtr, int mixingLevel)
{
    // validate the channel number
    if (streamChannelNum >= 0 && streamChannelNum < MAX_CHANNELS)
    {
        // get the channel structure
        auto &ch = channel[streamChannelNum];

        // cancel any running track program
        ch.trackPtr.Clear();

        // load the stream, using the stream channel as the program channel
        LoadAudioStream(streamChannelNum, streamChannelNum, 1, streamPtr);

        // set a default mixing level
        auto &m = ch.mixer[streamChannelNum];
        m.Reset();
        m.curLevel = m.fadeTargetLevel = mixingLevel << 6;
    }
}

void DCSDecoderNative::LoadAudioStream(int streamChannel, int sourceProgramChannelNum, int loopCounter, const ROMPointer &streamPtr)
{
    // load the channel into the stream
    InitChannelStream(channel[streamChannel], streamPtr);

    // if the stream doesn't have any frames, there's nothing to do
    if (channel[streamChannel].audioStream.numFrames == 0)
        return;

    // set the stream loop counter - this sets the number of times
    // that we play back this audio stream (separately from the
    // track-level looping opcodes)
    channel[streamChannel].audioStream.loopCounter = loopCounter;

    // If the stream channel was previously loaded from a different
    // source, reset its mixing level
    int oldSourceChannel = channel[streamChannel].sourceChannel;
    if (oldSourceChannel >= 0 && oldSourceChannel != static_cast<int>(sourceProgramChannelNum))
        channel[streamChannel].mixer[oldSourceChannel].Reset();

    // mark the stream channel with the controlling source channel (that's
    // us, the current channel)
    channel[streamChannel].sourceChannel = sourceProgramChannelNum;
}

void DCSDecoderNative::InitChannelStream(Channel &ch, ROMPointer streamPtr)
{
    // Read the frame counter from the stream - this is the number of
    // audio frames that the stream contains.  A frame is a set of
    // samples for one main loop pass, 240 PCM samples, which equals
    // 7.68ms at 31250 samples per second.
    auto nFrames = streamPtr.GetU16();

    // set the frame counter
    ch.audioStream.numFrames = nFrames;

    // initialize the stream's frame countdown timer to the total
    // number of frames
    ch.audioStream.frameCounter = nFrames;

    // the next thing in the stream is the 16-byte header - store
    // its location, since it's needed each time we decompress a
    // frame
    ch.audioStream.headerPtr = streamPtr;

    // Skip the header to get to the start of the audio sample data.
    // The header is always 16 bytes, except in one special case:  for
    // OS93a games only, if the first byte's high bit is set, the
    // header is only one byte.
    ch.audioStream.headerLength = (osVersion == OSVersion::OS93a && (streamPtr.PeekU8() & 0x80) != 0) ? 1 : 16;
    streamPtr.Modify(ch.audioStream.headerLength);

    // remember the pointer to the start of the audio stream
    ch.audioStream.playbackBitPtr = streamPtr;
    ch.audioStream.startPtr = streamPtr;
}

// Clear tracks
void DCSDecoderNative::ClearTracks()
{
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        channel[i].trackPtr.Clear();
        channel[i].audioStream.Clear();
    }
}

// Add a track command (public interface)
void DCSDecoderNative::AddTrackCommand(uint16_t trackNum)
{
    QueueCommand(trackNum);
}


// --------------------------------------------------------------------------
//
// Get information on a stream
//
DCSDecoderNative::StreamInfo DCSDecoderNative::GetStreamInfo(const ROMPointer &streamPtr)
{
    // remember the starting point of the stream, so that we can calculate
    // the total byte length when we reach the end of the stream
    const uint8_t *startp = streamPtr.p;

    // set up a temporary channel object, and load the stream into it
    Channel ch;
    InitChannelStream(ch, streamPtr);

    // initialize playback on the channel
    InitStreamPlayback(ch);

    // decode all of the frames - this will advance the pointer until it
    // reaches the end of the stream, which will tell us the size of the
    // stream in bytes
    for (unsigned int i = 0 ; i < ch.audioStream.numFrames ; ++i)
    {
        uint16_t buf[256];
        decoderImpl->DecompressFrame(ch, buf);
    }

    // figure the stream size
    int nBytes = static_cast<int>(ch.audioStream.playbackBitPtr.p.p - startp);

    // get the stream major type
    int streamType = (ch.audioStream.header[0] & 0x80) != 0 ? 1 : 0;

    // Get the subtype, used only in OS94 streams.
    int streamSubType = 0;
    if (osVersion == OSVersion::OS94 || osVersion == OSVersion::OS95)
        streamSubType = ((ch.audioStream.header[1] & 0x80) >> 6) | ((ch.audioStream.header[1] & 0x80) >> 7);

    // Figure the header length.  For all streams except OS93a Type 1,
    // the header is 16 bytes.  For OS93a Type 1, it's one byte.
    int headerLength = (osVersion == OSVersion::OS93a && streamType == 1) ? 1 : 16;

    // return the stream description
    auto info = StreamInfo{
        ch.audioStream.numFrames,
        nBytes,
        streamType,
        streamSubType
    };

    // populate the header in the return struct
    memset(info.header, 0, sizeof(info.header));
    memcpy(info.header, ch.audioStream.header, headerLength);

    // return the populated info struct
    return info;
}

// --------------------------------------------------------------------------
//
// Stream Decoding
//

// Decode a channel's stream.  This decompresses one frame from the
// specified channel, and updates the stream's frame counters.
void DCSDecoderNative::DecodeStream(uint16_t ch)
{
    // get the channel's audio stream struct
    auto &str = channel[ch].audioStream;

    // stop if the channel doesn't have an active stream
    if (str.playbackBitPtr.IsNull())
        return;

    // process the start-of-track table if we're at the start of the track
    if (str.playbackBitPtr == str.startPtr)
        InitStreamPlayback(channel[ch]);

    // Decompress the next frame
    decoderImpl->DecompressFrame(channel[ch], frameBuffer);

    // Decrement the stream's frame counter.  If it's non-zero, there
    // are more frames left to decode in the stream, so simply return
    // to let playback continue.
    if (--str.frameCounter != 0)
        return;

    // The frame counter has reached zero, so we've reached the end of the
    // stream.  Reset to the start of the stream, in case we're looping.
    str.frameCounter = str.numFrames;
    str.playbackBitPtr = str.startPtr;

    // If the stream loop counter is zero, it means "loop forever", so we can 
    // simply return and let the track keep playing (with no adjustment to the 
    // counter needed)
    if (str.loopCounter == 0)
        return;

    // The loop counter is non-zero, so it indicates the number of times we
    // should repeat the track.  Decrement it and check if it has reached
    // zero.  If not, return to let the track play again.
    if (--str.loopCounter != 0)
        return;

    // The track is now finished.  Clear the stream pointer and source
    // channel.
    str.playbackBitPtr.Clear();
    channel[ch].sourceChannel = -1;
}

// Initialize stream playback.  This is called at the start of decoding
// when the current stream pointer is at the beginning of the stream's
// ROM data.  This resets the decompression buffers and caches for the
// start of the new stream data.
void DCSDecoderNative::InitStreamPlayback(Channel &ch)
{
    // initialize the bit-reader buffer
    auto &stream = ch.audioStream;

    // Make a local copy of the 16-byte stream header (if it has a
    // header at all - OS93a Type 1 streams don't have headers)
    ROMPointer p2 = stream.headerPtr;
    unsigned int i = 0;
    for ( ; i < static_cast<unsigned int>(stream.headerLength) ; ++i)
        stream.header[i] = *p2++;

    // zero out any remaining bytes of the local header copy
    for ( ; i < 16 ; ++i)
    {
        // Note: this #pragma cruft is to work around a known GCC bug.
        // GCC incorrectly warns that 'i' is writing outside the array
        // bounds, even though 'i' is deterministically in bounds due
        // to the constant 'for' termination condition.  The compiler
        // can determine this statically, and in fact the whole point
        // of the warning is that GCC *is* determining i's range
        // statically, but figures it wrong and thinks 'i' is out of
        // bounds in some case.  'i' is obviously and provably in
        // bounds, and GCC is just wrong.  This is a known and long-
        // standing GCC bug, and the only reliable workaround seems to
        // be to disable the warning.  The warning could be meaningful
        // in other contexts, so we don't want to disable it globally
        // in the makefile.  Instead, e can use a #pragma to disable
        // it for this one line of code.  It's ugly, but I'd rather
        // not have spurious warnings, because I actually like to
        // fix every valid warning, so I don't want a bunch of noise
        // warnings to accumulate such that I have to get accustomed
        // to just ignoring them.
#ifdef __GCC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
        stream.header[i] = 0;

#ifdef __GCC__
#pragma GCC diagnostic pop
#endif
    }

    // initialize the frame band type buffer to all zeroes
    memset(stream.bandTypeBuf, 0, sizeof(stream.bandTypeBuf));
}

// --------------------------------------------------------------------------
//
// Decompress one frame from the current audio stream using the 1994-1998
// frame format.  This is the format used by all of the DCS pinball titles
// except for the first three, all of which were released in 1993 (STTNG,
// IJTPA, JD).JD
// 
// Throughout this subroutine, there are two different "headers" that
// we refer to many times:
// 
//   - The STREAM HEADER, which is the 16-byte header at the start of
//     the current audio stream.  Each element of this header specifies
//     the sample scaling factor for one group ("band") of samples in
//     each input frame.  The header also has some extra high bits
//     mixed in that select different format versions, which affect
//     how the control bytes in the stream are interpreted.  There's
//     a single stream header for the entire stream, and it never
//     changes.
// 
//   - The FRAME HEADER, which is a compressed bit vector at the start
//     of every frame.  There's a separate frame header for every frame.
//     The frame headers are encoded differentially, so there's a sort
//     of "running total" buffer that we keep from frame to frame for
//     the purpose of applying the deltas in each frame header to get
//     the final value.  The frame header, once decoded, contains one
//     byte for each byte of the stream header, and each byte in each
//     header corresponds to one band of samples in the input.  The
//     frame header bytes specify the bit width and bit encoding of
//     the samples for the corresponding block ("band") of inputs.
//     To interpret the input samples, we need the corresponding byte
//     from each header: the stream header byte to get the "scale" of 
//     the samples, which is the multiplication factor applied to the
//     stream input value to form the final value in the output; and
//     the frame header byte to determine the bit width and encoding
//     of the samples in the input stream.
//
void DCSDecoderNative::DecoderImpl94x::DecompressFrame(Channel &channel, uint16_t *outputBuffer)
{
    // set up pointers to the audio stream header buffer
    auto &stream = channel.audioStream;
    const uint8_t *hdr = &stream.header[0];

    // Save the sample at the second frame buffer element
    uint16_t outbuf1 = outputBuffer[1];

    // Bit $80 of the first stream header byte specifies the stream's 
    // major format type.  There are two format major types, which we
    // arbitrarily call Type 0 and Type 1.  The scaleCodebookEle formats within
    // each frame are identical in both cases, but the two types use
    // different codebooks for the samples:
    // 
    // Type 0 (first stream header byte bit $80 is 0):  The stream
    // header bytes (low 7 bits, byte & $7F) specify the scaling
    // factor for all samples in each band, uniformly across all
    // frames, using the 0deeeemm bit fields described later.  Each
    // frame header specifies the sample codebook for each format
    // directly.
    // 
    // Type 1 (first stream header byte bit $80 is 1):  Every byte
    // in the frame header serves as an index into a lookup table,
    // which specifies the sample codebook for the frame's samples
    // and also specifies an adjustment to the scaling factor code
    // in the stream header.  Format Type 1 thus allows every frame
    // to specify its own scaling factor.
    int frameFormatType = (hdr[0] & 0x80) >> 7;

    // The high bits of the second and third header bytes form an
    // additional, orthogonal sub-format type code.  Interpret
    // these as two bits of a binary integer, forming values 0..3.
    int frameSubFormatType = ((hdr[1] & 0x80) >> 6) | ((hdr[2] & 0x80) >> 7);

    // set up to read from the current channel's stream
    ROMBitPointer playbackBitPtr = stream.playbackBitPtr;

    // get the channel's mixing multiplier
    uint16_t mixingMultiplier = channel.mixingMultiplier;

    // Get the appropriate pre-adjustment table pointer, based on the
    // high bits of the 2nd and 3rd header bytes.
    // 
    // The original ROM code that implemented this table selection
    // appears to have a bug in its construction.  It takes the high 
    // bits of the second and third header bytes and combines them
    // into an int value with the second byte's high bit shifted left
    // one.  The possible combined values are thus $0000, $0080, $0100,
    // and $0180.  The ROM code tests this value for cases <=0, <=1,
    // <=3, and >3.  Out of these, the only cases that can possibly 
    // match are <=0 and >3.  The construction of the code (selecting 
    // one of four tables based on two bits of input) strongly suggests
    // that the real intention was to test for cases $0000, $0080,
    // $0100, and $0180, which would be equivalent to shifting the
    // combined value right by 7 bits and testing against 0, 1, 2,
    // and 3.  Even then, the original ROM code *still* looks buggy,
    // in that it tests for <=3 rather than <=2 as the third case.
    // The explanation that would make the most sense to me is that
    // no one ever ended up using sub-format codes $0080 or $0100,
    // but that's not it - I've found examples of at least $0100.
    // The other explanation is that sub-formats $0080, $0100, and
    // $0180 were all truly meant to use the same mapping tables,
    // and the other two tables are just dead code that no one ever
    // bothered to remove
    static const uint16_t preAdjMap0[0x0010] ={  // format subtype 0 table
        0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    static const uint16_t preAdjMap3[0x0010] ={  // format subtype 1-3 table
        0, 0, 0, 0, 1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4
    };
    const uint16_t *preAdjMap = (frameSubFormatType == 0) ? preAdjMap0 : preAdjMap3;

    // These two tables are in the original ADSP-2105 decoders, but
    // the code is structured in a way that makes it impossible for
    // either of these to be selected.  All paths lead to map 0 or 3.
    // I'm including these as comments for the sake of documentation.
    //
    // static const uint16_t preAdjMap1[0x0010] ={  // this table is never used
    //     0, 0, 0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    // };
    // static const uint16_t preAdjMap2[0x0010] ={  // this table is never used
    //     0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
    // };

    // Build the pre-adjustment table.  This provides an additional
    // adjustment to the scaling code specified in the stream header,
    // for bands 0-2 only, based on the PRIOR frame's band type codes.
    // The adjustment is added to the scaling type code in each band.
    // Because it's based on the previous band type codes, we have to
    // calculate the adjustments before reading the new frame header,
    // which updates the band type codes for the new frame.
    uint16_t preAdj[3] ={ 0, 0, 0 };
    for (int i = 0 ; i < 3 ; ++i)
        preAdj[i] = preAdjMap[stream.bandTypeBuf[i]];

    // Process the header adjustment block for the frame.  Each frame
    // starts with an array of up/down adjustments for the band sample
    // widths in the header, packed into a bit array using a variable 
    // bit width Huffman-like encoding.  Unpack each element and add
    // it to the running total in the working copy of the header.
    for (uint16_t i = 0 ; i < 16 && (stream.header[i] & 0x7F) != 0x7F ; ++i)
    {
        // Variable-bit-length decoding table.  This contains the decoding 
        // instructions for a Huffman-like encoding, where each symbol is 
        // encoded with a varying number of bits.  This array represents a
        // binary tree.  Each element is a node with two children, one that
        // we follow on a '0' bit in the input and the other for a '1' bit.
        // The '0' bit link is simply the next higher array element.  The
        // '1' bit link is the array element at the offset from the current
        // element given by the value in the node.
        // 
        // Terminal nodes (which have no children) are marked with the high
        // bit ($8000) set.  The remaining 15 bits contain the decoded
        // integer value for the input bit pattern that led to that node,
        // excess $2E.  
        //
        // Note that the values are all deltas from the previous frame, so
        // the decoded value is added to the previous type code to yield the
        // new type code.  (This minimizes the data size for the typical
        // case where the type code is the same from one frame to the next,
        // because the Huffman table encodes the final value $0000 in the
        // shortest bit string, '01'.  It only takes a maximum of 32 bits
        // to indicate a frame that has the same bit size settings as the
        // previous frame.)
        static const uint16_t huffTree[] ={
            0x003c, 0x0002, 0x802d, 0x0038, 0x0002, 0x8030, 0x0034, 0x0032,
            0x0030, 0x002e, 0x002c, 0x0002, 0x802a, 0x0028, 0x0026, 0x0024,
            0x0022, 0x0020, 0x001e, 0x001a, 0x0012, 0x0008, 0x0006, 0x0004,
            0x0002, 0x8038, 0x8023, 0x8025, 0x803a, 0x0008, 0x0006, 0x0004,
            0x0002, 0x8024, 0x8020, 0x8022, 0x8026, 0x801f, 0x0006, 0x0002,
            0x801e, 0x0002, 0x803c, 0x8021, 0x8027, 0x0002, 0x803b, 0x8039,
            0x8028, 0x8037, 0x8029, 0x8036, 0x8035, 0x8034, 0x8033, 0x8032,
            0x802b, 0x8031, 0x802c, 0x802f, 0x802e
        };

        // start at the root of the tree
        const uint16_t *node = huffTree;

        // Traverse the tree until we reach a terminal node marked with $8000
        do
        {
            // get the next input bit
            auto bit = playbackBitPtr.Get(1);

            // advance to the next tree node:  follow the link in the low byte
            // on a '1' bit, increment to the next element on a '0' bit
            node += (bit != 0) ? *node : 1;
        } 
        while ((*node & 0x8000) == 0);

        // Get the terminal node's value, in the low byte of the node.  The
        // value is stored excess $2E.  This is a differential value, so add 
        // it to the previous band type code for this slot.
        channel.audioStream.bandTypeBuf[i] += (*node & 0xFF) - 0x2E;
    }

    // loop through the bands listed in the header
    uint16_t outputBufIndex = 1;
    bool outbufValid = true;
    for (uint16_t bandIndex = 0 ; hdr < &stream.header[16] ; ++bandIndex)
    {
        // read the next stream header byte; stop if the low 7 bits are $7F
        int curStreamHdrByte = *hdr++ & 0x007F;
        if (curStreamHdrByte == 0x7F)
            break;

        // Band sample count table.  This gives the number of output 
        // samples generated per band.
        const static int outputCountTab[] ={
            7, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 32
        };
        int outputCount = outputCountTab[bandIndex];
        int outputInc = 1;

        // If bit $40 of the stream header byte is non-zero, there are
        // only half as many inputs as outputs, with the inputs mapping 
        // to outputs at every other sample slot.  (The skipped outputs
        // are set to zero.)
        if ((curStreamHdrByte & 0x40) != 0)
        {
            outputInc = 2;
            outputCount /= 2;
        }

        // Get the current band type code.  This is interpreted according
        // to the format type code in the main stream header:
        // 
        // Format type 0:
        //   Band type code = bit width of input samples
        // 
        // Format type 1:
        //   Band type code is an index into a lookup table that yields
        //   the sample bit width.
        // 
        // The special value zero means (for both format types) that there
        // are no input samples for this block, so all of the outputs for
        // the band are set to zero.
        int curBandTypeCode = stream.bandTypeBuf[bandIndex];
        if (curBandTypeCode == 0)
        {
            // Sample bit width is zero - this block's samples all have
            // value 0, encoded with no bits at all.  There's no input to
            // consume since the samples are all encoded with zero bits 
            // each, and there's nothing to add to the output since the
            // sample values are all zero.  Simply skip past the samples 
            // in the output buffer.
            outputBufIndex += outputCount;
        }
        else
        {
            // Non-zero sample bit width - this band contains input sample 
            // data.

            // The current stream header byte gives the scaling factor code (see
            // below for how this is interpreted), and the current frame header
            // byte gives the sample width, as a table index.  The table to use
            // depends upon the frame format type specified in bit $80 of the
            // first stream header byte.
            int scalingFactorCode = curStreamHdrByte;
            if (frameFormatType == 0)
            {
                // Frame format type 0.  The stream header byte for the band
                // specifies the sample scaling factor directly, and the band
                // type code is directly specified in the frame header.  No
                // additional adjustments to the scaling factor code or band
                // type code are needed.
            }
            else
            {
                // Frame format type 1.  The band type code in the frame header 
                // doesn't specify the sample size directly.  Instead, it's a 
                // selector into a lookup table, which contains the actual
                // input sample bit width plus an adjustment for the 
                // scaling factor code.
                struct BandXlat
                {
                    int typeCode;       // translated band type code
                    int scalingAdj;     // scaling code adjustment (added to scaling code)
                };
                const BandXlat *tableEntry = nullptr;
                if (bandIndex < 3)
                {
                    // sample bands 0..2 - add the pre-adjustment value for the band
                    curStreamHdrByte += preAdj[bandIndex];

                    // use the translation table for bands 0..2
                    static const BandXlat xlatBand02[0x0010] ={
                        { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x02, 0x00 }, { 0x03, 0x00 },
                        { 0x04, 0x00 }, { 0x04, 0x02 }, { 0x04, 0x05 }, { 0x05, 0x05 },
                        { 0x05, 0x09 }, { 0x05, 0x0d }, { 0x06, 0x0d }, { 0x06, 0x11 },
                        { 0x06, 0x15 }, { 0x07, 0x19 }, { 0x07, 0x1d }, { 0x08, 0x1d }
                    };
                    tableEntry = &xlatBand02[curBandTypeCode];
                }
                else if (bandIndex < 6)
                {
                    // sample bands 3..5
                    static const BandXlat xlatBand35[0x0010] ={
                        { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x02, 0x00 }, { 0x03, 0x00 },
                        { 0x04, 0x00 }, { 0x04, 0x02 }, { 0x04, 0x07 }, { 0x04, 0x0b },
                        { 0x05, 0x0b }, { 0x05, 0x0f }, { 0x05, 0x13 }, { 0x05, 0x17 },
                        { 0x06, 0x17 }, { 0x06, 0x1b }, { 0x06, 0x1f }, { 0x07, 0x1f }
                    };
                    tableEntry = &xlatBand35[curBandTypeCode];
                }
                else
                {
                    // sample bands 6..15
                    static const BandXlat xlatBand6F[0x0010] ={
                        { 0x00, 0x00 }, { 0x01, 0x00 }, { 0x02, 0x00 }, { 0x03, 0x00 },
                        { 0x03, 0x02 }, { 0x04, 0x02 }, { 0x04, 0x07 }, { 0x04, 0x0b },
                        { 0x05, 0x0b }, { 0x05, 0x0f }, { 0x05, 0x13 }, { 0x05, 0x17 },
                        { 0x06, 0x17 }, { 0x06, 0x1b }, { 0x06, 0x1f }, { 0x07, 0x23 }
                    };
                    tableEntry = &xlatBand6F[curBandTypeCode];
                }

                // Get the new band type code from the table, and adjust the
                // scaling factor code by adding the adjustment from the table.
                curBandTypeCode = tableEntry->typeCode;
                scalingFactorCode = curStreamHdrByte + tableEntry->scalingAdj;
            }

            // The sample scaling factor code is a combination of bit fields packed
            // into the low 6 bits of the value, as 'xxeeeemm' 
            //
            //   xx   = not used in this step
            //   eeee = exponent
            //   mm   = mantissa
            // 
            // The mantissa bits represent:
            //   00 -> $8000
            //   01 -> $9838
            //   10 -> $B505
            //   11 -> $D745
            // 
            // eeee is the binary exponent, represented excess 15 (i.e., subtract
            // 15 from the nominal value: 0001 = 1-15 = -14).
            static uint16_t scalingFactorTable[] ={ 0x8000, 0x9838, 0xb505, 0xd745 };
            uint16_t sampleScalingFactor = scalingFactorTable[scalingFactorCode & 0x0003] >> (15 - ((scalingFactorCode >> 2) & 0x000F));

            // decompress the current band into a working buffer, which we'll add 
            // into the combined output buffer later
            uint16_t curBandOutputBuf[0x20];
            uint16_t *curBandOutputPtr = curBandOutputBuf; 
            if (curBandTypeCode == 0)
            {
                // This is an internal error - flag the output so far as invalid
                // and stop the channel.
                outbufValid = false;
                channel.stop = true;
            }
            else if (curBandTypeCode <= 6)
            {
                // Codes 1-6 use a Huffman-like compression format that encodes
                // the samples with a varying number of bits.

                // figure the reference value for the block
                int sampleValueRef = 1 << (curBandTypeCode - 1);

                // Get the maximum bit width for the code words.  This is the
                // lookahead bit length that we have to read to get the codebook
                // table index.  This is only the lookahead amount - many codes
                // are smaller than the maximum.  The codebook tells us how many
                // bits we actually need to consume for each input.
                static int maxBitWidthTab[] ={ 0, 2, 3, 5, 7, 8, 9 };
                int maxBitWidth = maxBitWidthTab[curBandTypeCode];

                // Huffman codebooks for the sample bit sizes.  These tables map
                // the sample input bit patterns to their decoded values.  The 
                // samples are encoded with a Huffman-like varying-bit-length 
                // encoding, with a collection of pre-defined codebooks.  Each
                // codebook maps a varying-bit-length codeword to a fixed-width
                // integer value, for output bit widths 1 through 6.
                // 
                // These tables specify the decoding scheme by providing arrays
                // directly indexed by the *longest possible bit string* for each
                // encoding type.  There are six possible encodings, with maximum
                // bit string lengths of 2, 3, 5, 7, 8, and 9, respectively.  To
                // decode the bit strings using these arrays, we first read ahead
                // in the input stream by the maximum bit width, converting the
                // bit string into a binary integer value.  For example, if this
                // block has type code 3, it corresponds to codebook, which has a
                // maximum codeword length of 5 bits.  So we read ahead by 5 bits,
                // treating the 5 bits as a binary integer value, which can have
                // a value 0..31.  We use this as an index into the 5-bit mapping 
                // table.
                // 
                // The indexed lookup yields a value that encodes the decoded
                // integer value in the low byte, and the *actual* number of input
                // bits used to express that value in the high byte.  Since we've
                // only "peeked" at the bits in the stream so far, we use the high
                // byte to determine how many bits of input to consume from the
                // input stream.
                // 
                // The low byte is an unsigned 8-bit integer that's relative to
                // the "reference" value for the bit width, which is 2^(typecode-1).
                // For example, type code 2 has reference value 2^(2-1) == 4.  To 
                // get the final decoded value, we subtract the reference value 
                // from the unsigned 8-bit value in the low byte of the table.
                // 
                // The high bit ($80) of the value byte (low byte) has a special 
                // meaning.  If set, the sample generates *two* outputs with value
                // zero.  This is presumably such a common case that it provides
                // significant data compression to include the extra bit for it.
                //
                static const uint16_t codebook1[0x0004] ={
                    0x0201, 0x0200, 0x0180, 0x0180
                };
                static const uint16_t codebook2[0x0008] ={
                    0x0201, 0x0201, 0x0300, 0x0302, 0x0203, 0x0203, 0x0280, 0x0280
                };
                static const uint16_t codebook3[0x0020] ={
                    0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205,
                    0x0203, 0x0203, 0x0203, 0x0203, 0x0203, 0x0203, 0x0203, 0x0203,
                    0x0407, 0x0407, 0x0500, 0x0501, 0x0306, 0x0306, 0x0306, 0x0306,
                    0x0304, 0x0304, 0x0304, 0x0304, 0x0402, 0x0402, 0x0480, 0x0480
                };
                static const uint16_t codebook4[0x0080] ={
                    0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a,
                    0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a, 0x030a,
                    0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306,
                    0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306,
                    0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308,
                    0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308, 0x0308,
                    0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c,
                    0x0503, 0x0503, 0x0503, 0x0503, 0x050d, 0x050d, 0x050d, 0x050d,
                    0x040b, 0x040b, 0x040b, 0x040b, 0x040b, 0x040b, 0x040b, 0x040b,
                    0x0405, 0x0405, 0x0405, 0x0405, 0x0405, 0x0405, 0x0405, 0x0405,
                    0x060f, 0x060f, 0x0602, 0x0602, 0x0580, 0x0580, 0x0580, 0x0580,
                    0x060e, 0x060e, 0x0700, 0x0701, 0x0504, 0x0504, 0x0504, 0x0504,
                    0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309,
                    0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309, 0x0309,
                    0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307,
                    0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307, 0x0307
                };
                static const uint16_t codebook5[0x0100] ={
                    0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311,
                    0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311,
                    0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311,
                    0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311, 0x0311,
                    0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f,
                    0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f,
                    0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f,
                    0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f, 0x030f,
                    0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c,
                    0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c, 0x040c,
                    0x0518, 0x0518, 0x0518, 0x0518, 0x0518, 0x0518, 0x0518, 0x0518,
                    0x071d, 0x071d, 0x0800, 0x0801, 0x0606, 0x0606, 0x0606, 0x0606,
                    0x0516, 0x0516, 0x0516, 0x0516, 0x0516, 0x0516, 0x0516, 0x0516,
                    0x061a, 0x061a, 0x061a, 0x061a, 0x0680, 0x0680, 0x0680, 0x0680,
                    0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413,
                    0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413, 0x0413,
                    0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d,
                    0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d, 0x040d,
                    0x050a, 0x050a, 0x050a, 0x050a, 0x050a, 0x050a, 0x050a, 0x050a,
                    0x0704, 0x0704, 0x071c, 0x071c, 0x0608, 0x0608, 0x0608, 0x0608,
                    0x0515, 0x0515, 0x0515, 0x0515, 0x0515, 0x0515, 0x0515, 0x0515,
                    0x0607, 0x0607, 0x0607, 0x0607, 0x0619, 0x0619, 0x0619, 0x0619,
                    0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410,
                    0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410, 0x0410,
                    0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412,
                    0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412, 0x0412,
                    0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e,
                    0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e, 0x040e,
                    0x050b, 0x050b, 0x050b, 0x050b, 0x050b, 0x050b, 0x050b, 0x050b,
                    0x081f, 0x0802, 0x0705, 0x0705, 0x071b, 0x071b, 0x081e, 0x0803,
                    0x0617, 0x0617, 0x0617, 0x0617, 0x0609, 0x0609, 0x0609, 0x0609,
                    0x0514, 0x0514, 0x0514, 0x0514, 0x0514, 0x0514, 0x0514, 0x0514
                };
                static const uint16_t codebook6[0x0200] ={
                    0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d,
                    0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d,
                    0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d,
                    0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d, 0x041d,
                    0x083a, 0x083a, 0x0900, 0x0901, 0x070c, 0x070c, 0x070c, 0x070c,
                    0x0614, 0x0614, 0x0614, 0x0614, 0x0614, 0x0614, 0x0614, 0x0614,
                    0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519,
                    0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519, 0x0519,
                    0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527,
                    0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527, 0x0527,
                    0x0734, 0x0734, 0x0734, 0x0734, 0x0807, 0x0807, 0x0839, 0x0839,
                    0x062b, 0x062b, 0x062b, 0x062b, 0x062b, 0x062b, 0x062b, 0x062b,
                    0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420,
                    0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420,
                    0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420,
                    0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420, 0x0420,
                    0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422,
                    0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422,
                    0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422,
                    0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422, 0x0422,
                    0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e,
                    0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e,
                    0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e,
                    0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e, 0x041e,
                    0x0615, 0x0615, 0x0615, 0x0615, 0x0615, 0x0615, 0x0615, 0x0615,
                    0x070d, 0x070d, 0x070d, 0x070d, 0x0733, 0x0733, 0x0733, 0x0733,
                    0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526,
                    0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526, 0x0526,
                    0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a,
                    0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a, 0x051a,
                    0x093f, 0x093e, 0x0808, 0x0808, 0x0710, 0x0710, 0x0710, 0x0710,
                    0x0838, 0x0838, 0x0902, 0x0903, 0x070e, 0x070e, 0x070e, 0x070e,
                    0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421,
                    0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421,
                    0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421,
                    0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421, 0x0421,
                    0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f,
                    0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f,
                    0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f,
                    0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f, 0x041f,
                    0x062a, 0x062a, 0x062a, 0x062a, 0x062a, 0x062a, 0x062a, 0x062a,
                    0x0616, 0x0616, 0x0616, 0x0616, 0x0616, 0x0616, 0x0616, 0x0616,
                    0x0809, 0x0809, 0x0837, 0x0837, 0x072f, 0x072f, 0x072f, 0x072f,
                    0x0732, 0x0732, 0x0732, 0x0732, 0x0711, 0x0711, 0x0711, 0x0711,
                    0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b,
                    0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b, 0x051b,
                    0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525,
                    0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525, 0x0525,
                    0x093d, 0x0904, 0x080a, 0x080a, 0x070f, 0x070f, 0x070f, 0x070f,
                    0x0617, 0x0617, 0x0617, 0x0617, 0x0617, 0x0617, 0x0617, 0x0617,
                    0x0629, 0x0629, 0x0629, 0x0629, 0x0629, 0x0629, 0x0629, 0x0629,
                    0x072e, 0x072e, 0x072e, 0x072e, 0x0731, 0x0731, 0x0731, 0x0731,
                    0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524,
                    0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524, 0x0524,
                    0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c,
                    0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c, 0x051c,
                    0x0712, 0x0712, 0x0712, 0x0712, 0x0836, 0x0836, 0x093c, 0x093b,
                    0x072d, 0x072d, 0x072d, 0x072d, 0x080b, 0x080b, 0x0905, 0x0906,
                    0x0628, 0x0628, 0x0628, 0x0628, 0x0628, 0x0628, 0x0628, 0x0628,
                    0x0713, 0x0713, 0x0713, 0x0713, 0x0730, 0x0730, 0x0730, 0x0730,
                    0x0618, 0x0618, 0x0618, 0x0618, 0x0618, 0x0618, 0x0618, 0x0618,
                    0x0835, 0x0835, 0x0880, 0x0880, 0x072c, 0x072c, 0x072c, 0x072c,
                    0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523,
                    0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523, 0x0523
                };

                // Get the Huffman codebook as selected by the type code.  The
                // type code corresponds to the number of bits of "plain" integer
                // value encoded in the sample codewords.
                static const uint16_t *codebookByTypeCode[] ={ 
                    codebook1, codebook2, codebook3, codebook4, codebook5, codebook6 
                };
                const uint16_t *codebook = codebookByTypeCode[curBandTypeCode - 1];

                // process the samples
                for (int i = outputCount ; i != 0 ; --i)
                {
                    // peek at the next maxBitWidth bits of input
                    uint32_t lookahead = playbackBitPtr.Peek(maxBitWidth);

                    // decode the lookahead bits through the varying-bit-length lookup table
                    auto entry = codebook[lookahead];
                    int val = static_cast<int>(entry & 0xFF);
                    int nBits = static_cast<int>(entry >> 8);

                    // consume the bits indicated in the high byte
                    playbackBitPtr.Get(nBits);

                    // check bit $80 of the value byte
                    if ((val & 0x80) != 0)
                    {
                        // high bit set - output TWO samples with value zero (making sure 
                        // first that we have space available)
                        if (i >= 2)
                        {
                            // output the two zeroes
                            *curBandOutputPtr++ = 0;
                            *curBandOutputPtr++ = 0;

                            // deduct the extra output from the loop counter
                            --i;
                        }
                        else
                        {
                            outbufValid = false;
                            channel.stop = true;
                            i = 1;
                        }
                    }
                    else
                    {
                        // high bit is zero - the output value is the value byte minus the reference
                        *curBandOutputPtr++ = val - sampleValueRef;
                    }
                }
            }
            else
            {
                // For codes 7+, it's a simple array of fixed-bit-width samples,
                // encoded as signed 2's complement integer values.
                int sampleBitWidth = curBandTypeCode;
                for (auto i = outputCount ; i != 0 ; --i)
                    *curBandOutputPtr++ = static_cast<uint16_t>(playbackBitPtr.GetSigned(sampleBitWidth));
            }

            // If we encountered an error, the whole frame is assumed to be
            // corrupted - zero the output buffer.
            if (!outbufValid)
                memset(curBandOutputBuf, 0, sizeof(curBandOutputBuf));

            // Add the current band buffer into the aggregate output buffer.  The
            // raw samples are all scaled by the input block's sample scaling factor
            // and by the channel's mixing-level multiplier.
            for (uint16_t i = 0 ; i < outputCount ; ++i, outputBufIndex += outputInc)
            {
                auto scaledSample = static_cast<uint16_t>(static_cast<int64_t>(SIGNED(curBandOutputBuf[i])) * sampleScalingFactor);
                auto prod = (static_cast<uint64_t>(SIGNED(outputBuffer[outputBufIndex])) << 16) | scaledSample;
                prod += static_cast<int64_t>(SIGNED(scaledSample)) * mixingMultiplier;
                outputBuffer[outputBufIndex] = static_cast<uint16_t>((prod >> 16) & 0xFFFF);
            }
        }
    }

    // propagate the delta in the first sample to the zeroeth sample, and keep the original first sample
    auto delta = SaturateInt16(static_cast<int32_t>(SIGNED(outputBuffer[1])) - static_cast<int32_t>(SIGNED(outbuf1)));
    outputBuffer[0] = SaturateInt16(static_cast<int32_t>(SIGNED(delta)) + static_cast<int32_t>(SIGNED(outputBuffer[0])));
    outputBuffer[1] = outbuf1;

    // save back the updated stream pointer
    stream.playbackBitPtr = playbackBitPtr;
}

// --------------------------------------------------------------------------
//
// Decompress a frame from the 1993 format.  This is the format used in
// the first three DCS pinball titles, released in 1993: STTNG, IJTPA, JD.
// 
// The 1993 format contains the same information as the later format
// (frequency-domain samples for a 7.68ms time window, in the same 16-bit
// signed fixed-point 1.15 fraction format), but the details of the frame
// encoding are substantially different.  Some elements are arranged in a
// different order, and the Huffman codebooks are all different.  The
// formats are different enough that it's cleaner to handle them in
// separate functions than to try to unify them; a unified function would
// just end up as a gigantic 'if' with two branches, one for each version.
// 
// To make matters even more interesting, Judge Dredd has its own unique
// format for "Type 1" streams (marked by bit $80 set in the first byte of
// the stream header).  So we have to further specialize the decoders into
// OS93a and OS93b sub-types.  (Indiana Jones used the same software as
// Judge Dredd, so in principle it also needs the special OS93a Type 1
// stream decoder, but the original IJTPA ROMs don't actually contain any
// Type 1 streams.  Modded IJTPA ROMs could conceivably add some.)
// 
// Here's the division of labor for the OS93a and OS93b frame types:
// 
//  - This base class handles OS93a Type 0 streams and OS93b Type 0 
//    and 1 streams, since all of those have a common frame format.
// 
//  - The override in subclass DecoderImpl93a handles OS93a Type 1
//    streams
// 
void DCSDecoderNative::DecoderImpl93::DecompressFrame(Channel &channel, uint16_t *outputBuffer)
{
    // set up pointers to the stream
    auto &stream = channel.audioStream;
    ROMBitPointer playbackBitPtr = stream.playbackBitPtr;
    ROMPointer hdrPtr = stream.headerPtr;

    // Save the sample at the second frame buffer element
    uint16_t outbuf1 = outputBuffer[1];

    // get a pointer to the end of the header
    ROMPointer hdrEnd = hdrPtr;
    hdrEnd.p += 16;

    // get the format variation from the high bit of the first header byte
    int streamFormatType = (*hdrPtr & 0x80) >> 7;
    int bandSubType = (streamFormatType == 1 ? 0 : 2);

    // get the channel's mixing level multiplier, for scaling the samples
    // as we mix them into the output buffer
    const uint16_t mixingMultiplier = channel.mixingMultiplier;

    // decoder state variables
    bool isFirstBand = true;
    uint16_t prvInput = 0;
    uint16_t prvInputDelta = 0;
    bool reuseBandTypeCode = false;
    int curBandTypeCode = 0;

    // set I3 to the beginning of the output buffer (plus one sample)
    int outputBufIndex = 1;

    // process each band in the header
    for (int band = 0 ; hdrPtr.p < hdrEnd.p ; ++band)
    {
        // read the next header byte, masking out the high bit
        uint16_t curHdrByte = hdrPtr.GetU8() & 0x7F;

        // $7F marks the end of the header
        if (curHdrByte == 0x7F)
            break;

        // separate it into the bit fields: scaling factor index and shift, 
        // output stride (sample density)
        uint16_t scalingFactorIndex = curHdrByte & 0x0003;
        uint16_t scalingFactorShift = ((curHdrByte >> 2) & 0x000F) - 0x000F;
        uint16_t outputStrideCode = curHdrByte >> 6;

        // figure the scaling factor
        static const uint16_t scalingFactorMantissa[] ={ 0x8000, 0x9838, 0xb505, 0xd745 };
        uint16_t scalingFactor = static_cast<uint16_t>(BitShift32(scalingFactorMantissa[scalingFactorIndex], SIGNED(scalingFactorShift)));

        // figure how many outputs we're generating for this block, and
        // their spacing in the output buffer
        uint16_t nSamples = 0;          // number of input samples for this block
        int outputBufInc = 1;           // output buffer increment per sample generated
        int outputBufFixup = 0;         // adjustment to output buffer pointer after last sample (0 or -1)
        int outputBufStride = 16;       // total output buffer pointer increment for this block
        if (streamFormatType == 0)
        {
            if (outputStrideCode == 0)
            {
                nSamples = 16;
                outputBufInc = 1;
                outputBufFixup = 0;
                outputBufStride = 16;
            }
            else
            {
                ++outputBufIndex;
                nSamples = 16;
                outputBufInc = 2;
                outputBufFixup = -1;
                outputBufStride = 31;
            }
        }
        else
        {
            if (outputStrideCode == 0)
            {
                outputBufInc = 1;
                outputBufFixup = 0;
                nSamples = outputBufStride = (isFirstBand ? 15 : 16);
            }
            else
            {
                outputBufInc = 2;
                outputBufFixup = 0;
                nSamples = outputBufStride = 8;
            }
        }

        // If the last band used type 0, the new band can reuse type 0
        // by placing a '1' bit next in the stream.  A '0' bit means
        // that there's a new band type encoded as usual.
        if (reuseBandTypeCode)
            reuseBandTypeCode = (playbackBitPtr.Get(1) != 0);

        // If we didn't already have a band type code, or the "reuse"
        // bit was zero, get the next band type code
        if (!reuseBandTypeCode)
        {
            // The type code specification depends on the stream format type
            if (streamFormatType == 0)
            {
                // Stream format type 0
                // 
                // If the next bit is 1, the band has a new subtype; otherwise,
                // it reuses the subtype from the previous band.
                if (playbackBitPtr.Get(1) != 0)
                {
                    // The subtype is specified with one bit that selects the
                    // new subtype based on the old subtype.  (There are only
                    // three subtypes, so if we're changing, we can only change
                    // to one of the other two, thus we only need one bit to
                    // specify it.  The bit effectively specifies +1 or -1
                    // mod 2.)
                    static const uint16_t subTypeDec[] ={ 0x0002, 0x0000, 0x0001 };  // sub 1 mod 2
                    static const uint16_t subTypeInc[] ={ 0x0001, 0x0002, 0x0000 };  // add 1 mod 2
                    const uint16_t *subTypeXlat = (playbackBitPtr.Get(1) != 0) ? subTypeInc : subTypeDec;
                    bandSubType = subTypeXlat[bandSubType];
                }

                // The band type code is given by the next 4 bits, as an
                // independent value
                curBandTypeCode = playbackBitPtr.Get(4);
            }
            else
            {
                // Stream format type 1
                //
                // The type code is specified differentially from the last
                // frame, using a predefined Huffman codebook.  The bit string
                // also encodes whether the sub-type is kept the same or inverted.
                // Inverting the subtype means changing 0 to 1 and 1,2 to 0.
                stream.bandTypeBuf[band] += ReadHuff93(playbackBitPtr, bandSubType);
                curBandTypeCode = stream.bandTypeBuf[band];
            }
        }

        // helper function to mix a sample into the output buffer
        auto AddOutput = [outputBuffer, &scalingFactor, &outputBufIndex, mixingMultiplier, &outputBufInc]
        (uint16_t sample)
        {
            auto prod = static_cast<uint64_t>(static_cast<int64_t>(SIGNED(sample)) * scalingFactor);
            auto prodLow = static_cast<uint16_t>(prod & 0xFFFF);
            prod = (prod & 0xFFFF) | (static_cast<uint64_t>(SIGNED(outputBuffer[outputBufIndex])) << 16);
            prod += static_cast<uint64_t>(static_cast<int64_t>(SIGNED(prodLow)) * static_cast<uint64_t>(mixingMultiplier));
            outputBuffer[outputBufIndex] = static_cast<uint16_t>((prod >> 16) & 0xFFFF);
            outputBufIndex += outputBufInc;
        };

        // Check the band type code
        if (curBandTypeCode == 0)
        {
            // Code zero: there are no new inputs in this frame; it simply
            // fills in zeroes or repeats the last input, depending on the
            // frame subtype. 
            // 
            // A type zero frame can be repeated on the next frame in a
            // compact format (indicated by the first bit of the next frame
            // being set to 1).
            reuseBandTypeCode = true;

            // check the subtype
            switch (bandSubType)
            {
            case 0:
                // subtype 0: independent inputs, all zeroes - we can just
                // skip this block of outputs, since adding zero doesn't
                // change the outputs
                outputBufIndex += outputBufStride;

                prvInput = 0;
                prvInputDelta = 0;
                break;

            case 1:
                // Subtype 1: repeat the previous output.  Note that the arithmetic
                // in this loop has a slight anomaly vs all of the other parallel
                // cases, in that it carries forward the low 16 bits of the product
                // from sample to sample.  I think this is actually a bug in the
                // original DCS code - it looks like it was meant to work like all
                // of the other cases and just carry forward the previous sample
                // each time (that would be the correct parallel with Subtype 1
                // for the non-zero band type codes).  But it happens to carry
                // forward the low 16 bits of the last product as well, because
                // they moved the initial multiplier load outside of the loop,
                // rather than repeating it on each loop as they do in all of the
                // other cases.  It looks like it was meant to be an optimization:
                // "it's the same value every time, so let's just load it once."
                // But it changes the meaning of the loop subtly, and I think
                // unintentionally.  On each loop iteration after the first, the
                // low word of the multiplier starts out with whatever was left
                // over from the last calculation rather than being reloaded with
                // the previous sample, which is clearly the intention.  The low
                // word of the multiplier isn't directly retained in the output,
                // but it determines the rounding direction of the subsequent
                // addition step, so it does change the output.  But since it's a
                // rounding error, it makes only a tiny difference - a few parts
                // per thousand even when accumulating across the 16 samples of
                // the band.  It's not audible, and no one would have noticed it
                // in practical testing, short of comparing the exact PCM outputs
                // (which I *am* doing, and which is how I found it).  If we
                // "fix" it by changing the loop to call AddSample(prvInput),
                // which is clearly the intention of the code, we get 8 frames
                // of diffs in STTNG running the validation suite (i.e., playing
                // every track of every game once and comparing the PCM output
                // to the ADSP-2105 emulator implementation; and that's 8 frames
                // containing any diffs at all, out of 184000 played in STTNG
                // alone).  I think it's more faithful to the spirit of the format
                // design to use my "fixed" version, but the higher priority is
                // exact replication of the original implementation, even if it
                // means replicating its bugs, because that's the only way to be
                // certain the new decoder isn't adding new bugs of its own.
                //
                // This whole code block clearly *should* have been written:
                //
                //   for (uint16_t i = 0 ; i < nSamples ; ++i) 
                //     AddSample(prvInput);
                {
                    auto prod = static_cast<int64_t>(SIGNED(prvInput)) * scalingFactor;
                    auto prodLow = static_cast<int16_t>(prod & 0xFFFF);
                    for (uint16_t i = 0 ; i < nSamples ; ++i)
                    {
                        // Note: this is the point where the anomaly mentioned
                        // above occurs.  Rather than reloading prod's low word
                        // with prvInput here, we carry forward the leftover
                        // bits from the previous loop iteration.  The leftover
                        // bits will only affect the result to the extent that
                        // they cause a different rounding result on the next
                        // step from the result we would have gotten if prvInput
                        // had been reloaded instead.
                        prod = (prod & 0xFFFF) | (static_cast<int64_t>(SIGNED(outputBuffer[outputBufIndex])) << 16);

                        prod += static_cast<int64_t>(prodLow) * mixingMultiplier;
                        outputBuffer[outputBufIndex] = static_cast<uint16_t>((prod >> 16) & 0xFFFF);
                        outputBufIndex += outputBufInc;
                    }
                    prvInputDelta = 0;
                    outputBufIndex += outputBufFixup;
                }
                break;

            case 2:
                // subtype 2: repeat the previous output with an increment
                for (uint16_t i = 0 ; i < nSamples ; ++i)
                {
                    prvInput += prvInputDelta;
                    AddOutput(prvInput);
                }
                outputBufIndex += outputBufFixup;
                break;
            }
        }
        else
        {
            // Non-zero band type code - this band has fixed-bit-width inputs
            uint16_t inputBuf[0x10];
            uint16_t *p2 = inputBuf;

            // figure the input bit width from the band type code
            int bitWidth = curBandTypeCode;
            if (streamFormatType == 0)
                bitWidth += 1;

            // read nSamples * signed inputs with the specified fixed bit width
            for (uint16_t i = nSamples ; i != 0 ; --i)
                *p2++ = playbackBitPtr.GetSigned(bitWidth);

            // process the inputs
            p2 = inputBuf;
            switch (bandSubType)
            {
            case 0:
                // Type 0 - inputs are independent values
                for (uint16_t i = 0 ; i < nSamples ; ++i)
                    AddOutput(*p2++);

                // carry over the previous input and delta
                prvInput = *(p2-1);
                prvInputDelta = prvInput - *(p2-2);
                break;

            case 1:
                // Type 1 - inputs are differential (each input is a delta added to
                // the previous input)
                for (uint16_t i = 0 ; i < nSamples ; ++i)
                {
                    prvInputDelta = *p2++;
                    prvInput += prvInputDelta;
                    AddOutput(prvInput);
                }
                break;

            case 2:
                // Type 2 - inputs are doubly differential (each input is a delta
                // applied to the previous delta, which is then added to the previous
                // input)
                for (uint16_t i = 0 ; i < nSamples ; ++i)
                {
                    prvInputDelta += *p2++;
                    prvInput += prvInputDelta;
                    AddOutput(prvInput);
                }
                break;
            }

            // skip extra outputs as needed
            outputBufIndex += outputBufFixup;
        }

        isFirstBand = false;
    }

    // propagate the delta in the first sample to the zeroeth sample, and keep the original first sample
    auto delta = SaturateInt16(static_cast<int32_t>(SIGNED(outputBuffer[1])) - static_cast<int32_t>(SIGNED(outbuf1)));
    outputBuffer[0] = SaturateInt16(static_cast<int32_t>(SIGNED(delta)) + static_cast<int32_t>(SIGNED(outputBuffer[0])));
    outputBuffer[1] = outbuf1;

    // save the updated playback pointer
    stream.playbackBitPtr = playbackBitPtr;
}

// Read a Huffman-encoded frame type code from a 1993 frame
int DCSDecoderNative::DecoderImpl93::ReadHuff93(ROMBitPointer &p, int &bandSubType)
{
    // Huffman-like varying-bit-length format decoding table.  This array
    // represents a binary tree, in an efficient but somewhat obtuse
    // format.  Each element represents a node, either a node with
    // two children (bit $8000 is zero) or a terminal node (bit $8000
    // is set).  
    //
    // For a node with children, the "0" bit child is the element at
    // the index in the low byte of the table entry, and the "1" child
    // is the element at the index in the high byte.
    // 
    // For a terminal node (bit $8000 is set), the low byte contains
    // the final 
    //
    static int huffTree[] ={
        0x7a01, 0x0302, 0x800e, 0x7904, 0x7605, 0x0706, 0x802e, 0x0908,
        0x802d, 0x730a, 0x700b, 0x0d0c, 0x8013, 0x6d0e, 0x120f, 0x1110,
        0x802b, 0x800b, 0x1413, 0x8015, 0x2a15, 0x2916, 0x1817, 0x8017,
        0x2819, 0x211a, 0x1e1b, 0x1d1c, 0x8037, 0x8026, 0x201f, 0x8008,
        0x8019, 0x2322, 0x8009, 0x2524, 0x801d, 0x2726, 0x8006, 0x801c,
        0x800a, 0x8031, 0x6c2b, 0x392c, 0x382d, 0x2f2e, 0x8018, 0x3730,
        0x3431, 0x3332, 0x8027, 0x8036, 0x3635, 0x8004, 0x8025, 0x8034,
        0x802a, 0x6b3a, 0x6a3b, 0x633c, 0x403d, 0x3f3e, 0x801a, 0x8038,
        0x6241, 0x6142, 0x5c43, 0x5944, 0x5445, 0x4946, 0x4847, 0x8000,
        0x8001, 0x534a, 0x524b, 0x4d4c, 0x8024, 0x4f4e, 0x8021, 0x5150,
        0x803a, 0x803b, 0x8023, 0x8020, 0x5855, 0x5756, 0x803c, 0x803d,
        0x8002, 0x5b5a, 0x8022, 0x8003, 0x605d, 0x5f5e, 0x801f, 0x801e,
        0x8039, 0x801b, 0x8007, 0x6764, 0x6665, 0x8035, 0x8029, 0x6968,
        0x8028, 0x8005, 0x8033, 0x8032, 0x8016, 0x6f6e, 0x8030, 0x8014,
        0x7271, 0x802c, 0x800c, 0x7574, 0x802f, 0x8012, 0x7877, 0x800d,
        0x8011, 0x8010, 0x800f
    };
    int index = 0;
    int ele = huffTree[0];
    do
    {
        // get the next bit
        auto bit = p.Get(1);

        // traverse to the next element, based on the current bit
        index = (bit != 0) ? ele >> 8 : ele & 0xFF;
        ele = huffTree[index];
    } while ((ele & 0x8000) == 0);

    // get terminal value from the low 6 bits of the final node
    int val = ele & 0x3F;

    // The value is encoded excess 0x0F for values up to 0x1D, and
    // excess 0x2E otherwise.  
    if (val < 0x1E)
    {
        // 0x00..0x1D - the value is excess 0x0F
        val -= 0x000F;
    }
    else
    {
        // 0x1E and higher - the value is excess 0x2E
        val -= 0x002E;

        // this also inverts the band sub-type code: if it's zero,
        // change it to 1, and if it's non-zero, change it to zero
        bandSubType = (bandSubType != 0 ? 0 : 1);
    }

    return val;
}

// --------------------------------------------------------------------------
//
// Frame decompressor for OS93a.
//
// Judge Dredd uses a unique interpretation of frames for "Type 1" streams
// (streams marked with the high bit ($80) set in the first header byte).
// All OS93 versions use a common interpretation for "Type 0" streams.  We
// check the stream to see if it's Type 0 or Type 1 (the only two types),
// calling the common handler for Type 0, and doing the required special
// decoding for Type 1.

// sample-pair lookup table for OS93a Type 1 frame decompression
static const uint16_t os93a_type1_samplePairTable[] ={
    0x0000, 0x0000, 0x0000, 0x0000, 0x2aab, 0x0000, 0xd555, 0x0000, 0xd554, 0xd554, 0x2aac, 0xd554, 0x2aac, 0x2aac, 0xd554, 0x2aac,
    0xb296, 0xe062, 0xb704, 0x28d0, 0x300d, 0xbb8f, 0xe873, 0xafc3, 0x5378, 0xfae5, 0x0000, 0x0000, 0xf267, 0x5283, 0x3808, 0x3e13,
    0x9f0a, 0x087c, 0xb68b, 0x3fdb, 0xea1b, 0x5ed6, 0x2608, 0x5998, 0x4975, 0xc025, 0x15e5, 0xa12a, 0xd9f8, 0xa668, 0xac92, 0xcddf,
    0xe90d, 0x2631, 0x1d3b, 0x21a1, 0x2904, 0xee97, 0x0000, 0x0000, 0x536e, 0x3221, 0x60f6, 0xf784, 0xfc1e, 0xd39d, 0xd496, 0xf5fa,
    0xd5f4, 0x3491, 0xf3ec, 0x4a53, 0xbd97, 0xf73d, 0xba3f, 0x1bce, 0x4137, 0x0c09, 0x4492, 0xe7e7, 0x1763, 0x3eb5, 0x39d7, 0x303b,
    0xe5c6, 0xec0a, 0xe088, 0x0c5c, 0x29cf, 0xceaf, 0x04ca, 0xdf7d, 0x1ec5, 0xf4fe, 0x0000, 0x0000, 0xfa86, 0x2161, 0x19f3, 0x15c8,
    0xb8d8, 0xaf1d, 0x9b58, 0xd9ed, 0x11b3, 0x93f9, 0xe35f, 0x9a04, 0xd305, 0x6274, 0x0b8f, 0x6b87, 0x9482, 0x0d1b, 0xa9a3, 0x4105,
    0x6b4d, 0xfb9e, 0x5d19, 0xc994, 0x3d54, 0x58ff, 0x61d4, 0x2e18, 0xe710, 0xc443, 0xc62a, 0xd401, 0x3b68, 0xa839, 0x0ae9, 0xb87b,
    0x6300, 0x13fb, 0x0602, 0x64d1, 0xe8e2, 0x5aca, 0xcbc2, 0x50c2, 0x343e, 0xaf3e, 0x515e, 0xb945, 0x573e, 0xd782, 0x5d1f, 0xf5be,
    0x9720, 0xcdc8, 0xae5f, 0xb993, 0xc59e, 0xa55d, 0xdcde, 0x9128, 0xaea2, 0x46bb, 0xa8c2, 0x287e, 0xa2e1, 0x0a42, 0x9d00, 0xec05,
    0x745e, 0xe189, 0x7a3f, 0xffc6, 0x68e0, 0x3238, 0x51a1, 0x466d, 0x113d, 0x86fa, 0x2e5d, 0x9101, 0x4b7d, 0x9b08, 0x6e7e, 0xc34c,
    0xb483, 0x64f8, 0x9182, 0x3cb4, 0x8ba2, 0x1e77, 0x85c1, 0x003a, 0x3a62, 0x5aa3, 0x2322, 0x6ed8, 0xeec3, 0x7906, 0xd1a3, 0x6eff,
    0x05bf, 0xd7a9, 0x22df, 0xe1b0, 0x28c0, 0xffed, 0x2ea0, 0x1e29, 0x0ba0, 0xf5e5, 0x1180, 0x1422, 0xf460, 0x0a1b, 0xee80, 0xebde,
    0xd160, 0xe1d7, 0xe89f, 0xcda1, 0xffde, 0xb96c, 0x1cfe, 0xc373, 0x1761, 0x325f, 0xfa41, 0x2857, 0xdd21, 0x1e50, 0xd740, 0x0013,
    0x3481, 0x3c66, 0x1d42, 0x509b, 0x0022, 0x4694, 0xe302, 0x3c8d, 0x3a1e, 0xcd7a, 0x3fff, 0xebb7, 0x45e0, 0x09f4, 0x4bc0, 0x2831,
    0xcb7f, 0xc39a, 0xe2be, 0xaf65, 0xf9fe, 0x9b2f, 0x171e, 0xa536, 0xc5e2, 0x3286, 0xc001, 0x1449, 0xba20, 0xf60c, 0xb440, 0xd7cf,
    0x3914, 0xa101, 0x26cc, 0x9605, 0x5da4, 0xb6f9, 0x4b5c, 0xabfd, 0xef35, 0x9fb7, 0xdc8e, 0xaa0e, 0x1484, 0x8b09, 0x01dc, 0x9560,
    0xa497, 0xc913, 0xa438, 0xde67, 0xc9e6, 0xb465, 0xb73f, 0xbebc, 0xa31a, 0x1e60, 0xa2bb, 0x33b3, 0xa3d9, 0xf3ba, 0xa379, 0x090d,
    0xd993, 0x54a8, 0xebdb, 0x5fa4, 0xb503, 0x3eaf, 0xc74b, 0x49ac, 0x2372, 0x55f2, 0x361a, 0x4b9b, 0xfe24, 0x6aa0, 0x10cb, 0x6049,
    0x5bc8, 0x2199, 0x5c27, 0x0c46, 0x48c1, 0x4144, 0x5b69, 0x36ed, 0x5d45, 0xcc4d, 0x4afd, 0xc151, 0x5c87, 0xf6f3, 0x5ce6, 0xe1a0,
    0xef94, 0x8a64, 0xdced, 0x94bb, 0xb444, 0x6956, 0x023c, 0x800d, 0xa4f7, 0xb3c0, 0x924f, 0xbe17, 0xca45, 0x9f12, 0xb79e, 0xa969,
    0x9131, 0xfe11, 0x90d2, 0x1364, 0x91f0, 0xd36b, 0x9191, 0xe8be, 0xa25c, 0x4907, 0xb4a4, 0x5403, 0x9073, 0x28b7, 0x9013, 0x3e0a,
    0xeb7c, 0x74f7, 0xfdc4, 0x7ff3, 0xc6ec, 0x5eff, 0xd934, 0x69fb, 0x35bb, 0x60ee, 0x4862, 0x5697, 0x106c, 0x759c, 0x2313, 0x6b45,
    0x6e10, 0x2c95, 0x6e6f, 0x1742, 0x5b09, 0x4c40, 0x6db1, 0x41e9, 0x6f8d, 0xd749, 0x6fed, 0xc1f6, 0x6ecf, 0x01ef, 0x6f2e, 0xec9c,
    0xda52, 0x2a01, 0xec9a, 0x34fd, 0xc869, 0x09b2, 0xc80a, 0x1f05, 0x2431, 0x2b4b, 0x36d8, 0x20f4, 0xfee2, 0x3ffa, 0x118a, 0x35a2,
    0x37f6, 0xe0fb, 0x25ae, 0xd5ff, 0x3738, 0x0ba1, 0x3797, 0xf64e, 0xee17, 0xdfb1, 0xdb70, 0xea08, 0x1366, 0xcb03, 0x00bf, 0xd55a,
    0xecf9, 0x1faa, 0xff41, 0x2aa6, 0xdb10, 0xff5b, 0xdab1, 0x14ae, 0x24f0, 0x00a5, 0x254f, 0xeb52, 0x11e9, 0x204f, 0x2490, 0x15f8,
    0xedb8, 0xf504, 0xed59, 0x0a57, 0x1307, 0xe056, 0x005f, 0xeaad, 0x12a7, 0xf5a9, 0x0000, 0x0000, 0xffa1, 0x1553, 0x1248, 0x0afc,
    0x1425, 0xa05c, 0x017d, 0xaab3, 0x38b5, 0xb654, 0x266d, 0xab58, 0xc987, 0xc9b8, 0xb6df, 0xd410, 0xeed6, 0xb50a, 0xdc2e, 0xbf61,
    0xb5c2, 0x1409, 0xb562, 0x295c, 0xb680, 0xe963, 0xb621, 0xfeb6, 0xec3b, 0x4a51, 0xfe83, 0x554d, 0xc7aa, 0x3458, 0xd9f3, 0x3f54,
    0x3679, 0x3648, 0x4921, 0x2bf0, 0x112a, 0x4af6, 0x23d2, 0x409f, 0x4a3e, 0xebf7, 0x4a9e, 0xd6a4, 0x4980, 0x169d, 0x49df, 0x014a,
    0x13c5, 0xb5af, 0x011e, 0xc006, 0x3856, 0xcba8, 0x260d, 0xc0ac, 0xc928, 0xdf0c, 0xc8c8, 0xf45f, 0xee76, 0xca5e, 0xdbcf, 0xd4b5,
    0xb29b, 0xef66, 0xb874, 0xe1a1, 0xbe4c, 0xd3dc, 0xc424, 0xc617, 0xb8ec, 0x22d5, 0xafeb, 0x16e2, 0xa6eb, 0x0af0, 0xacc3, 0xfd2b,
    0x0560, 0xb107, 0x1439, 0xaf35, 0x2312, 0xad62, 0x31eb, 0xab8f, 0xc9fc, 0xb852, 0xd8d5, 0xb67f, 0xe7ae, 0xb4ad, 0xf687, 0xb2da,
    0x41b4, 0x2c24, 0x3bdc, 0x39e9, 0x3604, 0x47ae, 0x272b, 0x4981, 0x5915, 0xf510, 0x533d, 0x02d5, 0x4d65, 0x109a, 0x478c, 0x1e5f,
    0xdcee, 0x529e, 0xd3ee, 0x46ac, 0xcaed, 0x3ab9, 0xc1ed, 0x2ec7, 0x1852, 0x4b53, 0x0979, 0x4d26, 0xfaa0, 0x4ef9, 0xebc7, 0x50cb,
    0xcd25, 0xd209, 0xd2fd, 0xc444, 0xe1d6, 0xc272, 0xf0af, 0xc09f, 0xb5c4, 0x091d, 0xbb9c, 0xfb58, 0xc174, 0xed93, 0xc74c, 0xdfce,
    0x3513, 0xc547, 0x3e14, 0xd139, 0x4714, 0xdd2b, 0x5015, 0xe91e, 0xff88, 0xbecc, 0x0e61, 0xbcfa, 0x1d39, 0xbb27, 0x2c12, 0xb954,
    0x38b4, 0x2032, 0x32db, 0x2df7, 0x2d03, 0x3bbc, 0x1e2a, 0x3d8e, 0x413c, 0xeaf0, 0x4a3c, 0xf6e3, 0x4464, 0x04a8, 0x3e8c, 0x126d,
    0xd9c6, 0x38e7, 0xd0c5, 0x2cf4, 0xc7c5, 0x2102, 0xbec4, 0x1510, 0x0f51, 0x3f61, 0x0078, 0x4134, 0xf19f, 0x4306, 0xe2c7, 0x44d9,
    0xc49c, 0x074b, 0xca75, 0xf986, 0xd04d, 0xebc1, 0xd625, 0xddfc, 0xe89f, 0x3714, 0xdf9e, 0x2b22, 0xd69e, 0x1f2f, 0xcd9d, 0x133d,
    0x1761, 0xc8ec, 0x263a, 0xc719, 0x2f3b, 0xd30c, 0x383b, 0xdefe, 0xdbfe, 0xd037, 0xead6, 0xce64, 0xf9af, 0xcc91, 0x0888, 0xcabf,
    0x1189, 0xd6b1, 0x2062, 0xd4de, 0x2962, 0xe0d1, 0x3263, 0xecc3, 0xdf26, 0xe9ee, 0xe4fe, 0xdc29, 0xf3d7, 0xda56, 0x02b0, 0xd884,
    0x2402, 0x2fc9, 0x152a, 0x319c, 0x0651, 0x336f, 0xf778, 0x3541, 0x3b64, 0xf8b5, 0x358b, 0x067a, 0x2fb3, 0x143f, 0x29db, 0x2204,
    0x238a, 0xee96, 0x2c8b, 0xfa88, 0x26b2, 0x084d, 0x20da, 0x1612, 0xedff, 0xe81b, 0xfcd8, 0xe649, 0x0bb1, 0xe476, 0x1a89, 0xe2a3,
    0xe577, 0x1d5d, 0xdc76, 0x116a, 0xd375, 0x0578, 0xd94e, 0xf7b3, 0x1b02, 0x23d7, 0x0c29, 0x25aa, 0xfd50, 0x277c, 0xee77, 0x294f,
    0xf127, 0x01d3, 0xf6ff, 0xf40e, 0x05d8, 0xf23b, 0x14b1, 0xf068, 0x0000, 0x0000, 0x0ed9, 0xfe2d, 0x0901, 0x0bf2, 0xfa28, 0x0dc5,
    0xf44f, 0x1b8a, 0xeb4f, 0x0f98, 0xe24e, 0x03a5, 0xe827, 0xf5e0, 0x1db2, 0xfc5b, 0x17d9, 0x0a20, 0x1201, 0x17e5, 0x0328, 0x19b7,
    0x9799, 0xcb8f, 0x9d72, 0xbdca, 0xa34a, 0xb005, 0xa922, 0xa240, 0x8f11, 0x00d1, 0x8611, 0xf4de, 0x8be9, 0xe719, 0x91c1, 0xd954,
    0xea5e, 0x8d30, 0xf937, 0x8b5e, 0x0810, 0x898b, 0x16e9, 0x87b8, 0xb7fb, 0xa06d, 0xc6d4, 0x9e9b, 0xccac, 0x90d6, 0xdb85, 0x8f03,
    0xe917, 0x7848, 0xda3e, 0x7a1a, 0xd13e, 0x6e28, 0xc83d, 0x6236, 0x247b, 0x70fd, 0x15a2, 0x72d0, 0x06c9, 0x74a2, 0xf7f0, 0x7675,
    0x9562, 0x343f, 0x8c61, 0x284d, 0x923a, 0x1a88, 0x8939, 0x0e96, 0xb964, 0x6408, 0xb064, 0x5816, 0xa763, 0x4c24, 0x9e62, 0x4031,
    0x76c7, 0xf16a, 0x70ef, 0xff2f, 0x79ef, 0x0b22, 0x7417, 0x18e7, 0x619e, 0xbfcf, 0x6a9e, 0xcbc1, 0x739f, 0xd7b3, 0x7c9f, 0xe3a5,
    0x4ddd, 0x51ce, 0x4805, 0x5f93, 0x392c, 0x6165, 0x3354, 0x6f2a, 0x6e3f, 0x26ac, 0x6867, 0x3471, 0x628e, 0x4236, 0x5cb6, 0x4ffb,
    0xac4b, 0xbbf7, 0xb223, 0xae32, 0xd5ad, 0x9cc8, 0xe486, 0x9af5, 0x94e9, 0xf30c, 0x9ac2, 0xe546, 0xa09a, 0xd781, 0xa672, 0xc9bc,
    0x2ec2, 0x91d8, 0x469c, 0x9bf8, 0x4f9c, 0xa7ea, 0x589d, 0xb3dc, 0xf35f, 0x9923, 0x0238, 0x9750, 0x1110, 0x957d, 0x1fe9, 0x93ab,
    0x53b5, 0x4409, 0x2a53, 0x6338, 0x1b7a, 0x650b, 0x0ca1, 0x66dd, 0x6b17, 0x0cf4, 0x653e, 0x1aba, 0x5f66, 0x287f, 0x598e, 0x3644,
    0xb63c, 0x4a51, 0xad3b, 0x3e5f, 0xa43b, 0x326c, 0x9b3a, 0x267a, 0xfdc8, 0x68b0, 0xeef0, 0x6a83, 0xe017, 0x6c55, 0xbf3d, 0x5643,
    0xed86, 0xa6e8, 0xfc5f, 0xa515, 0x0b38, 0xa342, 0x1a11, 0xa170, 0xbb23, 0xba25, 0xc0fc, 0xac60, 0xcfd5, 0xaa8d, 0xdeae, 0xa8ba,
    0x52c5, 0xc1a1, 0x5bc5, 0xcd94, 0x64c6, 0xd986, 0x6dc6, 0xe578, 0x28ea, 0x9f9d, 0x37c3, 0x9dca, 0x40c3, 0xa9bd, 0x49c4, 0xb5af,
    0xce15, 0x5471, 0xc515, 0x487e, 0xbc14, 0x3c8c, 0xb314, 0x309a, 0x03a1, 0x5aeb, 0xf4c8, 0x5cbe, 0xe5ef, 0x5e90, 0xd716, 0x6063,
    0xa3c2, 0xf139, 0xa99b, 0xe374, 0xaf73, 0xd5af, 0xb54b, 0xc7ea, 0xaa13, 0x24a7, 0xa112, 0x18b5, 0x9812, 0x0cc3, 0x9dea, 0xfefe,
    0x5eee, 0xe74b, 0x67ee, 0xf33d, 0x6216, 0x0102, 0x5c3e, 0x0ec7, 0x3aeb, 0xb782, 0x43ec, 0xc374, 0x4cec, 0xcf66, 0x55ed, 0xdb59,
    0x3f04, 0x53a0, 0x302b, 0x5573, 0x2152, 0x5746, 0x127a, 0x5918, 0x5665, 0x1c8c, 0x508d, 0x2a51, 0x4ab5, 0x3816, 0x44dd, 0x45db,
    0xe9fc, 0xbddb, 0xd96f, 0xc5ed, 0xff31, 0xbf57, 0xf496, 0xbe99, 0x1467, 0xc0d2, 0x09cc, 0xc014, 0x299d, 0xc24e, 0x1f02, 0xc190,
    0x3ed3, 0xc3ca, 0x3438, 0xc30c, 0x3d8a, 0xd628, 0x38e1, 0xcc9a, 0x46dc, 0xe944, 0x4233, 0xdfb6, 0x502f, 0xfc61, 0x4b86, 0xf2d2,
    0x4ee6, 0x0ebf, 0x54d8, 0x05ef, 0x4302, 0x2060, 0x48f4, 0x178f, 0x371e, 0x3200, 0x3d10, 0x2930, 0x35d5, 0x445f, 0x312c, 0x3ad1,
    0x1aae, 0x4bb3, 0x2549, 0x4c71, 0x0578, 0x4a38, 0x1013, 0x4af5, 0xf042, 0x48bc, 0xfadd, 0x497a, 0xdb0c, 0x4740, 0xe5a7, 0x47fe,
    0x3798, 0xdef8, 0x32ef, 0xd56a, 0x40eb, 0xf215, 0x3c42, 0xe886, 0x4a3d, 0x0531, 0x4594, 0xfba3, 0x3e59, 0x16d2, 0x444b, 0x0e01,
    0x3275, 0x2872, 0x3867, 0x1fa2, 0x2691, 0x3a13, 0x2c83, 0x3142, 0x209f, 0x42e3, 0x2b3a, 0x43a1, 0x0b6a, 0x4167, 0x1604, 0x4225,
    0xf634, 0x3fec, 0x00cf, 0x40a9, 0xe0fe, 0x3e70, 0xeb99, 0x3f2e, 0xcd11, 0x2a96, 0xd1ba, 0x3424, 0xc3be, 0x177a, 0xc868, 0x2108,
    0xba6c, 0x045d, 0xbf15, 0x0deb, 0xbbb5, 0xf1ff, 0xb5c3, 0xfacf, 0xc799, 0xe05e, 0xc1a7, 0xe92e, 0xd37d, 0xcebe, 0xcd8b, 0xd78e,
    0x5f73, 0x06ad, 0x5aca, 0xfd1e, 0x538f, 0x184d, 0x5981, 0x0f7d, 0x47ab, 0x29ee, 0x4d9d, 0x211e, 0x3bc7, 0x3b8e, 0x41b9, 0x32be,
    0x2fe3, 0x4d2f, 0x4070, 0x451d, 0x29f2, 0x55ff, 0x348d, 0x56bd, 0x14bc, 0x5484, 0x1f57, 0x5541, 0xff86, 0x5308, 0x0a21, 0x53c6,
    0xea50, 0x518c, 0xf4eb, 0x524a, 0xd51a, 0x5011, 0xdfb5, 0x50ce, 0xcbc8, 0x3cf4, 0xc5d6, 0x45c5, 0xb7db, 0x291a, 0xbc84, 0x32a8,
    0xae89, 0x15fe, 0xb332, 0x1f8c, 0xa536, 0x02e2, 0xa9e0, 0x0c70, 0xa67f, 0xf083, 0xa08d, 0xf953, 0xb263, 0xdee2, 0xac71, 0xe7b3,
    0xd663, 0x3db2, 0xd071, 0x4682, 0xc276, 0x29d8, 0xc71f, 0x3366, 0xb924, 0x16bc, 0xbdcd, 0x204a, 0xafd1, 0x039f, 0xb47a, 0x0d2e,
    0xb11a, 0xf141, 0xab28, 0xfa11, 0xbcfe, 0xdfa0, 0xb70c, 0xe871, 0xc8e2, 0xce00, 0xc2f0, 0xd6d0, 0xdf61, 0xbd1d, 0xced4, 0xc52f,
    0xe552, 0xb44d, 0xdab7, 0xb38f, 0xfa88, 0xb5c8, 0xefed, 0xb50b, 0x0fbe, 0xb744, 0x0523, 0xb686, 0x24f4, 0xb8c0, 0x1a59, 0xb802,
    0x3a2a, 0xba3b, 0x2f8f, 0xb97e, 0x437c, 0xcd58, 0x496e, 0xc487, 0x4cce, 0xe074, 0x4825, 0xd6e6, 0x5620, 0xf390, 0x5177, 0xea02,
    0xf205, 0xe356, 0xec13, 0xec26, 0x073b, 0xe4d1, 0xfca0, 0xe413, 0x1c71, 0xe64d, 0x11d6, 0xe58f, 0x25c3, 0xf969, 0x211a, 0xefdb,
    0x247a, 0x0bc8, 0x2a6c, 0x02f7, 0x1896, 0x1d68, 0x1e88, 0x1498, 0x0809, 0x257b, 0x12a4, 0x2639, 0xf2d3, 0x23ff, 0xfd6e, 0x24bd,
    0xe38f, 0x19b3, 0xe838, 0x2341, 0xda3d, 0x0697, 0xdee6, 0x1025, 0xdb86, 0xf438, 0xd594, 0xfd09, 0xe76a, 0xe298, 0xe178, 0xeb68,
    0xf7f7, 0xda85, 0xed5c, 0xd9c7, 0x0d2d, 0xdc01, 0x0292, 0xdb43, 0x2263, 0xdd7d, 0x17c8, 0xdcbf, 0x2bb5, 0xf099, 0x270c, 0xe70b,
    0x0a9b, 0x00be, 0x0000, 0x0000, 0xfa0e, 0x08d0, 0x04a9, 0x098e, 0xfb57, 0xf672, 0xf565, 0xff42, 0x108d, 0xf7ee, 0x05f2, 0xf730,
    0x0f44, 0x0a4c, 0x1536, 0x017c, 0xfeb7, 0x125e, 0x0952, 0x131c, 0xef73, 0x0812, 0xf41c, 0x11a1, 0xf0bc, 0xf5b4, 0xeaca, 0xfe84,
    0x0149, 0xeda2, 0xf6ae, 0xece4, 0x167f, 0xef1d, 0x0be4, 0xee5f, 0x1fd1, 0x023a, 0x1b28, 0xf8ab, 0x13ed, 0x13da, 0x19df, 0x0b0a,
    0x0360, 0x1bed, 0x0dfb, 0x1caa, 0xee2a, 0x1a71, 0xf8c5, 0x1b2f, 0xe4d8, 0x0755, 0xe981, 0x10e3, 0xe621, 0xf4f6, 0xe02f, 0xfdc6,
    0x39b0, 0x0d43, 0x3fa2, 0x0473, 0x2dcc, 0x1ee4, 0x33be, 0x1614, 0x21e8, 0x3085, 0x27da, 0x27b4, 0x115b, 0x3897, 0x1bf6, 0x3955,
    0xfc26, 0x371b, 0x06c0, 0x37d9, 0xe6f0, 0x35a0, 0xf18b, 0x365d, 0xd7ac, 0x2b54, 0xdc55, 0x34e2, 0xce59, 0x1837, 0xd303, 0x21c5,
    0xc507, 0x051b, 0xc9b0, 0x0ea9, 0xc650, 0xf2bd, 0xc05e, 0xfb8d, 0xd234, 0xe11c, 0xcc42, 0xe9ec, 0xde18, 0xcf7b, 0xd826, 0xd84c,
    0xeea5, 0xc769, 0xe40a, 0xc6ab, 0x03da, 0xc8e5, 0xf940, 0xc827, 0x1910, 0xca60, 0x0e75, 0xc9a3, 0x2e46, 0xcbdc, 0x23ab, 0xcb1e,
    0x3507, 0x03b5, 0x305e, 0xfa27, 0x2923, 0x1556, 0x2f15, 0x0c86, 0x1d3f, 0x26f6, 0x2331, 0x1e26, 0x0cb2, 0x2f09, 0x174d, 0x2fc7,
    0xf77c, 0x2d8d, 0x0217, 0x2e4b, 0xe247, 0x2c11, 0xece1, 0x2ccf, 0xd8f4, 0x18f5, 0xdd9d, 0x2283, 0xcfa2, 0x05d9, 0xd44b, 0x0f67,
    0xd0eb, 0xf37a, 0xcaf9, 0xfc4b, 0xdccf, 0xe1da, 0xd6dd, 0xeaaa, 0xe8b3, 0xd039, 0xe2c1, 0xd90a, 0xfde9, 0xd1b5, 0xf34e, 0xd0f7,
    0x131f, 0xd331, 0x0884, 0xd273, 0x2854, 0xd4ac, 0x1db9, 0xd3ef, 0x31a7, 0xe7c9, 0x2cfd, 0xde3b, 0x3af9, 0xfae5, 0x3650, 0xf157,
    0x9361, 0x1d52, 0x980a, 0x26e1, 0x9292, 0xdca9, 0x8eb8, 0x13c4, 0x9e76, 0xcb08, 0x9884, 0xd3d9, 0xb4f5, 0xba26, 0xa468, 0xc238,
    0xc0d8, 0xa885, 0xb04c, 0xb097, 0xf27f, 0x904e, 0xe7e4, 0x8f90, 0x07b5, 0x91c9, 0xfd1a, 0x910b, 0x1ceb, 0x9345, 0x1250, 0x9287,
    0x3221, 0x94c1, 0x2786, 0x9403, 0x4c00, 0x9fcb, 0x3cbc, 0x957f, 0x50a9, 0xa959, 0x569b, 0xa088, 0x6a88, 0xb463, 0x5b44, 0xaa17,
    0x73da, 0xc77f, 0x6496, 0xbd33, 0x7291, 0xd9dd, 0x6de8, 0xd04f, 0x7be3, 0xecfa, 0x773a, 0xe36b, 0x7a9a, 0xff58, 0x75f1, 0xf5ca,
    0x0c5e, 0x9b57, 0x01c3, 0x9a9a, 0x2194, 0x9cd3, 0x16f9, 0x9c15, 0x36ca, 0x9e4f, 0x2c2f, 0x9d91, 0x5fed, 0xb3a5, 0x4165, 0x9f0d,
    0x693f, 0xc6c1, 0x59fb, 0xbc75, 0x67f6, 0xd91f, 0x634d, 0xcf91, 0x7148, 0xec3c, 0x6c9f, 0xe2ae, 0x677c, 0x2c27, 0x6d6e, 0x2357,
    0x5b98, 0x3dc8, 0x618a, 0x34f8, 0x4fb4, 0x4f69, 0x6041, 0x4756, 0x181c, 0x7070, 0x545d, 0x58f7, 0x02e6, 0x6ef5, 0x0d81, 0x6fb2,
    0xedb0, 0x6d79, 0xf84b, 0x6e37, 0xd87a, 0x6bfd, 0xe315, 0x6cbb, 0xa605, 0x438b, 0xb549, 0x4dd7, 0x9cb3, 0x306f, 0xabf7, 0x3abb,
    0x9218, 0x2fb1, 0xa15c, 0x39fd, 0x88c6, 0x1c95, 0x8d6f, 0x2623, 0x8a0f, 0x0a36, 0x841d, 0x1306, 0x8b57, 0xf7d8, 0x8566, 0x00a8,
    0x8ca0, 0xe579, 0x86ae, 0xee4a, 0x8de9, 0xd31b, 0x87f7, 0xdbeb, 0x99cd, 0xc17a, 0x93db, 0xca4a, 0xa5b1, 0xafda, 0xaa5a, 0xb968,
    0xb194, 0x9e39, 0xb63e, 0xa7c7, 0xccbc, 0x96e4, 0xbc2f, 0x9ef7, 0xd2ae, 0x8e14, 0xd757, 0x97a2, 0xedd6, 0x86bf, 0xdd49, 0x8ed2,
    0x030c, 0x883b, 0xf871, 0x877d, 0x1842, 0x89b7, 0x0da7, 0x88f9, 0x2d78, 0x8b33, 0x22dd, 0x8a75, 0x7f43, 0x08e6, 0x3813, 0x8bf0,
    0x7952, 0x11b6, 0x74a9, 0x0828, 0x7809, 0x2415, 0x7360, 0x1a87, 0x6c25, 0x35b6, 0x7217, 0x2ce5, 0x6adc, 0x4814, 0x6633, 0x3e86,
    0x5ef8, 0x59b5, 0x5a4f, 0x5026, 0x43d1, 0x6109, 0x4e6c, 0x61c7, 0x3ddf, 0x69d9, 0x487a, 0x6a97, 0x22b7, 0x712e, 0x2d52, 0x71ec,
    0x1cc5, 0x79fe, 0x2760, 0x7abc, 0x078f, 0x7883, 0x122a, 0x7941, 0xf259, 0x7707, 0xfcf4, 0x77c5, 0xdd23, 0x758b, 0xe7be, 0x7649,
    0xc344, 0x6a81, 0xcddf, 0x6b3f, 0xbe9b, 0x60f3, 0xc936, 0x61b1, 0xa4bc, 0x55e9, 0xaf57, 0x56a7, 0x9b6a, 0x42cd, 0xaaae, 0x4d19,
    0x3f28, 0x577b, 0x3a7e, 0x4ded, 0x2400, 0x5ed0, 0x2e9b, 0x5f8d, 0x0eca, 0x5d54, 0x1965, 0x5e12, 0xf994, 0x5bd8, 0x042f, 0x5c96,
    0xe45e, 0x5a5d, 0xeef9, 0x5b1a, 0xcf28, 0x58e1, 0xd9c3, 0x599f, 0xca7f, 0x4f53, 0xc48d, 0x5823, 0xc12d, 0x3c36, 0xbb3b, 0x4507,
    0xad40, 0x285c, 0xb1e9, 0x31ea, 0xa3ee, 0x1540, 0xa897, 0x1ece, 0x9a9b, 0x0224, 0x9f45, 0x0bb2, 0x9be4, 0xefc5, 0x95f2, 0xf896,
    0xa7c8, 0xde25, 0xa1d6, 0xe6f5, 0xb3ac, 0xcc84, 0xadba, 0xd554, 0xca2b, 0xbba1, 0xb99e, 0xc3b4, 0xd60e, 0xaa01, 0xc582, 0xb213,
    0xbe47, 0xcd42, 0xb855, 0xd612, 0xd4c6, 0xbc5f, 0xc439, 0xc472, 0xe0a9, 0xaabf, 0xd01d, 0xb2d1, 0xf5df, 0xac3a, 0xeb44, 0xab7c,
    0x0b15, 0xadb6, 0x007a, 0xacf8, 0x204b, 0xaf32, 0x15b0, 0xae74, 0x3581, 0xb0ad, 0x2ae6, 0xafef, 0x4ab7, 0xb229, 0x401c, 0xb16b,
    0x5409, 0xc545, 0x44c5, 0xbaf9, 0x52c0, 0xd7a4, 0x4e17, 0xce16, 0x5c12, 0xeac0, 0x5769, 0xe132, 0x6565, 0xfddc, 0x60bb, 0xf44e,
    0x641c, 0x103b, 0x6a0e, 0x076a, 0x5838, 0x21db, 0x5e2a, 0x190b, 0x4c54, 0x337c, 0x5246, 0x2aac, 0x4b0b, 0x45da, 0x4662, 0x3c4c,
    0x1e0e, 0x67a0, 0x28a9, 0x685e, 0x08d8, 0x6624, 0x1373, 0x66e2, 0xf3a2, 0x64a9, 0xfe3d, 0x6566, 0xde6c, 0x632d, 0xe907, 0x63eb,
    0xb9f2, 0x5765, 0xd3d1, 0x626f, 0xb0a0, 0x4449, 0xbfe4, 0x4e95, 0xa74e, 0x312d, 0xb692, 0x3b79, 0x9dfc, 0x1e10, 0xa2a5, 0x279e,
    0x94aa, 0x0af4, 0x9953, 0x1482, 0x9149, 0xef07, 0x9001, 0x0166, 0x9d2d, 0xdd67, 0x973b, 0xe637, 0xa911, 0xcbc6, 0xa31f, 0xd496,
    0xbf90, 0xbae3, 0xaf03, 0xc2f6, 0xcb73, 0xa943, 0xbae7, 0xb155, 0xe1f2, 0x9860, 0xc6ca, 0x9fb5, 0xf728, 0x99dc, 0xec8d, 0x991e,
    0xdc00, 0xa130, 0xd165, 0xa073, 0xf136, 0xa2ac, 0xe69b, 0xa1ee, 0x066c, 0xa428, 0xfbd1, 0xa36a, 0x1ba2, 0xa5a3, 0x1107, 0xa4e6,
    0x30d8, 0xa71f, 0x263d, 0xa661, 0x460e, 0xa89b, 0x3b73, 0xa7dd, 0x4f60, 0xbbb7, 0x5552, 0xb2e7, 0x58b2, 0xced3, 0x5ea4, 0xc603,
    0x6204, 0xe1f0, 0x5d5b, 0xd862, 0x6b56, 0xf50c, 0x66ad, 0xeb7e, 0x6eb7, 0x10f9, 0x6fff, 0xfe9a, 0x62d3, 0x2299, 0x68c5, 0x19c9,
    0x56ef, 0x343a, 0x5ce1, 0x2b6a, 0x55a6, 0x4698, 0x50fd, 0x3d0a, 0x49c2, 0x5839, 0x4519, 0x4eab, 0x3344, 0x691c, 0x3936, 0x604b
};


// OS93a Type 1 frame decompression
void DCSDecoderNative::DecoderImpl93a::DecompressFrame(Channel &channel, uint16_t *outputBuffer)
{
    // get the playback pointer and header byte
    auto &stream = channel.audioStream;
    ROMBitPointer playbackBitPtr = stream.playbackBitPtr;
    uint8_t hdrByte = *stream.headerPtr;

    // "Type 0" streams (bit $80 of first header byte is zero) use the 
    // unified OS93a/OS93b format.  We can simply invoke the common base 
    // class handler for these streams.
    if ((hdrByte & 0x80) == 0)
        return DecoderImpl93::DecompressFrame(channel, outputBuffer);

    // It's a "Type 1" stream (bit $80 of first header byte is set). 
    // This is a unique format that only appears in a few tracks in
    // Judge Dredd.
    int prvScaleCode = 0x1A;
    uint16_t mixingMultiplier = channel.mixingMultiplier;

    // The header byte has three fields:
    // 
    // t pp bbbbb
    //
    // t     ($80) = stream type, which we already know is Type 1 ($80)
    // pp    ($60) = codebook selector for the per-band bit-width field (see below)
    // bbbbb ($1F) = number of bands in each frame
    //
    int prefixCodebookSelector = (hdrByte & 0x60);
    int numBands = (hdrByte & 0x1F);

    // Each frame consists of a series of bands, with a fixed number of
    // stream inputs in each band, as listed in this table.  Note that
    // there are twice as many outputs as inputs - each bit-stream input
    // represents TWO frame buffer elements.
    static const int inputsPerBand[] ={ 2, 2, 2, 2, 3, 4, 5, 6, 5, 6, 7, 9, 11, 14, 12, 12, 12, 13 };
    const int *bandp = inputsPerBand;

    // Huffman codebooks for the band bit-width field.  Bits $60 in the stream 
    // header byte select the codebook.  Each codebook is expressed as an index
    // table for the next four bits of the stream.  Four bits is just the
    // maximum length, needed to construct the index; the actual number of
    // bits of the prefix value is specified by prefixBits scaleCodebookEle in the
    // matching codebook entry.
    static const struct { 
        int bandBits; 
        int prefixBits;
    }
    bandBitsCodebooks[] ={
        // Group 0 -> (hdr & 0x60) == 0x00
        { 0x0000, 0x03 }, { 0x0000, 0x03 }, { 0xffff, 0x04 }, { 0x0005, 0x04 },
        { 0x0001, 0x03 }, { 0x0001, 0x03 }, { 0x0002, 0x03 }, { 0x0002, 0x03 },
        { 0x0003, 0x02 }, { 0x0003, 0x02 }, { 0x0003, 0x02 }, { 0x0003, 0x02 },
        { 0x0004, 0x02 }, { 0x0004, 0x02 }, { 0x0004, 0x02 }, { 0x0004, 0x02 },

        // Group 1 -> (hdr & 0x60) == 0x20
        { 0x0000, 0x03 }, { 0x0000, 0x03 }, { 0xffff, 0x04 }, { 0x0003, 0x04 },
        { 0x0004, 0x04 }, { 0x0007, 0x04 }, { 0x0001, 0x03 }, { 0x0001, 0x03 },
        { 0x0002, 0x03 }, { 0x0002, 0x03 }, { 0x0005, 0x03 }, { 0x0005, 0x03 },
        { 0x0006, 0x02 }, { 0x0006, 0x02 }, { 0x0006, 0x02 }, { 0x0006, 0x02 },

        // Group 2 -> (hdr & 0x60) == 0x40
        { 0x0000, 0x04 }, { 0x0001, 0x04 }, { 0xffff, 0x04 }, { 0x0002, 0x04 },
        { 0x0003, 0x04 }, { 0x0008, 0x04 }, { 0x0004, 0x03 }, { 0x0004, 0x03 },
        { 0x0005, 0x03 }, { 0x0005, 0x03 }, { 0x0006, 0x03 }, { 0x0006, 0x03 },
        { 0x0007, 0x02 }, { 0x0007, 0x02 }, { 0x0007, 0x02 }, { 0x0007, 0x02 },

        // Group 3 -> (hdr & 0x60) == 0x60
        { 0x0000, 0x04 }, { 0x0001, 0x04 }, { 0xffff, 0x04 }, { 0x0002, 0x04 },
        { 0x0003, 0x04 }, { 0x0009, 0x04 }, { 0x0004, 0x03 }, { 0x0004, 0x03 },
        { 0x0005, 0x03 }, { 0x0005, 0x03 }, { 0x0006, 0x03 }, { 0x0006, 0x03 },
        { 0x0007, 0x03 }, { 0x0007, 0x03 }, { 0x0008, 0x03 }, { 0x0008, 0x03 }
    };
    auto const *bandBitsCodebook = &bandBitsCodebooks[prefixCodebookSelector >> 1];

    // scan the bands
    int outBufIndex = 0;
    for (int bandNo = 0 ; bandNo < numBands ; ++bandNo)
    {
        // the number of input in each band is given by the predefined table
        int numInputs = *bandp++;

        // Read the Huffman-coded band prefix - peek at the next 4 bits to
        // get the codebook index, which will tell us how many bits the prefix
        // actually consumes.
        auto const &bandBitsCodebookEle = bandBitsCodebook[playbackBitPtr.Peek(4)];
        uint16_t bandBits = bandBitsCodebookEle.bandBits;

        // skip the prefix bits now that we know the actual codeword size
        playbackBitPtr.Get(bandBitsCodebookEle.prefixBits);

        // a band bit width of $FFFF marks the end of the frame
        if (bandBits == 0xFFFF)
            break;

        // read the next scaleCodebookEle if this band group has a non-zero bit width
        if (bandBits != 0)
        {
            // The next entry is another Huffman-coded bit string, with the codebook
            // below.  This is a two-level indexing codebook: we index first by the
            // first 4 bits of the input.  If that entry has a second-level table
            // index, we read the next four bits to index into the second table.
            static const struct
            {
                int value;
                int nBits;
                int subTableIndex;
            }
            scaleCodebook[] ={
                { 0x0000, 0x02, 0x0000 }, { 0x0000, 0x02, 0x0000 }, { 0x0000, 0x02, 0x0000 }, { 0x0000, 0x02, 0x0000 },
                { 0x0001, 0x02, 0x0000 }, { 0x0001, 0x02, 0x0000 }, { 0x0001, 0x02, 0x0000 }, { 0x0001, 0x02, 0x0000 },
                { 0x0034, 0x04, 0x0000 }, { 0x0035, 0x04, 0x0000 }, { 0x0002, 0x04, 0x0000 }, { 0x0003, 0x04, 0x0000 },
                { 0xFFFF, 0x04, 0x0010 }, { 0xFFFF, 0x04, 0x0020 }, { 0xFFFF, 0x04, 0x0030 }, { 0xFFFF, 0x04, 0x0040 },
                { 0x002c, 0x07, 0x0000 }, { 0x002c, 0x07, 0x0000 }, { 0x002d, 0x07, 0x0000 }, { 0x002d, 0x07, 0x0000 },
                { 0x002e, 0x07, 0x0000 }, { 0x002e, 0x07, 0x0000 }, { 0x002f, 0x07, 0x0000 }, { 0x002f, 0x07, 0x0000 },
                { 0x0030, 0x07, 0x0000 }, { 0x0030, 0x07, 0x0000 }, { 0x0031, 0x07, 0x0000 }, { 0x0031, 0x07, 0x0000 },
                { 0x0032, 0x07, 0x0000 }, { 0x0032, 0x07, 0x0000 }, { 0x0033, 0x07, 0x0000 }, { 0x0033, 0x07, 0x0000 },
                { 0x0004, 0x07, 0x0000 }, { 0x0004, 0x07, 0x0000 }, { 0x0005, 0x07, 0x0000 }, { 0x0005, 0x07, 0x0000 },
                { 0x0006, 0x07, 0x0000 }, { 0x0006, 0x07, 0x0000 }, { 0x0007, 0x07, 0x0000 }, { 0x0007, 0x07, 0x0000 },
                { 0x0008, 0x07, 0x0000 }, { 0x0008, 0x07, 0x0000 }, { 0x0009, 0x07, 0x0000 }, { 0x0009, 0x07, 0x0000 },
                { 0x000a, 0x07, 0x0000 }, { 0x000a, 0x07, 0x0000 }, { 0x000b, 0x07, 0x0000 }, { 0x000b, 0x07, 0x0000 },
                { 0x001c, 0x08, 0x0000 }, { 0x001d, 0x08, 0x0000 }, { 0x001e, 0x08, 0x0000 }, { 0x001f, 0x08, 0x0000 },
                { 0x0020, 0x08, 0x0000 }, { 0x0021, 0x08, 0x0000 }, { 0x0022, 0x08, 0x0000 }, { 0x0023, 0x08, 0x0000 },
                { 0x0024, 0x08, 0x0000 }, { 0x0025, 0x08, 0x0000 }, { 0x0026, 0x08, 0x0000 }, { 0x0027, 0x08, 0x0000 },
                { 0x0028, 0x08, 0x0000 }, { 0x0029, 0x08, 0x0000 }, { 0x002a, 0x08, 0x0000 }, { 0x002b, 0x08, 0x0000 },
                { 0x000c, 0x08, 0x0000 }, { 0x000d, 0x08, 0x0000 }, { 0x000e, 0x08, 0x0000 }, { 0x000f, 0x08, 0x0000 },
                { 0x0010, 0x08, 0x0000 }, { 0x0011, 0x08, 0x0000 }, { 0x0012, 0x08, 0x0000 }, { 0x0013, 0x08, 0x0000 },
                { 0x0014, 0x08, 0x0000 }, { 0x0015, 0x08, 0x0000 }, { 0x0016, 0x08, 0x0000 }, { 0x0017, 0x08, 0x0000 },
                { 0x0018, 0x08, 0x0000 }, { 0x0019, 0x08, 0x0000 }, { 0x001a, 0x08, 0x0000 }, { 0x001b, 0x08, 0x0000 }
            };

            // read the next 4 bits and look up the codebook entry, and skip the actual bit count
            auto const *scaleCodebookEle = &scaleCodebook[playbackBitPtr.Peek(4)];
            playbackBitPtr.Get(scaleCodebookEle->nBits);

            // if there's a second-level table, traverse into it with the next 4 bits
            if (scaleCodebookEle->value == 0xFFFF)
            {
                scaleCodebookEle = &scaleCodebook[scaleCodebookEle->subTableIndex + playbackBitPtr.Peek(4)];
                playbackBitPtr.Get(scaleCodebookEle->nBits - 4);
            }

            // Figure the new scale code.  The codebook entry is a delta from the
            // previous base value, and the final scale code is the new base value
            // plus the bit width contribution.  The code space wraps at 0x39.
            int scaleCode = prvScaleCode + scaleCodebookEle->value - 1 + bandBits*2;
            if (scaleCode > 0x39)
                scaleCode -= 0x36;

            // Remember the new scale code (the final code minus the sample bit
            // width contribution).
            prvScaleCode = scaleCode - bandBits*2;

            // Compute the scaling factor for the inputs.  Note that this is using
            // ADSP-2105-style 1.15 integer fraction arithmetic, so the scale factor's
            // mathematical value is the 2's complement value divided by 32768.
            uint16_t shift = scaleCode >> 2;
            int exponent = scaleCode & 3;
            uint32_t scaleFactor = 0x8000;
            for (uint16_t i = 0 ; i < exponent ; ++i)
                scaleFactor = (scaleFactor * 0x9838) >> 15;
            scaleFactor <<= shift;

            // combine the scale with the channel's mixing multiplier to get the
            // final scaling factor
            scaleFactor = ((scaleFactor >> 16) * mixingMultiplier) >> 15;

            // calculate the base index in the pair lookup table
            const uint16_t *sampleTableBase = &os93a_type1_samplePairTable[2 << bandBits];

            // Read the stream inputs
            for (int i = 0 ; i < numInputs ; ++i)
            {
                // read the input
                uint16_t sample = static_cast<uint16_t>(playbackBitPtr.Get(bandBits));

                // use it to index the pair table
                const uint16_t *sampleTablePtr = sampleTableBase + (sample * 2);

                // read the first table entry, multiply by the scale factor, and add into the frame buffer
                auto mr = static_cast<uint64_t>(outputBuffer[outBufIndex]) << 16;
                outputBuffer[outBufIndex++] = MultiplyRoundAdd(mr, *sampleTablePtr++, scaleFactor);

                // repeat for the second table entry
                mr = static_cast<uint64_t>(outputBuffer[outBufIndex]) << 16;
                outputBuffer[outBufIndex++] = MultiplyRoundAdd(mr, *sampleTablePtr++, scaleFactor);
            }
        }
        else
        {
            // Zero bits per input, so the inputs are coded implicitly as
            // all zero values, without using any bits in the bit stream.
            // Since the outputs are additive, adding zero doesn't change
            // the frame buffer, so we can just skip this block of outputs.
            // There are twice as many outputs as inputs per band, since
            // each bit-stream input represents two frame samples.
            outBufIndex += numInputs * 2;
        }
    }

    // store back the final bit pointer
    stream.playbackBitPtr = playbackBitPtr;
}

// --------------------------------------------------------------------------
//
// Update the per-channel mixing levels.  This updates fade timers
// and calculates the current sample multiplier factors to adjust
// the reference-level samples in the input stream to the current
// mixing level in each channel.  This routine also processes channel
// timers for track program playback.
//
void DCSDecoderNative::UpdateMixingLevels()
{
    // update mixing fades on all channels
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        auto *mixer = &channel[i].mixer[0];
        for (int j = 0 ; j < MAX_CHANNELS ; ++j, ++mixer)
        {
            if (mixer->fadeSteps == 1)
            {
                // final step - peg to the target level
                mixer->fadeSteps = 0;
                mixer->curLevel = mixer->fadeTargetLevel;
            }
            else if (mixer->fadeSteps > 1)
            {
                // more steps to go - apply one fade step
                mixer->fadeSteps -= 1;
                mixer->curLevel += mixer->fadeDelta;
                
                // limit to +/- 8191
                if (mixer->curLevel > 8191)
                    mixer->curLevel = 8191;
                else if (mixer->curLevel < -8191)
                    mixer->curLevel = -8191;
            }
        }
    }

    // calculate the aggregate mixing level multipliers for all channels
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        // add up the mixer adjustment levels for the channel
        int mixerSum = 0;
        auto *mixer = &channel[i].mixer[0];
        for (int j = 0 ; j < MAX_CHANNELS ; ++j, ++mixer)
            mixerSum += mixer->curLevel;

        // limit to +/- 8191
        if (mixerSum > 8191)
            mixerSum = 8191;
        else if (mixerSum < -8191)
            mixerSum = -8191;

        // get the high 10 bits + 0x80
        uint16_t mixerExp = static_cast<uint16_t>(((mixerSum >> 6) & 0x3FF) + 0x80);

        // Get the starting value for the multiplier.  For IJTPA and JD, this
        // is the fixed starting value $7FFF.  For all later games, it's the
        // channel's "global mixing level" shifted left by 7 bits.  (The global
        // mixing level is a parameter that can be set via a command from the
        // WPC host, as a one byte value, 0..255.  The default value is 255,
        // so the default starting multiplier is $7F00 - almost the same as
        // the older OS93a fixed starting value of $7FFF, so it works out to
        // about the same final value if the WPC host doesn't explicitly change
        // the level.)
        uint16_t multiplier = (osVersion == OSVersion::OS93a) ? 0x7FFF : channel[i].channelVolume << 7;

        // If the special "max mixing level" flag is set, use the maximum
        // volume for the channel, regardless of the channelVolume setting.
        if (channel[i].maxMixingLevelOverride)
            multiplier = 0xFF << 7;

        // Calculate the attenuation level.  This is in effect calculating 
        // pow(0.9733, 255 - mixerExp) via a series of fixed-point (1.15 format)
        // integer multiplies.  We could do roughly the same calculation more
        // concisely with floating point values via pow(), but the result
        // diverges a bit from the 1.15-format result, particularly for smaller
        // values of mixerExp, because of the greater accumulated rounding
        // errors with the 1.15-format.  To match the original decoder's
        // output, we need to use the same arithmetic.
        uint16_t prod = 0x7C94;
        for (int j = 0, bit = 0x0001 ; j < 8 ; ++j, bit <<= 1)
        {
            if ((mixerExp & bit) == 0)
                multiplier = static_cast<uint16_t>((multiplier * prod) >> 15);
            prod = static_cast<uint16_t>((prod * prod) >> 15);
        }
        channel[i].mixingMultiplier = multiplier << 1;
    }

    // process channel timers
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        // Increment the track counter for the channel.  This controls the timing
        // of the track's byte-code program.
        channel[i].trackCounter += 1;

        // If there's an event timer for the channel, process it.  An event timer
        // is active if its interval setting is non-zero.
        if (channel[i].hostEventTimer.Update())
            host->ReceiveDataPort(channel[i].hostEventTimer.data);
    }
}

// --------------------------------------------------------------------------
//
// Initialization
//

// Initialize the decoder
bool DCSDecoderNative::Initialize()
{
    // Select the frame decoder implementation based on the detected
    // ROM software version.
    switch (osVersion)
    {
    case OSVersion::OS93a:
        decoderImpl.reset(new DecoderImpl93a(this));
        break;

    case OSVersion::OS93b:
        decoderImpl.reset(new DecoderImpl93(this));
        break;

    default:
        decoderImpl.reset(new DecoderImpl94x(this));
        break;
    }

    // initialize channel buffers
    InitChannels();

    // Set up the autobuffer.  There's not *really* an autobuffer in this
    // implementation, but out program design has a notional autobuffer to
    // make it easier to implement an emulated version of the decoder.
    // And keeping the equivalent of the autobuffer makes it easier to
    // replicate the overall program flow of the ROM code, since that code
    // is designed around placing samples into its autobuffer.  (The term
    // "autobuffer" is an ADSP-2105-ism that refers to a DMA mechanism
    // that asynchronously transfers data from a designated range of RAM
    // to an external peripheral via an on-chip serial port.  The original
    // DCS ROM code uses this mechanism to clock samples out to the DAC.)
    // 
    // In the original DCS-95 code, the PCM samples are placed at every
    // other word of the output buffer.  The intervening words are left
    // unused.  The older pre-95 code placed the samples sequentially.
    // Nothing else about the buffering setup was different, so I suspect
    // that the every-other-word arrangement was in anticipation of an
    // upgrade to stereo that never happened.  (It didn't happen for the
    // pinball machines, anyway.  They continued to use DCS boards in
    // video games after the Williams pinball era ended, and some of the
    // later video games did have multi-channel versions of DCS.  Those
    // aren't relevant to the pinball software, though.)  For this
    // implementation, we use the older sequential spacing, since
    // there's nothing to be gained from the double spacing.  This 
    // doesn't save us any memory, since we allocate a backing array to
    // cover the whole 0x4000-word space of the ADSP-2105 anyway, but it
    // might improve performance a little bit, since modern CPUs tend to
    // be sensitive to locality of reference due to their heavy use of
    // caching.
    //
    // Note that the original implementation uses double-buffering.  The
    // hardware playback pointer sees the whole thing as one contiguous
    // buffer, but the decoder treats this as two buffers back-to-back,
    // swapping between them on each pass.  This ensures that the playback
    // pointer and decoder write pointer never collide - they're always
    // working on opposite halves of the buffer.  In this C++ version,
    // the hardware playback details are handled by the host program, so
    // we have no need for double-buffering at this level.  We thus only
    // need the first half of the nominal buffer defined here.
    autobuffer.Set(outputBuffer, 0x1E0, 1);

    // Set the default master volume level 
    SetMasterVolume(defaultVolume);

    // clear any stale data out of the data port
    ClearDataPort();
    nDataPortBytes = 0;

    // success
    return true;
}

// Initialize channel structures
void DCSDecoderNative::InitChannels()
{
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
    {
        channel[i].stop = false;
        channel[i].channelVolume = 0xFF;
        channel[i].mysteryOpParams.Reset();
    }
}

// Reset mixing levels for a channel.  This resets the mixing level
// contribution from channel 'ch' to all of the other channels.  (Each
// channel's track program has the ability to set an advisory level in
// every other channel.  Each channel tracks all of the contributions
// from all of the other channels separately.  This routine resets
// the contributions from channel 'ch' to all of the other channels.)
void DCSDecoderNative::ResetMixingLevels(int ch)
{
    // reset the given channel's contribution to all of the
    // other channels' mixing levels
    for (int i = 0 ; i < MAX_CHANNELS ; ++i)
        channel[i].mixer[ch].Reset();
}


// --------------------------------------------------------------------------
//
// Volume controls
//

//
// Set the master volume level (0..255)
//
void DCSDecoderNative::SetMasterVolume(int vol)
{
    nominalVolume = static_cast<uint8_t>(vol > 255 ? 255 : vol < 0 ? 0 : vol);
    if (vol != 0)
    {
        // Figure the PCM multiplier.  The original ROM code uses fixed-point
        // (1.15) arithmetic to calculate
        //
        //   <PCM multiplier> = 0.5*pow(0.981201, 255.0 - vol)
        // 
        // On a modern platform, we could express that more clearly (and more
        // compactly) with floats, but the results diverge from the original
        // a little bit, especially at lower values of 'vol', due to rounding
        // errors in the 1.15 version.  For the sake of strict replication
        // of the original behavior, we'll use the same fixed-point arithmetic
        // calculation.
        uint16_t s = vol;
        uint16_t x = 0x3fff, y = 0x7d98;   // 1.15 fractional values = 0.49997, 0.981201
        for (int i = 0 ; i < 8 ; ++i) 
        {
            if ((s & 0x0001) == 0)
                x = ((x * y) >> 15) & 0xFFFF;
            y = ((y * y) >> 15) & 0xFFFF;
            s >>= 1;
        }
        volumeMultiplier = x << 1;
    }
    else
    {
        // zero volume -> completely mute by setting the PCM multiplier to zero
        volumeMultiplier = 0;
    }
}

//
// Set a channel's volume level
//
void DCSDecoderNative::SetChannelVolume(int ch, uint8_t level)
{
    if (ch >= 0 && ch < MAX_CHANNELS)
        channel[ch].channelVolume = level;
}

// --------------------------------------------------------------------------
//
// Data port access
//
void DCSDecoderNative::IRQ2Handler()
{
    // retrieve the next byte
    uint8_t data = ReadDataPort();

    // If the data port timeout has expired, clear any buffered bytes
    if (dataPortTimeout >= 13)
        nDataPortBytes = 0;

    // process the byte according to how many bytes are buffered
    switch (nDataPortBytes)
    {
    case 0:
        // This is the first byte, so just store it for later
        dataPortWord = (static_cast<uint16_t>(data & 0xFF) << 8);
        nDataPortBytes = 1;
        break;

    case 1:
        // Second byte.  Combine it with the first byte to form a
        // 16-bit word.
        dataPortWord |= (data & 0xFF);

        // check for 4-byte sequences and special 2-byte codes
        if ((dataPortWord >= 0x55AA && dataPortWord <= 0x55B2)
            || (dataPortWord >= 0x55BA && dataPortWord <= 0x55C1))
        {
            // This is the start of a 4-byte sequence.  Remember the
            // extended command code and await the next byte.
            dataPortExt = dataPortWord;
            nDataPortBytes = 2;
        }
        else if (dataPortWord > 0x55B2 && dataPortWord < 0x55BA)
        {
            // this range is invalid - discard it
            nDataPortBytes = 0;
        }
        else if (dataPortWord == 0x55C2 || dataPortWord == 0x55C3)
        {
            // 0x55C2, 0x55C3 - version number query.  Echoes back the major
            // and minor version number for the sound board software.  This
            // was added in the DCS-95 software; earlier versions don't
            // answer this query.  The last DCS-95 release for WPC games was
            // in Cactus Canyon and Champion Pub, which reported version 1.05.
            host->ReceiveDataPort(static_cast<uint8_t>(
                (dataPortWord == 0x55C2 ? (reportedVersion >> 8) : reportedVersion) & 0xFF));

            // this completes the command
            nDataPortBytes = 0;
        }
        else if ((dataPortWord & 0x8000) != 0)
        {
            // high bit set -> invalid command - discard it
            nDataPortBytes = 0;
        }
        else if (dataPortWord == 0x03E7 && gameID == GameID::TOTAN)
        {
            // This is an egregious hack, but it's not OUR hack - it's
            // straight out of the original equipment.  The TOTAN IRQ2
            // handler specifically checks for command code $03E7 and
            // responds by sending $11 back on the data port, rather
            // than queueing the command to the track sequencer as
            // normal.  It's almost certainly a last-minute bug fix
            // by a naive intern who didn't understand how the track
            // sequencer works, because TOTAN *also* has a standard
            // track program for 03E7 defined, which sends back byte
            // $10.  Clearly the intern was fixing a bug report saying
            // that the $10 should be a $11 instead, and didn't know 
            // howto fix it the right way, so they fixed it in this
            // particular wrong way.  For the sake of bug-for-bug
            // compatibility, we have to include our own version of
            // this regrettable coding error.
            host->ReceiveDataPort(0x11);

            // this completes the command
            nDataPortBytes = 0;
        }
        else
        {
            // It's a two-byte command, which is a track number to be
            // loaded on the next main loop invocation.  Queue it for
            // processing in the main loop.
            commandQueue.emplace_back(dataPortWord);

            // this completes the sequence
            nDataPortBytes = 0;
        }
        break;

    case 2:
        // Third byte of a four-byte sequence - queue it awaiting the
        // fourth byte.
        dataPortWord = (data & 0xFF);
        nDataPortBytes = 3;
        break;

    case 3:
        // Fourth byte of a four-byte sequence.  The fourth byte must be
        // the bitwise complement of the third byte; if not, discard the
        // whole command
        if (dataPortWord == (data ^ 0xFF))
        {
            if (dataPortExt == 0x55AA)
            {
                // 55 AA vol ~vol -> set master volume
                SetMasterVolume(static_cast<uint8_t>(dataPortWord));
            }
            else if (dataPortExt <= 0x55B2)
            {
                // 55 AB..B2 lvl ~lvl - set channel mixing level [channel - 0xAB]
                SetChannelVolume(dataPortExt - 0x55AB, static_cast<uint8_t>(dataPortWord));
            }
            else if (dataPortExt >= 0x55BA && dataPortExt <= 0x55C1)
            {
                // 55 BA..C1
                // I have no idea what this is for.  The value excess $55BA is an index
                // into an 8-element WORD[] array at DM($1700) and an 8-element DWORD[]
                // array at DM($1708).  The data byte is stored at both tables (in the
                // low word of the DWORD table entry; the high word is zeroed).  These
                // locations are used in track opcodes $10, $11, and $12, which I've
                // never seen used by any of the DCS-95 games, so perhaps these are
                // only used by DCS-based video games, or were added in anticipation
                // of future use that never actually happened.  Or maybe they're just
                // for testing/debugging.
                int ch = dataPortExt - 0x55BA;
                if (ch >= 0 && ch < MAX_CHANNELS)
                {
                    channel[ch].mysteryOpParams.target = 0;
                    channel[ch].mysteryOpParams.command = dataPortWord;
                }
            }
        }

        // this completes the sequence - reset the counter
        nDataPortBytes = 0;
        break;
    }

    // reset the timeout
    dataPortTimeout = 0;
}


// --------------------------------------------------------------------------
//
// ADSP-2105 helper functions, for special types and operators in the
// processor's instruction set
//

// Find the floating-point normalization shift for a 32-bit mantissa
int DCSDecoderNative::CalcExp32(uint32_t xop)
{
    int res = 0;
    if ((xop & 0x80000000) != 0)
    {
        for ( ; (xop & 0x40000000) != 0 ; --res, xop <<= 1) ;
    }
    else
    {
        for ( ; res > -31 && (xop & 0x40000000) == 0 ; --res, xop <<= 1) ;
    }
    return res;
}

// Normalize a 32 bit floating-point value, reproducing the effect of the
// ADSP-2105's EXP and NORM opcodes.  This works on a 32-bit mantissa in
// a single operation.  The ADSP-2105 opcodes operate on 16-bit quantities,
// but they're meant to be used in a specific sequence to produce a 32-bit
// result.  We combine the operations into their semantic equivalent for
// greater clarity and convenience.
//
// On return the mantissa is shifted as needed to normalize it, and the
// exponent is filled in.
uint16_t DCSDecoderNative::Normalize32(uint32_t &mantissa)
{
    // first, derive the exponent
    int exp = CalcExp32(mantissa);

    // now shift the value by the negative of the exponent
    if (exp <= -32)
        mantissa = 0;
    else if (exp < 0)
        mantissa <<= -exp;

    // return the exponent
    return static_cast<uint16_t>(exp & 0xFFFF);
}

// Arithmetic shift
uint32_t DCSDecoderNative::BitShiftSigned32(int32_t val, int by)
{
    // for positive numbers and left shifts, it's the same as LSHIFT
    if (val >= 0 || by >= 0)
        return BitShift32(val, by);

    // Negative number with right shift requires filling bits at the high
    // end with '1' bits.  The C++ right-shift operator's behavior for
    // negative signed values is implementation-defined, so we have to
    // make sure the bits are filled in explicitly for full portability.
    by = -by;
    if (by < 32)
        return (val >> by) | (~0UL << (32 - by));
    else
        return ~0UL;
}

uint16_t DCSDecoderNative::RoundMultiplyResult(uint64_t &mr, int32_t prod)
{
    int64_t res = mr + 0x8000;
    if ((prod & 0xFFFF) == 0x8000)
        res &= ~0x10000ULL;

    // store back the result
    mr = static_cast<uint64_t>(res);

    // return the MR1 portion
    return MR1(mr);
}

uint16_t DCSDecoderNative::RoundMultiplyResult(int32_t prod)
{
    int64_t res = static_cast<int64_t>(prod) + 0x8000;
    if ((prod & 0xFFFF) == 0x8000)
        res &= ~0x10000ULL;

    // return the MR1 portion
    return MR1(res);
}

uint16_t DCSDecoderNative::MultiplyAndRound(uint64_t &mr, uint16_t a, uint16_t b)
{
    int32_t prod = (static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b))) << 1;
    mr = prod;
    return RoundMultiplyResult(mr, prod);
}

uint16_t DCSDecoderNative::MultiplyAndRound(uint16_t a, uint16_t b)
{
    int32_t prod = (static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b))) << 1;
    auto mr = static_cast<uint64_t>(prod);
    return RoundMultiplyResult(mr, prod);
}

uint16_t DCSDecoderNative::MultiplyRoundAdd(uint64_t &mr, uint16_t a, uint16_t b)
{
    int32_t prod = (static_cast<int32_t>(SIGNED(a)) * static_cast<int32_t>(SIGNED(b))) << 1;
    int64_t res = static_cast<int64_t>(mr) + prod;
    mr = static_cast<uint64_t>(res);
    return RoundMultiplyResult(mr, prod);
}

uint16_t DCSDecoderNative::MultiplyRoundSub(uint64_t &mr, uint16_t a, uint16_t b)
{
    int32_t prod = (static_cast<int32_t>(SIGNED(a)) * static_cast<int32_t>(SIGNED(b))) << 1;
    int64_t res = static_cast<int64_t>(mr) - prod;
    mr = static_cast<uint64_t>(res);
    return RoundMultiplyResult(mr, prod);
}

uint16_t DCSDecoderNative::MulSS(uint16_t a, uint16_t b)
{
    int64_t prod = (static_cast<int64_t>(SIGNED(a)) * static_cast<int64_t>(SIGNED(b))) << 1;
    return MR1(prod);
}

uint16_t DCSDecoderNative::MulSS(uint64_t &mr, uint16_t a, uint16_t b)
{
    int64_t prod = (static_cast<int64_t>(SIGNED(a)) * static_cast<int64_t>(SIGNED(b))) << 1;
    mr = static_cast<uint64_t>(prod);
    return MR1(prod);
}

uint16_t DCSDecoderNative::MulSU(uint16_t a, uint16_t b)
{
    int64_t prod = (static_cast<int64_t>(SIGNED(a)) * static_cast<int64_t>(b)) << 1;
    return MR1(prod);
}

uint16_t DCSDecoderNative::MulSU(uint64_t &mr, uint16_t a, uint16_t b)
{
    int64_t prod = (static_cast<int64_t>(SIGNED(a)) * static_cast<int64_t>(b)) << 1;
    mr = static_cast<uint64_t>(prod);
    return MR1(mr);
}
