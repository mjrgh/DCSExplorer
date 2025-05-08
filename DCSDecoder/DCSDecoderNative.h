// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Decoder - universal native decoder.  This subclass implements
// a DCS audio player in portable C++ code.
//
#pragma once
#include <memory>
#include "DCSDecoder.h"

class DCSDecoderNative : public DCSDecoder
{
public:
    // construction
    DCSDecoderNative(Host *host);

    // Run the main loop, decoding the next 240 samples
    virtual void MainLoop() override;

    // Process pending data port input
    virtual void IRQ2Handler() override;

    // get the decoder subclass name
    virtual const char *Name() const override { return "Universal native decoder"; }

    // In lieu of populating the ROM array, initialize the
    // decoder in standalone mode.  This doesn't allow playing
    // back track programs, since there are no ROMs to hold
    // the tracks, but it does allow playing streams that are
    // directly loaded from external sources.  The caller must
    // specify the DCS software version to emulate (which
    // implies a hardware version), since we'd normally infer
    // that from the ROM image.
    void InitStandalone(OSVersion osVersion);

    // Set the master volume level, 0..255.  This is a roughly
    // logarithmic scale from completely muted at 0 to the 
    // reference PCM level (the level of the original recording)
    // at 255.
    //
    // The volume can also be set by sending a special byte 
    // sequence to the sound data port: 55 AA <vol> ~<vol>
    // (where <vol> is the volume level, 0..255, and ~<vol> is
    // the bitwise NOT of that byte).  That's the way the WPC
    // board controls the volume in the original hardware
    // implementation.
    virtual void SetMasterVolume(int vol) override;

    // Set the version number to report in 55C2/55C3 data port
    // commands.  By default, we report version 1.06, which is higher
    // than any of the released pinball titles.  (The last WPC titles
    // released, Cactus Canyon and Champion Pub, in 1998, report
    // version 1.05; earlier DCS-95 titles report 1.03 or 1.04.
    // Versions prior to 1.03, including the first DCS-95 release
    // and all original DCS hardware games, don't implement the
    // 55C2/55C3 queries at all.)
    // 
    // The reason we use a default version report that's higher than
    // that reported by any of the official releases is that it
    // distinguishes this implementation from any of the original
    // ROM versions, but also implies that we support a superset of
    // the features of any official release.  The host program can
    // explicitly set a different version number if needed.  This
    // is mostly for the sake of any situations that arise where
    // the WPC software asks for the version and then does something
    // different depending on which version it detects.  I doubt any
    // any such cases exist - I suspect the version query was purely
    // for the sake of in-house development and testing - but the
    // explicit setting is here just in case any such situations do
    // ever turn up.
    //
    // If you want the decoder to report the same version number
    // that the loaded ROM would report, load the ROM and then
    // do this:  SetReportedVersionNumber(GetVersionNumber()).
    void SetReportedVersionNumber(uint16_t vsn) { reportedVersion = vsn; }

    // Load an audio stream into a channel.  This directly loads
    // a stream without going through the "track program" mechanism.
    // This is useful for tasks such as extracting streams or
    // playing streams directly.  The address is given as a direct
    // pointer, to allow decoding streams from arbitrary sources,
    // even from outside of ROM images.  If you have a ROM image
    // and you want to get a byte pointer from a 24-bit linear ROM
    // address (such as the operand of a "Load Track" opcode in a
    // track program), use MakeROMPointer().
    // 
    // The mixing level corresponds to the 1-byte mixing level in 
    // the track programs (opcodes 0x07-0x0C), which are signed 
    // 8-bit ints (-128..+127).  Ideally, the mixing level should
    // be obtained from a track program, since each stream seems
    // to be designed to be played back at a particular level, and
    // there's no coding directly in the stream that indicates what
    // that is.  The track programs almost universally set a level
    // immediately before loading a stream, so that's the best
    // way to determine the natural level of a stream.  Levels
    // around 0x64 seem to be typical in practice, with variation
    // from about 0x60 to 0x70.
    void LoadAudioStream(int channel, const ROMPointer &streamPtr, int mixingLevel);

    // is a stream currently playing in the given channel?
    bool IsStreamPlaying(int channel);

    // Get information on a stream.  Note that this is relatively
    // time-consuming, because it requires decoding the entire
    // stream's contents as though playing it back.
    struct StreamInfo
    {
        // size of the stream in DCS frames (240 samples, 7.68ms)
        int nFrames;

        // size of the stream in bytes
        int nBytes;

        // frame format major type (0 or 1)
        int formatType;

        // frame format subtype (0..3)
        int formatSubType;

        // stream header
        uint8_t header[16];
    };
    StreamInfo GetStreamInfo(const ROMPointer &streamPtr);

    // Clear all tracks
    void ClearTracks();

    // Add a track command to the queue
    void AddTrackCommand(uint16_t trackNum);

protected:
    // Initialize the decoder
    virtual bool Initialize() override;

    // Frame buffer.  This contains the frequency-domain data
    // points decoded from the current compressed frame, and is
    // used to transform the data in-place to the time domain to
    // generate the PCM samples.  Each compressed frame directly
    // contains 256 data points, but before the domain transform
    // step, this is doubled to 512 data points using the MDCT
    // symmetries.
    uint16_t frameBuffer[0x200];

    // Output buffer.  This is the final PCM sample output buffer.
    uint16_t outputBuffer[240];

    // Overlap buffer - this is a portion of the output buffer carried 
    // across frames.
    uint16_t overlapBuffer[0x10];

    // Current nominal volume, 0.255, as set by the 55 AA data port 
    // command or by a call to SetMasterVolume().
    uint8_t nominalVolume = 0x67;

    // Current volume level PCM multiplier value.  This value is
    // derived from the nominal volume level; it's the multiplier
    // applied to all PCM samples when generating the final output
    // buffer.  This is interpreted as a 1.15 fixed-point fraction
    // (the mathematical value is the x/32768, where x is the 2's
    // complement value).
    uint16_t volumeMultiplier = 0x0391;

    // Version number to report in 55C2/55C3 data port queries.
    // For the sake of distinguishing this decoder version from the
    // original ROM versions, use a default of 1.06 (the official
    // DCS-95 releases reported 1.03 through 1.05).  The host can
    // override this as desired.
    uint16_t reportedVersion = 0x0106;

    // set a channel's volume level (data port commands 55AB..55B2)
    void SetChannelVolume(int channel, uint8_t level);

    // queue a command
    void QueueCommand(uint16_t cmd) { commandQueue.emplace_back(cmd); }

    // Pending command queue.  This stores the decoded commands
    // waiting to be executed.  A command is a two-byte sequence
    // that selects a track to load.  Commands are processed at
    // the start of each call into the main decoder loop.
    std::list<uint16_t> commandQueue;

    // Data port byte buffer.  This stores incoming bytes written
    // to the data port until they form a complete command, at
    // which point the command is added to the command queue (or
    // executed immediately, in the case of volume commands).
    uint16_t dataPortWord = 0;
    uint16_t dataPortExt = 0;
    int nDataPortBytes = 0;

    // Data port timeout counter.  This counts the number of calls
    // to the main loop since the last data port byte was written.
    int dataPortTimeout = 0;

    // Load a track.  This selects the track as the active track for a
    // designated channel.
    void LoadTrack(int channel, const ROMPointer &track);

    // Execute a track.  This interprets and executes an active track's 
    // byte-code program.
    void ExecTrack(int curChannel);

    // Load an audio stream
    void LoadAudioStream(int streamChannel, int sourceProgramChannel, int loopCount, const ROMPointer &streamPtr);

    // Set up a channel object with a new stream
    struct Channel;
    void InitChannelStream(Channel &channel, ROMPointer streamPtr);

    // decode a stream for the current frame
    void DecodeStream(uint16_t ch);

    // Initialize stream playback.  This stream decoder calls this first
    // when the stream's current playback position pointer is at the start
    // of the stream data.
    void InitStreamPlayback(Channel &ch);

    // calculate channel mixing levels
    void UpdateMixingLevels();

     // clear buffers for the given channel
    void ResetMixingLevels(int ch);

    // initialize channel memory structs
    void InitChannels();

    // ROM bit vector reader.  This treats ROM as a packed array of bits,
    // providing access in arbitrary word sizes up to 24 bits.  The order
    // within bytes is most significant bit first.
    class ROMBitPointer
    {
    public: 
        ROMBitPointer() { }
        ROMBitPointer(const ROMPointer &p) : p(p) { }

        // some operations map directly to the underlying ROM pointer
        bool IsNull() const { return p.IsNull(); }
        void Clear() { p.Clear(); }
        bool operator==(const ROMPointer &p) const { return this->p == p; }

        // read the next n bits (up to 24 bits)
        uint32_t Get(int n)
        {
            // read the next bits
            uint32_t result = Peek(n);

            // consume the bits
            nBits -= n;
            buf <<= n;

            // return the result
            return result;
        }

        int32_t GetSigned(int n)
        {
            // read the unsigned value
            int32_t result = static_cast<int32_t>(Get(n));

            // if the high bit is set, sign-extend to 32 bits
            if ((result & (1 << (n - 1))) != 0)
                result |= 0xFFFFFFFF << n;

            return result;
        }

        // peek at the next n bits (up to 24 bits)
        uint32_t Peek(int n)
        {
            // add more bytes to the buffer until we have enough bits to satisfy
            // the request
            while (nBits <= n)
            {
                buf |= *p++ << (24 - nBits);
                nBits += 8;
            }

            // fulfill the request from the high end of the buffer
            return static_cast<uint32_t>(buf >> (32 - n));
        }

        // current ROM byte pointer
        ROMPointer p;

        // Lookahead buffer
        uint32_t buf = 0;

        // number of bits available in the lookahead
        int nBits = 0;
    };

    // Channel data structure
    //
    // The DCS-93 code supports 4 channels, and most of the later games
    // games allow 6 channels.  Safe Cracker uses 8 channels.  We set
    // the limit to the largest number actually used by extant titles,
    // which is 8.  Games don't care if more channels are available than
    // they actually use, so setting the limit to 8 won't affect the
    // behavior of the games that only assume 4 or 6 channels.
    //
    // Note that channels are purely for mixing purposes, to allow
    // multiple clips to play back simultaneously to be combined into
    // the final output.  These channels aren't for stereo or surround
    // sound - everything is mixed together at the final output stage
    // to form the final monophonic signal.
    static const int MAX_CHANNELS = 8;
    struct Channel
    {
        // Track pointer.  This is the next execution location in 
        // the track's byte-code program.
        ROMPointer trackPtr;

        // Track counter.  This is set to zero when the opcode is initially 
        // executed, and incremented on each main loop pass, so it's equivalent 
        // to a timer in units of about 7.68ms.  Each opcode in a track program
        // is preceded by a U16 count value that indicates how long the program
        // should pause before executing the new opcode. E.g., if an opcode
        // is preceded by a count of 10, it means that execution pauses for
        // 77.5ms before proceeding.  (A count prefix of zero means that the
        // program continues to the next opcode immediately, so groups of
        // opcodes that are to be executed as a group all have prefixes of 0.
        // The special counter prefix 0xFFFF halts the program indefinitely.)
        uint16_t trackCounter = 0;

        // Next track type.  This is the type code for the track in
        // nextTrackLink.
        uint8_t nextTrackType = 0;

        // Next track link.  This stores a link to another track that can be
        // triggered on this channel at a later time from *another* track's
        // program via opcode 0x05.  The meaning of the information stored
        // here depends on nextTrackType:
        //
        // - When nextTrackType is 2, this is a track number (same meaning as
        //   in a data port command code).  Triggering the link via opcode
        //   0x05 simply queues the command code, as though it had been
        //   received on the data port.
        //
        // - When nextTrackType is 3, this is a Var:Track index value.  The high
        //   byte is an index into the "variables" array that can be written
        //   from a track program via opcode 0x06, and the low byte is an
        //   index into the catalog[$0043] pointer array.  The variable value
        //   is then used as an index into *that* array.  There are so many
        //   levels of indirection going on that I have no idea what the point
        //   of this is.
        // 
        //      hi = high byte of nextTrackLink
        //      lo = low byte of nextTrackLink
        // 
        //      V = SavedVariables[hi]
        //      Table = U24 ROM pointer at catalog[$0043], points to array of U24 pointers
        //      SubTable = U24 ROM pointer at Table[lo], points to array of U16 command codes
        //      Command = SubTable[V]
        //
        uint16_t nextTrackLink = 0;

        // Stop channel.  When this flag is set, the channel is stopped (and
        // all playback status cleared) in the next main loop iteration.  This
        // is used to force a stop on a channel if the frame decompressor
        // encounters an error in the audio stream data.
        bool stop = false;

        // Current audio stream.  This contains information on the audio
        // stream currently playing in the channel.
        struct AudioStream
        {
            // Clear the stream
            void Clear()
            {
                headerPtr.Clear();
                startPtr.Clear();
                playbackBitPtr.Clear();
            }

            // Header location.  This points to the start of the 16-byte
            // header at the start of the stream.
            ROMPointer headerPtr;

            // header length
            int headerLength = 16;

            // Start of the sample streatm.  This points to the first byte
            // of the stream data, immediately following the header.
            ROMPointer startPtr;

            // Current playback pointer.  The stream (following the header)
            // is a packed bit vector, with elements of varying bit width.
            // The playback position thus has to be specified to the bit.
            ROMBitPointer playbackBitPtr;

            // Stream header.  This is a local copy of the 16-byte header at 
            // the start of the current stream, with the bytes zero-padded to
            // 16 bits.
            //
            // Each element of the header is an 8-bit floating-point value
            // specifying the multiplier for all of the samples in one band
            // of data.  The bits are packed in this format:
            //
            //    sseeeemm
            //
            // ss   = output buffer step size (increment between samples
            //        in the output buffer): 0 -> step=1, nonzero -> step=2
            // eeee = binary exponent, excess 15 (0001 -> -14, 0010 -> -13, etc)
            // mm   = mantissa
            // 
            // The mantissa codes the following values:
            // 
            // 00 -> $8000
            // 01 -> $9838
            // 10 -> $B505
            // 11 -> $D745
            //
            // The end of the table is marked with an entry with all 7 low
            // bits set to 1 ($FF or $7F).
            //
            // If the first header byte has its high bit ($80) set, every
            // frame can have a different volume code specified through a
            // table lookup using the frame band data block.
            // 
            uint8_t header[16] ={ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

            // Band type codes.  Each frame starts with an array of type
            // codes, one per header slot.  These correspond to the bands
            // specified in the header - there's one band type array entry
            // for each entry in the header.
            //
            // In each frame, the band type codes are specified as deltas
            // from the prior frame, so we need to keep the last frame's
            // values in memory until we decode the next frame.  That's
            // what this buffer is for.
            uint16_t bandTypeBuf[16] ={ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

            // Stream frame counter  This is the number of frames the stream
            // contains.  A frame represents the decoding output for one main
            // loop pass - 240 samples, about 7.68ms of audio data.  This
            // counter is initialized from the stream header when the stream
            // is loaded, and is decremented on each main loop pass while the
            // stream is active.  When it reaches zero, playback has reached
            // the end of the stream.
            uint16_t frameCounter = 0;

            // Number of frames in the audio stream.  This is set from the
            // stream header when the stream is loaded, and is used to reset
            // the current frame counter when the stream loops.
            uint16_t numFrames = 0;

            // Stream loop counter.  This is the number of times to repeat
            // the stream.  When playback reaches the end of the stream, we
            // decrement this counter and loop the stream back to the start,
            // stopping when we reach zero.  As per the usual convention,
            // an initial counter value of zero means loop forever.
            uint16_t loopCounter = 0;

        }
        audioStream;

        // Controlling source channel for audio stream.  This is the 
        // channel with the track program that loaded this channel's 
        // current audio stream.
        int sourceChannel = -1;

        // Mixing controls.  Each channel can set the mixing level in
        // each *other* channel, separately, with the option to fade to
        // a new level.  This allows a sound effect to temporarily reduce
        // the mixing level of the main music channel, for example.
        //
        // Because each channel can independently set a mixing level for
        // every other channel, each channel needs an array of mixer
        // structs, indexed by the source channel.  The mixing levels
        // in the array all apply to this channel; the array index 
        // represents the source channel that's applying the effect.
        struct MixingControl
        {
            // current mixing level
            int curLevel = 0;

            // Target level.  When a fade is in effect, this is the
            // final level when the fade is completed.
            int fadeTargetLevel = 0;

            // Fade delta.  This is the increment to add on each fade
            // step when a fade is in effect, until the level reaches
            // the target level.
            int fadeDelta = 0;

            // number of fade steps
            int fadeSteps = 0;

            // reset - sets the adjustment level and fade counter to zero
            void Reset() { curLevel = 0; fadeTargetLevel = 0; fadeSteps = 0; }
        };
        MixingControl mixer[MAX_CHANNELS];

        // Special mixing level flag: Use the maximum mixing level for
        // this channel, overriding the normal mixing calculation.  This
        // is a very odd special feature only implemented in the 1.05
        // software.  The flag is controlled by track program opcode $04,
        // Write Data Port Byte.  If the opcode $04 operand is $01, it
        // clears this flag on channel 5; if it's $69, it sets the flag
        // on channel 5.  In the original 1.05 software, this flag is
        // just a global variable, which is adequate because it only
        // affects channel 5.  For our purposes, it's an attribute of
        // the channel.  But there's no way for the ROM track programs
        // to set this flag for any channel other than channel 5.
        bool maxMixingLevelOverride = false;


        // Current aggregate mixing level multiplier for the channel. 
        // This is derived by adding up the individual mixing levels and
        // then using the result as an exponent to calculate an attenuation
        // level on a logarithmic volume scale.  Note that this value is
        // interpreted as a 1.15 fixed-point fraction (that is, its
        // mathematical value is the nominal 2's complement value divided
        // by 32768).
        uint16_t mixingMultiplier = 0x7FFF;

        // Host event timer.  Each channel's track program can set up an
        // event timer that writes notifications to the host data port at
        // timed intervals.  This lets the host perform actions in sync
        // with the music playback.  
        // 
        // This mechanism is implemented in all versions of the DCS ROM
        // program, but the track program opcode ($04) to activate it is 
        // only implemented in OS93a, used only for IJTPA and JD.  Opcode
        // $04 was redefined in OS93b and all later versions to simply
        // send a byte to the host immediately.  But the host event timer
        // mechanism is preserved in all of the subsequent versions, even
        // though it's essentially dead code from STTNG onwards.
        struct HostEventTimer
        {
            // byte to write to the data port at each timed event
            uint8_t data = 0;

            // Timer interval.  This is in units of main loop iterations, which
            // corresponds to 240 PCM samples, or about 7.68ms of real time. 
            // Zero means that the event timer for the channel is disabled.
            uint16_t interval = 0;

            // Current countdown timer.  This is initialized to the interval
            // value when the timer is set up, and decremented on each main loop
            // pass until it reaches zero, at which point the data byte is sent
            // to the host and the countdown is reset to the interval value.
            uint16_t counter = 0;

            // set up the timer
            void Set(uint8_t data, uint16_t interval)
            {
                this->data = data;
                this->interval = this->counter = interval;
            }
            
            // clear the timer
            void Clear() { interval = counter = 0; }

            // Update the counter.  If the event is active, this decrements
            // the counter.  If the counter reaches zero, it resets the counter
            // to the interval value and returns true.  Returns false if the
            // event is inactive or the counter hasn't reached zero yet.
            bool Update()
            {
                // check if the interval is active
                if (interval != 0)
                {
                    // it's active - decrement the counter
                    --counter;

                    // check if it's reached the event time at counter == 0
                    if (counter == 0)
                    {
                        // event firing time - reset the counter and return true
                        counter = interval;
                        return true;
                    }
                }

                // inactive or not yet at an event time - return false
                return false;
            }
        }
        hostEventTimer;

        // Channel volume.  Commands 55 AB-B2 allow the host to explicitly
        // set a relative volume level per channel.  The level is a linear
        // multiplier applied to the PCM samples, 0..255.
        uint16_t channelVolume = 0xff;

        // Loop stack.  Each channel has a stack of loop points.
        // Track control byte code 0x0E pushes the current playback
        // position onto the stack; code 0x0F pops the stack and
        // sets the playback position back to the saved point.
        // The counter specifies the number of times to loop, with
        // the usual special case that 0 means loop forever.
        struct LoopPos
        {
            LoopPos(uint16_t counter, const ROMPointer &pos) : counter(counter), pos(pos) { }
            uint16_t counter;
            ROMPointer pos;
        };
        std::list<LoopPos> loopStack;

        // push/pop a loop position
        void PushPos(uint16_t counter, const ROMPointer &pos);
        void PopPos(ROMPointer &pos);

        // Opcodes of Mystery, $10, $11, $12
        // Command codes 55 BA..BF xx ~xx
        // 
        // These elements are per-channel arrays that appear in the DCS-95
        // ROM code, but which do nothing.  Track opcodes $10, $11, and $12
        // and command codes 55 BA..BF write to these structures.  They're
        // never used anywhere else in the code.  They probably represent
        // work towards future features that were never completed.  The
        // track opcodes use a set/increase/decrease idiom, with what
        // looks like a time ramp calculated in the increase/decrease ops,
        // in that the calculate the quotient of two operands, which are
        // probably a delta value and a time step count, respectively,
        // just like the channel volume fade ops.  There's no practical
        // reason to even keep the code for these ops or commands because
        // they don't appear to be used by any existing game ROMs.  (As
        // you'd sort of expect given that whatever they were supposed
        // to do is clearly not implemented - the memory locations that
        // the ops set up are never accessed anywhere else in the code.)
        // I'm keeping them for the sake of documentation, and also in
        // case I'm just missing something and they turn out to be used
        // somewhere after all.
        struct MysteryOpParams
        {
            // current value
            uint16_t current = 0x007F;

            // target value
            uint16_t target = 0x007F;

            // command value
            uint32_t command = 0x0000007F;

            // step counter value
            uint16_t stepCounter = 0xFFFF;

            // Step size.  In the original code, this was represented
            // as an ADSP-2105 normalized 16-bit floating point value,
            // which is a rather tedious type to work with.  The intent
            // of the code is clearly to perform a fractional adjustment
            // over a number of steps, so we'll make things a whole lot
            // simpler by using a native float.
            float stepSize = 0.0f;

            // set to the specified target value with immediate effect
            void Set(uint16_t newTarget)
            {
                current = target = newTarget;
                command = 0;
                stepCounter = 0xFFFF;
                stepSize = 0.0f;
            }

            void Reset() 
            {
                current = target = 0x7F;
                command = 0x0000007F;
                stepCounter = 0xFFFF;
                stepSize = 0.0f;
            }

        } mysteryOpParams;
    };
    Channel channel[MAX_CHANNELS];

    // Channel "ready" mask.  This is a bit mask set to indicate which
    // channels have pending work in their track programs.  Each 
    // channel's bit is given by (1 << channelNumber).  The channel bit
    // is set to 1 when the channel is finished processing for the current
    // main loop pass.  It's cleared to 0 when there's more work pending.
    unsigned channelMask = 0;

    // Track program "variables".  A track byte code program can store a
    // byte value at a specified 8-bit index via opcode 0x06.  This can
    // later be retrieved via opcode 0x05.  In the original ROM code,
    // it's not entirely clear how much memory is reserved for the array
    // that holds the variable values, but it appears to be at most 0x50
    // entries.  However, the variable index can in principle be any
    // 8-bit value, so I'm reserving a full 0x100 entries.  None of the
    // official DCS ROMs use opcode 0x06 at all, so we don't even need
    // to implement this to run the original ROMs, but we might still
    // encounter it someday because our own compiler can generate these
    // references.  Note that even though this native implementation can
    // accept any 8-bit variable index, the compiler should still impose
    // a limit that's compatible with the original ADSP-2105 programs.
    // The original software doesn't have any bounds checking when
    // writing to variable slots from opcode 0x06, so the compiler
    // should provide some.
    uint8_t trackProgramVariables[0x100];

    // Handle a mixing level control opcode in the track program (
    // opcodes 0x07-0x0C)
    // 
    // mode:
    //   0 = set new absolute level to 2nd byte param
    //   1 = increase by (2nd param: BYTE)
    //   2 = decrease by (2nd param: BYTE)
    // 
    // fade:
    //   false = apply change immediately
    //   true = fade over (3rd param: WORD) steps
    // 
    // Byte-code stream parameters:
    //   BYTE = target channel number
    //   BYTE = new level/delta, signed BYTE, actual level is the byte*64
    //   WORD = only present if fade==true -> number of fade steps
    // 
    // Note that the "mode" value is implicit in the opcode.  For opcodes
    // 0x07-0x09, subtract 0x07 to get the mode; for opcodees 0x0A-0x0C,
    // subtract 0x0A.
    //
    void MixingLevelOp(int curChannel, ROMPointer &p, int mode, bool fade);


    // Decoder variations.  This class virtualizes the frame
    // decompression and IMDCT routines to match the version of
    // the ROM we're working with.  This is necessary because
    // there are really two different DCS formats across all of
    // the released pinball games: the 1.0 format, used in the
    // three games released in 1993, and the 1.1 format, used for
    // all later games.  The two formats differ in two important
    // ways.  The first is that they use substantially different
    // layouts for the compressed frame data, as stored in the ROM
    // audio streams.  The information coded into a frame is
    // essentially the same in both versions (a set of 16-bit
    // frequency-domain data points for a 7.68ms sampling window),
    // but the order of the fields, the Huffman codebooks, and
    // some other details are different.  They're different enough
    // that it's cleaner to use two completely separate de-
    // compression routines to handle the respective version
    // than to try to unify them into a single routine.  The
    // other difference between the versions is that they use
    // slightly different algorithms to convert the frequency-
    // domain data into the time domain.  The two algorithms
    // implement identical mathematical transformations, but they
    // do si using slightly different sequences of intermediate
    // calculations, so the final PCM numbers end up disagreeing
    // by a few parts per thousand due to different amounts of
    // accumulated rounding error.  The difference isn't enough
    // to be audible, so we don't absolutely need to implement
    // both algorithms - we could just use the 1.01 algorithm
    // for the 1.0 data and it would sound indistinguishable
    // from using the 1.0 algorithm.  But for the sake of
    // rigorous testing against the reference implementation,
    // we'd like the PCM numbers to come out *exactly* the same
    // in all cases, and to do that we have to implement both
    // algorithms.
    class DecoderImpl
    {
    public:
        DecoderImpl(DCSDecoderNative *decoder) : decoder(decoder) { }
        DCSDecoderNative *decoder;

        // Decompress a frame
        virtual void DecompressFrame(Channel &channel, uint16_t *frameBuffer) = 0;

        // Transform decompressed frame data into PCM samples.  This 
        virtual void TransformFrame(int volShift) = 0;
    };

    // active decoder implementation
    std::unique_ptr<DecoderImpl> decoderImpl;

    // Decoder implementation - common base class for 1993 ROMs.  The 1993
    // ROMs all use the same interpretation of "high bit clear" streams,
    // but interpret "high bit set" streams differently.
    class DecoderImpl93 : public DecoderImpl
    {
    public:
        DecoderImpl93(DCSDecoderNative *decoder) : DecoderImpl(decoder) { }
        virtual void DecompressFrame(Channel &channel, uint16_t *frameBuffer) override;
        virtual void TransformFrame(int volShift) override;

    protected:
        // decode a band type code via the 93 huffman codeback
        int ReadHuff93(ROMBitPointer &p, int &bandSubType);
    };

    // Decoder implementation for OS93a ROMs (Judge Dredd and IJTPA).  This
    // version has a completely different interpretation for some stream types.
    class DecoderImpl93a : public DecoderImpl93
    {
    public:
        DecoderImpl93a(DCSDecoderNative *decoder) : DecoderImpl93(decoder) { }
        virtual void DecompressFrame(Channel &channel, uint16_t *frameBuffer) override;
    };

    // Decoder implementation for OS93b (STTNG).  This has no differences
    // from the base OS93 decoder, but we define a class for it for the
    // sake of documentation and a more coherent hierarchy.
    class DecoderImpl93b : DecoderImpl93
    {
    public:
        DecoderImpl93b(DCSDecoderNative *decoder) : DecoderImpl93(decoder) { }
    };

    // Decoder implementation for 1994+ ROMs
    class DecoderImpl94x : public DecoderImpl
    {
    public:
        DecoderImpl94x(DCSDecoderNative *decoder) : DecoderImpl(decoder) { }
        virtual void DecompressFrame(Channel &channel, uint16_t *frameBuffer) override;
        virtual void TransformFrame(int volShift) override;
    };


    //
    // Helper functions for special-purpose ADSP-2105 operations
    // 
    // The original DCS code was written for the ADSP-2105 processor, which has
    // some special hardware-level arithmetic functions that have no portable
    // C++ equivalents.  In order to get numerically identical results, we have 
    // to do our calculations using the same special types and functions.  The
    // special ADSP-2105 functions can all be implemented in terms of the
    // standard C++ integer types and standard C++ arithmetic coperators.
    // (This isn't a complete set of special ADSP-2105 arithmetic functions;
    // it's just the subset we need for a compatible decoder implementation.)

    // convert int value from signed to unsigned or vice versa, keeping the same type size
    template<typename T> inline static typename std::make_signed<T>::type SIGNED(T val) { return static_cast<typename std::make_signed<T>::type>(val); }
    template<typename T> inline static typename std::make_unsigned<T>::type UNSIGNED(T val) { return static_cast<typename std::make_unsigned<T>::type>(val); }

    // saturate a 16-bit signed integer value to range -32768..32767
    static uint16_t SaturateInt16(int val) { return val < -32768 ? -32768 : val > 32767 ? 32767 : val; }

    // Perform a bit shift with a positive or negative shift count.  A positive
    // shift value is a left shift; a negative value is a right shift.
    //
    // This is a logical (not arithmetic) shift: when shifting right, vacated
    // high bits are filled with zeroes unconditionally.
    //
    // (We have to use this function instead of the native C++ << operator 
    // because the native operator's behavior with a negative shift count 
    // is not portably defined.  The C++ standard explicitly specifies that
    // the result of (x << y) for y < 0 is implementation-defined.)
    static uint32_t BitShift32(uint32_t val, int by) { return by >= 0 ? val << by : val >> -by; }

    // Bit-shift a signed 32-bit int, with sign-bit fill when right-shifing.
    // As with BitShift32(), positive shift counts are left shifts, and negative
    // shift counts are right shifts.
    // 
    // This is an arithmetic shift: when shifting right, vacated high bits are
    // filled with the sign bit from the original value.
    // 
    // (This function is required in place of the C++ << operator becuase that
    // operator's behavior with negative shift counts is not portably defined,
    // and because the >> operator's behavior with signed values is also not
    // portably defined.)
    static uint32_t BitShiftSigned32(int32_t val, int by);

    // Post-increment an index (increment may be negative).  This is a
    // convenience function to simplify cases where index values must be
    // increments or decremented by more than one place at a time.
    static uint16_t PostInc(uint16_t &idx, int inc) { uint16_t ret = idx; idx += inc; return ret; }

    // Find the shift value needed to normalize a 32-bit signed mantissa for normalized
    // floating point representation, using ADSP-2105 logic.  The return value is the
    // exponent of the normalized value, and is the negative of the left-shift needed
    // to normalize the mantissa (i.e., eliminate all but one of the sign bits at the
    // most significant end).  A normalized fractional integer on the ADSP-2105 is in
    // 1.X format (1.31 for a UINT32, 1.15 for a UINT16), meaning that it has one bit
    // (the sign bit) before the binary point, and all remaining bits are fractional
    // (i.e., they come after the binary point).  In this format, the mathematical
    // value of the mantissa is the 2's complement value divided by 2^(N-1), where N
    // is the number of bits in the value (16 or 32).  This can then be combined with
    // an exponent to form a rational number at greater scales.  The mathematical
    // value of the combined value is the mantissa value (again, the 2's complement
    // value divided by 2^(N-1) multiplied by 2^exponent.  
    static int CalcExp32(uint32_t val);

    // Normalize a 32-bit floating point value, using ADSP-2105 logic.  This derives
    // the exponent (using the same logic as CalcExp32) and replaces the mantissa
    // value with the shifted value based on the derived exponent.  Returns the
    // derived exponent, as a UINT16 value.
    static uint16_t Normalize32(uint32_t &mantissa);

    // Simulate MAC operations.  The forms that take a uint64_t& MR argument
    // accumulate the result in the provided MR variable, and return the MR1
    // portion (the high-order 16 bits of the 32-bit multiply result).  The
    // forms without MR argument just return the MR1 value without storing
    // the full 64-bit result.

    // get the MR0/MR1 value from MR
    static uint16_t MR0(uint64_t mr) { return static_cast<uint16_t>(mr & 0xFFFF); }
    static uint16_t MR1(uint64_t mr) { return static_cast<uint16_t>((mr >> 16) & 0xFFFF); }

    // Multiply and round fractional integer values, using ADSP-2105 logic.
    static uint16_t RoundMultiplyResult(uint64_t &mr, int32_t product);
    static uint16_t RoundMultiplyResult(int32_t product);

    static uint16_t MultiplyAndRound(uint64_t &mr, uint16_t a, uint16_t b);
    static uint16_t MultiplyAndRound(uint16_t a, uint16_t b);

    // Round the result of a signed integer multiply, using the ADSP-2105's logic.
    static uint16_t MultiplyRoundAdd(uint64_t &mr, uint16_t a, uint16_t b);
    static uint16_t MultiplyRoundSub(uint64_t &mr, uint16_t a, uint16_t b);

    // Calculate the product of two 1.15 numbers, as a 1.15 result, signed*signed
    static uint16_t MulSS(uint64_t &mr, uint16_t a, uint16_t b);
    static uint16_t MulSS(uint16_t a, uint16_t b);

    // Calculate the product of two 1.15 numbers, as a 1.15 result, signed*unsigned
    static uint16_t MulSU(uint64_t &mr, uint16_t a, uint16_t b);
    static uint16_t MulSU(uint16_t a, uint16_t b);
 };
