// Copyright 2023 Michael J Roberts
// BSD 3-clause license - NO WARRANTY
//
// DCS Encoder
// 
// This class encodes DCS audio streams from PCM data.  The streams
// use the same audio format as the original DCS ROMs in the Williams/
// Bally/Midway pinball titles released from 1994 through 1998, and
// can be played back through the original software in those ROMs.
// 
// (The three DCS pinball titles released in 1993 [Indiana Jones: The
// Pinball Adventure, Star Trek: The Next Generation, and Judge Dredd]
// used a different, incompatible encoding format that this encoder
// doesn't support.  However, the audio board on those games is
// identical to the boards on the 1994-early 1995 games, so DCSEncoder
// streams can still be played back on the 1993 gmaes as long as you
// also replace the ROM software with the 1994 version.  There's no
// compatibility issue at the hardware level; it's just a matter of
// the software version in the 1993 games.)
//
// This class is designed primarily for use with the DCS Encoder 
// application, but it doesn't have any dependencies on that project,
// so it could easily be used as a standalone class.  The encoding
// process is fairly straightforward:
//
//   - Create a DCSEncoder instance
//   - call encoder->OpenStream() to set up a new audio stream
//   - call encoder->WriteStream() repeatedly to write the PCM samples
//     making up the audio clip
//   - call encoder->CloseStream() to close the stream; this returns
//     the encoded DCS audio clip as a byte array
//
// Alternatively, EncodeFile() lets you transcode an MP3, Ogg, FLAC,
// or WAV file to a DCS stream in a single function call.

#pragma once
#include <stdint.h>
#include <string>
#include <memory>
#include <list>
#include <functional>
#include "../libsamplerate/src/samplerate.h"

class DCSEncoder
{
protected:
	struct Stream;

public:
	DCSEncoder();
	~DCSEncoder();

	// Public typedef for internal Stream type.  Clients use Stream
	// objects as handles for operating on streams, so we need to define
	// the type, but the struct contents are only for our internal use.
	typedef struct Stream Stream;

	// Encoded DCS audio object descriptor.  This represents the binary 
	// data for an encoded DCS audio stream.
	struct DCSAudio
	{
		size_t nBytes = 0;               // size in bytes
		int nFrames = 0;                 // number of DCS frames (7.68ms each)
		std::unique_ptr<uint8_t> data;   // encoded stream data
	};

	// Compression quality parameters.  These can be adjusted by the 
	// caller prior to encoding a stream to provide guidance to the
	// compression algorithm.
	struct CompressionParams
	{
		// Format version.  This specifies the software version
		// we're encoding for.  There are there different, mutually
		// incompatible formats:
		//
		//   0x9301  - the "1993a" format, used by Indiana Jones and
		//             Judge Dredd
		//
		//   0x9302  - the "1993" format, used by Star Trek: The Next
		//             Generation
		//
		//   0x9400  - the "1994+" format, used by every other DCS
		//             pinball title
		//
		// Currently, we can only create recordings for the 1994+
		// format.  The format code is thus mostly to allow for
		// future work to support the two 1993 format variations.
		uint16_t formatVersion = 0x9400;

		// Stream format major type:
		//
		//   Type 0 -> fixed scaling factor per band in every frame
		//   Type 1 -> variable scaling factor per band, per frame
		//
		// Format Type 1 uses a collection of pre-determined bit
		// codings that appear to have been hand-tuned, prsumably to
		// optimize the balance between quality and compression ratio.
		// Almost all of the streams in the original DCS ROMs use the
		// Type 1 option, although a few use Type 0, so the original
		// encoder probably used some sort of optimization criteria
		// to decide which to use on a track-by-track basis.
		// 
		// The special value -1 instructs the encoder to try both
		// formats and pick the one that yields the smaller result.
		int streamFormatType = 1;

		// Stream format minor type, 0 to 3.  This affects the
		// scaling range per frame; type 3 has a wider scaling 
		// range than type 0, which probably works better for 
		// material with higher dynamic range.  Nearly all of the 
		// original DCS ROMs use subtype 3.
		// 
		// All versions of the original ADSP-2105 DCS decoder
		// program treat subtypes 1, 2, and 3 identically.  From
		// examining the code, it appears that the DCS designers
		// actually intended to implement four distinct subtypes,
		// in that the code that checks the subtypes selects a
		// data table based on the subtype, and there are four
		// such tables embedded in the code.  The idea that four
		// subtypes were intended is corroborated by the use of
		// sutbyptes 1 and 2 in a few streams in the original DCS
		// ROMs.  Whatever the intention was, the reality is that
		// the part of the decoder program that checks the subtype
		// ends up treating all of the non-zero subtypes as
		// subtype 3.  It's even pretty clear looking at that
		// code that the arithmetic was intended to distinguish
		// the three types and just got it wrong due to a small
		// arithmetic error.
		//
		// The special value -1 instructs the encoder to try both
		// subtype options (0 and 3) and pick the one that yields
		// the smaller result.  (There's no point in also trying
		// subtypes 1 and 2, because the decoder treats those as
		// identical to subtype 3.)
		int streamFormatSubType = 3;

		// Power band cutoff.  The DCS format encodes the audio data
		// in the frequency domain, dividing the frequency spectrum
		// into 16 bands of about 1kHz each.  One method it uses to
		// reduce the data size is to discard some of the higher
		// frequency bands entirely.  This can often be done without
		// much audible effect because the highest few bands typically
		// contain only a small portion of the overall audio power, 
		// hence their contribution to the overall signal is small,
		// hence discarding them has little or no audible effect.
		// This parameter lets you adjust the cutoff point that the
		// encoder uses to decide which bands to keep and which to
		// drop in the final recording.  The value specifies a
		// fraction of the total RMS audio power in the signal to
		// keep.  The encoder averages the power across all of the
		// frames, and then adds up the cumulative power in each
		// band, starting at the lower-frequency band.  The band
		// where the cumulative power reaches this fraction is the
		// cutoff point: the remaining higher frequency bands are
		// omitted from the final signal.  Setting this to 1.0 will
		// force the encoder to retain all of the bands.
		float powerBandCutoff = 0.97f;

		// Target bit rate, in bits per second.  The encoder uses
		// this to set the initial frequency band scaling factors, 
		// which determine the approximate number of bits needed to
		// encode the individual audio samples in the band.  This
		// setting is only a hint - the encoder doesn't attempt to
		// maintain a constant bit rate matching this setting, but
		// merely uses it to set up the encoding such that typical
		// material will stay around this rate.
		int targetBitRate = 128000;

		// Minimum dynamic range for an encoded band per frame.
		// If the dynamic range in a frame for a given band is below
		// this limit, we'll simply omit the band from the frame.
		float minimumDynamicRange = 10.0f / 32768.0f;

		// Quantization error limit.  This sets the upper limit on 
		// the RMS quantization error per band per frame.  The
		// encoder will attempt to choose an encoding for each band
		// that produces an error at or below this limit.
		float maximumQuantizationError = 10.0f / 32768.0f;

	};
	CompressionParams compressionParams;

	// Encode an MP3, Ogg Vorbis, FLAC, or WAV file.  The function inspects
	// the file's contents to determine which audio format is uses and
	// transcodes the audio data into a DCS stream.  On success, the new DCS
	// stream object is loaded into dcsObj, and the function returns true.
	// On failure, errorMessage is set to a descriptive erorr string, and the
	// function returns false.
	enum class OpenStreamStatus;
	bool EncodeFile(const char *filename, DCSAudio &dcsObj, 
		std::string &errorMessage, OpenStreamStatus *statusPtr = nullptr);

	// Encode a WAV file into a DCS audio stream.  This loads the contents of
	// the WAV file, and encodes it into a DCS stream.  On success, the new
	// stream object is loaded into dcsObj, and the function returns true.  On
	// error, errorMessage is set to a descriptive error string, and the
	// function returns false.
	//
	// This function does the same thing as passing a WAV file to EncodeFile().
	// The only difference is in the implementation; this version reads the WAV
	// file directly, whereas EncodeFile() uses a third-party library to read
	// the file contents.  A more minimal 
	bool EncodeWAVFile(const char *filename, DCSAudio &dcsObj, std::string &errorMessage);

	// Encode a DCS file into a DCS audio stream.  This loads the contents of
	// a raw DCS stream file created with DCS Explorer and creates a new
	// stream object from the loaded data.  If the stream uses a format
	// version that's compatible with the current encoding parameters, the
	// original raw data stream is returned without any re-encoding.  If it
	// uses an incompatible version, we'll automatically decode the stream
	// and re-encode it using the current encoding parameters.
	bool EncodeDCSFile(const char *filename, DCSAudio &dcsObj, 
		std::string &errorMessage, OpenStreamStatus *statusPtr = nullptr);

	// Determine if a file is a raw DCS stream file, containing an audio
	// stream extracted in raw format from a DCS ROM by DCS Explorer.
	// Returns true if it has a valid DCS raw stream file header, false if
	// not. 
	//
	// If 'formatVersion' is not null, and the file has a valid DCS raw
	// stream file header, *formatVersion is filled in with the version
	// code for the format, as used in CompressionParams::formatVersion
	// (0x9301 for 1993a, 0x9302 for 1993b, 0x9400 for 1994+).
	static bool IsDCSFile(const char *filename, int *formatVersion = nullptr);

	// Begin a new audio stream.  Creates and returns a new stream object, 
	// which can be used to write PCM data into a DCS object.  If an error 
	// occurs, returns null and fills in the error string with a descriptive 
	// message.
	enum class OpenStreamStatus
	{
		OK,                   // success
		UnsupportedFormat,    // target DCS format is unsupported
		LibSampleRateError,   // error initializing libsamplerate stream
		OutOfMemory,          // no memory for new objects
		Error                 // other unclassified error
	};
	Stream *OpenStream(int sampleRate, std::string &errorMessage, OpenStreamStatus *status = nullptr);

	// Write samples to a stream in progress.  Samples can be provided
	// as signed INT16 values (-32768 to +32767) or as floats normalized
	// to -1.0f to +1.0f.
	void WriteStream(Stream *stream, const int16_t *pcm, size_t numSamples);
	void WriteStream(Stream *stream, const float *pcm, size_t numSamples);

	// End the current stream.  This fills in any unused space at the end
	// of the last frame with silence and passes back the encoded DCS audio
	// object.
	bool CloseStream(Stream *stream, DCSAudio &obj, std::string &errorMessage);

	// string utilities
	static std::string format(const char *fmt, ...);
	static std::string vformat(const char *fmt, va_list va);

protected:
	// Huffman codebook entry.  The DCS format uses Huffman-type codes,
	// with varying-bit-length codewords, for a number of items.  The
	// codebooks are pre-defined and specific to the contexts where
	// they're used.  This struct is for creating mapping tables from
	// plain numerical values to the corresponding codewords.  An array
	// of these forms an encoding codebook for a particular context.
	struct CodebookEntry
	{
		// the original numerical value (the "plaintext")
		int plainValue;

		// The corresponding encoded bit string.  These codes are
		// inherently varying-bit-length, so the entry only uses as
		// many bits of this UINT32 as it actually needs, right-
		// justifying them (that is, placing them at the least
		// significant end, with the unused high bits set to zero).
		uint32_t codeWord;

		// Number of bits of the code word actually used.
		int nBits;
	};

	// Stream descriptor.  This encpasulates a stream in progress.
	struct Stream
	{
		Stream(int sampleRate);
		~Stream();

		// libsamplerate context
		SRC_STATE *lsrState = nullptr;

		// sample rate ratio between the input stream and the DCS native rate
		double sampleRateRatio = 1.0;

		// Buffered input samples.  We collect input samples here, after
		// conversion to the DCS sample rate, until we have enough samples
		// to form a complete frame, at which point we send them to the
		// DCS encoder.
		float inputBuf[256];
		int nInputBuf = 0;

		// Overall dynamic range per band, across the whole stream
		struct Range { float lo; float hi; };
		Range range[16];

		// sum of power by band, for averaging
		float powerSum[16];

		// Transformed frame list.  As we process PCM input samples, we
		// break them into frames (fixed time windows), and transform each
		// frame into the frequency domain for storage.  These frequency
		// domain frames correspond to DCS frames; the final DCS stream
		// consists of the series of frames, in time order, encoded in
		// the DCS compression format.  We store the uncompressed frames
		// here during the initial pass over the PCM stream so that we
		// can defer selecting the compression parameters until after
		// analyzing all of the input.
		struct Frame
		{
			Frame(const float *src, const CompressionParams &params);

			// Frequency-domain samples - this is the frame buffer
			// after the transform into the frequency domain.  This
			// is the uncompressed version of the frame; the frame as
			// stored in the DCS stream will be encoded via the DCS
			// compression scheme.  Note that 
			float f[256];

			// dynamic range of samples by band
			Range range[16];

			// power by band
			float power[16];
		};
		std::list<Frame> frames;
	};

	// Bit writer.  DCS streams are encoded in code words of varying
	// bit width, packed together with no alignment on byte boundaries,
	// so we need to be able to write in units of arbitrary numberes of
	// bits.  This bit writer handles that by buffering 32 bits at a
	// time and then writing them out to the stream.  The writer
	// stores the stream in chunks, allocated on demand.
	struct BitWriter
	{
		BitWriter();

		// Write bits.  The bits are passed in the least significant
		// bits of val.  Up to 32 bits can be written at once.
		void Write(uint32_t val, int nBits);

		// Write a codebook entry
		void Write(const CodebookEntry &code) { Write(code.codeWord, code.nBits); }

		// flush the buffered bits
		inline void Flush();

		// figure the overall stream size in bytes
		size_t CalcStreamSize() const;

		// calculate the stream header size
		size_t CalcStreamHeaderSize() const;

		// store the stream in a DCS object
		bool Store(DCSAudio &obj, int nFrames, std::string &errorMessage) const;

		// compression parameters used to create the stream
		CompressionParams params;

		// current buffered bits
		uint32_t bits = 0;

		// number of bits currently buffered
		int nBits = 0;

		// output chunk
		static const size_t ChunkSize = 1024;
		struct Chunk
		{
			uint8_t data[ChunkSize];
			size_t nBytes = 0;
		};

		// DCS stream header
		uint8_t header[16];

		// Current band type code buffer.  Band type codes are stored
		// differentially from one frame to the next, so we need to keep
		// track of the current code as we work through the frame list.
		int bandTypeCode[16];

		// output chunk list
		std::list<Chunk> chunks;
	};

	// write zero or more samples to a steram
	void WriteStream(Stream *stream, const int16_t *pcm, size_t numSamples, bool eof);
	void WriteStream(Stream *stream, const float *pcm, size_t numSamples, bool eof);

	// Compress a stream.  This is called after finishing a stream with
	// CloseStream(), to generate the compressed stream with the given
	// parameters.
	//
	// Note that the parameters must specify a concrete format type and
	// subtype - the special wildcard value -1 isn't allowed here.
	bool CompressStream(Stream *stream, BitWriter &w, int bandsToKeep,
		std::string &errorMessage);

	// Transform a frame from PCM samples to a DCS-formatted frequency-
	// domain frame.  This creates the uncompressed version of the DCS 
	// frame and adds it to the stream's sample list.
	void TransformFrame(Stream *stream);

	// Perform the DCS Discrete Fourier Transform (DFT), using "algorithm 3"
	// from my DCS technical reference document.  This is a much simpler
	// approach from the inverse decoder algorithm, but accomplishes the
	// same transform, albeit slightly less efficiently.  I prefer the
	// simpler approach here to the inverse-decoder-transform, since the
	// decoder transform is so much more complicated for very little gain 
	// in efficiency.  This one is much easier to understand.
	void DFTAlgorithmNew(float fbuf[258], Stream *stream);

	// Perform the DCS Discrete Fourier Transform (DFT), using the
	// mathematical inverse of the transform algorithm implemented in the
	// original ADSP-2105 OS94 decoders.
	void DFTAlgorithmOrig(float fbuf[258], Stream *stream);

	// Dual FFT.  This performs two independent FFTs: one on the even-numbered
	// elements, and one on the odd-numbered elements.  It works as though we
	// populated the array with just the even-numbered elements (zeroing the
	// odd-numbered elements) and ran a normal FFT, and then did the same thing
	// again with just the odd-numbered elements populated.  The final result
	// is the concatenation of the two arrays.
	//
	// Outputs[0..63]   = DFT(x0, 0, x2, 0, x4, 0, x6, ..., x126, 0)
	// Outputs[64..127] = DFT(0, x1, 0, x3, 0, x5, 0, x7, ..., 0, x127)
	//
	// The DCS decoder performs the inverse of this transform by running the
	// Cooley-Tukey iterative in-place algorithm for one iteration short of
	// the full transform.  That has the effect of performing two independent
	// FFTs on the halves of the data set with alternating elements omitted.
	void DualFFT(float outbuf[256], const float inbuf[256]);

	// Compress a frame.  This takes a frame in the DCS raw format and
	// appends it to the compressed bit stream.  We need separate
	// implementations for the different format versions since their
	// details are so different.

	// 94+ version - used for all software versions except those shipped
	// with the first three games released in 1993 (IJTPA, JD, STTNG)
	bool CompressFrame94(BitWriter &bitWriter,
		int frameNo, Stream::Frame &frame, std::string &errorMessage);

	// 93a version - used for IJTPA and JD only
	bool CompressFrame93a(BitWriter &bitWriter, 
		int frameNo, Stream::Frame &frame, std::string &errorMessage);

	// 93b version - used for STTNG only
	bool CompressFrame93b(BitWriter &bitWriter,
		int frameNo, Stream::Frame &frame, std::string &errorMessage);

	// Find the best compression format option for a band.  This tries
	// each available band type code option, and returns the one that
	// yields the smallest encoding (smallest bit width) while meeting
	// the parameter setting for maximum quantization error.  When two
	// or more encodings of equal size pass the maximum error test, the
	// one of those that minimizes the error is selected.  When no
	// encodings pass the maximum error test, the one from the whole
	// set that minimizes error is selected.
	//
	// minNewCode and maxNewCode limit the search range for the new
	// code.  The stream format encodes each new band code as a delta
	// from the previous code using a Huffman-type coding, and some of
	// codebooks don't have code points for every possible delta across
	// the full 0-15 range that the band types can take on.  The limits
	// vary by stream type, so we need the caller to tell us the limits
	// that apply in the current context.  Note that the limits can be
	// outside of the 0..15 range - that's fine, since we only search
	// the intersection of the inherent 0..15 range and the caller's
	// specified min..max range.
	struct BandEncoding 
	{ 
		int bitWidth;		// bit width of the encoding
		int scaleCode;		// scaling factor code
		int refVal;			// reference value - zero point for encoded values
	};
	struct BandTestResult
	{
		int bandTypeCode = -1;		// this band's type code
		float errSum = 0.0f;		// error sum for this encoding
		int bitWidth = 0;			// encoding bit width
		bool pass = false;			// does the error sum pass the Maximum Error parameter test?
	};
	BandTestResult FindBestBandEncoding(const CompressionParams &params,
		std::function<BandEncoding(int band, int bandTypeCode)> interpretBandTypeCode,
		int minNewCode, int maxNewCode, int band, const float *firstSample, int nSamples);

	// Get the best of a set of test results.  Returns the index of the
	// best result from the provided set.
	int FindBestResult(const BandTestResult *results, int nResults);

	// bit-reversed indexing table for a 9-bit address space
	int bitRev9[512];
};
