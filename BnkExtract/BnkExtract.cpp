// BnkExtract.cpp - Extract audio streams from DCS2 .BNK files to WAV
//
// Usage:
//   BnkExtract <file.BNK> [file.LST] [--outdir <dir>]
//
// Parses the BNK track index, decodes each audio stream via DCSDecoderNative
// in standalone mode (no ROM required), and writes one WAV per track.
// If a matching .LST file is supplied, track descriptions are used as filenames.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <map>
#include <filesystem>

#include "../DCSDecoder/DCSDecoder.h"
#include "../DCSDecoder/DCSDecoderNative.h"

// ---------------------------------------------------------------------------
// BNK file layout constants
// ---------------------------------------------------------------------------

static const size_t BNK_COUNT_OFFSET      = 0x80;  // LE uint16 - number of index slots
static const size_t BNK_INDEX_OFFSET      = 0x86;  // LE uint32 per slot, relative to DATA_BASE
static const size_t BNK_DATA_BASE         = 0x5FA; // start of playlist + stream data
static const size_t BNK_PLAYLIST_STRIDE   = 18;    // bytes per playlist program entry
static const size_t BNK_STREAM_ADDR_OFF   = 11;    // offset within playlist entry: 3-byte BE relative stream addr
// The stored addr is relative to the addr field's own file position (var_start = playlistOff + BNK_STREAM_ADDR_OFF).
// absolute_stream_addr = stored_BE24 + var_start
// The stream at absolute_stream_addr is standard DCS format: [U16 BE nFrames][16-byte header][packed bits]

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read_le32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
static uint32_t read_be24(const uint8_t *p) { return (p[0]<<16) | (p[1]<<8) | p[2]; }
static uint16_t read_be16(const uint8_t *p) { return (uint16_t)((p[0]<<8) | p[1]); }

// Sanitise a description string for use as a filename component.
static std::string sanitise(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<'  || c == '>' || c == '|')
            out += '_';
        else if ((unsigned char)c < 32)
            out += '_';
        else
            out += c;
    }
    // trim trailing spaces/underscores
    while (!out.empty() && (out.back() == ' ' || out.back() == '_'))
        out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// LST parser  -  builds map: track_number -> description string
// ---------------------------------------------------------------------------

static std::map<int, std::string> parseLst(const char *lstPath)
{
    std::map<int, std::string> result;
    FILE *fp = nullptr;
    if (fopen_s(&fp, lstPath, "r") != 0 || fp == nullptr)
        return result;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // Lines of interest look like:
        //   "  1    $0001   1       84.5    97.0  _ With the running one-hander"
        // Fields: dec  $hex  ntrks  video  pinball  description...
        int dec;
        int hex;
        if (sscanf_s(line, " %d $%x", &dec, &hex) == 2) {
            // description starts after the 5th whitespace-delimited token
            const char *p = line;
            int tokens = 0;
            while (*p && tokens < 5) {
                while (*p == ' ' || *p == '\t') ++p;  // skip whitespace
                while (*p && *p != ' ' && *p != '\t') ++p;  // skip token
                ++tokens;
            }
            while (*p == ' ' || *p == '\t') ++p;  // skip whitespace before description
            // strip trailing newline
            std::string desc(p);
            while (!desc.empty() && (desc.back() == '\n' || desc.back() == '\r' || desc.back() == ' '))
                desc.pop_back();
            // strip leading underscore+space that some entries use as a prefix
            if (desc.size() >= 2 && desc[0] == '_' && desc[1] == ' ')
                desc = desc.substr(2);
            if (!desc.empty())
                result[hex] = desc;
        }
    }
    fclose(fp);
    return result;
}

// ---------------------------------------------------------------------------
// WAV writer  -  mirrors the approach in DCSExplorer.cpp
// ---------------------------------------------------------------------------

static bool writeWav(const char *path, DCSDecoder *decoder, uint16_t nFrames)
{
    // Two extra frames ensures playback tapers to silence
    nFrames += 2;

    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || fp == nullptr) {
        printf("  ERROR: cannot open output file \"%s\" (errno %d)\n", path, errno);
        return false;
    }

    // 44-byte RIFF/WAV header  (31250 Hz, mono, 16-bit PCM)
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    uint32_t dataBytes = (uint32_t)nFrames * 240 * 2;
    memcpy(&hdr[0], "RIFF\0\0\0\0WAVEfmt ", 16);
    *reinterpret_cast<uint32_t*>(&hdr[4])  = dataBytes + 44 - 8;
    *reinterpret_cast<uint32_t*>(&hdr[16]) = 16;
    *reinterpret_cast<uint16_t*>(&hdr[20]) = 1;
    *reinterpret_cast<uint16_t*>(&hdr[22]) = 1;
    *reinterpret_cast<uint32_t*>(&hdr[24]) = 31250;
    *reinterpret_cast<uint32_t*>(&hdr[28]) = 31250 * 2;
    *reinterpret_cast<uint16_t*>(&hdr[32]) = 2;
    *reinterpret_cast<uint16_t*>(&hdr[34]) = 16;
    memcpy(&hdr[36], "data", 4);
    *reinterpret_cast<uint32_t*>(&hdr[40]) = dataBytes;

    bool ok = (fwrite(hdr, 44, 1, fp) == 1);

    for (uint16_t frame = 0; frame < nFrames && ok; ++frame) {
        int16_t buf[240];
        for (int si = 0; si < 240; ++si)
            buf[si] = decoder->GetNextSample();
        ok = (fwrite(buf, sizeof(buf), 1, fp) == 1);
    }

    ok = (fclose(fp) == 0) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Track entry
// ---------------------------------------------------------------------------

struct Track {
    int      trackNum;
    uint32_t streamAddr;   // absolute file offset of DCS stream: [U16 nFrames][16-byte hdr][data]
    uint8_t  mixLevel;     // mixing level from playlist entry byte[6]
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    printf("BnkExtract - DCS2 BNK audio extractor\n\n");

    // --- parse arguments ---
    const char *bnkPath = nullptr;
    const char *lstPath = nullptr;
    std::string outDir  = ".";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--outdir") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (!bnkPath) {
            bnkPath = argv[i];
        } else if (!lstPath) {
            lstPath = argv[i];
        }
    }

    if (!bnkPath) {
        printf("Usage: BnkExtract <file.BNK> [file.LST] [--outdir <dir>]\n");
        return 1;
    }

    // --- load BNK ---
    FILE *fp = nullptr;
    if (fopen_s(&fp, bnkPath, "rb") != 0 || fp == nullptr) {
        printf("ERROR: cannot open \"%s\"\n", bnkPath);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    size_t bnkSize = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bnk(bnkSize);
    if (fread(bnk.data(), 1, bnkSize, fp) != bnkSize) {
        printf("ERROR: read failed for \"%s\"\n", bnkPath);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    // --- validate header ---
    if (bnkSize < BNK_DATA_BASE + 18) {
        printf("ERROR: file too small to be a valid BNK\n");
        return 1;
    }
    // BNK files start with "DCS " ASCII
    if (bnk[0] != 'D' || bnk[1] != 'C' || bnk[2] != 'S' || bnk[3] != ' ') {
        printf("ERROR: not a DCS BNK file (missing 'DCS ' header)\n");
        return 1;
    }

    // Print the title string from offset 0
    char title[64];
    memcpy(title, bnk.data(), 63);
    title[63] = '\0';
    // null-terminate at first 0xFF
    for (int i = 0; i < 63; ++i) if ((uint8_t)title[i] == 0xFF) { title[i] = '\0'; break; }
    printf("BNK: %s\n", title);
    printf("File: %s  (%zu bytes)\n\n", bnkPath, bnkSize);

    // --- parse index ---
    uint16_t slotCount = read_le16(bnk.data() + BNK_COUNT_OFFSET);
    if (BNK_INDEX_OFFSET + (size_t)slotCount * 4 > bnkSize) {
        printf("ERROR: index extends beyond file\n");
        return 1;
    }

    // Collect valid tracks
    std::vector<Track> tracks;
    for (int i = 0; i < (int)slotCount; ++i) {
        uint32_t rel = read_le32(bnk.data() + BNK_INDEX_OFFSET + i * 4);
        if (rel == 0xFFFFFFFF) continue;
        if (i == 0 && rel == 0) continue;  // slot 0 unused sentinel

        size_t playlistOff = BNK_DATA_BASE + rel;
        if (playlistOff + BNK_PLAYLIST_STRIDE > bnkSize) continue;

        // The stored address is relative to the address field's own file position.
        size_t varStart = playlistOff + BNK_STREAM_ADDR_OFF;
        uint32_t storedAddr = read_be24(bnk.data() + varStart);
        if (storedAddr == 0) continue;
        uint32_t streamAddr = storedAddr + (uint32_t)varStart;
        if (streamAddr + 18 >= bnkSize) continue;

        Track t;
        t.trackNum   = i;
        t.streamAddr = streamAddr;
        t.mixLevel   = bnk[playlistOff + 6];  // byte[6] = channel mixing level
        tracks.push_back(t);
    }

    if (tracks.empty()) {
        printf("ERROR: no valid tracks found in BNK\n");
        return 1;
    }

    printf("Found %zu tracks\n\n", tracks.size());

    // --- load LST names ---
    std::map<int, std::string> names;
    if (lstPath)
        names = parseLst(lstPath);

    // --- ensure output directory exists ---
    std::filesystem::create_directories(outDir);

    // --- set up decoder ---
    DCSDecoder::MinHost host;
    DCSDecoderNative decoder(&host);
    decoder.InitStandalone(DCSDecoder::OSVersion::OS95);
    decoder.SoftBoot();
    decoder.SetMasterVolume(0xFF);

    // --- extract each track ---
    int nOk = 0, nError = 0, nSkip = 0;
    for (auto &t : tracks) {
        // Build output filename
        std::string name;
        auto it = names.find(t.trackNum);
        if (it != names.end())
            name = sanitise(it->second);
        if (name.empty())
            name = "track";

        char filename[512];
        snprintf(filename, sizeof(filename), "%s/%s_%04X.wav",
            outDir.c_str(), name.c_str(), t.trackNum);

        // Stream at streamAddr is standard DCS format: [U16 BE nFrames][16-byte hdr][data]
        DCSDecoder::ROMPointer streamPtr(0, bnk.data() + t.streamAddr);

        auto info = decoder.GetStreamInfo(streamPtr);
        if (info.nFrames <= 0) {
            printf("  SKIP $%04X  (GetStreamInfo returned 0 frames)\n", t.trackNum);
            ++nSkip;
            continue;
        }

        decoder.SoftBoot();
        decoder.LoadAudioStream(0, streamPtr, t.mixLevel);

        bool ok = writeWav(filename, &decoder, (uint16_t)info.nFrames);
        if (ok) {
            printf("  OK   $%04X  %5d frames  %s\n", t.trackNum, info.nFrames, filename);
            ++nOk;
        } else {
            printf("  ERR  $%04X  %s\n", t.trackNum, filename);
            ++nError;
        }
    }

    printf("\nDone: %d written, %d errors, %d skipped\n", nOk, nError, nSkip);
    return (nError > 0) ? 1 : 0;
}
