#!/usr/bin/env python3
"""
Check DCS stream address alignment across both NBA ROM sets.
Looks for 2-byte, 4-byte, and 8-byte alignment violations on every
stream referenced by a track program.
"""

import struct
import sys
import os

ROM_SIZE = 0x100000  # 1MB per chip

def load_roms(directory):
    """Load u2..u6 (or however many exist) from a directory.
    Accepts u{n}.bin/.ROM, U{n}.bin/.ROM, or *.u{n} filename patterns."""
    roms = {}
    for i in range(2, 10):
        candidates = []
        # u2.bin / U2.ROM style
        for prefix in (f"u{i}", f"U{i}"):
            for ext in ("bin", "BIN", "rom", "ROM"):
                candidates.append(os.path.join(directory, f"{prefix}.{ext}"))
        # anything.u2 / anything.U2 style
        for entry in os.listdir(directory):
            low = entry.lower()
            if low.endswith(f".u{i}"):
                candidates.append(os.path.join(directory, entry))
        for path in candidates:
            if os.path.exists(path):
                with open(path, "rb") as f:
                    data = f.read()
                roms[i - 2] = data  # chip index 0 = U2
                break
    return roms

def u8(data, off):  return data[off]
def u16be(data, off): return struct.unpack_from(">H", data, off)[0]
def u24be(data, off):
    return (data[off] << 16) | (data[off+1] << 8) | data[off+2]

def make_rom_pointer(roms, linear_addr):
    """Resolve a 24-bit linear ROM address to (chip_index, offset)."""
    chip = (linear_addr >> 20) & 0x07
    offset = linear_addr & 0x0FFFFF
    return chip, offset

def read_at(roms, linear_addr, n):
    """Read n bytes from a linear ROM address."""
    chip, off = make_rom_pointer(roms, linear_addr)
    if chip not in roms:
        return None
    data = roms[chip]
    if off + n > len(data):
        return None
    return data[off:off+n]

def read_u8(roms, linear_addr):
    b = read_at(roms, linear_addr, 1)
    return b[0] if b else None

def read_u16be(roms, linear_addr):
    b = read_at(roms, linear_addr, 2)
    return struct.unpack(">H", b)[0] if b else None

def read_u24be(roms, linear_addr):
    b = read_at(roms, linear_addr, 3)
    return (b[0] << 16) | (b[1] << 8) | b[2] if b else None

def find_catalog(u2):
    """Find catalog offset in U2 (tries 0x3000, 0x4000, 0x6000)."""
    for ofs in (0x3000, 0x4000, 0x6000):
        size = u16be(u2, ofs) * 4096
        chip_sel = u16be(u2, ofs + 2) >> 8
        cksum = u16be(u2, ofs + 4)
        if chip_sel == 0 and cksum == 0 and size == len(u2):
            return ofs
    return None

def get_stream_header(roms, stream_addr):
    """
    Read the stream preamble to understand its type and configuration.
    Returns dict with frame_count, header bytes, and packed-bit start address.
    Most formats: 2-byte frame count + 16-byte header = 18 bytes preamble.
    OS93a type1:  2-byte frame count + 1-byte header = 3 bytes preamble.
    """
    b = read_at(roms, stream_addr, 20)
    if b is None:
        return None
    frame_count = (b[0] << 8) | b[1]
    header = b[2:18]
    return {
        "frame_count": frame_count,
        "header": header,
        "raw20": b,
    }

def scan_tracks(roms, label):
    u2 = roms[0]
    cat_ofs = find_catalog(u2)
    if cat_ofs is None:
        print(f"  [{label}] ERROR: could not find catalog in U2")
        return {}

    track_index_addr = u24be(u2, cat_ofs + 0x40)
    n_tracks         = u16be(u2, cat_ofs + 0x46)

    print(f"\n{'='*70}")
    print(f"  ROM set : {label}")
    print(f"  Catalog : U2[${cat_ofs:05X}]")
    print(f"  Track index : U2[${track_index_addr:05X}]  ({n_tracks} tracks)")
    print(f"{'='*70}")

    # Map: stream_addr -> list of (track_num, channel)
    stream_refs = {}

    for track_num in range(n_tracks + 1):
        track_linear = u24be(u2, track_index_addr + track_num * 3)
        if track_linear == 0 or track_linear == 0xFFFFFF:
            continue

        chip, off = make_rom_pointer(roms, track_linear)
        if chip not in roms:
            continue
        data = roms[chip]
        if off >= len(data):
            continue

        # Read track header: type byte + channel byte (at minimum)
        track_type = data[off]
        if track_type != 1:
            # Only type 1 tracks have byte-code programs with Play opcodes
            continue

        channel = data[off + 1]
        p = off + 2  # start of byte-code program

        # Walk the byte-code program
        for _ in range(4096):  # safety limit
            if p + 2 > len(data):
                break
            delay = struct.unpack_from(">H", data, p)[0]
            p += 2
            if p >= len(data):
                break
            opcode = data[p]; p += 1

            if opcode == 0x00:  # End
                break
            elif opcode == 0x01:  # Play stream
                if p + 4 > len(data):
                    break
                ch       = data[p];     p += 1
                s_addr   = (data[p] << 16) | (data[p+1] << 8) | data[p+2]; p += 3
                repeat   = data[p];     p += 1
                refs = stream_refs.setdefault(s_addr, [])
                refs.append((track_num, ch))
            elif opcode == 0x02:  p += 1      # Stop
            elif opcode == 0x03:  p += 2      # Queue
            elif opcode == 0x04:  p += 3      # varies
            elif opcode == 0x05:  p += 1
            elif opcode == 0x06:  p += 1
            elif opcode == 0x07:  p += 2
            elif opcode == 0x08:  p += 2
            elif opcode == 0x09:  p += 2
            elif opcode == 0x0A:  p += 2
            elif opcode == 0x0B:  p += 4
            elif opcode == 0x0C:  p += 3
            else:
                break  # unknown opcode, stop walking

    return stream_refs

def alignment_flags(addr):
    """Return a string describing alignment violations."""
    flags = []
    if addr & 1:  flags.append("ODD(not 2-byte aligned!)")
    elif addr & 3: flags.append("not 4-byte aligned")
    elif addr & 7: flags.append("not 8-byte aligned")
    else:          flags.append("8-byte aligned")
    return ", ".join(flags)

def report(label, roms, stream_refs, highlight_tracks=None):
    print(f"\n--- {label}: stream alignment report ---")
    print(f"{'Stream addr':>12}  {'Align':>6}  {'mod4':>4}  {'mod8':>4}  {'mod16':>5}  {'Tracks (sample)':40}")
    print("-" * 90)

    violations_4  = []
    violations_8  = []
    violations_16 = []

    for addr in sorted(stream_refs):
        tracks = stream_refs[addr]
        track_str = ", ".join(f"${t:04X}/ch{c}" for t,c in tracks[:4])
        if len(tracks) > 4:
            track_str += f" (+{len(tracks)-4} more)"

        mod2  = addr & 1
        mod4  = addr & 3
        mod8  = addr & 7
        mod16 = addr & 15

        flag = ""
        if mod2:  flag = " *** ODD - DEFINITELY BROKEN ***"
        elif mod4: flag = " * not 4-byte aligned"
        elif mod8: flag = " * not 8-byte aligned"

        # highlight track $01a6 et al.
        hilite = ""
        if highlight_tracks:
            matching = [t for t,c in tracks if t in highlight_tracks]
            if matching:
                hilite = f"  <-- track(s) {[f'${x:04X}' for x in matching]}"

        if mod4: violations_4.append(addr)
        if mod8: violations_8.append(addr)
        if mod16: violations_16.append(addr)

        print(f"  ${addr:06X}    {'E' if not mod2 else 'O':>2}      {mod4:>1}     {mod8:>1}      {mod16:>2}   {track_str}{flag}{hilite}")

    print(f"\n  Summary: {len(stream_refs)} streams total")
    print(f"    Odd-aligned (mod2!=0)  : {sum(1 for a in stream_refs if a&1)}")
    print(f"    Not 4-byte aligned     : {len(violations_4)}")
    print(f"    Not 8-byte aligned     : {len(violations_8)}")
    print(f"    Not 16-byte aligned    : {len(violations_16)}")


def check_bank_boundary(addr, preamble_bytes=18):
    """Return True if the preamble crosses a 4KB bank boundary."""
    bank_end = (addr & ~0xFFF) + 0x1000
    return (addr + preamble_bytes) > bank_end

def main():
    if len(sys.argv) < 2:
        print("Usage: check_alignment.py <rom_dir> [<rom_dir2>]")
        print("  With one dir:  report alignment violations for that ROM set.")
        print("  With two dirs: compare both sets side by side.")
        sys.exit(1)

    dirs = sys.argv[1:]
    labels = [os.path.basename(os.path.abspath(d)) for d in dirs]

    print("Loading ROMs...")
    all_roms = []
    for d, label in zip(dirs, labels):
        roms = load_roms(d)
        if not roms:
            print(f"  ERROR: no ROM files found in {d}")
            sys.exit(1)
        chips = [f"U{k+2}" for k in sorted(roms.keys())]
        print(f"  {label}: loaded {chips}")
        all_roms.append(roms)

    all_streams = []
    for roms, label in zip(all_roms, labels):
        streams = scan_tracks(roms, label)
        all_streams.append(streams)
        report(label, roms, streams)

    # Bank-boundary violation report
    for roms, streams, label in zip(all_roms, all_streams, labels):
        violations = [(addr, tracks) for addr, tracks in streams.items()
                      if check_bank_boundary(addr)]
        print(f"\n--- {label}: 4KB bank-boundary violations (preamble crosses page) ---")
        if not violations:
            print("  None found.")
        else:
            for addr, tracks in sorted(violations):
                bank_end = (addr & ~0xFFF) + 0x1000
                overflow = (addr + 18) - bank_end
                track_str = ", ".join(f"${t:04X}/ch{c}" for t, c in tracks[:4])
                print(f"  ${addr:06X}  overflows by {overflow} bytes into next page  [{track_str}]")

if __name__ == "__main__":
    main()
