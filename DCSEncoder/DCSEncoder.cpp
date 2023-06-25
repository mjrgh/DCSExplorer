// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder
//
// This module implements an audio encoder that can generate streams
// conforming to the DCS 1993 and 1994+ formats.  The encoder takes a 
// PCM audio stream as input, and produces a DCS audio object as output.
// The resulting audio object uses the native DCS byte format, so it's
// suitable for embedding directly in a DCS ROM. 
// 
// The encoder can produce two format families: the 1993 format, used
// in the the first DCS pinball titles (the 1993 titles Indiana Jones,
// Judge Dredd, and Star Trek: The Next Generation), and the 1994+
// format, used in all subsequent DCS pinball games.  Between the two
// format families, all of the DCS titles are covered, so you can use
// the encoder to create new audio tracks for any DCS pinball game.
//
// The DCSEncoder class only handles DCS audio stream encoding.  The
// separate DCSCompiler class handles the rest of the DCS ROM creation
// process, which involves a couple of other object types and a sort of
// container format for the overall ROM layout.  DCSCompiler can create
// entire new ROM sets suitable for burning to EPROMs and installing in
// physical DCS boards.
//

#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include "DCSEncoder.h"
#include "../DCSDecoder/DCSDecoderNative.h"
#include "../libsamplerate/src/samplerate.h"

#pragma comment(lib, "libsamplerate")

// a much-used constant
static const float PI = 3.1415926536f;

// Table of DCS band sample counts for the 1994+ format.  Each frame
// consists of 255 frequency-domain samples.  The samples are divided
// into 16 bands of varying (but pre-determined) sizes, as listed here.
//
// The 1993 and 1994 formats divide up the bands differently.  The
// 1994 distribution is non-uniform, whereas the 1993 format has 16
// samples in every band, with one exception: OS93b Type 1 streams
// have 15 samples in the first band, and 16 each in the rest.
static const uint16_t bandSampleCounts94[] ={
    7, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 32
};
static const uint16_t bandSampleCounts93[] ={
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
};
static const uint16_t bandSampleCounts93b_Type1[] ={
    15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
};

// Normalization factors for the band sample counts (the inverse
// of the sample count, times 16, to scale per-band sums as though
// every band had 16 elements).  Note that we fold the OS93 types
// into a single table, since they're all *almost* identical, with
// just the slight difference for 93b Type 1 streams (15 samples
// in the first band rather than 16).
static const float bandSampleNorm94[] ={
    16.0f/7, 16.0f/8, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16,
    16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/16, 16.0f/32
};
static const float bandSampleNorm93[] ={
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
};

// Scaling factor code table.  This maps a DCS scaling factor code
// (as the array index, 0x00..0x3F) to the numerical value of the
// scaling factor.  This is just a pre-computed version of the DCS
// scaling factor formula, so that we don't have to do the
// calculation every time we need the multiplier value for a given
// scaling factor code.
static const int scalingFactors[] ={
    1,  // 00
    1,  // 01
    1,  // 02
    1,  // 03
    2,  // 04
    2,  // 05
    2,  // 06
    3,  // 07
    4,  // 08
    4,  // 09
    5,  // 0a
    6,  // 0b
    8,  // 0c
    9,  // 0d
    11,  // 0e
    13,  // 0f
    16,  // 10
    19,  // 11
    22,  // 12
    26,  // 13
    32,  // 14
    38,  // 15
    45,  // 16
    53,  // 17
    64,  // 18
    76,  // 19
    90,  // 1a
    107,  // 1b
    128,  // 1c
    152,  // 1d
    181,  // 1e
    215,  // 1f
    256,  // 20
    304,  // 21
    362,  // 22
    430,  // 23
    512,  // 24
    608,  // 25
    724,  // 26
    861,  // 27
    1024,  // 28
    1217,  // 29
    1448,  // 2a
    1722,  // 2b
    2048,  // 2c
    2435,  // 2d
    2896,  // 2e
    3444,  // 2f
    4096,  // 30
    4871,  // 31
    5792,  // 32
    6888,  // 33
    8192,  // 34
    9742,  // 35
    11585,  // 36
    13777,  // 37
    16384,  // 38
    19484,  // 39
    23170,  // 3a
    27554,  // 3b
    32768,  // 3c
    38968,  // 3d
    46341,  // 3e
    55109   // 3f
};

DCSEncoder::DCSEncoder()
{
    // build the 9-bit bit-reversed indexing table (for the FFT)
    for (int i = 0 ; i < 512 ; ++i)
    {
        int rev = 0;
        int addr = i;
        for (int bit = 0 ; bit < 9 ; ++bit)
        {
            rev = (rev << 1) | (addr & 1);
            addr >>= 1;
        }
        bitRev9[i] = rev;
    }
}

DCSEncoder::~DCSEncoder()
{
}

DCSEncoder::Stream::Stream(int sampleRate)
{
    // clear the input buffer
    memset(inputBuf, 0, sizeof(inputBuf));

    // clear the power sum
    memset(powerSum, 0, sizeof(powerSum));
    
    // Start with 16 samples already in the buffer, representing the
    // phantom overlap from the non-existent prior frame.
    nInputBuf = 16;

    // Create the libsamplerate state object.  We can afford to use the 
    // slower high quality level, since we don't need to run in real time.
    int err = 0;
    lsrState = src_new(SRC_SINC_BEST_QUALITY, 1, &err);

    // set the libsamplerate sampling rate
    sampleRateRatio = 31250.0 / sampleRate;
    src_set_ratio(lsrState, sampleRateRatio);
}

DCSEncoder::Stream::~Stream()
{
    if (lsrState != nullptr)
        src_delete(lsrState);
}

// Encode a WAV file.  EncodeFile() also reads WAV files, among other formats.
// The reason we provide a separate, dedicated WAV-only reader anyway is that
// this version doesn't have any external dependencies (whereas EncodeFile()
// requires linking libnyquist, to provide readers for the various formats it
// supports).  It's conceivable that some project ocnfigurations might want to
// omit libnyquist (e.g., to minimize build size, or to simplify the build
// process), in which case it might still be nice to have a WAV file reader.
// WAV files are simple enough that we can provide a built-in reader without
// the need for any external dependencies.
bool DCSEncoder::EncodeWAVFile(const char *filename, DCSAudio &dcsObj, std::string &errorMessage)
{
    // open the WAV file
    FILE *fp = nullptr;
    if (int err = fopen_s(&fp, filename, "rb"); err != 0 || fp == nullptr)
    {
        errorMessage = format("Unable to open WAV file \"%s\" (error %d)", filename, err);
        return false;
    }

    // set up to close the WAV file on exit
    std::unique_ptr<FILE, int(*)(FILE*)> ufp(fp, &fclose);

    // read the WAV header
    uint8_t hdr[44];
    if (fread(hdr, sizeof(hdr), 1, fp) != 1)
    {
        errorMessage = format("Unable to read WAV file header for \"%s\" (error %d)", filename, errno);
        return false;
    }

    // do some basic checks to make sure it looks like a valid WAV file
    if (memcmp(&hdr[0], "RIFF", 4) != 0
        || memcmp(&hdr[8], "WAVE", 4) != 0
        || memcmp(&hdr[12], "fmt ", 4) != 0)
    {
        errorMessage = format("File \"%s\" does not appear to be a valid WAV file", filename);
        return false;
    }

    // get the channel count, sample rate, and sample bit width
    auto U2 = [&hdr](int ofs) { return hdr[ofs] | (static_cast<int>(hdr[ofs+1]) << 8); };
    auto U4 = [&hdr, &U2](int ofs) { return U2(ofs) | (U2(ofs+2) << 16); };
    int formatType = U2(20);
    int nChannels = U2(22);
    int sampleRate = U4(24);
    int bitsPerSample = U2(34);
    uint32_t dataSize = U4(40);

    // figure the bytes per sample
    size_t bytesPerSample = nChannels * bitsPerSample / 8;

    // Only allow PCM format
    if (formatType != 1)
    {
        errorMessage = format("WAV file \"%s\" uses an unrecognized format (only uncompressed PCM audio is supported", filename);
        return false;
    }

    // Only allow mono or stereo
    if (nChannels != 1 && nChannels != 2)
    {
        errorMessage = format("WAV file \"%s\" has an unsupported format (%d channels; only stereo or mono are supported)", filename, nChannels);
        return false;
    }

    // Find the DATA chunk
    while (memcmp(&hdr[36], "data", 4) != 0)
    {
        // skip ahead by the chunk size
        if (fseek(fp, dataSize, SEEK_CUR) < 0)
            break;

        // read the next chunk header
        if (fread(&hdr[36], 8, 1, fp) != 1)
            break;

        // get the chunk size
        dataSize = U4(40);
    }

    // make sure we found the data section
    if (memcmp(&hdr[36], "data", 4) != 0)
    {
        errorMessage = format("WAV file \"%s\" has an unsupported format (no DATA chunk found)", filename);
        return false;
    }

    // Set up a reader for the sample type:  reads the next sample, advances
    // the pointer, and decrements the remaining byte count.
    int16_t (*ReadSample)(uint8_t* &, size_t &) = nullptr;
    if (bitsPerSample == 8)
    {
        ReadSample = [](uint8_t* &p, size_t &rem) ->int16_t { 
            rem -= 1;
            return (static_cast<int16_t>(*p++) - 128) * 256;
        };
    }
    else if (bitsPerSample == 16)
    {
        ReadSample = [](uint8_t* &p, size_t &rem) ->int16_t {
            rem -= 2;
            uint16_t sample = p[0] | (static_cast<int16_t>(static_cast<int8_t>(p[1])) << 8);
            p += 2;
            return sample;
        };
    }
    else
    {
        errorMessage = format("WAV file \"%s\" has an unsupported format (%d bits per sample; only 8- and 16-bit samples are supported)", filename, nChannels);
        return false;
    }

    // create a DCS encoder stream
    std::unique_ptr<Stream> stream(OpenStream(sampleRate, errorMessage));
    if (stream == nullptr)
        return false;

    // Process the WAV file contents
    while (dataSize != 0)
    {
        // read a chunk from the file
        uint8_t buf[256];
        size_t n = fread(buf, 1, sizeof(buf), fp);

        // we shouldn't encounter EOF before exhausing the data size from the header
        if (n == 0)
        {
            errorMessage = format("Unexpected end of file reading WAV file \"%s\"", filename);
            return false;
        }

        // deduct this chunk from the remaining data size
        dataSize -= static_cast<uint32_t>(n);

        // process the samples per the format
        int16_t samples[256];
        int nSamples = 0;
        for (uint8_t *p = buf ; n >= bytesPerSample ; )
        {
            // fetch the next sample
            int16_t sample = ReadSample(p, n);

            // if it's a stereo source, read another sample, and average the two
            // channels to form a mono signal for the DCS input
            if (nChannels == 2 && n >= bytesPerSample)
                sample = static_cast<int16_t>(
                    (static_cast<int32_t>(sample) + static_cast<int32_t>(ReadSample(p, n)) + 1)
                    /2);

            // add the sample to the buffer
            samples[nSamples++] = sample;
        }

        // send the samples to the DCS encoder
        WriteStream(stream.get(), samples, nSamples);
    }

    // close the stream
    if (!CloseStream(stream.get(), dcsObj, errorMessage))
        return false;

    // success
    return true;
}

bool DCSEncoder::IsDCSFile(const char *filename, int *formatVersion)
{
    // open the file and check the header to see if it looks like
    // a DCS Explorer-created raw DCS stream file
    FILE *fp = nullptr;
    if (fopen_s(&fp, filename, "rb") != 0 || fp == nullptr)
        return false;

    // set up to close the file when done
    std::unique_ptr<FILE, int(*)(FILE*)> fpHolder(fp, fclose);

    // Check the file header:
    // 
    //  "DCSa"         4 bytes, literal text, file type signature (for "DCS audio")
    //  <fmtVersion>   UINT16, big-endian; 0x9301 = DCS-93a (IJTPA, JD), 
    //                 0x9302 = DCS-93b (STTNG), 0x9400 = DCS-94+, all games
    //                 after the first three DCS-93 titles
    //  <channels>     UINT16, big-endian; number of audio channels, always 0x0001
    //  <rate>         UINT16, big-endian; samples per second, always $7A12 (31250)
    //  <reserved*22>  22 bytes, reserve for future use, currently all zeroes
    //  <dataSize>     UINT32, big-endian, size of data section
    //  <streamData>   Stream data, size given by <dataSize>
    //
    uint8_t hdr[36];
    if (fread(hdr, 1, _countof(hdr), fp) == _countof(hdr)
        && memcmp(&hdr[0], "DCSa", 4) == 0        // signature match
        && (hdr[4] == 0x93 || hdr[4] == 0x94)     // major format 93 or 94
        && (hdr[6] == 0 && hdr[7] == 1)           // number of channels = 1
        && (hdr[8] == 0x7A && hdr[9] == 0x12))    // sample rate = $7A12 = 31250
    {
        // it's a DCS file - if they want to know the format version, return it
        if (formatVersion != nullptr)
            *formatVersion = (static_cast<int>(hdr[4]) << 8) | hdr[5];

        // indicate it's a DCS file
        return true;
    }
    else
    {
        // not a DCS file
        return false;
    }
}

bool DCSEncoder::EncodeDCSFile(const char *filename, DCSAudio &dcsObj,
    std::string &errorMessage, OpenStreamStatus *statusPtr)
{
    // return an error message and status
    auto Status = [&errorMessage, statusPtr](OpenStreamStatus statusVal, const char *msg = nullptr)
    {
        // set the error message text
        if (msg != nullptr)
            errorMessage = msg;

        // set the status, if the caller requested it
        if (statusPtr != nullptr)
            *statusPtr = statusVal;

        // return true on success, false on failure
        return statusVal == OpenStreamStatus::OK;
    };

    // open the file and check the header to see if it looks like
    // a DCS Explorer-created raw DCS stream file
    FILE *fp = nullptr;
    if (fopen_s(&fp, filename, "rb") != 0 || fp == nullptr)
        return Status(OpenStreamStatus::Error, "Unable to open file");

    // set up to close the file when done
    std::unique_ptr<FILE, int(*)(FILE*)> fpHolder(fp, fclose);

    // Read the first few bytes of the file, to check for our DCS Audio header 
    // format:
    // 
    //  "DCSa"         4 bytes, literal text, file type signature (for "DCS audio")
    //  <fmtVersion>   UINT16, big-endian; 0x9301 = DCS-93a (IJTPA, JD), 
    //                 0x9302 = DCS-93b (STTNG), 0x9400 = DCS-94+, all games
    //                 after the first three DCS-93 titles
    //  <channels>     UINT16, big-endian; number of audio channels, always 0x0001
    //  <rate>         UINT16, big-endian; samples per second, always $7A12 (31250)
    //  <reserved*22>  22 bytes, reserve for future use, currently all zeroes
    //  <dataSize>     UINT32, big-endian, size of data section
    //  <streamData>   Stream data, size given by <dataSize>
    // 
    // All integer fields are big-endian, in keeping with the ubiquitous use
    // of big-endian encoding for everything else in the DCS ROMs, which
    // stemmed from the native byte ordering of the ADSP-2105 hardware platform
    // they used.
    uint8_t hdr[36];
    if (fread(hdr, 1, _countof(hdr), fp) == _countof(hdr)
        && memcmp(&hdr[0], "DCSa", 4) == 0        // signature match
        && (hdr[4] == 0x93 || hdr[4] == 0x94)     // major format 93 or 94
        && (hdr[6] == 0 && hdr[7] == 1)           // number of channels = 1
        && (hdr[8] == 0x7A && hdr[9] == 0x12))    // sample rate = $7A12 = 31250
    {
        // It's one of our DCS files - assuming it uses the same format
        // version as the target game, we can copy its encoded stream
        // bytes without any transcoding.   We will have to decode it
        // and re-encode it if it's for a different format version,
        // however.

        // get the number of bytes in the stream data section
        uint32_t nBytes = (static_cast<uint32_t>(hdr[32]) << 24)
            | (static_cast<uint32_t>(hdr[33]) << 16)
            | (static_cast<uint32_t>(hdr[34]) << 8)
            | (static_cast<uint32_t>(hdr[35]) << 0);

        // allocate space
        std::unique_ptr<uint8_t> data(new (std::nothrow) uint8_t[nBytes]);
        if (data == nullptr)
        {
            return Status(OpenStreamStatus::OutOfMemory,
                format("Out of memory loading DCS stream data from %s", filename).c_str());
        }

        // read the stream data
        if (fread(data.get(), 1, nBytes, fp) != nBytes)
        {
            return Status(OpenStreamStatus::Error,
                format("Error reading DCS stream data from %s (error %d)", filename, errno).c_str());
        }

        // get the frame count - it's the first UINT16 in the stream data
        uint16_t nFrames = (static_cast<uint16_t>(data.get()[0]) << 8) | data.get()[1];

        // get the format version
        uint16_t formatVersion = (static_cast<uint16_t>(hdr[4]) << 8) | hdr[5];

        // Get the stream's major type from the header - this is encoded in
        // the high bit of the first header byte.  The header starts at the
        // third byte of the stream data, immediatley following the frame
        // count prefix.
        int streamMajorType = (data.get()[2] & 0x80) >> 7;

        // Check to see if this stream type is compatible with our target
        // format:
        //
        // - If the format versions match exactly, it's compatible.  Each
        //   DCS OS version can decode all of the stream types defined for 
        //   that exact version.
        //
        // - If the stream format version is one of the 1993 versions, and
        //   the target format is the other 1993 versions, and the stream's
        //   major type is Type 0, it's compatible.  The 1993 Type 0 formats
        //   are identical in 1993a and 1993b.
        // 
        // Otherwise, it's incompatible and must be re-encoded in the target
        // format.
        if (formatVersion == compressionParams.formatVersion
            || ((formatVersion & 0xFF00) == 0x9300 
                && (compressionParams.formatVersion & 0xFF00) == 0x9300
                && streamMajorType == 0))
        {
            // It's identical to the target format, so we can use this stream
            // directly, without having to re-encode it in the target format.
            // Simply pass it back to the caller.
            dcsObj.nBytes = nBytes;
            dcsObj.nFrames = nFrames;
            dcsObj.data.reset(data.release());
        }
        else
        {
            // The stream uses an incompatible DCS format.  Set up a universal decoder,
            // and set it to the selected source format version.
            DCSDecoder::MinHost hostifc;
            DCSDecoderNative decoder(&hostifc);

            // figure the DCSDeocder OS version corersponding to the source format
            DCSDecoder::OSVersion sourceOSVer;
            switch (formatVersion)
            {
            case 0x9301:
                sourceOSVer = DCSDecoder::OSVersion::OS93a;
                break;

            case 0x9302:
                sourceOSVer = DCSDecoder::OSVersion::OS93b;
                break;

            case 0x9400:
                sourceOSVer = DCSDecoder::OSVersion::OS94;
                break;

            default:
                return Status(OpenStreamStatus::Error,
                    format("%s uses an unrecognized DCS stream format version (%04x)",
                        filename, formatVersion).c_str());
            }

            // initialize the decoder in stand-alone mode (no ROMs loaded)
            decoder.InitStandalone(sourceOSVer);
            decoder.SoftBoot();

            // load the stream into the decoder
            DCSDecoder::ROMPointer rp(0, data.get());
            decoder.LoadAudioStream(0, rp, 0xFF);

            // create a DCS encoder stream for the new stream
            std::unique_ptr<Stream> stream(OpenStream(31250, errorMessage, statusPtr));
            if (stream == nullptr)
                return Status(OpenStreamStatus::Error);

            // Decode all of the frames and send them to the stream.  Add one extra
            // frame to make sure we fade to silence after the last source frame.
            for (uint16_t frame = 0 ; frame < nFrames + 1 ; ++frame)
            {
                // decode a frame from the source (always 240 samples)
                int16_t buf[240];
                for (int s = 0 ; s < 240 ; ++s)
                    buf[s] = decoder.GetNextSample();

                // send the samples to the encoder stream
                WriteStream(stream.get(), buf, 240);
            }

            // close the stream
            if (!CloseStream(stream.get(), dcsObj, errorMessage))
            {
                if (statusPtr != nullptr) *statusPtr = OpenStreamStatus::Error;
                return Status(OpenStreamStatus::Error);
            }
        }

        // success
        return Status(OpenStreamStatus::OK);
    }

    // not a DCS file
    return Status(OpenStreamStatus::UnsupportedFormat, "Not a raw DCS stream file");
}


DCSEncoder::Stream *DCSEncoder::OpenStream(int sampleRate, std::string &errorMessage, OpenStreamStatus *statusPtr)
{
    // return a failure code
    auto Error = [&errorMessage, statusPtr](OpenStreamStatus statusVal, const char *msg)
    {
        // pass back the status code if it was requested
        if (statusPtr != nullptr)
            *statusPtr = statusVal;

        // set the error text
        errorMessage = msg;

        // return null to indicate failure
        return nullptr;
    };

    // create a new stream
    std::unique_ptr<Stream> stream(new (std::nothrow) Stream(sampleRate));
    if (stream == nullptr)
        return Error(OpenStreamStatus::OutOfMemory, "Out of memory");

    // we can't proceed if we couldn't create a libsamplerate context
    if (stream->lsrState == nullptr)
        return Error(OpenStreamStatus::LibSampleRateError, "Error creating libsamplerate context");

    // indicate success and release the stream object to the caller's custody
    if (statusPtr != nullptr) *statusPtr = OpenStreamStatus::OK;
    return stream.release();
}

void DCSEncoder::WriteStream(Stream *stream, const int16_t *pcm, size_t numSamples)
{
    WriteStream(stream, pcm, numSamples, false);
}

void DCSEncoder::WriteStream(Stream *stream, const float *pcm, size_t numSamples)
{
    WriteStream(stream, pcm, numSamples, false);
}

void DCSEncoder::WriteStream(Stream *stream, const int16_t *pcm, size_t numSamples, bool eof)
{
    // convert to float in batches
    float floatBuf[256];
    while (numSamples != 0)
    {
        // convert a batch of samples to float
        size_t cur;
        for (cur = 0 ; cur < _countof(floatBuf) && numSamples != 0 ; ++cur, --numSamples)
            floatBuf[cur++] = static_cast<float>(*pcm++) / 32768.0f;

        // write the batch
        WriteStream(stream, floatBuf, cur, false);
    }

    // write the EOF marker if desired
    if (eof)
        WriteStream(stream, floatBuf, 0, true);
}

void DCSEncoder::WriteStream(Stream *stream, const float *pcm, size_t numSamples, bool eof)
{
    // ignore if libsamplerate isn't working
    if (stream == nullptr)
        return;

    // process the samples
    while (numSamples != 0 || eof)
    {
        // set up the libsamplerate input buffer
        float inbuf[16];
        SRC_DATA d;
        d.data_in = inbuf;
        d.src_ratio = stream->sampleRateRatio;

        // add as many samples as we have remaining, or up to the block size
        size_t curSize = numSamples < _countof(inbuf) ? numSamples : _countof(inbuf);
        d.input_frames = static_cast<long>(curSize);
        for (size_t i = 0 ; i < curSize ; ++i)
            inbuf[i] = *pcm++;

        // consume the samples
        numSamples -= curSize;

        // set the lsr input buffer EOF flag if there are no more samples
        // AND the caller told us we're at EOF
        d.end_of_input = (numSamples == 0 && eof);

        // if we've reached the last sample, this consumes our EOF flag
        if (numSamples == 0)
            eof = false;

        // set up to write to our temporary buffer
        float outbuf[512];
        d.data_out = outbuf;
        d.output_frames = static_cast<long>(_countof(outbuf));

        // process the samples
        d.output_frames_gen = 0;
        d.input_frames_used = 0;
        if (src_process(stream->lsrState, &d) != 0)
            return;

        // buffer the output
        float *p = outbuf;
        long outCnt = d.output_frames_gen;
        for (long i = 0; i < outCnt; ++i)
        {
            // add it to the input buffer
            stream->inputBuf[stream->nInputBuf++] = *p++;

            // If we have one full frame (256 samples), transform it
            // to the frequency domain and add it to our frame list
            if (stream->nInputBuf == 256)
                TransformFrame(stream);
        }
    }
}

// scaling factor pre-adjustment maps for stream subtypes 0 and 3
static const uint16_t preAdjMap0[16] ={
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};
static const uint16_t preAdjMap3[16] ={
   0, 0, 0, 0, 1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4
};

bool DCSEncoder::CloseStream(Stream *stream, DCSAudio &obj, std::string &errorMessage)
{
    // Flush any partially processed samples still pending in the
    // libsamplerate context by writing a final zero-length input
    // buffer with the EOF flag set.
    WriteStream(stream, static_cast<const float *>(nullptr), 0, true);

    // If we have any samples buffered in a partial final input
    // frame, fill out the remainder of the frame with silence, 
    // and encode the frame.  Note that the buffer always starts
    // with 16 samples, representing the overlap with the prior
    // frame, so a frame with 16 samples is effectively empty.
    if (stream->nInputBuf != 16)
    {
        // fill out the rest of the frame with zeroes
        while (stream->nInputBuf < 256)
            stream->inputBuf[stream->nInputBuf++] = 0;

        // transform the frame into frequency-domain samples
        TransformFrame(stream);
    }

    // get the normalization table for the format version
    const float *bandSampleNorm = ((compressionParams.formatVersion & 0xFF00) == 0x9300) ?
        bandSampleNorm93 : bandSampleNorm94;

    // Normalize the power sums by band size, and compute RMS
    float rmsPower[16], totalPower = 0.0f;
    for (int i = 0 ; i < 16 ; ++i)
    {
        rmsPower[i] = sqrtf(stream->powerSum[i] * bandSampleNorm[i]);
        totalPower += rmsPower[i];
    }

    // Figure the cumulative power starting with the lowest
    // frequencies
    float powerNorm = 1.0f/totalPower;
    int bandsToKeep = 16;
    if (totalPower != 0.0f)
    {
        float powerBelow = 0.0f;
        for (int i = 0 ; i < 16 ; ++i)
        {
            // calculate the cumulative power up to this band
            powerBelow += rmsPower[i] * powerNorm;

            // if we've reached the total power cutoff point, discard
            // the higher-frequency bands
            if (powerBelow >= compressionParams.powerBandCutoff)
            {
                bandsToKeep = i;
                break;
            }
        }
    }

    // get the desired stream type
    int desiredStreamType = compressionParams.streamFormatType;
    int desiredStreamSubType = compressionParams.streamFormatSubType;

    // For OS93, there's no such thing as a subtype, so we arbitrarily
    // label the one possibility "subtype 0" internally (we only need
    // a label at all so that we have something to match in the loop
    // below that selects which formats to consider).
    if ((compressionParams.formatVersion & 0xFF00) == 0x9300)
        desiredStreamSubType = 0;

    // Run through the possible stream formats
    static const struct { int major; int minor; } formats[] ={
        { 0, 0 }, 
        { 0, 3 }, 
        { 1, 0 }, 
        { 1, 3 } 
    };
    BitWriter bitWriter[_countof(formats)];
    int bestIdx = -1;
    size_t bestSize = 0;
    for (size_t i = 0 ; i < _countof(formats) ; ++i)
    {
        // The OS93 formats don't have any sub-format variations, so we only need 
        // to try each major format once.  Arbitrarily choose subformat 0 as the
        // one to try.  (It doesn't matter which, as long as we try only one,
        // since the OS93 compressor algorithms ignore the subtype selection.
        // It wouldn't even do any harm to try every alternative, but doing so is
        // a waste of time and memory, since every subtype for a given OS93 major
        // type will yield an identical stream.)
        auto &f = formats[i];
        if ((compressionParams.formatVersion & 0xFF00) == 0x9300 && f.minor != 0)
            continue;

        // We don't currently provide an encoder for OS93a Type 1, so don't
        // try it on a wildcard match.  If it was explicitly selected, though,
        // go ahead and try it, so that we generate a specific error explaining
        // that it's not supported (rather than failing with the vague "no
        // matching format found")
        if (compressionParams.formatVersion == 0x9301 && f.major == 1
            && desiredStreamType < 0)
            continue;

        // Check for a match to this format in the compression parameters.
        // A negative value in the parameters is a wildcard that matches any
        // format; a non-negative value is a definite format that must match
        // exactly.
        if ((desiredStreamType < 0 || desiredStreamType == f.major)
            && (desiredStreamSubType < 0 || desiredStreamSubType == f.minor))
        {
            // Try compressing with the caller's parameters, modified to
            // specify this exact stream type and subtype (in case these
            // were specified as wildcards).
            auto &bw = bitWriter[i];
            bw.params = compressionParams;
            auto &params = bw.params;
            params.streamFormatType = f.major;
            params.streamFormatSubType = f.minor;
            if (!CompressStream(stream, bitWriter[i], bandsToKeep, errorMessage))
                return false;

            // note if this is the best (or only) result so far
            size_t size = bitWriter[i].CalcStreamSize();
            if (bestIdx < 0 || size < bestSize)
            {
                bestIdx = static_cast<int>(i);
                bestSize = size;
            }
        }
    }

    // If we didn't find any matching formats, fail
    if (bestIdx < 0)
    {
        errorMessage = "No available stream format types match the requested encoding parameters";
        return false;
    }

    // convert the winner to a DCS object
    if (!bitWriter[bestIdx].Store(obj, static_cast<int>(stream->frames.size()), errorMessage))
        return false;

    // success
    return true;
}

bool DCSEncoder::CompressStream(Stream *stream, BitWriter &bitWriter,
    int bandsToKeep, std::string &errorMessage)
{
    // get the parameters from the bit writer
    auto &params = bitWriter.params;

    // get the band sizes (samples per band) according to the format
    const uint16_t *bandSampleCounts =
        (params.formatVersion == 0x9400) ? bandSampleCounts94 : 
        (params.streamFormatType == 1) ? bandSampleCounts93b_Type1 : bandSampleCounts93;

    // Figure the initial bit allocation for each frequency band,
    // according to the target bit rate.  First, figure how many
    // bits per frame we have to work with given the DCS frame
    // size and rate (31250 PCM samples per second arranged in
    // frames of 240 samples = 130 frames per second, with 255
    // frequency-domain samples per frame).
    const float framesPerSecond = 31250.0f/240.0f;
    const float bitsPerFrame = static_cast<float>(params.targetBitRate) / framesPerSecond;

    // Allocate bits to bands with a simple psychoacoustic model
    // that assigns highest priority to the low-frequency band,
    // dropping off with increasing frequency on a roughly log
    // curve.  First figure a normalization factor based on the
    // curve and the samples per band.
    static const int bandShare[16] ={ 16, 14, 12, 10, 9, 8, 6, 5, 4, 4, 3, 3, 3, 3, 2, 2 };
    float bandShareNorm = 0;
    for (int i = 0 ; i < bandsToKeep ; ++i)
        bandShareNorm += static_cast<float>(bandShare[i] * bandSampleCounts[i]);

    // now allocate the bits per band
    int bitsPerBand[16];
    for (int i = 0 ; i < bandsToKeep ; ++i)
    {
        bitsPerBand[i] = static_cast<int>(
            static_cast<float>(bandShare[i]) / bandShareNorm * bitsPerFrame);
    }

    // Now figure the scaling factor per band, by figuring the
    // range of values we have to represent in each band and the
    // target number of bits we want to use per sample in the
    // band.  The target number of bits determines the range of
    // the encoded int values, so the scaling factor is the value
    // that multiplies the extremes of the N-bit integer range to
    // yield the extremes of the actual sample range.
    for (int band = 0 ; band < bandsToKeep ; ++band)
    {
        // get the full-scale range for the band
        float lo = stream->range[band].lo * -32768.0f;
        float hi = stream->range[band].hi * 32768.0f;
        if (lo < 0) lo = 0;
        if (hi < 0) hi = 0;
        float fullScale = hi > lo ? hi : lo;

        // figure the target scaling factor
        int divider = 1 << bitsPerBand[band];
        int target = fullScale != 0 ? static_cast<int>(ceil(fullScale / divider)) : 1;

        // Now find the entry in the scaling factor code table that
        // comes closest from above.  We need a factor equal to or
        // greater than the target, since a smaller factor wouldn't
        // let us recover the highest/lowest value.
        bitWriter.header[band] = 0;
        for (int j = 0 ; j < static_cast<int>(_countof(scalingFactors)) ; ++j)
        {
            int f = scalingFactors[j];
            if (f < target)
                bitWriter.header[band] = j;
            else
                break;
        }

        // For OS94 major format type 1, we adjust the scaling in every band
        // according to the band type code translation tables.  This doesn't
        // apply to any OS93 formats.
        if (params.formatVersion == 0x9400 && params.streamFormatType == 1)
        {
            // Figure the adjustment by band.  The actual adjustment varies
            // according to the encoding type chosen.  Since we're basing the
            // default scaling factor on the 6-bit encoding, use the inverse
            // adjustment for the first 6-bit encoding in the Format Type 1
            // table for the band.
            int adjust = (band < 3) ? 0x0d : 0x17;

            // The scaling code for bands 0-2 is further adjusted according 
            // to the pre-adjustment table selected by the stream sub-format.  
            // The adjustment in each frame is based on the prior frame's
            // band type code, which can vary in every frame, so it's not
            // uniform.  Pick a typical value.
            adjust += params.streamFormatSubType == 0 ? 1 : 3;

            // apply the adjustment, but don't go below zero
            if (bitWriter.header[band] > adjust)
                bitWriter.header[band] -= adjust;
            else
                bitWriter.header[band] = 0;
        }
    }

    // fill in 0xFF codes in the unused bands
    for (int band = bandsToKeep ; band < 16 ; ++band)
        bitWriter.header[band] = 0xFF;

    // set bit 0x80 in the first byte if we're using major type 1
    if (params.streamFormatType != 0)
        bitWriter.header[0] |= 0x80;

    // set bit 0x80 in bytes [1] and [2] according to the subtype
    bitWriter.header[1] |= (params.streamFormatSubType & 0x02) << 6;
    bitWriter.header[2] |= (params.streamFormatSubType & 0x01) << 7;

    // Select the encoding algorithm:
    //
    // - For OS93a Type 1 frames, use the 93a compressor
    // 
    // - For OS93a Type 0 frames and all OS93b frames, use the 93b 
    //   compressor (OS93a and OS93b Type 0 are identical, and OS93b
    //   Type 0 and Type 1 are similar enough to handle together)
    // 
    // - For all others, use the 94 compressor
    //
    auto compressFrame = &DCSEncoder::CompressFrame94;
    if (params.formatVersion == 0x9301 && params.streamFormatType == 1)
        compressFrame = &DCSEncoder::CompressFrame93a;
    else if ((params.formatVersion & 0xFF00) == 0x9300)
        compressFrame = &DCSEncoder::CompressFrame93b;

    // Compress all of the frames into the bit stream
    int frameNo = 0;
    for (auto &frame : stream->frames)
    {
        if (!(this->*compressFrame)(bitWriter, frameNo++, frame, errorMessage))
            return false;
    }

    // flush the bit writer
    bitWriter.Flush();

    // success
    return true;
}

void DCSEncoder::TransformFrame(Stream *stream)
{
    // save the last 16 samples for the overlap in the next frame
    float overlapBuf[16];
    for (int i = 0 ; i < 16 ; ++i)
        overlapBuf[i] = stream->inputBuf[240+i];

    // Apply the window function to the first and last 16 samples
    static const float windowFunc[] ={
        0.010179f, 0.040507f, 0.090368f, 0.158746f, 0.244250f, 0.345139f, 0.459359f, 0.584585f,
        0.647178f, 0.752018f, 0.829799f, 0.888221f, 0.932184f, 0.964581f, 0.986700f, 0.998439f
    };
    for (int i = 0 ; i < 16 ; ++i)
    {
        stream->inputBuf[i] *= windowFunc[i];
        stream->inputBuf[255 - i] *= windowFunc[i];
    }

    // Apply the Fourier transform.
    //
    // In principle, I prefer DFTAlgorithmNew() to Orig(), because the
    // "new" algorithm is a lot simpler and clearer.  However, it seems
    // to have some very tiny differences numerically that add enough
    // noise/distortion to be noticeable.  So, for now, I'm using the
    // "original" algorithm, since that seems to produce the best results.
    // We *should* be able to replace this with any equivalent DFT math,
    // but there might be some subtle assumptions that I haven't figure
    // out yet in the original algorithm - probably something about the
    // values at the edges, at samples [0], [1], [0x80], [0x81], [0xfe],
    // and [0xff], and the way those affect nearby samples.  The
    // original algorithm is very sensitive to getting those exactly
    // right, so maybe the simplified generic algorithm is missing
    // something at the edges.
    float fbuf[0x102];
    DFTAlgorithmOrig(fbuf, stream);

    // For all of the frame formats except 1993a Type 1, sample [1]
    // is omitted from the frame.  This sample is inherently always
    // zero by the way the transform is defined (it's the sine(0*n)
    // sum, so every coefficient in the sum is zero), so it saves a
    // tiny bit of memory to omit it.
    fbuf[1] = fbuf[0];

    // fbuf[1..255] now contains the DCS frame in uncompressed format.
    // Save it in the frame list for compression when we construct the
    // final output stream.
    auto &frame = stream->frames.emplace_back(&fbuf[1], compressionParams);

    // Copy the overlap buffer (the last 16 samples of the current
    // frame, before applying the overlap coefficients) to the start of
    // the next frame
    stream->nInputBuf = 16;
    for (int i = 0 ; i < 16 ; ++i)
        stream->inputBuf[i] = overlapBuf[i];

    // save the power sums and high/low extremes
    bool isFirstFrame = stream->frames.size() == 1;
    for (int i = 0 ; i < 16 ; ++i)
    {
        stream->powerSum[i] += frame.power[i];
        if (isFirstFrame || frame.range[i].lo < stream->range[i].lo)
            stream->range[i].lo = frame.range[i].lo;
        if (isFirstFrame || frame.range[i].hi > stream->range[i].hi)
            stream->range[i].hi = frame.range[i].hi;
    }
}

// Simplified DFT algorithm.  This computes the real DFT by treating
// the PCM sample array as a collection of 128 complex numbers
// arranged in the array in real/imaginary pairs.  We compute the
// 128-point complex DFT of this set, then use the "split" algorithm
// to separate the sine and cosine sums, obtaining the 256 real
// number components of the DFT of the real inputs.
//
// A complex DFT has the form
//
//    X[k] = sum over N of x[n] * exp(2*PI*j*k*n/N)  
//    (j = imaginary unit = square root of -1)
//
// Using Euler's formula, this decomposes into
//
//    X[k] = sum over N of x[n]*cos(2*PI*k*n/N) - j*x[n]*sin(2*PI*k*n/N)
//         = X_real[k] - j*X_imaginary[k]
//
// For DCS purposes, we're after the X_real[k] and X_imaginary[k]
// values, which are real numbers.  There are 128 of each, making
// a set of 256 real numbers.
// 
// When all of the x[n] values are real, the correspondence is
// straightforward.  We can compute a 256-point complex DFT and
// pull out the real and imaginary parts of the first 128 outputs.
// (The second 128 outputs are complex conjugates of the first 128,
// so they're purely redundant and can be discarded.)
// 
// A more efficient approach is to perform a 128-point complex
// DFT, treating the 256 real inputs as though they were 128
// complex numbers arranged in real/imaginary pairs.  In this
// case, the sine and cosine components of the 256-point DFT can
// be extracted from the 128-point DFT results by exploiting
// symmetries.  The math can be found in numerous references.
//
void DCSEncoder::DFTAlgorithmNew(float outbuf[258], Stream *stream)
{
    // Load the bit-reversed inputs
    double buf[258];
    const float *pInput = stream->inputBuf;
    for (int i = 0 ; i < 256 ; i += 2)
    {
        // figure the bit-reversed destination address
        int bi = bitRev9[i];

        // load the next real/imaginary pair
        buf[bi++] = *pInput++;
        buf[bi++] = *pInput++;
    }

    // pre-calculate the DFT coefficients
    static bool coeffInited = false;
    static double coeff[896];
    if (!coeffInited)
    {
        coeffInited = true;
        double *cp = coeff;
        for (int s = 1 ; s <= 7 ; ++s)
        {
            int m = (1 << s);
            for (int k = 0 ; k < 128 ; k += m)
            {
                for (int j = 0 ; j < m/2 ; ++j)
                {
                    double theta = -2.0f*PI*static_cast<double>(j)/static_cast<double>(m);
                    *cp++ = cos(theta);
                    *cp++ = sin(theta);
                }
            }
        }
    }

    // Perform a 128-point complex DFT using Cooley-Tukey
    const double *pCoeff = coeff;
    for (int s = 1 ; s <= 7 ; ++s)
    {
        int m = (1 << s);
        for (int k = 0, kn = 0 ; k < 128 ; k += m, ++kn)
        {
            for (int j = 0 ; j < m/2 ; ++j)
            {
                // get the coefficients from the precomputed table
                // theta = -2*PI*static_cast<float>(j)/static_cast<float>(m);
                double costh = *pCoeff++;
                double sinth = *pCoeff++;

                // get a
                int tIdx = (k + j + m/2)*2;
                double ar = buf[tIdx];
                double ai = buf[tIdx + 1];

                // figure t
                double tr = ar*costh - ai*sinth;
                double ti = ar*sinth + ai*costh;

                // get u
                int uIdx = (k + j)*2;
                double ur = buf[uIdx];
                double ui = buf[uIdx + 1];

                // store u'
                buf[uIdx] = tr + ur;
                buf[uIdx+1] = ti + ui;

                // store t'
                buf[tIdx] = ur - tr;
                buf[tIdx+1] = ui - ti;
            }
        }
    }

    // Initialize the split algorithm coefficients.  These depend only
    // on the loop index in the loop below, so we can pre-compute them
    // and store them in a static array for fast lookup.
    static bool inited = false;
    static double Ai[128], Ar[128], Bi[128], Br[128];
    if (!inited)
    {
        inited = true;
        for (int k = 0 ; k < 128 ; ++k)
        {
            double th = 3.14159265358979323846 * k / 128;
            Ai[k] = -cos(th);
            Ar[k] = 1.0 - sin(th);
            Bi[k] = -Ai[k];
            Br[k] = 1.0 + sin(th);
        }
    }

    // Split the real and imaginary parts of the complex DFT to
    // recover the real DFT coefficients.  Apply normalization at
    // the same time.
    buf[256] = buf[0];
    buf[257] = buf[1];
    buf[128] += buf[129];
    buf[129] = 0;
    buf[0] += buf[128];
    for (int k = 0 ; k < 128 ; ++k)
    {
        const double fnorm = -1.0/512.0;
        int kr = k*2, ki = kr + 1;
        outbuf[kr] = static_cast<float>((buf[kr]*Ar[k] - buf[ki]*Ai[k] + buf[256-kr]*Br[k] + buf[258-ki]*Bi[k]) * fnorm);
        outbuf[ki] = static_cast<float>((buf[ki]*Ar[k] + buf[kr]*Ai[k] + buf[256-kr]*Bi[k] - buf[258-ki]*Br[k]) * fnorm);
    }
    outbuf[129] = 0;
    outbuf[256] = 0;
    outbuf[257] = 0;
}

// "Original flavor" DFT algorithm.  This is an inversion of the
// decoding transform used in the original ADSP-2105 decoders.
void DCSEncoder::DFTAlgorithmOrig(float fbuf[258], Stream *stream)
{
    // Apply the "dual half" Fourier transform:
    // 
    //   fbuf[0..127]   = FFT(x0, 0, x2, 0, x4, 0, ..., x126, 0)
    //   fbuf[128..255] = FFT(0, x1, 0, x3, 0, x5, ..., 0, x127)
    //
    DualFFT(fbuf, stream->inputBuf);

    // The original ADSP-2105 decoder algorithm makes a hard
    // assumption that element [1] is always zero in the sample
    // buffer immediately after decompression.  We need it to
    // be zero POST-twiddling, but we can't just zero it by
    // fiat after the twiddling steps, because it's an input to
    // the twiddling that affects dependent values.  We thus
    // have to set the value now, as input to the twiddler, to
    // the input value that yields zero at the end of the
    // twiddling.  (The twiddling is reversible and linear, so
    // this just amounts to solving the algebra for a zero
    // output value.)
    //
    // Note that element [1] in the underling RDFT is the 
    // sine(0) sum, so it's guaranteed to be zero if we were
    // to calculate the RDFT directly.
    fbuf[0x1] = (fbuf[0x0] + fbuf[0x80])/2.0f;

    // Element [0x81] in the final twiddler output is also
    // always zero in the original recordings.  Unlike element
    // [1], this doesn't seem to be an assumption that's
    // embedded in the algorithm itself, but it nonetheless seems
    // to always be true observationally.  So perhaps it's just
    // an artifcat of the original encoder algorithm that isn't
    // important to preserve, but I'm going to preserve it anyway
    // since it always seems to hold for the original DCS ROM
    // data.  As with [0x1], this value affects other outputs
    // from the twiddler, so we have to solve for the right value
    // to feed in here to get the desired output.
    fbuf[0x81] = fbuf[0x1];

    // Likewise for elements [0x100] and [0x101].  Note that
    // these form a phantom element at the start of the next
    // frame, that's only needed to make the loop bounds simpler
    // to calculate.
    fbuf[0x100] = fbuf[0x1];
    fbuf[0x101] = fbuf[0x1];

    // even/odd folding
    {
        float *p0 = fbuf;
        float *p1 = fbuf + 0x80;
        for (int i = 0 ; i < 0x0040 ; ++i)
        {
            float x0 = p0[0];
            float y0 = p0[1];
            float x1 = p1[0];
            float y1 = p1[1];

            *p0++ = (x0 + x1)/2.0f;
            *p0++ = (y0 + y1)/2.0f;

            *p1++ = (x0 - x1)/2.0f;
            *p1++ = (y0 - y1)/2.0f;
        }
    }

    // twiddling
    {
        static const float twiddleCoefficients[128] ={
            -1.0000000f, 0.0000000f, -0.9996948f, -0.0245361f, -0.9988098f, -0.0490723f, -0.9972839f, -0.0735779f,
            -0.9951782f, -0.0980225f, -0.9924927f, -0.1224060f, -0.9891663f, -0.1467285f, -0.9852905f, -0.1709595f,
            -0.9807739f, -0.1950989f, -0.9757080f, -0.2191162f, -0.9700317f, -0.2429810f, -0.9637756f, -0.2667236f,
            -0.9569397f, -0.2902832f, -0.9495239f, -0.3136902f, -0.9415588f, -0.3368835f, -0.9329834f, -0.3598938f,
            -0.9238892f, -0.3826904f, -0.9142151f, -0.4052429f, -0.9039917f, -0.4275513f, -0.8932190f, -0.4496155f,
            -0.8819275f, -0.4714050f, -0.8700867f, -0.4928894f, -0.8577271f, -0.5140991f, -0.8448486f, -0.5350037f,
            -0.8314819f, -0.5555725f, -0.8175964f, -0.5758057f, -0.8032227f, -0.5957031f, -0.7883606f, -0.6152344f,
            -0.7730103f, -0.6343994f, -0.7572021f, -0.6531677f, -0.7409363f, -0.6715698f, -0.7242432f, -0.6895447f,
            -0.7070923f, -0.7070923f, -0.6895447f, -0.7242432f, -0.6715698f, -0.7409363f, -0.6531677f, -0.7572021f,
            -0.6343994f, -0.7730103f, -0.6152344f, -0.7883606f, -0.5957031f, -0.8032227f, -0.5758057f, -0.8175964f,
            -0.5555725f, -0.8314819f, -0.5350037f, -0.8448486f, -0.5140991f, -0.8577271f, -0.4928894f, -0.8700867f,
            -0.4714050f, -0.8819275f, -0.4496155f, -0.8932190f, -0.4275513f, -0.9039917f, -0.4052429f, -0.9142151f,
            -0.3826904f, -0.9238892f, -0.3598938f, -0.9329834f, -0.3368835f, -0.9415588f, -0.3136902f, -0.9495239f,
            -0.2902832f, -0.9569397f, -0.2667236f, -0.9637756f, -0.2429810f, -0.9700317f, -0.2191162f, -0.9757080f,
            -0.1950989f, -0.9807739f, -0.1709595f, -0.9852905f, -0.1467285f, -0.9891663f, -0.1224060f, -0.9924927f,
            -0.0980225f, -0.9951782f, -0.0735779f, -0.9972839f, -0.0490723f, -0.9988098f, -0.0245361f, -0.9996948f
        };
        const float *pTwiddle = twiddleCoefficients;
        float *p0 = fbuf;
        float *p1 = fbuf + 0x100;
        for (int i = 0 ; i < 0x0040 ; ++i, p0 += 2, p1 -= 2)
        {
            // a0 = x0,y0 = buf[i*2]
            // a1 = x1,y1 = buf[256 - i*2]
            float x0 = p0[0];
            float y0 = p0[1];
            float x1 = p1[0];
            float y1 = p1[1];

            // sum = (a0 - a1*)/2  (a1* = complex conjugate of a1)
            float xsum = (x0 - x1)/2.0f;
            float ysum = (y0 + y1)/2.0f;

            // float theta = 2*PI*(i - 128.0f)/256
            auto costh = *pTwiddle++;
            auto sinth = *pTwiddle++;

            // buf[i*2] = (a0 + a1*)/2  (a1* = complex conjugate)
            p0[0] = (x0 + x1)/2.0f;
            p0[1] = (y0 - y1)/2.0f;

            // buf[256 - i*2] = (a0 - a1*)/2 * exp(i*theta)
            p1[0] = xsum*sinth - ysum*costh;
            p1[1] = xsum*costh + ysum*sinth;
        }
    }

    // high/low folding
    {
        float *p0 = fbuf;
        float *p1 = fbuf + 0x100;
        for (int i = 0 ; i < 0x0040 ; ++i, p0 += 2, p1 -= 2)
        {
            float x0 = -p0[0];
            float y0 = -p0[1];

            float x1 = -p1[0];
            float y1 = -p1[1];

            p0[0] = (x0 + x1)/2.0f;
            p0[1] = (y0 + y1)/2.0f;

            p1[0] = (x0 - x1)/2.0f;
            p1[1] = (y0 - y1)/2.0f;
        }
        fbuf[0x80] = -fbuf[0x80];
        fbuf[0x81] = -fbuf[0x81];
    }

    // Fix the signs on the second-half odd elements
    for (int i = 129 ; i < 256 ; i += 2)
        fbuf[i] = -fbuf[i];
}

void DCSEncoder::DualFFT(float buf[256], const float inbuf[256])
{
    // Load the inputs
    for (int idx = 0, i = 0 ; i < 128 ; ++i)
    {
        // figure the bit-reversed destination address
        int bi = bitRev9[idx];

        // load the next real/imaginary pair
        buf[bi++] = inbuf[idx++];
        buf[bi++] = inbuf[idx++];
    }

    // The first time through, calculate the FFT coefficients and store them in
    // a static array.  Each coefficient is a function only of the loop indices,
    // so they're the same every time through the loop.  Note that we have to
    // calculate the coefficients for the 7th outer loop iteration even though 
    // we only run the loop 6 times, because we do a sort of half loop pass in
    // an additional post-processing step, where we use the 7th-iteration
    // coefficients.
    //
    // The value we call theta is the real value theta in the Euler formula for a 
    // complex exponent, exp(i*theta) = i*cos(theta) + sin(theta).  The Cooley-
    // Tukey FFT coefficient w[j,m] = exp(-2*pi*i*j/m), so theta = -2*pi*j/m.
    static float coeff[896];
    static bool coeffInited = false;
    if (!coeffInited)
    {
        coeffInited = true;
        float *pCoeff = coeff;
        for (int s = 1 ; s <= 7 ; ++s)
        {
            int m = (1 << s);
            for (int k = 0 ; k < 128 ; k += m)
            {
                for (int j = 0 ; j < m/2 ; ++j)
                {
                    float theta = -2*PI*static_cast<float>(j)/static_cast<float>(m);
                    *pCoeff++ = cosf(theta);
                    *pCoeff++ = sinf(theta);
                }
            }
        }
    }

    // Calculate the FFT, using the Cooley-Tukey iterative in-place
    // algorithm.  C-T is a divide-and-conquer algorithm that divides
    // the set into a series of power-of-two partitions and calculates 
    // the transform on each of the partitions separately, then merges
    // the results to obtain the transform of the combined set.  The
    // outermost partition is the two halves of the full set, so by
    // stopping one iteration short, we end with separate FFTs on the
    // first half and second half of the set.
    const float *pCoeff = coeff;
    for (int s = 1 ; s <= 6 ; ++s)
    {
        int m = (1 << s);
        for (int k = 0, kn = 0 ; k < 128 ; k += m, ++kn)
        {
            for (int j = 0 ; j < m/2 ; ++j)
            {
                // get the coefficients from the precomputed table
                // theta = -2*PI*static_cast<float>(j)/static_cast<float>(m);
                float costh = *pCoeff++;
                float sinth = *pCoeff++;

                // get a
                int tIdx = (k + j + m/2)*2;
                float ar = buf[tIdx];
                float ai = buf[tIdx + 1];

                // figure t
                float tr = ar*costh - ai*sinth;
                float ti = ar*sinth + ai*costh;

                // get u
                int uIdx = (k + j)*2;
                float ur = buf[uIdx];
                float ui = buf[uIdx + 1];

                // store u'
                buf[uIdx] = tr + ur;
                buf[uIdx+1] = ti + ui;

                // store t'
                buf[tIdx] = ur - tr;
                buf[tIdx+1] = ui - ti;
            }
        }
    }

    // Adjust the odd elements.  Right now, we have the FFT of each half
    // of the input set.  The sets are arranged as though they were of the
    // full size, with elements at every other even-numbered index, and
    // the odd-numbered indices unpopulated.  The final step, which we
    // skipped in the loop above, merges the odd-numbered slots back into 
    // the overall set.
    //
    // For DCS purposes, though, we don't want the merged set.  Instead,
    // we want the separate DFTs of the even-numbered and odd-numbered
    // inputs.  The first half of buf[] already contains the even-numbered
    // set in exactly the form we need.  The second half of buf[] contains
    // the odd-numbered inputs, but they're multiplied by the even-numbered
    // coefficients, because of C-T's divide-and-conquer approach.  We want
    // to apply the odd-numbered coefficients instead, which we can do with
    // one final pass.
    pCoeff = coeff + _countof(coeff) - 63*2;
    for (int j = 1 ; j < 64 ; ++j)
    {
        // float theta = -2*PI*static_cast<float>(j)/128.0f;
        float costh = *pCoeff++;
        float sinth = *pCoeff++;

        // get t
        int tIdx = 128 + j*2;
        float ar = buf[tIdx];
        float ai = buf[tIdx + 1];

        // figure and store t'
        buf[tIdx] = ar*costh - ai*sinth;
        buf[tIdx+1] = ar*sinth + ai*costh;
    }

    // Normalize the outputs.  Note that most textbook DFT treatments use
    // convention that the *inverse* transform is normalized to 1/N (N is
    // the number of complex element), but that's arbitrary; the forward
    // and reverse normalization factors just have to multiply to 1/N.
    // We have to use 1/N normalization on the forward transform because
    // that's just the way DCS is defined.  Note also that the factor is
    // 1/64: it's not 1/256 (the array size) because N is the number of
    // *complex* elements (we represent each complex element as a pair of
    // array entries giving the real and imaginary components), and it's
    // not 1/128 (the number of complex elements in the array) because
    // we're actually performing two separate DFTs here, each one
    // operating on half of the array, so our 256-real-number array
    // really represents two sub-arrays of 64 complex numbers each.
    const float fnorm = 1/64.0f;
    for (int i = 0 ; i < 256 ; ++i)
        buf[i] *= fnorm;
}

// Find the best compression option for a band
DCSEncoder::BandTestResult DCSEncoder::FindBestBandEncoding(
    const CompressionParams &params,
    std::function<BandEncoding(int band, int bandTypeCode)> interpretBandTypeCode,
    int minNewCode, int maxNewCode, int band, const float *samples, int nSamples)
{
    // Figure the maximum acceptable sum of error squares
    float errSumMax = (params.maximumQuantizationError * params.maximumQuantizationError)
        * static_cast<float>(nSamples);

    // test each coding
    BandTestResult testResult[16];
    for (int testCode = 1 ; testCode <= 15 ; ++testCode)
    {
        // Skip type codes that are outside of the allowable range.  The stream
        // formats limit the deltas for code changes between adjacent frames,
        // so certain codes might be off-limits depending on the code used in
        // the previous frame.
        if (testCode < minNewCode || testCode > maxNewCode)
            continue;

        // Interpret the band code into the sample bit width and scaling factor
        auto enc = interpretBandTypeCode(band, testCode);
        float scaleFactor = static_cast<float>(scalingFactors[enc.scaleCode]);

        // For comparison purposes, reference the values to the midpoint of
        // the unsigned range.  This isn't accurate for all formats; some store
        // the stream values as signed 2's complement values.  But it doesn't
        // really matter for our purposes here; we just care about what the
        // value looks like when reduced the selected bit width.  Storing a
        // signed N-bit value in an unsigned N-bit field with a bias to the
        // midpoint of the unsigned range works out the same way in terms of
        // the round trip, without having to worry about sign extension when
        // widening the value back to a native type.
        int refVal = (enc.bitWidth != 0) ? (1 << (enc.bitWidth - 1)) : 0;

        // get the storage mask: keep the low-order N bits
        int mask = (0xFFFF >> (16 - enc.bitWidth));

        // Figure the round-trip result of encoding and decoding each value in the
        // buffer using this format, and figure the quantization error for each
        // sample (the difference between the original sample value and its
        // reconstructed value after the round trip).  Collect the sum of squares
        // of the errors to represent the cumulative error for the band.
        float quantErrSquaredSum = 0.0f;
        for (int i = 0 ; i < nSamples ; ++i)
        {
            // get the original sample value
            float orig = samples[i];

            // figure the sample value as stored
            int scaled = static_cast<int>(roundf(orig * 32768.0f / scaleFactor));
            int stored = (scaled + refVal) & mask;

            // figure the reconstructed value
            float reconstructed = static_cast<float>((stored - refVal) * scaleFactor) / 32768.0f;

            // figure the quantization error - the difference between the
            // reconstructed value and the original value - and add it to
            // the sum of squares
            float quantErr = reconstructed - orig;
            quantErrSquaredSum += quantErr * quantErr;
        }

        // store the result
        bool pass = quantErrSquaredSum <= errSumMax;
        testResult[testCode] ={ testCode, quantErrSquaredSum, enc.bitWidth, pass };
    }

    // Return the best result
    return testResult[FindBestResult(testResult, static_cast<int>(_countof(testResult)))];
}

int DCSEncoder::FindBestResult(const BandTestResult *results, int nResults)
{
    // Find the passing result with the narrowest bit coding, if any
    int narrowestPass = -1;
    for (int i = 0 ; i < nResults ; ++i)
    {
        auto &result = results[i];
        if (result.pass && (narrowestPass == -1 || result.bitWidth < narrowestPass))
            narrowestPass = result.bitWidth;
    }

    // If we found any codings that produced acceptable quantization errors
    // (at or below the maximum error setting in the parameters), choose the
    // one with the lowest error among those with the narrowest bit width
    // producing a passing result.  If we didn't find any encodings that
    // passes the maximum error test, choose the one from the overall set
    // that minimizes error.
    float minErrSum = -1.0f;
    int bestResultIndex = 0;
    for (int i = 0 ; i < nResults ; ++i)
    {
        // ignore the band if the type code is invalid - this indicates
        // that this entry isn't populated
        auto &tr = results[i];
        if (tr.bandTypeCode < 0)
            continue;

        // consider this one if it matches the narrowest passing bit
        // width, or if we're considering all bit widths
        if (narrowestPass == -1 || tr.bitWidth == narrowestPass)
        {
            // remember it if it's the best so far
            if (minErrSum < 0 || tr.errSum < minErrSum)
            {
                // this is the best so far - keep it
                bestResultIndex = i;
                minErrSum = tr.errSum;
            }
        }
    }

    // return the best result
    return bestResultIndex;
}

// Frame compression for the 1994+ format, used in all original DCS software
// except the software shipped with the first three games released in 1993
// (IJTPA, JD, STTNG).  This format is used uniformly in all 26 of the games
// shipped from 1994 to 1998.
bool DCSEncoder::CompressFrame94(BitWriter &bitWriter,
    int frameNo, Stream::Frame &frame, std::string &errorMessage)
{
    // Frame header codebook, indexed by (plain text value + 16).
    static const CodebookEntry frameHeaderCodes[] ={
        { -16, 0x00050404, 20 },
        { -15, 0x00050403, 20 },
        { -14, 0x00282011, 23 },
        { -13, 0x000a080b, 21 },
        { -12, 0x00141009, 22 },
        { -11, 0x00141001, 22 },
        { -10, 0x00282010, 23 },
        {  -9, 0x000a0801, 21 },
        {  -8, 0x000a0805, 21 },
        {  -7, 0x00028203, 19 },
        {  -6, 0x00005041, 16 },
        {  -5, 0x00001411, 14 },
        {  -4, 0x00000140, 10 },
        {  -3, 0x00000029,  7 },
        {  -2, 0x0000000b,  5 },
        {  -1, 0x00000000,  2 },
        {   0, 0x00000001,  1 },
        {   1, 0x00000003,  3 },
        {   2, 0x00000004,  4 },
        {   3, 0x00000015,  6 },
        {   4, 0x00000051,  8 },
        {   5, 0x000000a1,  9 },
        {   6, 0x00000283, 11 },
        {   7, 0x00000505, 12 },
        {   8, 0x00000a09, 13 },
        {   9, 0x00002821, 15 },
        {  10, 0x00141000, 22 },
        {  11, 0x00014103, 18 },
        {  12, 0x00050401, 20 },
        {  13, 0x00014102, 18 },
        {  14, 0x000a080a, 21 }
    };

    // Sample codebooks (not in the sense of "samples of codebooks", but rather
    // "codebooks for encoding samples").  sampleCodebookN is the codebook for
    // samples of bit width N.  Samples are signed integers, so these encode
    // N-bit 2's-complement numbers.  The 1-bit codebook can thus encode the
    // values -1 and 0; the 2-bit codebook encodes values -2..+1; etc.
    static const CodebookEntry sampleCodebook1[] ={
        {  -1, 0x00000001,   2 },
        {   0, 0x00000000,   2 },
    };

    static const CodebookEntry sampleCodebook2[] ={
        {  -2, 0x00000002,   3 },
        {  -1, 0x00000000,   2 },
        {   0, 0x00000003,   3 },
        {   1, 0x00000002,   2 },
    };

    static const CodebookEntry sampleCodebook3[] ={
        {  -4, 0x00000012,   5 },
        {  -3, 0x00000013,   5 },
        {  -2, 0x0000000e,   4 },
        {  -1, 0x00000001,   2 },
        {   0, 0x00000006,   3 },
        {   1, 0x00000000,   2 },
        {   2, 0x00000005,   3 },
        {   3, 0x00000008,   4 },
    };

    static const CodebookEntry sampleCodebook4[] ={
        {  -8, 0x0000005a,   7 },
        {  -7, 0x0000005b,   7 },
        {  -6, 0x00000029,   6 },
        {  -5, 0x0000000e,   5 },
        {  -4, 0x00000017,   5 },
        {  -3, 0x00000009,   4 },
        {  -2, 0x00000001,   3 },
        {  -1, 0x00000007,   3 },
        {   0, 0x00000002,   3 },
        {   1, 0x00000006,   3 },
        {   2, 0x00000000,   3 },
        {   3, 0x00000008,   4 },
        {   4, 0x00000006,   4 },
        {   5, 0x0000000f,   5 },
        {   6, 0x0000002c,   6 },
        {   7, 0x00000028,   6 },
    };

    static const CodebookEntry sampleCodebook5[] ={
        { -16, 0x0000005a,   8 },
        { -15, 0x0000005b,   8 },
        { -14, 0x000000e9,   8 },
        { -13, 0x000000ef,   8 },
        { -12, 0x0000004c,   7 },
        { -11, 0x00000075,   7 },
        { -10, 0x00000017,   6 },
        {  -9, 0x0000002a,   6 },
        {  -8, 0x00000027,   6 },
        {  -7, 0x0000003d,   6 },
        {  -6, 0x00000012,   5 },
        {  -5, 0x0000001c,   5 },
        {  -4, 0x00000004,   4 },
        {  -3, 0x00000008,   4 },
        {  -2, 0x0000000d,   4 },
        {  -1, 0x00000001,   3 },
        {   0, 0x0000000b,   4 },
        {   1, 0x00000000,   3 },
        {   2, 0x0000000c,   4 },
        {   3, 0x00000007,   4 },
        {   4, 0x0000001f,   5 },
        {   5, 0x00000014,   5 },
        {   6, 0x0000000c,   5 },
        {   7, 0x0000003c,   6 },
        {   8, 0x0000000a,   5 },
        {   9, 0x0000002b,   6 },
        {  10, 0x0000001a,   6 },
        {  11, 0x00000076,   7 },
        {  12, 0x0000004d,   7 },
        {  13, 0x0000002c,   7 },
        {  14, 0x000000ee,   8 },
        {  15, 0x000000e8,   8 },
    };

    static const CodebookEntry sampleCodebook6[] ={
        { -32, 0x00000022,   9 },
        { -31, 0x00000023,   9 },
        { -30, 0x000000fa,   9 },
        { -29, 0x000000fb,   9 },
        { -28, 0x00000181,   9 },
        { -27, 0x000001ce,   9 },
        { -26, 0x000001cf,   9 },
        { -25, 0x0000002a,   8 },
        { -24, 0x00000079,   8 },
        { -23, 0x000000a8,   8 },
        { -22, 0x000000c1,   8 },
        { -21, 0x000000e6,   8 },
        { -20, 0x00000009,   7 },
        { -19, 0x00000032,   7 },
        { -18, 0x0000003f,   7 },
        { -17, 0x00000061,   7 },
        { -16, 0x0000003d,   7 },
        { -15, 0x00000057,   7 },
        { -14, 0x00000070,   7 },
        { -13, 0x00000076,   7 },
        { -12, 0x00000005,   6 },
        { -11, 0x00000018,   6 },
        { -10, 0x00000029,   6 },
        {  -9, 0x00000031,   6 },
        {  -8, 0x0000003c,   6 },
        {  -7, 0x00000003,   5 },
        {  -6, 0x0000000e,   5 },
        {  -5, 0x00000016,   5 },
        {  -4, 0x0000001b,   5 },
        {  -3, 0x00000000,   4 },
        {  -2, 0x00000005,   4 },
        {  -1, 0x00000009,   4 },
        {   0, 0x00000003,   4 },
        {   1, 0x00000008,   4 },
        {   2, 0x00000004,   4 },
        {   3, 0x0000001f,   5 },
        {   4, 0x0000001a,   5 },
        {   5, 0x00000017,   5 },
        {   6, 0x0000000d,   5 },
        {   7, 0x00000004,   5 },
        {   8, 0x0000003a,   6 },
        {   9, 0x00000032,   6 },
        {  10, 0x00000028,   6 },
        {  11, 0x0000000b,   6 },
        {  12, 0x0000007b,   7 },
        {  13, 0x00000072,   7 },
        {  14, 0x00000066,   7 },
        {  15, 0x00000055,   7 },
        {  16, 0x00000077,   7 },
        {  17, 0x00000067,   7 },
        {  18, 0x00000056,   7 },
        {  19, 0x00000033,   7 },
        {  20, 0x00000014,   7 },
        {  21, 0x000000f4,   8 },
        {  22, 0x000000e2,   8 },
        {  23, 0x000000a9,   8 },
        {  24, 0x0000007c,   8 },
        {  25, 0x0000002b,   8 },
        {  26, 0x00000010,   8 },
        {  27, 0x000001c7,   9 },
        {  28, 0x000001c6,   9 },
        {  29, 0x00000180,   9 },
        {  30, 0x000000f1,   9 },
        {  31, 0x000000f0,   9 },
    };

    struct SampleCodebook
    {
        int rangeLo;
        int rangeHi;
        CodebookEntry doubleZero;
        const CodebookEntry *codes;
    };
    static const SampleCodebook sampleCodebooks[] ={
        { -1, 0, { -1, 0x00000001, 1 }, sampleCodebook1 },
        { -2, 1, { -1, 0x00000003, 2 }, sampleCodebook2 },
        { -4, 3, { -1, 0x0000000f, 4 }, sampleCodebook3 },
        { -8, 7, { -1, 0x00000015, 5 }, sampleCodebook4 },
        { -16, 15, { -1, 0x0000001b, 6 }, sampleCodebook5 },
        { -32, 31, { -1, 0x000000f5, 8 }, sampleCodebook6 },
    };

    // get the parameters from the bit writer
    auto &params = bitWriter.params;

    // get the stream type and sub-type
    int streamType = params.streamFormatType;
    int streamSubType = params.streamFormatSubType;

    // Figure the scaling factor pre-adjustment for bands 0-2
    uint16_t preAdj[3];
    const uint16_t *preAdjMap = streamSubType == 0 ? preAdjMap0 : preAdjMap3;
    for (int i = 0 ; i < 3 ; ++i)
        preAdj[i] = preAdjMap[bitWriter.bandTypeCode[i]];

    // Interpret a band type
    auto InterpretBandTypeCode = [this, &preAdj, streamType, streamSubType, &bitWriter](int band, int bandTypeCode) -> BandEncoding
    {
        // band type code 0 is the special zero-bit format, where all samples
        // are encoded as zero values, and since they're all zeroes, we don't
        // have to store any bits in the stream to represent this (beyond the
        // band type code itself)
        if (bandTypeCode == 0)
            return BandEncoding{ 0, 0, 0 };

        // get the scaling code from the header
        int scalingCode = bitWriter.header[band] & 0x3F;

        // interpret the band type code and scaling code according to the stream type
        if (streamType == 0)
        {
            // Type 0 -> the band code is given directly in the frame header,
            // and the stream header byte gives the scaling factor code.  Band
            // codes 1-6 use predefined Huffman codebooks with a reference value
            // at the midpoint of the 2's complement unsigned range.  Band codes
            // 7+ are encoded as uncompressed signed 2's complement values.
            int refVal = (bandTypeCode <= 6) ? (1 << (bandTypeCode - 1)) : 0;
            return BandEncoding{ bandTypeCode, scalingCode, refVal };
        }
        else
        {
            // Type 1 -> the band code is a lookup table index, and the
            // lookup table specifies the bit width and scaling factor.
            // The lookup table varies by band.
            int bitWidth = 0;
            int scalingAdj = 0;
            if (band < 3)
            {
                // band 0-2 - translate the band type code through the lookup
                // table for these bands
                static const uint16_t xlat02[0x0010] ={
                    0x0000, 0x0100, 0x0200, 0x0300, 0x0400, 0x0402, 0x0405, 0x0505,
                    0x0509, 0x050d, 0x060d, 0x0611, 0x0615, 0x0719, 0x071d, 0x081d
                };
                uint16_t tableEntry = xlat02[bandTypeCode];

                // the bit width is the high 8 bits of the table entry
                bitWidth = tableEntry >> 8;

                // the scaling adjustment is the low 8 bits of the table entry
                scalingAdj = (tableEntry & 0xFF);

                // these bands also apply the scaling code pre-adjustment
                scalingAdj += preAdj[band];
            }
            else if (band < 6)
            {
                // sample bands 3..5 - translate the band type code through the
                // lookup code for these bands
                static const uint16_t xlat35[0x0010] ={
                    0x0000, 0x0100, 0x0200, 0x0300, 0x0400, 0x0402, 0x0407, 0x040b,
                    0x050b, 0x050f, 0x0513, 0x0517, 0x0617, 0x061b, 0x061f, 0x071f
                };
                uint16_t tableEntry = xlat35[bandTypeCode];
                bitWidth = tableEntry >> 8;
                scalingAdj = (tableEntry & 0xFF);
            }
            else
            {
                // sample bands 6..15 - translate the band type code through the
                // lookup table for these bands
                static const uint16_t xlat6F[0x0010] ={
                    0x0000, 0x0100, 0x0200, 0x0300, 0x0302, 0x0402, 0x0407, 0x040b,
                    0x050b, 0x050f, 0x0513, 0x0517, 0x0617, 0x061b, 0x061f, 0x0723
                };
                uint16_t tableEntry = xlat6F[bandTypeCode];
                bitWidth = tableEntry >> 8;
                scalingAdj = (tableEntry & 0xFF);
            }

            // Figure the reference value.  The compressed encodings (1-6) use
            // a reference value at the halfway point of the range, so that all
            // encoded values are positive binary integers; the uncompressed
            // encodings (7-15) use 2's complement notation, so the reference
            // point is zero.
            int refVal = (bitWidth >= 1 && bitWidth <= 6) ? (1 << (bitWidth - 1)) : 0;

            // The high byte of the table entry is the band type code, and
            // the low byte is the scaling code adjustment.  
            return BandEncoding{ bitWidth, scalingCode + scalingAdj, refVal };
        }
    };

    // Build the frame header.  This consists of one codeword from the
    // frame header codebook per populated band.  The codeword specifies
    // the band sample type, and is given as the DIFFERENCE from the
    // corresponding value in the previous frame's header.
    for (int band = 0, firstSample = 0 ; band < 16 && (bitWriter.header[band] & 0x7f) != 0x7f ; ++band)
    {
        // get the nubmer of samples in this band
        int nSamples = bandSampleCounts94[band];

        // If this band has a negligible dynamic range (below the minimum
        // threshold set in the parameters), encode it with the special
        // "all zeroes" band type code, which doesn't require any bits in
        // the stream.
        const auto &range = frame.range[band];
        int oldCode = bitWriter.bandTypeCode[band];
        int newCode;
        if (range.hi - range.lo < params.minimumDynamicRange)
        {
            // use band type code 0 = 0 bits per sample
            newCode = 0;
        }
        else
        {
            // Figure the new band type code that optimizes storage size at
            // an acceptable error level.  Because of the delta encoding for
            // the new type code, the new code can only differ from the old
            // code by -16 to +14.
            newCode = FindBestBandEncoding(params, InterpretBandTypeCode,
                oldCode - 16, oldCode + 14, band, &frame.f[firstSample], nSamples).bandTypeCode;
        }

        // Figure the difference from the old code, and make sure it's in the valid 
        // encoding range.  (This should always be guaranteed, since we limit the
        // search, but check anyway to protect against errors in our own logic
        // selecting the new code.)
        int delta = newCode - oldCode;
        if (delta < -16 || delta > 14)
        {
            errorMessage = "Internal error in frame compression: frame type code difference out of range";
            return false;
        }

        // write the differential code
        bitWriter.Write(frameHeaderCodes[delta + 16]);

        // save the new code in the stream record
        bitWriter.bandTypeCode[band] = newCode;

        // advance the base sample index
        firstSample += nSamples;
    }

    // Populate the retained bands.  The first non-populated band is marked
    // with 0x7F in the low-order bits of the band's header byte.
    for (int band = 0, firstSample = 0 ; band < 16 && (bitWriter.header[band] & 0x7f) != 0x7f ; ++band)
    {
        // interpret the band type code into the encoding parameters for the band
        auto enc = InterpretBandTypeCode(band, bitWriter.bandTypeCode[band]);

        // mask for the number of bits stored
        int mask = 0xFFFF >> (16 - enc.bitWidth);

        // get the scaling factor
        float scaleFactor = static_cast<float>(scalingFactors[enc.scaleCode]);

        // get the sample codebook, if applicable
        const SampleCodebook *codebook = (enc.bitWidth >= 1 && enc.bitWidth <= 6) ? &sampleCodebooks[enc.bitWidth - 1] : nullptr;

        // if the band contains any samples, write them
        int nSamples = bandSampleCounts94[band];
        if (enc.bitWidth != 0)
        {
            // figure the scaled sample values
            int staging[32];
            const float *pSample = &frame.f[firstSample];
            for (int i = 0 ; i < nSamples ; ++i)
            {
                // get the next sample as a signed 16-bit int, scaled by the scaling factor,
                // and add it to the staging buffer
                staging[i] = static_cast<int>(roundf(*pSample++ * 32768.0f / scaleFactor));
            }

            // write the samples
            for (int i = 0 ; i < nSamples; ++i)
            {
                // Check for the special case where we have two consecutive zeroes.
                // The compressed codebooks have a special representation for this
                // case, which only takes up one sample slot in the stream.
                int sample = staging[i];
                if (sample == 0 && i + 1 < nSamples && staging[i+1] == 0 && codebook != nullptr)
                {
                    // we have a double zero - write the special codebook code
                    bitWriter.Write(codebook->doubleZero);

                    // skip the next sample, since the single codeword we just
                    // wrote represents two consecutive samples
                    ++i;
                }
                else
                {
                    // figure the sample relative to the reference value and masked
                    // to the bit width
                    sample += enc.refVal;
                    sample &= mask;

                    // write the sample via the compression codebook, if there is one,
                    // or as a simple 2's complement value if there's no codebook
                    if (codebook != nullptr)
                        bitWriter.Write(codebook->codes[sample]);
                    else
                        bitWriter.Write(sample, enc.bitWidth);
                }
            }
        }

        // advance the base sample index
        firstSample += nSamples;
    }

    // success
    return true;
}

// Frame compression for the 1993b format.  This is used in STTNG for all 
// streams, and for IJTPA and JD for Type 0 streams.  (1993a uses a
// completely different format for streams with the Type 1 bit.)
bool DCSEncoder::CompressFrame93b(BitWriter &bitWriter,
    int frameNo, Stream::Frame &frame, std::string &errorMessage)
{
    // Band type Huffman codebooks.
    //
    // Each frequency band in this type of frame starts with a Huffman-coded
    // differential band type code.  This bit string encodes two pieces of
    // information: the delta for the band type, and whether or not to
    // logically invert the band subtype code.  Entries from the first table
    // leave the subtype code unchanged; entries from the second table
    // invert the subtype, changing zero to 1 and non-zero to zero.

    // Codebook for "Keep the same Band Subtype as the last frame"
    static const CodebookEntry huffTabKeep[] ={
        { -16, 0x00000000,  0 },   //  -16, unused
        { -15, 0x0132e880, 26 },   //  -15, 01001100101110100010000000
        { -14, 0x0132e881, 26 },   //  -14, 01001100101110100010000001
        { -13, 0x00997443, 25 },   //  -13, 0100110010111010001000011
        { -12, 0x004cba23, 24 },   //  -12, 010011001011101000100011
        { -11, 0x0004cb8a, 20 },   //  -11, 01001100101110001010
        { -10, 0x0004cba7, 20 },   //  -10, 01001100101110100111
        {  -9, 0x0004cb2e, 20 },   //   -9, 01001100101100101110
        {  -8, 0x0004cba3, 20 },   //   -8, 01001100101110100011
        {  -7, 0x00026592, 19 },   //   -7, 0100110010110010010
        {  -6, 0x000132ca, 18 },   //   -6, 010011001011001010
        {  -5, 0x00004cb3, 16 },   //   -5, 0100110010110011
        {  -4, 0x000004c9, 12 },   //   -4, 010011001001
        {  -3, 0x0000009b,  9 },   //   -3, 010011011
        {  -2, 0x0000000a,  5 },   //   -2, 01010
        {  -1, 0x00000000,  2 },   //   -1, 00
        {   0, 0x00000001,  1 },   //    0, 1
        {   1, 0x00000003,  3 },   //    1, 011
        {   2, 0x0000000b,  5 },   //    2, 01011
        {   3, 0x0000004f,  8 },   //    3, 01001111
        {   4, 0x00000098,  9 },   //    4, 010011000
        {   5, 0x00000267, 11 },   //    5, 01001100111
        {   6, 0x000004ca, 12 },   //    6, 010011001010
        {   7, 0x0000132f, 14 },   //    7, 01001100101111
        {   8, 0x00002658, 15 },   //    8, 010011001011000
        {   9, 0x00009970, 17 },   //    9, 01001100101110000
        {  10, 0x00026593, 19 },   //   10, 0100110010110010011
        {  11, 0x0004cba0, 20 },   //   11, 01001100101110100000
        {  12, 0x00099745, 21 },   //   12, 010011001011101000101
        {  13, 0x0004cb2f, 20 },   //   13, 01001100101100101111
        {  14, 0x00026596, 19 },   //   14, 0100110010110010110
        {  15, 0x00000000,  0 }    //   15, unused
    };

    // Codebook for "Invert the Band Subtype"
    static const CodebookEntry huffTabInvert[] ={
        { -16, 0x004cba25, 24 },   //  -16, 010011001011101000100101
        { -15, 0x004cba24, 24 },   //  -15, 010011001011101000100100
        { -14, 0x0132e883, 26 },   //  -14, 01001100101110100010000011
        { -13, 0x09974412, 29 },   //  -13, 01001100101110100010000010010
        { -12, 0x004cba22, 24 },   //  -12, 010011001011101000100010
        { -11, 0x0265d105, 27 },   //  -11, 010011001011101000100000101
        { -10, 0x04cba208, 28 },   //  -10, 0100110010111010001000001000
        {  -9, 0x0004cb8b, 20 },   //   -9, 01001100101110001011
        {  -8, 0x00026591, 19 },   //   -8, 0100110010110010001
        {  -7, 0x0004cb88, 20 },   //   -7, 01001100101110001000
        {  -6, 0x0004cba6, 20 },   //   -6, 01001100101110100110
        {  -5, 0x0004cba5, 20 },   //   -5, 01001100101110100101
        {  -4, 0x00004cb9, 16 },   //   -4, 0100110010111001
        {  -3, 0x000004c8, 12 },   //   -3, 010011001000
        {  -2, 0x0000009a,  9 },   //   -2, 010011010
        {  -1, 0x00000012,  6 },   //   -1, 010010
        {   0, 0x00000008,  5 },   //    0, 01000
        {   1, 0x0000004e,  8 },   //    1, 01001110
        {   2, 0x00000266, 11 },   //    2, 01001100110
        {   3, 0x0000132d, 14 },   //    3, 01001100101101
        {   4, 0x00004cbb, 16 },   //    4, 0100110010111011
        {   5, 0x00009975, 17 },   //    5, 01001100101110101
        {   6, 0x000132e3, 18 },   //    6, 010011001011100011
        {   7, 0x0004cba4, 20 },   //    7, 01001100101110100100
        {   8, 0x0004cb89, 20 },   //    8, 01001100101110001001
        {   9, 0x00026590, 19 },   //    9, 0100110010110010000
        {  10, 0x0004cba1, 20 },   //   10, 01001100101110100001
        {  11, 0x00265d13, 23 },   //   11, 01001100101110100010011
        {  12, 0x132e8826, 30 },   //   12, 010011001011101000100000100110
        {  13, 0x132e8827, 30 },   //   13, 010011001011101000100000100111
        {  14, 0x0132e884, 26 },   //   14, 01001100101110100010000100
        {  15, 0x0132e885, 26 },   //   15, 01001100101110100010000101
    };

    // get the parameters from the bit writer
    auto &params = bitWriter.params;

    // get the stream type (OS93 doesn't have stream subtypes)
    int streamType = params.streamFormatType;
    
    // get the band sample counts based on the stream type
    const uint16_t *sampleCounts = (streamType == 1) ? bandSampleCounts93b_Type1 : bandSampleCounts93;

    // Interpret a band type code into the bit width and scale code
    auto InterpretBandTypeCode = [streamType, &bitWriter](int band, int bandTypeCode) -> BandEncoding
    {
        // Band type code 0 is the special "zero bits" encoding, where all samples
        // in the band are zero, represented with no bits at all in the stream
        // (since they're all a fixed value that can be inferred entirely from the
        // type code itself).
        if (bandTypeCode == 0)
            return { 0, 0, 0 };

        // The bit width is the band type code for Type 0 streams, and 
        // the code + 1 for Type 0 streams
        int bitWidth = bandTypeCode;
        if (streamType == 0)
            bitWidth += 1;

        // The scaling code is given by the low 6 bits of the band's header byte
        int scalingCode = bitWriter.header[band] & 0x3F;

        // Return the result.  All OS93 encodings use simple 2's complement 
        // representations with a reference point of zero.
        return { bitWidth, scalingCode, 0 };
    };

    // Previous band's band type code
    int lastBandTypeCode = -1;

    // Band subtype code: this specifies how the samples in the current band
    // are encoded:
    //
    //   Band Subtype 0 -> each sample is an independent input
    //   Band Subtype 1 -> each sample is a delta from the previous input
    //   Band Subtype 2 -> each sample is a delta from the previous delta
    //
    // The initial subtype at the start of a frame is determined by the stream
    // major type: Type 1 stream -> start with band subtype 0, Type 0 stream
    // -> start with band subtype 2.  Type 1 streams can only ever use subtypes
    // 0 and 1; type 0 streams can use all three subtypes.
    int lastBandSubType = (streamType == 1 ? 0 : 2);

    // Last sample of the previous band.  The OS93b format has two differential
    // modes, where new samples are encoded as deltas from the last sample or
    // as deltas from the last deltas.  These carry forward across bands, so
    // we need to track the last sample of each prior band.  Theese values start
    // at zero in each frame.
    int prvSample = 0;
    int prvDelta = 0;

    // Loop through the bands
    int firstSample = 0;
    for (int band = 0 ; band < 16 ; ++band)
    {
        // stop when we reach the last band, marked with the low 7 bits in the
        // header byte set to $7F
        int curHdrByte = bitWriter.header[band] & 0x7F;
        if (curHdrByte == 0x7F)
            break;

        // get the nubmer of samples in the band
        int nSamples = sampleCounts[band];

        // get the scaling code for the band from the low 6 bits of the header byte
        int scalingCode = curHdrByte & 0x3F;
        float scalingFactor = static_cast<float>(scalingFactors[scalingCode]);

        // get the previous frame's band type code
        int oldBandTypeCode = bitWriter.bandTypeCode[band];

        // Figure the min and max band type codes available for a given subtype
        struct CodeRange { int lo; int hi; };
        auto GetCodeRange = [streamType, &lastBandSubType, &oldBandTypeCode](int newBandSubType) -> CodeRange
        {
            if (streamType == 0)
            {
                // For Type 0 streams, the new band type is specified as an
                // independent 4-bit value, so we have access to the full 0..15 
                // range in all cases.
                return { 0, 15 };
            }
            else
            {
                // For Type 1 streams, the new band type is specified as a delta
                // from the previous code, so it's limited to what we can express
                // with the Huffman codebook.  To make matters more interesting,
                // the choice of codebook is a function of the new band subtype,
                // and the codebooks have different delta ranges!  We use the
                // "Keep" codebook is the subtype isn't changing, and we use the
                // "Invert" codebook if it is.  The "Keep" codebook can express
                // a range of -15 to +14, and the "Invert" can express -16 to +15.
                if (newBandSubType == lastBandSubType)
                {
                    // "Keep" codebook - range is -15 to +14
                    return { oldBandTypeCode - 15, oldBandTypeCode + 14 };
                }
                else
                {
                    // "Invert" codebook - range is -16 to +15.  This covers
                    // the entire gamut, since the old code can only be 0..15,
                    //  so we could just return 0..15.  But let's make the
                    // delta calculation explicit anyway.
                    return { oldBandTypeCode - 16, oldBandTypeCode + 15 };
                }
            }
        };

        // Calculate the integer versions of the direct, delta, and double-delta
        // codings for this block.  This will let us pick the one that yields the
        // most compact storage within our error limits.
        int prvFrameLastSample = prvSample;
        int prvFrameLastDelta = prvDelta;
        int buf0[16], buf1[16], buf2[16];
        for (int i = 0 ; i < nSamples ; ++i)
        {
            // calculate the scaled direct sample value
            int cur = static_cast<int>(roundf(frame.f[firstSample + i] * 32768.0f / scalingFactor));
            buf0[i] = cur;
            buf1[i] = cur - prvSample;
            buf2[i] = cur - prvSample - prvDelta;

            // update the previous sample and delta
            prvDelta = buf1[i];
            prvSample = buf0[i];
        }

        // Find the best encoding for Subtype 0, which encodes all of the 
        // band's elements as independent values.
        auto range0 = GetCodeRange(0);
        int bandCodeSubtype0 = FindBestBandEncoding(
            params, InterpretBandTypeCode, range0.lo, range0.hi, band, &frame.f[firstSample], nSamples).bandTypeCode;

        // Subtypes 1 and 2 encode the sample differentially before scaling,
        // so we simply need to evaluate how many bits it will take to
        // express all of the deltas.
        auto GetDeltaBandCode = [&nSamples, streamType](const int *buf)
        {
            // find the min and max of the samples
            int lo = *buf++, hi = lo;
            for (int i = 1 ; i < nSamples ; ++i)
            {
                int s = *buf++;
                if (s < lo)
                    lo = s;
                if (s > hi)
                    hi = s;
            }

            // get the higher positive value into hi
            if (hi < 0) hi = -hi;
            if (lo < 0) lo = -lo;
            if (lo > hi) hi = lo;

            // if both sides are zero, we can use the zero-bit format
            if (hi == 0)
                return 0;

            // figure the number of bits needed to store hi in 2's complement format
            int nBits = 1;
            for (; hi != 0 ; hi >>= 1, ++nBits);

            // for Type 0 streams, bit width = band code + 1, so band code = bit width - 1
            int bandCode = nBits - (streamType == 0 ? 1 : 0);

            // the result is the band code
            return bandCode;
        };
        int bandCodeSubtype1 = GetDeltaBandCode(buf1);
        int bandCodeSubtype2 = GetDeltaBandCode(buf2);

        // Choose the narrowest encoding.  The band code numbering is in increasing
        // order of bit width, so we can use it as a proxy - i.e., just pick the
        // lowest numbered band code.  Type 0 streams can use any subtype; Type 1
        // streams can only use subtype 0 and 1.  Start by assuming subtype 0.
        int newBandTypeCode = bandCodeSubtype0;
        int newBandSubType = 0;

        // If subtype 1 is better, use it instead.  If it's equally good and
        // the last band was also subtype 1, use subtype 1, since it costs
        // extra bits to change subtypes.
        if (bandCodeSubtype1 < newBandTypeCode
            || (bandCodeSubtype1 == newBandTypeCode && lastBandSubType == 1))
            newBandSubType = 1, newBandTypeCode = bandCodeSubtype1;

        // if subtype 2 is better, and we're allowed to use it, use it instead
        if (streamType == 0 && bandCodeSubtype2 < newBandTypeCode)
             newBandSubType = 2, newBandTypeCode = bandCodeSubtype2;

        // If the last band's type code was 0, the new band starts with a bit
        // indicating whether or not to repeat the same band type and subtype
        // for the new band:  a 1 bit means repeat it, a 0 bit means get a new
        // code.  The special repeating mode for band type code 0 produces an
        // extremely compact coding (just 1 bit per band) for frames that are
        // mostly empty.
        if (lastBandTypeCode == 0 && newBandTypeCode == 0
            && lastBandSubType == newBandSubType)
        {
            // This is the special case where we can repeat the band type 0
            // with a single '1' bit at the start of the band.  This is the
            // only thing we write to this band. 
            bitWriter.Write(1, 1);
        }
        else
        {
            // We don't have two type 0 bands in a row, so we have to write
            // band the type code delta for this band.  If the last band was
            // a type 0 band, we need to write a 0 bit first to indicate that
            // we're NOT repeating the type 0 band this time.
            if (lastBandTypeCode == 0)
                bitWriter.Write(0, 1);

            // the type code spec format depends on the stream format type
            if (streamType == 0)
            {
                // Stream Type 0

                // The first bit of this band is '0' if we're reusing the
                // previous subtype, '1' if we're changing the subtype.
                if (newBandSubType == lastBandSubType)
                {
                    // keep the subtype - write a '0' bit
                    bitWriter.Write(0, 1);
                }
                else
                {
                    // Change the subtype - write a '0' bit, followed by
                    // a bit indicating the delta: '0' for subtract one,
                    // '1' for add one.
                    static const int from0[] ={ 9999, 1, 0 };  // 0->1 = inc mod 2 = '1' bit, 0->2 = dec mod 2 = '0' bit
                    static const int from1[] ={ 0, 9999, 1 };  // 1->0 = dec mod 2 = '0' bit, 1->2 = inc mod 2 = '1' bit
                    static const int from2[] ={ 1, 0, 9999 };  // 2->0 = inc mod 2 = '1' bit, 2->1 = dec mod 2 = '0' bit
                    static const int *const deltaBit[] ={ from0, from1, from2 };
                    bitWriter.Write(1, 1);
                    bitWriter.Write(deltaBit[lastBandSubType][newBandSubType], 1);
                }

                // Now write the new band type code as a 4-bit int
                bitWriter.Write(newBandTypeCode, 4);
            }
            else
            {
                // Stream Type 1: write the Huffman-coded delta between the
                // band type from the previous frame and the new band type
                // for this frame.  If the band subtype is changing, use the
                // "Invert" codebook, otherwise use the "Keep" codebook.
                // Each codebook encodes the same delta values, but they
                // use different bit strings to do it, and the decoder
                // recognizes the distinct bit strings as carrying this
                // additional meaning of whether or not we're switching
                // subtypes.
                //
                // Note that the delta here is between *frames*, not just
                // between adjacent bands.
                auto *codebook = (newBandSubType == lastBandSubType) ? huffTabKeep : huffTabInvert;
                int delta = newBandTypeCode - bitWriter.bandTypeCode[band];
                bitWriter.Write(codebook[delta + 16]);
            
                // Update the frame-to-frame band type code memory, so that
                // we can figure the delta for next time.  Note that we only
                // update this when we write a new delta, and must not update
                // it on frames where a Band Type 0 is carried over from the
                // previous band via the special "reuse band type 0" bit.
                // The decoder logic only updates the frame-to-frame memory
                // on frames containing a delta value, so we have to do the
                // same to keep the reference point in sync with the decoder.
                bitWriter.bandTypeCode[band] = newBandTypeCode;
            }

            // Now we just have to write the samples, as simple N-bit
            // integers using the selected bit width and scaling factor.
            if (newBandTypeCode == 0)
            {
                // Zero band type - the samples are stored as all zeroes with
                // zero bits each, so there's nothing to write.  All we have to
                // do is update the 'prv' values to reflect the last samples
                // being written as zeroes.
                switch (newBandSubType)
                {
                case 0:
                    // the samples are all zeroes, so this zeroes out the previous
                    // sample and delta
                    prvSample = 0;
                    prvDelta = 0;
                    break;

                case 1:
                    // the deltas are all zeroes, so this leaves the previous sample
                    // unchanged and zeroes out the previous delta
                    prvSample = prvFrameLastSample;
                    prvDelta = 0;
                    break;

                case 2:
                    // the double-deltas are all zeroes, so this leaves the previous
                    // sample and delta unchanged
                    prvSample = prvFrameLastSample;
                    prvDelta = prvFrameLastDelta;
                    break;
                }
            }
            else
            {
                // Non-zero band type - write the samples
                int nBits = newBandTypeCode + (streamType == 0 ? 1 : 0);
                int mask = (1 << nBits) - 1;

                // pick the sample buffer corresponding to the band subtype
                const int *p = newBandSubType == 0 ? buf0 :
                    newBandSubType == 1 ? buf1 : buf2;

                // write the samples
                for (int i = 0 ; i < nSamples ; ++i)
                    bitWriter.Write(*p++ & mask, nBits);
            }
        }

        // remember this band type code and band subtype for the next band
        lastBandTypeCode = newBandTypeCode;
        lastBandSubType = newBandSubType;

        // advance past this band's samples
        firstSample += nSamples;
    }

    // success
    return true;
}

// Frame compression for the 1993a format (the DCS software version
// shipped with Indiana Jones and Judge Dredd).  This uses the same
// format as the 1993b software for Type 0 streams, and a completely
// different format for its Type 1 streams.
bool DCSEncoder::CompressFrame93a(BitWriter &bitWriter,
    int frameNo, Stream::Frame &frame, std::string &errorMessage)
{
    // OS93a and OS93b Type 0 streams are identical, so use the common
    // handler in the 93B encoder if we're preparing a Type 0 stream.
    auto &params = bitWriter.params;
    if (params.streamFormatType == 0)
        return CompressFrame93b(bitWriter, frameNo, frame, errorMessage);

    // We don't support encoding of OS93a Type 1 streams.  This format
    // is completely different from 93a/b Type 0, so it would need a
    // whole separate encoder implementation.  It would be possible to
    // add one, but at the moment I don't think it's worth the effort,
    // because (a) even without it, we can still patch 1993a ROMs with
    // new material, by using Type 0 streams, so we don't lose any
    // overall capabilities by omitting it; and (b) 93a Type 1 seems
    // to have been almost abandoned from the start, in that it was
    // only ever used in Judge Dredd, and only for about 10% of the
    // streams in that game.
    //
    // Note that the only reason we support encoding to the 1993
    // formats at all is for the sake of people who might want to 
    // patch one of the OS93 games.  Those creating whole new ROMs
    // can build them around OS94 or OS95 instead, which use the
    // improved OS94+ formats.  So we only need to support the OS93
    // formats well enough for patching in a few new tracks.  The 
    // main value of the alternative formats is that each format is
    // optimized for different source material characteristics, so
    // having access to more formats allows for better comperssion
    // by letting the encoder choose the format for each track that
    // yields the smallest output.  Omitting support for one of the
    // format alternatives thus doesn't sacrifice any functionality;
    // it just means some tracks won't compress as well as they might
    // have with more format options available.  Since our goal with
    // the 1993 formats is only to support patching of existing ROMs,
    // and since that use case by definition only involves encoding
    // a small amount of new material, optimization isn't all that
    // important.  (I really do mean "by definition": the defining
    // characteristic of a patch is that you're making limited
    // changes to an existing program.  If you're making extensive
    // changes, it's not a patch - it's a new work or at least a
    // new version.)
    // 
    // None of this precludes adding more thorough 1993 format
    // support in the future.  I just don't see much value in it
    // at the moment.
    //
    // So for now, just return an error if the caller attempts to
    // code to 93a Type 1.
    errorMessage = "This program doesn't currently support encoding to OS93a "
        "Type 1 streams. Specify Type 0 in the encoding parameters instead.";
    return false;
}


// --------------------------------------------------------------------------
//
// Raw frequency-domain frame
//
DCSEncoder::Stream::Frame::Frame(const float *src, const CompressionParams &params)
{
    // copy the raw sample set
    memcpy(f, src, 256 * sizeof(f[0]));

    // get the band sample count list for the format version
    const uint16_t *bandSampleCounts =
        (params.formatVersion == 0x9400) ? bandSampleCounts94 : bandSampleCounts93;

    // calculate the dynamic range in each band
    const float *pSrc = src;
    for (int band = 0 ; band < 16 ; ++band)
    {
        // scan for the range and power sum
        float lo = *pSrc++, hi = lo;
        float power = lo*lo;
        for (int j = bandSampleCounts[band] ; j > 1 ; --j)
        {
            float s = *pSrc++;
            power += s*s;
            if (s < lo)
                lo = s;
            if (s > hi)
                hi = s;
        }

        // store the range and power
        range[band] ={ lo, hi };
        this->power[band] = power;
    }
}

// --------------------------------------------------------------------------
//
// Bit stream writer
//


DCSEncoder::BitWriter::BitWriter()
{
    // Clear the header.  Set all elements to 0xFF, since this is the
    // code indicating the end of the valid header section.  (Actually,
    // only the low 7 bits matter, so 0x7F would also work.  But the
    // original DCS ROMs all use 0xFF fill, so we'll do the same for
    // the sake of appearances.)
    memset(header, 0xFF, sizeof(header));

    // Clear the current band type code list to zeroes.  This tracks
    // the contents of the corresponding buffer that the decoder uses
    // during decompression, and that starts off as all zeroes at the
    // start of each stream, so we have to do the same.
    memset(bandTypeCode, 0, sizeof(bandTypeCode));
}

void DCSEncoder::BitWriter::Write(uint32_t newBits, int nNewBits)
{
    // shift in as many bits as we have space for
    int avail = 32 - nBits;
    int copy = nNewBits <= avail ? nNewBits : avail;
    if (copy != 0)
    {
        bits <<= copy;
        bits |= ((newBits >> (nNewBits - copy)) & (0xFFFFFFFF >> (32 - copy)));
        nBits += copy;
    }

    // if we didn't copy all of the bits, flush the buffered bits and
    // shift in the remainder
    if (copy != nNewBits)
    {
        // count the copied bits
        nNewBits -= copy;

        // flush the buffered bits
        Flush();

        // shift in the remaining new bits
        bits = newBits & (0xFFFFFFFF >> (32 - nNewBits));
        nBits = nNewBits;
    }
}

void DCSEncoder::BitWriter::Flush()
{
    if (nBits != 0)
    {
        // shift the remaining bits to the most significant end of the buffer
        bits <<= (32 - nBits);

        // figure the number of bytes we need to store this many bits
        int nBytes = ((nBits + 7) / 8);

        // add a new chunk if necessary
        if (chunks.size() == 0 || chunks.back().nBytes + nBytes >= ChunkSize)
            chunks.emplace_back();

        // copy out the bytes from the most significant end
        auto &chunk = chunks.back();
        for (; nBytes != 0 ; --nBytes, bits <<= 8)
            chunk.data[chunk.nBytes++] = (bits >> 24);

        // clear the buffered bits
        bits = 0;
        nBits = 0;
    }
}

size_t DCSEncoder::BitWriter::CalcStreamSize() const
{
    // Figure the amount of space required for the DCS stream: two bytes
    // for frame count prefix, plus the size of the header, plus the
    // combined byte sizes of the packed bit stream chunks.
    size_t streamBytes = 2 + CalcStreamHeaderSize();
    for (auto &chunk : chunks)
        streamBytes += chunk.nBytes;

    // return the result
    return streamBytes;
}

size_t DCSEncoder::BitWriter::CalcStreamHeaderSize() const
{
    // For OS93a Type 1 streams, the stream header is one byte.
    // For all other stream types, the header is 16 bytes.
    if (params.formatVersion == 0x9301 && params.streamFormatType == 1)
        return 1;
    else
        return 16;
}

bool DCSEncoder::BitWriter::Store(DCSAudio &obj, int nFrames, std::string &errorMessage) const
{
    // allocate space for the contiguous stream representation
    obj.nBytes = CalcStreamSize();
    obj.nFrames = nFrames;
    obj.data.reset(new (std::nothrow) uint8_t[obj.nBytes]);
    
    // make sure we allocated the memmory
    if (obj.data == nullptr)
    {
        errorMessage = "Out of memory creating DCS stream";
        return false;
    }

    // store the frame count prefix, in big-endian format
    uint8_t *p = obj.data.get();
    *p++ = static_cast<uint8_t>((obj.nFrames >> 8) & 0xFF);
    *p++ = static_cast<uint8_t>(obj.nFrames & 0xFF);

    // store the stream header
    size_t headerSize = CalcStreamHeaderSize();
    memcpy(p, header, headerSize);
    p += headerSize;

    // store the compressed audio data bit stream, one chunk at a time
    for (auto &chunk : chunks)
    {
        // store this chunk
        memcpy(p, chunk.data, chunk.nBytes);
        p += chunk.nBytes;
    }

    // success
    return true;
}

// --------------------------------------------------------------------------
//
// String utilities
//

std::string DCSEncoder::vformat(const char *fmt, va_list va)
{
    // figure the buffer size required
    va_list va2;
    va_copy(va2, va);
    int len = _vscprintf(fmt, va2);
    va_end(va2);

    // validate the length
    if (len < 0)
        return "[Format Error]";

    // allocate the buffer and format the text into it
    std::unique_ptr<char> buf(new char[len + 1]);
    vsprintf_s(buf.get(), len + 1, fmt, va);

    // return a std::string constructed from the buffer
    return std::string(buf.get());
}

std::string DCSEncoder::format(const char *fmt, ...)
{
    // format via vformat()
    va_list va;
    va_start(va, fmt);
    auto str = vformat(fmt, va);
    va_end(va);

    // return the result
    return str;
}
