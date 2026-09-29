// Bounded pre-decode walk of a THP video component.
//
// THPVideoDecode (aurora::thp) reads the JPEG headers and the entropy-coded
// scan with no input length and writes Y/U/V texels for the dimensions given
// in the frame's SOF segment. This walk accepts the same marker grammar, decodes
// the same Huffman symbols in the same MCU order, and counts every bit the
// decoder will read. A component passes only if all of those bits lie within
// its size and the SOF dimensions match the stream's video info, which sized
// the output textures. No pixels are produced here.

#include <petari/movie_thp.hpp>

#include <cstring>

namespace PetariNative::Movie {
namespace {

struct Huffman {
    std::uint8_t counts[17];
    std::uint16_t firstCodes[17];
    bool valid;
};

struct Context {
    bool quantValid[3];
    Huffman huffman[4];  // index = id * 2 + class (0 = DC, 1 = AC)
    std::uint8_t quantSelect[3];
    std::uint8_t dcTable[3];
    std::uint8_t acTable[3];
    std::uint16_t width;
    std::uint16_t height;
    bool haveFrame;
    std::uint16_t restartInterval;
};

// Bit cursor over the scan. Only positions are tracked: the walk needs code
// lengths and symbols, not coefficient values. Bytes past the component read
// as zero in peeks; every consume is checked against the component size.
class Bits {
public:
    Bits(const std::uint8_t* data, std::size_t sizeBytes, std::size_t startByte)
        : mData(data), mSize(sizeBytes), mLimit(sizeBytes * 8), mPosition(startByte * 8) {}

    std::uint32_t peek16() const {
        const std::size_t byte = mPosition >> 3;
        std::uint32_t window = 0;
        for (std::size_t i = 0; i < 3; i++) {
            window = (window << 8) | (byte + i < mSize ? mData[byte + i] : 0);
        }
        return (window >> (8 - (mPosition & 7))) & 0xFFFF;
    }

    bool skip(unsigned count) {
        if (mPosition + count > mLimit) {
            return false;
        }
        mPosition += count;
        return true;
    }

    void byteAlign() { mPosition = (mPosition + 7) & ~std::size_t{7}; }

private:
    const std::uint8_t* mData;
    std::size_t mSize;
    std::size_t mLimit;
    std::size_t mPosition;
};

const char* parseQuant(const std::uint8_t* p, std::size_t size, Context& c) {
    std::size_t pos = 0;
    while (pos < size) {
        if (size - pos < 65) {
            return "THP JPEG quantization table is truncated";
        }
        const std::uint8_t descriptor = p[pos];
        if ((descriptor >> 4) != 0 || (descriptor & 15) >= 3) {
            return "THP JPEG quantization table is not an 8-bit table 0..2";
        }
        c.quantValid[descriptor & 15] = true;
        pos += 65;
    }
    return nullptr;
}

const char* parseHuffman(const std::uint8_t* p, std::size_t size, Context& c) {
    std::size_t pos = 0;
    while (pos < size) {
        if (size - pos < 17) {
            return "THP JPEG Huffman table is truncated";
        }
        const std::uint8_t descriptor = p[pos++];
        const unsigned tableClass = descriptor >> 4;
        const unsigned id = descriptor & 15;
        if (tableClass > 1 || id > 1) {
            return "THP JPEG Huffman table is not class 0..1, id 0..1";
        }
        Huffman table{};
        std::size_t symbols = 0;
        for (unsigned length = 1; length <= 16; length++) {
            table.counts[length] = p[pos++];
            symbols += table.counts[length];
        }
        if (symbols > 256 || symbols > size - pos) {
            return "THP JPEG Huffman table symbols are truncated";
        }
        pos += symbols;
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= 16; length++) {
            if (code + table.counts[length] > (1u << length)) {
                return "THP JPEG Huffman table is oversubscribed";
            }
            table.firstCodes[length] = static_cast<std::uint16_t>(code);
            code = (code + table.counts[length]) << 1;
        }
        table.valid = true;
        c.huffman[id * 2 + tableClass] = table;
    }
    return nullptr;
}

const char* parseFrame(const std::uint8_t* p, std::size_t size, Context& c) {
    if (size < 6) {
        return "THP JPEG frame header is truncated";
    }
    if (p[0] != 8) {
        return "THP JPEG sample precision is not 8";
    }
    if (p[5] != 3) {
        return "THP JPEG frame does not have three components";
    }
    if (size < 15) {
        return "THP JPEG frame header is truncated";
    }
    c.height = static_cast<std::uint16_t>((p[1] << 8) | p[2]);
    c.width = static_cast<std::uint16_t>((p[3] << 8) | p[4]);
    if (c.width == 0 || c.height == 0) {
        return "THP JPEG frame has zero dimensions";
    }
    for (unsigned i = 0; i < 3; i++) {
        const std::uint8_t sampling = p[7 + i * 3];
        if (sampling != (i == 0 ? 0x22 : 0x11)) {
            return "THP JPEG frame is not 4:2:0 sampled";
        }
        if (p[8 + i * 3] >= 3) {
            return "THP JPEG component selects a quantization table above 2";
        }
        c.quantSelect[i] = p[8 + i * 3];
    }
    c.haveFrame = true;
    return nullptr;
}

const char* parseScan(const std::uint8_t* p, std::size_t size, Context& c) {
    if (size < 1) {
        return "THP JPEG scan header is truncated";
    }
    if (p[0] != 3) {
        return "THP JPEG scan does not have three components";
    }
    if (size < 10) {
        return "THP JPEG scan header is truncated";
    }
    for (unsigned i = 0; i < 3; i++) {
        const unsigned dc = p[2 + i * 2] >> 4;
        const unsigned ac = p[2 + i * 2] & 15;
        if (dc > 1 || ac > 1 || !c.huffman[dc * 2].valid || !c.huffman[ac * 2 + 1].valid) {
            return "THP JPEG scan references a missing Huffman table";
        }
        c.dcTable[i] = static_cast<std::uint8_t>(dc);
        c.acTable[i] = static_cast<std::uint8_t>(ac);
    }
    if (p[7] != 0 || p[8] != 63 || p[9] != 0) {
        return "THP JPEG scan is not a baseline sequential scan";
    }
    return nullptr;
}

// Symbol values are needed for run/size decoding, so keep each table's values,
// plus a lookup of every code up to kLookupBits long.
constexpr unsigned kLookupBits = 9;

struct SymbolTables {
    std::uint8_t values[4][256];
    std::uint16_t offsets[4][17];
    // (length << 8) | symbol; 0 when no code of at most kLookupBits matches.
    std::uint16_t lookup[4][1u << kLookupBits];
};

// Same result as reading one bit at a time and stopping at the shortest length
// whose code is assigned, which is how THPVideoDecode decodes.
bool decodeSymbol(Bits& bits, const Huffman& table, const SymbolTables& symbols, unsigned index,
                  std::uint8_t* symbol) {
    const std::uint32_t peek = bits.peek16();
    const std::uint16_t entry = symbols.lookup[index][peek >> (16 - kLookupBits)];
    if (entry != 0) {
        *symbol = static_cast<std::uint8_t>(entry);
        return bits.skip(entry >> 8);
    }
    for (unsigned length = kLookupBits + 1; length <= 16; length++) {
        const std::uint32_t code = peek >> (16 - length);
        if (code >= table.firstCodes[length] && code - table.firstCodes[length] < table.counts[length]) {
            *symbol = symbols.values[index][symbols.offsets[index][length] + code - table.firstCodes[length]];
            return bits.skip(length);
        }
    }
    return false;
}

const char* walkBlock(Bits& bits, const Context& c, const SymbolTables& symbols, unsigned component) {
    const unsigned dcIndex = c.dcTable[component] * 2;
    const unsigned acIndex = c.acTable[component] * 2 + 1;
    std::uint8_t dcBits;
    if (!decodeSymbol(bits, c.huffman[dcIndex], symbols, dcIndex, &dcBits)) {
        return "THP JPEG DC code is invalid or truncated";
    }
    if (dcBits > 16 || !bits.skip(dcBits)) {
        return "THP JPEG DC difference is invalid or truncated";
    }
    unsigned coefficient = 1;
    while (coefficient < 64) {
        std::uint8_t runSize;
        if (!decodeSymbol(bits, c.huffman[acIndex], symbols, acIndex, &runSize)) {
            return "THP JPEG AC code is invalid or truncated";
        }
        const unsigned run = runSize >> 4;
        const unsigned size = runSize & 15;
        if (size == 0) {
            if (run == 15) {
                coefficient += 16;
                continue;
            }
            break;
        }
        coefficient += run;
        if (coefficient >= 64) {
            return "THP JPEG AC run passes the end of the block";
        }
        if (!bits.skip(size)) {
            return "THP JPEG AC value is truncated";
        }
        coefficient++;
    }
    return nullptr;
}

void recordSymbols(const std::uint8_t* p, std::size_t size, const Context& c, SymbolTables& symbols) {
    // Second pass over a validated DHT segment to keep the symbol values.
    std::size_t pos = 0;
    while (pos < size) {
        const std::uint8_t descriptor = p[pos++];
        const unsigned index = (descriptor & 15) * 2 + (descriptor >> 4);
        std::size_t count = 0;
        std::uint16_t offset = 0;
        for (unsigned length = 1; length <= 16; length++) {
            symbols.offsets[index][length] = offset;
            offset = static_cast<std::uint16_t>(offset + p[pos + length - 1]);
            count += p[pos + length - 1];
        }
        pos += 16;
        std::memset(symbols.values[index], 0, sizeof(symbols.values[index]));
        std::memcpy(symbols.values[index], p + pos, count);
        pos += count;

        // Longest first, so the shortest matching code owns each lookup entry.
        const Huffman& table = c.huffman[index];
        std::memset(symbols.lookup[index], 0, sizeof(symbols.lookup[index]));
        for (unsigned length = kLookupBits; length >= 1; length--) {
            for (std::uint32_t k = 0; k < table.counts[length]; k++) {
                const std::uint32_t code = table.firstCodes[length] + k;
                const std::uint16_t entry = static_cast<std::uint16_t>(
                    (length << 8) | symbols.values[index][symbols.offsets[index][length] + k]);
                const unsigned shift = kLookupBits - length;
                for (std::uint32_t fill = 0; fill < (1u << shift); fill++) {
                    symbols.lookup[index][(code << shift) | fill] = entry;
                }
            }
        }
    }
}

}  // namespace

const char* validateThpVideoComponent(const void* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    Context c{};
    SymbolTables symbols{};
    std::size_t pos = 0;
    std::size_t scanStart = 0;
    for (;;) {
        if (pos >= size || p[pos] != 0xFF) {
            return "THP JPEG marker is missing or truncated";
        }
        pos++;
        std::uint8_t marker;
        do {
            if (pos >= size) {
                return "THP JPEG marker is truncated";
            }
            marker = p[pos++];
        } while (marker == 0xFF);
        if (marker == 0xD8) {
            continue;
        }
        if (size - pos < 2) {
            return "THP JPEG segment length is truncated";
        }
        const std::size_t length = (std::size_t{p[pos]} << 8) | p[pos + 1];
        if (length < 2 || length - 2 > size - pos - 2) {
            return "THP JPEG segment is truncated";
        }
        const std::uint8_t* segment = p + pos + 2;
        const std::size_t segmentSize = length - 2;
        pos += length;
        const char* error = nullptr;
        if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
            continue;
        }
        switch (marker) {
        case 0xC0:
            error = parseFrame(segment, segmentSize, c);
            break;
        case 0xC4:
            error = parseHuffman(segment, segmentSize, c);
            if (error == nullptr) {
                recordSymbols(segment, segmentSize, c, symbols);
            }
            break;
        case 0xDB:
            error = parseQuant(segment, segmentSize, c);
            break;
        case 0xDD:
            if (segmentSize != 2) {
                error = "THP JPEG restart interval segment has the wrong size";
            } else {
                c.restartInterval = static_cast<std::uint16_t>((segment[0] << 8) | segment[1]);
            }
            break;
        case 0xDA:
            error = parseScan(segment, segmentSize, c);
            scanStart = pos;
            break;
        default:
            return "THP JPEG uses an unsupported marker";
        }
        if (error != nullptr) {
            return error;
        }
        if (marker == 0xDA) {
            break;
        }
    }

    if (!c.haveFrame) {
        return "THP JPEG scan precedes its frame header";
    }
    if (c.width != width || c.height != height) {
        return "THP JPEG frame dimensions differ from the stream's video info";
    }
    for (unsigned i = 0; i < 3; i++) {
        if (!c.quantValid[c.quantSelect[i]]) {
            return "THP JPEG component uses an undefined quantization table";
        }
    }

    Bits bits(p, size, scanStart);
    const unsigned mcuColumns = (c.width + 15) / 16;
    const unsigned mcuRows = (c.height + 15) / 16;
    unsigned restartCount = 0;
    for (unsigned row = 0; row < mcuRows; row++) {
        for (unsigned column = 0; column < mcuColumns; column++) {
            static constexpr unsigned kBlockComponents[6] = {0, 0, 0, 0, 1, 2};
            for (unsigned component : kBlockComponents) {
                if (const char* error = walkBlock(bits, c, symbols, component)) {
                    return error;
                }
            }
            if (c.restartInterval != 0 && ++restartCount == c.restartInterval) {
                bits.byteAlign();
                restartCount = 0;
            }
        }
    }
    return nullptr;
}

}  // namespace PetariNative::Movie
