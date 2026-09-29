#!/usr/bin/env python3
"""Checks AFC decoding against Nintendo's own data on an extracted SMG disc.

Every looping wave in the game's wave systems (AudioRes/SMR.szs) stores the two
decoded samples that precede the block containing its loop start (the DSP
reloads them as predictor history when it loops). Decoding each wave up to that
block with the native DSP's AFC rules must reproduce them exactly.

Usage: audio_afc_verify.py <extracted disc root> [repository root]

The AFC coefficient table is read from JASDSPInterface.cpp (DSPADPCM_FILTER).
The decoding rules mirror native/platform/audio/dsp_renderer.cpp decodeAfc.
"""
import os
import re
import struct
import sys


def yaz0(data):
    if data[:4] != b"Yaz0":
        raise ValueError("not Yaz0")
    size = struct.unpack(">I", data[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[i])
                i += 1
            else:
                b1, b2 = data[i], data[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def wave_systems(init):
    """Offsets of WSYS blocks in JAudio init data (AA_< ... >_AA)."""
    words = {b"bst ": 2, b"bstn": 2, b"bsc ": 2, b"bnk ": 2, b"sect": 2, b"bms ": 3, b"bmp ": 3, b"cdf ": 2, b"raa ": 2}
    if init[:4] != b"AA_<":
        raise ValueError("not JAudio init data")
    i, result = 4, []
    while init[i:i + 4] != b">_AA":
        tag = init[i:i + 4]
        if tag == b"ws  ":
            result.append(struct.unpack(">I", init[i + 8:i + 12])[0])
            i += 16
        elif tag in words:
            i += 4 + 4 * words[tag]
        else:
            raise ValueError("unknown init tag %r at %#x" % (tag, i))
    return result


def afc_coefficients(repo):
    src = open(os.path.join(repo, "src/JSystem/JAudio2/JASDSPInterface.cpp")).read()
    body = re.search(r"DSPADPCM_FILTER\[64\] = \{(.*?)\};", src, re.S).group(1)
    raw = bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}\b", body))
    return [struct.unpack(">h", raw[i * 2:i * 2 + 2])[0] for i in range(32)]


def decode_afc(data, samples, coef, high_quality=True):
    out, yn1, yn2, p = [], 0, 0, 0
    while len(out) < samples:
        header = data[p]
        p += 1
        scale, index = 1 << (header >> 4), header & 15
        deltas = []
        if high_quality:
            for byte in data[p:p + 8]:
                for nibble in (byte >> 4, byte & 15):
                    v = (nibble << 12) & 0xFFFF
                    deltas.append((v - 0x10000 if v & 0x8000 else v) >> 1)
            p += 8
        else:
            for byte in data[p:p + 4]:
                for k in range(4):
                    v = (((byte >> (6 - 2 * k)) & 3) << 14) & 0xFFFF
                    deltas.append((v - 0x10000 if v & 0x8000 else v) >> 1)
            p += 4
        c0, c1 = coef[index * 2], coef[index * 2 + 1]
        for delta in deltas:
            s = max(-0x8000, min(0x7FFF, (scale * delta + yn1 * c0 + yn2 * c1) >> 11))
            out.append(s)
            yn2, yn1 = yn1, s
    return out


def main():
    disc = sys.argv[1]
    repo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "../../..")
    files = os.path.join(disc, "files") if os.path.isdir(os.path.join(disc, "files")) else os.path.join(disc, "DATA/files")
    audio = os.path.join(files, "AudioRes")
    coef = afc_coefficients(repo)
    init = yaz0(open(os.path.join(audio, "SMR.szs"), "rb").read())
    tested = exact = 0
    failures = []
    for o in wave_systems(init):
        winf = struct.unpack(">I", init[o + 16:o + 20])[0]
        groups = struct.unpack(">I", init[o + winf + 4:o + winf + 8])[0]
        for g in range(groups):
            go = struct.unpack(">I", init[o + winf + 8 + 4 * g:o + winf + 12 + 4 * g])[0]
            name = init[o + go:o + go + 0x70].split(b"\0")[0].decode()
            count = struct.unpack(">I", init[o + go + 0x70:o + go + 0x74])[0]
            path = os.path.join(audio, "Waves", name)
            if not os.path.exists(path):
                continue
            archive = open(path, "rb").read()
            for k in range(count):
                wo = struct.unpack(">I", init[o + go + 0x74 + 4 * k:o + go + 0x78 + 4 * k])[0]
                entry = init[o + wo:o + wo + 0x30]
                fmt = entry[1]
                offset, size, loop, loop_start, _, _ = struct.unpack(">IIIIII", entry[8:32])
                yn1, yn2 = struct.unpack(">hh", entry[32:36])
                block = loop_start & ~15
                if fmt not in (0, 1) or loop != 0xFFFFFFFF or block < 2:
                    continue
                decoded = decode_afc(archive[offset:offset + size], block, coef, fmt == 0)
                tested += 1
                if decoded[block - 1] == yn1 and decoded[block - 2] == yn2:
                    exact += 1
                elif len(failures) < 10:
                    failures.append("%s wave %d: decoded %d,%d stored %d,%d" % (name, k, decoded[block - 1], decoded[block - 2], yn1, yn2))
    print("looping AFC waves: %d, predictor history reproduced exactly: %d" % (tested, exact))
    for f in failures:
        print("  mismatch:", f)
    return 0 if tested and exact == tested else 1


if __name__ == "__main__":
    sys.exit(main())
