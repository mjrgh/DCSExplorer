// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder - Encode an audio file
//
// This module contains DCSEncoder::EncodeFile(), separating it from the main
// encoder implementation in order to make its inclusion in the build optional.
// The reason that you might want to omit this module is that it depends upon
// a third-party library, libnyquist, so including this module also requires
// building and linking libnyquist into final product.  And conversely,
// removing this module from the build also eliminates all dependencies on
// libnyquist, so you can also remove that from the build if you remove this
// module.  libnyquist's role in the project is to decode MP3, Ogg, and FLAC
// files, for transcoding from those formats into DCS streams.  A project that
// doesn't need to read from any of those formats could therefore omit this
// module from the build and still use the rest of the DCSEncoder class.  The
// DCSEncoder class has a built-in WAV file reader that doesn't require any
// external libraries, and you don't need any sort of file reader if your
// PCM audio source isn't a file in the first place, such as reading directly
// from a system audio input device.

#include <stdio.h>
#include <stdlib.h>
#include "DCSEncoder.h"
#include "../libnyquist/include/libnyquist/Decoders.h"

#pragma comment(lib, "libnyquist")

bool DCSEncoder::EncodeFile(const char *filename, DCSAudio &dcsObj, 
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

    // Check for DCS files created from DCSExplorer's raw DCS stream
    // extraction function.  This allows exporting a stream from one game
    // ROM set and reusing it in another game, without any transcoding,
    // as long as the two games use compatible versions of the firmware.
    if (IsDCSFile(filename))
        return EncodeDCSFile(filename, dcsObj, errorMessage);

    // load the file
    nqr::AudioData fileData;
    nqr::NyquistIO loader;
    try
    {
        loader.Load(&fileData, filename);
    }
    catch (std::exception &e)
    {
        return Status(OpenStreamStatus::Error,
            format("Error loading file \"%s\": %s", filename, e.what()).c_str());
    }

    // we can only handle mono or stereo format
    if (!(fileData.channelCount == 1 || fileData.channelCount == 2))
    {
        return Status(OpenStreamStatus::Error,
            format("File \"%s\" has an unsupported channel format (only mono and stereo are supported)").c_str());
    }

    // create a DCS encoder stream
    std::unique_ptr<Stream> stream(OpenStream(fileData.sampleRate, errorMessage));
    if (stream == nullptr)
        return Status(OpenStreamStatus::Error);

    const float *p = fileData.samples.data();
    const float *endp = p + fileData.samples.size();
    while (p < endp)
    {
        // process the samples per the format
        float samples[256];
        int nSamples;
        for (nSamples = 0 ; nSamples < 256 && p < endp ; )
        {
            // fetch the next sample
            float sample = *p++;

            // if it's a stereo source, read another sample, and average the two
            // channels to form a mono signal for the DCS input
            if (fileData.channelCount == 2 && p < endp)
                sample = (sample + *p++) / 2.0f;

            // buffer it
            samples[nSamples++] = sample;
        }

        // send the samples to the DCS encoder
        WriteStream(stream.get(), samples, nSamples);
    }

    // close the stream
    if (!CloseStream(stream.get(), dcsObj, errorMessage))
        return Status(OpenStreamStatus::Error);

    // success
    return Status(OpenStreamStatus::OK);
}
