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
static const size_t BNK_INDEX_OFFSET      = 0x86;  // LE uint32 per slot, relative to data base
// Data base (playlist table start) = BNK_INDEX_OFFSET + slotCount * 4  (computed at runtime)
static const size_t BNK_PLAYLIST_STRIDE   = 18;    // bytes per playlist entry
static const size_t BNK_PROGRAM_HDR       = 2;     // bytes before DCS bytecode in each playlist entry
// Each playlist entry: [2-byte prefix: type, channel] [DCS track program bytecode]
// Bytecode format: [U16 BE countPrefix][U8 opcode][args] ...
//   0x00 = Stop
//   0x01 = Play  [U8 ch][U24 BE relative stream addr][U8 loop]
//   0x07-0x09 = Mixing level set   [U8 ch][U8 val]
//   0x0A-0x0C = Mixing level fade  [U8 ch][U8 val][U16 steps]
//   0x0D = NOP
//   0x0E = Loop start  [U8 count]
//   0x0F = Loop end
// Stream addr in opcode 0x01 is relative to its own 3-byte field position.
// Stream is standard DCS: [U16 BE nFrames][16-byte header][packed bits]

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
// WAV writer - writes nFrames + 2 tail frames from the decoder
// ---------------------------------------------------------------------------

static bool writeOneWav(const char *path, DCSDecoderNative &decoder,
                        const uint8_t *bnkData, uint32_t absAddr,
                        uint8_t mixLevel, int nFrames)
{
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || fp == nullptr) {
        printf("  ERROR: cannot open \"%s\" (errno %d)\n", path, errno);
        return false;
    }

    uint32_t wavFrames = (uint32_t)nFrames + 2;
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    uint32_t dataBytes = wavFrames * 240 * 2;
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

    DCSDecoder::ROMPointer streamPtr(0, bnkData + absAddr);
    decoder.SoftBoot();
    decoder.LoadAudioStream(0, streamPtr, mixLevel);
    for (uint32_t f = 0; f < wavFrames && ok; ++f) {
        int16_t buf[240];
        for (int s = 0; s < 240; ++s) buf[s] = decoder.GetNextSample();
        ok = (fwrite(buf, sizeof(buf), 1, fp) == 1);
    }

    ok = (fclose(fp) == 0) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Track entry - one or more streams played sequentially
// ---------------------------------------------------------------------------

struct StreamRef {
    uint32_t absAddr;   // absolute file offset of DCS stream
    uint8_t  mixLevel;
    uint8_t  loopCount; // 0 = play once (stored as 1 in bytecode), >1 = repeat N times
};

struct Track {
    int                   trackNum;
    size_t                poff;     // file offset of the 18-byte playlist entry
    std::vector<StreamRef> streams;
};

// ---------------------------------------------------------------------------
// Disassemble DCS track program bytecode, returning a formatted string in the
// same style as DCSDecoder::ExplainTrackProgram / DCSExplorer -p output.
// poff = file offset of the 18-byte playlist entry; bnkSize guards reads.
// linePrefix is prepended to every output line (e.g. "    ").
// ---------------------------------------------------------------------------
static std::string disassembleProgram(
    const uint8_t *bnk, size_t bnkSize, size_t poff, const char *linePrefix)
{
    std::string out;
    size_t p = poff + BNK_PROGRAM_HDR;

    uint8_t trackChannel = (poff < bnkSize) ? bnk[poff + 1] : 0;

    std::string loopIndent;
    const char *perLoopIndent = "  ";
    std::vector<uint8_t> loopCountStack;  // loop counts, to detect infinite-loop termination

    auto appendLine = [&](const std::string &instr, const std::string &hexDesc) {
        if (!out.empty()) out += "\n";
        char buf[256];
        snprintf(buf, sizeof(buf), "%-60s    // %s",
            (loopIndent + instr).c_str(), hexDesc.c_str());
        out += linePrefix;
        out += buf;
    };

    for (int guard = 0; guard < 4096 && p + 2 < bnkSize; ++guard) {
        uint16_t waitCount = (uint16_t)((bnk[p] << 8) | bnk[p+1]);  p += 2;
        if (p >= bnkSize) break;
        uint8_t op = bnk[p++];

        std::string wait;
        if (waitCount == 0xFFFFu)
            wait = "Wait(Forever) ";
        else if (waitCount != 0)
        { char tmp[32]; snprintf(tmp, sizeof(tmp), "Wait(%u) ", waitCount); wait = tmp; }

        char hex[64];
        snprintf(hex, sizeof(hex), "%04X %02X", waitCount, op);
        std::string hexDesc = hex;

        std::string instr;
        bool done = false;

        switch (op) {
        case 0x00:
            instr = "End;";
            done = true;
            break;

        case 0x01:
            if (p + 4 >= bnkSize) { done = true; break; }
            {
                uint8_t  ch     = bnk[p++];
                size_t   vs     = p;
                uint32_t stored = read_be24(bnk + p);  p += 3;
                uint8_t  repeat = bnk[p++];
                uint32_t absAddr = stored + (uint32_t)vs;
                char hx[32]; snprintf(hx, sizeof(hx), " %02X %06X %02X", ch, stored, repeat);
                hexDesc += hx;
                std::string chTag = (ch == trackChannel) ? "" : [&]{ char t[32]; snprintf(t, sizeof(t), "channel %d,", ch); return std::string(t); }();
                char istr[80];
                if (repeat == 0)
                    snprintf(istr, sizeof(istr), "Play(%sstream $%06X, repeat forever);", chTag.c_str(), absAddr);
                else if (repeat == 1)
                    snprintf(istr, sizeof(istr), "Play(%sstream $%06X);", chTag.c_str(), absAddr);
                else
                    snprintf(istr, sizeof(istr), "Play(%sstream $%06X, repeat %d);", chTag.c_str(), absAddr, repeat);
                instr = istr;
            }
            break;

        case 0x02:
            if (p < bnkSize) {
                uint8_t ch = bnk[p++];
                char hx[16]; snprintf(hx, sizeof(hx), " %02X", ch); hexDesc += hx;
                char istr[48]; snprintf(istr, sizeof(istr), "Stop(channel %d);", ch);
                instr = istr;
            }
            break;

        case 0x07: case 0x08: case 0x09:
            if (p + 1 < bnkSize) {
                uint8_t ch  = bnk[p++];
                uint8_t lvl = bnk[p++];
                char hx[16]; snprintf(hx, sizeof(hx), " %02X %02X", ch, lvl); hexDesc += hx;
                std::string chTag = (ch == trackChannel) ? "" : [&]{ char t[32]; snprintf(t, sizeof(t), "channel %d, ", ch); return std::string(t); }();
                const char *verb = (op == 0x07) ? "level" : (op == 0x08) ? "increase" : "decrease";
                char istr[80]; snprintf(istr, sizeof(istr), "SetMixingLevel(%s%s %d);", chTag.c_str(), verb, lvl);
                instr = istr;
            }
            break;

        case 0x0A: case 0x0B: case 0x0C:
            if (p + 3 < bnkSize) {
                uint8_t  ch    = bnk[p++];
                uint8_t  lvl   = bnk[p++];
                uint16_t steps = read_be16(bnk + p); p += 2;
                char hx[24]; snprintf(hx, sizeof(hx), " %02X %02X %04X", ch, lvl, steps); hexDesc += hx;
                std::string chTag = (ch == trackChannel) ? "" : [&]{ char t[32]; snprintf(t, sizeof(t), "channel %d, ", ch); return std::string(t); }();
                const char *verb = (op == 0x0A) ? "level" : (op == 0x0B) ? "increase" : "decrease";
                char istr[80]; snprintf(istr, sizeof(istr), "SetMixingLevel(%s%s %u, steps %u);", chTag.c_str(), verb, lvl, steps);
                instr = istr;
            }
            break;

        case 0x0D:
            instr = "NOP;";
            break;

        case 0x0E:
            if (p < bnkSize) {
                uint8_t cnt = bnk[p++];
                char hx[8]; snprintf(hx, sizeof(hx), " %02X", cnt); hexDesc += hx;
                char istr[32];
                if (cnt != 0) snprintf(istr, sizeof(istr), "Loop (%d) {", cnt);
                else          snprintf(istr, sizeof(istr), "Loop {");
                instr = istr;
                loopCountStack.push_back(cnt);
            }
            break;

        case 0x0F:
            // dedent before printing the closing brace
            if (!loopIndent.empty()) loopIndent = loopIndent.substr(2);
            instr = "}";
            // a Loop(0) (infinite) end means execution can never continue past here
            if (!loopCountStack.empty()) {
                if (loopCountStack.back() == 0) done = true;
                loopCountStack.pop_back();
            }
            break;

        default:
            { char istr[32]; snprintf(istr, sizeof(istr), "InvalidOpcode$%02X;", op); instr = istr; }
            done = true;
            break;
        }

        // For loop-end, loopIndent was already reduced above; append normally.
        // For loop-start, append then increase indent.
        appendLine(wait + instr, hexDesc);
        if (op == 0x0E) loopIndent += perLoopIndent;

        if (done || waitCount == 0xFFFFu) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parse DCS track program bytecode from a BNK playlist entry.
// Returns list of stream references in play order (infinite loops unwound once).
// poff = file offset of the 18-byte entry; bnkSize guards reads.
// ---------------------------------------------------------------------------
static std::vector<StreamRef> parseProgram(
    const uint8_t *bnk, size_t bnkSize, size_t poff)
{
    std::vector<StreamRef> result;
    size_t p = poff + BNK_PROGRAM_HDR;  // skip 2-byte prefix

    // Default mix level from byte[6] of entry (before bytecode)
    uint8_t defaultMix = (poff + 6 < bnkSize) ? bnk[poff + 6] : 0x64;
    uint8_t curMix = defaultMix;

    int loopDepth = 0;
    for (int guard = 0; guard < 4096 && p + 2 < bnkSize; ++guard) {
        uint16_t cp  = (uint16_t)((bnk[p] << 8) | bnk[p+1]);  p += 2;
        if (cp == 0xFFFF) break;
        if (p >= bnkSize) break;
        uint8_t op = bnk[p++];

        switch (op) {
        case 0x00:  // Stop
            return result;

        case 0x01:  // Play stream
            if (p + 4 >= bnkSize) return result;
            {
                uint8_t  ch   = bnk[p++];  (void)ch;
                size_t   vs   = p;
                uint32_t stored = read_be24(bnk + p);  p += 3;
                uint8_t  loop = bnk[p++];
                uint32_t abs  = stored + (uint32_t)vs;
                if (abs + 18 < bnkSize) {
                    uint16_t nf = read_be16(bnk + abs);
                    if (nf > 0)
                        result.push_back({ abs, curMix, loop });
                }
            }
            break;

        case 0x07: case 0x08: case 0x09:  // Mix level set
            if (p + 1 < bnkSize) { p++; curMix = bnk[p++]; }
            break;

        case 0x0A: case 0x0B: case 0x0C:  // Mix level fade
            if (p + 3 < bnkSize) { p++; curMix = bnk[p++]; p += 2; }
            break;

        case 0x0D:  // NOP
            break;

        case 0x0E:  // Loop start
            if (p < bnkSize) { p++; loopDepth++; }
            break;

        case 0x0F:  // Loop end
            loopDepth = (loopDepth > 0) ? loopDepth - 1 : 0;
            break;

        default:
            return result;  // unknown opcode — stop parsing
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    printf("BnkExtract - DCS2 BNK audio extractor\n\n");

    // --- parse arguments ---
    const char *bnkPath  = nullptr;
    const char *lstPath  = nullptr;
    std::string outDir   = ".";
    bool samplesMode     = false;
    bool programMode     = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--outdir") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--samples") == 0) {
            samplesMode = true;
        } else if (strcmp(argv[i], "--program") == 0 || strcmp(argv[i], "-p") == 0) {
            programMode = true;
        } else if (!bnkPath) {
            bnkPath = argv[i];
        } else if (!lstPath) {
            lstPath = argv[i];
        }
    }

    if (!bnkPath) {
        printf("Usage: BnkExtract <file.BNK> [file.LST] [--outdir <dir>] [--samples] [-p]\n");
        printf("  --samples  Write each individual stream as a separate WAV\n");
        printf("  -p         Print track program disassembly (DCSExplorer format)\n");
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
    if (bnkSize < 0x100) {
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

    // Data base = right after the index table
    size_t dataBase = BNK_INDEX_OFFSET + (size_t)slotCount * 4;

    // Collect valid tracks by parsing each playlist entry's bytecode
    std::vector<Track> tracks;
    for (int i = 0; i < (int)slotCount; ++i) {
        uint32_t rel = read_le32(bnk.data() + BNK_INDEX_OFFSET + i * 4);
        if (rel == 0xFFFFFFFF) continue;

        size_t poff = dataBase + rel;
        if (poff + BNK_PROGRAM_HDR + 3 >= bnkSize) continue;

        auto streams = parseProgram(bnk.data(), bnkSize, poff);
        if (streams.empty()) continue;

        Track t;
        t.trackNum = i;
        t.poff     = poff;
        t.streams  = std::move(streams);
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
    decoder.SetDefaultVolume(0xFF);
    decoder.SoftBoot();

    // --- print track programs if requested ---
    if (programMode) {
        for (auto &t : tracks) {
            uint8_t channel = (t.poff + 1 < bnkSize) ? bnk.data()[t.poff + 1] : 0;

            // Header matches DCSExplorer -p format; use file offset in place of ROM address
            printf("Track $%04x Channel %d {    // Address $%06zx\n",
                t.trackNum, channel, t.poff);

            std::string prog = disassembleProgram(bnk.data(), bnkSize, t.poff, "    ");
            if (!prog.empty())
                printf("%s\n", prog.c_str());

            printf("};\n");
        }
    }

    // --- extract each track ---
    int nOk = 0, nError = 0, nSkip = 0;
    for (auto &t : tracks) {
        std::string name;
        auto it = names.find(t.trackNum);
        if (it != names.end())
            name = sanitise(it->second);
        if (name.empty())
            name = "track";

        if (samplesMode) {
            // Write each unique stream address as a separate WAV, numbered in
            // first-appearance order (duplicates within a track are skipped).
            std::vector<uint32_t> seen;
            size_t sampleNum = 0;
            for (size_t si = 0; si < t.streams.size(); ++si) {
                auto &sr = t.streams[si];
                // Skip duplicate stream addresses within this track
                bool dup = false;
                for (auto a : seen) if (a == sr.absAddr) { dup = true; break; }
                if (dup) continue;
                seen.push_back(sr.absAddr);
                ++sampleNum;

                DCSDecoder::ROMPointer streamPtr(0, bnk.data() + sr.absAddr);
                auto info = decoder.GetStreamInfo(streamPtr);
                if (info.nFrames <= 0) {
                    printf("  SKIP $%04X/%zu  (0 frames)\n", t.trackNum, sampleNum);
                    ++nSkip;
                    continue;
                }

                char filename[512];
                snprintf(filename, sizeof(filename), "%s/%s_%04X_%zu.wav",
                    outDir.c_str(), name.c_str(), t.trackNum, sampleNum);

                bool ok = writeOneWav(filename, decoder, bnk.data(),
                                      sr.absAddr, sr.mixLevel, info.nFrames);
                if (ok) {
                    printf("  OK   $%04X/%zu  %5d frames  %s\n",
                        t.trackNum, sampleNum, info.nFrames, filename);
                    ++nOk;
                } else {
                    printf("  ERR  $%04X/%zu  %s\n", t.trackNum, sampleNum, filename);
                    ++nError;
                }
            }
        } else {
            // Write all streams concatenated into one WAV
            char filename[512];
            snprintf(filename, sizeof(filename), "%s/%s_%04X.wav",
                outDir.c_str(), name.c_str(), t.trackNum);

            std::vector<int> frameCounts;
            int totalFrames = 0;
            bool valid = true;
            for (auto &sr : t.streams) {
                DCSDecoder::ROMPointer streamPtr(0, bnk.data() + sr.absAddr);
                auto info = decoder.GetStreamInfo(streamPtr);
                if (info.nFrames <= 0) { valid = false; break; }
                int n = info.nFrames * (sr.loopCount > 1 ? sr.loopCount : 1);
                frameCounts.push_back(n);
                totalFrames += n;
            }
            if (!valid || totalFrames <= 0) {
                printf("  SKIP $%04X  (no valid frames)\n", t.trackNum);
                ++nSkip;
                continue;
            }

            FILE *fp = nullptr;
            if (fopen_s(&fp, filename, "wb") != 0 || fp == nullptr) {
                printf("  ERR  $%04X  cannot open %s\n", t.trackNum, filename);
                ++nError;
                continue;
            }

            uint32_t wavFrames = (uint32_t)totalFrames + 2;
            uint8_t hdr[44];
            memset(hdr, 0, sizeof(hdr));
            uint32_t dataBytes = wavFrames * 240 * 2;
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

            for (size_t si = 0; si < t.streams.size() && ok; ++si) {
                auto &sr = t.streams[si];
                DCSDecoder::ROMPointer streamPtr(0, bnk.data() + sr.absAddr);
                uint8_t loopCount = sr.loopCount > 1 ? sr.loopCount : 1;
                for (uint8_t loop = 0; loop < loopCount && ok; ++loop) {
                    decoder.SoftBoot();
                    decoder.LoadAudioStream(0, streamPtr, sr.mixLevel);
                    for (int f = 0; f < frameCounts[si] / loopCount && ok; ++f) {
                        int16_t buf[240];
                        for (int s = 0; s < 240; ++s) buf[s] = decoder.GetNextSample();
                        ok = (fwrite(buf, sizeof(buf), 1, fp) == 1);
                    }
                }
            }
            // 2 tail frames for decay
            if (ok) {
                decoder.SoftBoot();
                for (int f = 0; f < 2 && ok; ++f) {
                    int16_t buf[240];
                    for (int s = 0; s < 240; ++s) buf[s] = decoder.GetNextSample();
                    ok = (fwrite(buf, sizeof(buf), 1, fp) == 1);
                }
            }
            ok = (fclose(fp) == 0) && ok;

            if (ok) {
                printf("  OK   $%04X  %5d frames (%zu stream%s)  %s\n",
                    t.trackNum, totalFrames, t.streams.size(),
                    t.streams.size() == 1 ? "" : "s", filename);
                ++nOk;
            } else {
                printf("  ERR  $%04X  %s\n", t.trackNum, filename);
                ++nError;
            }
        }
    }

    printf("\nDone: %d written, %d errors, %d skipped\n", nOk, nError, nSkip);
    return (nError > 0) ? 1 : 0;
}
