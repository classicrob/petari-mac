// Port of MSL's wstring.c and wprintf.c for 16-bit code units. See utf16.h.
#include <petari/utf16.h>

#include <cstdio>
#include <cstring>

namespace {
enum Justification { kRightJustification, kLeftJustification, kZeroFill };
enum Sign { kOnlyMinus, kSignAlways, kSpaceHolder };
enum Argument {
    kNormalArgument,
    kCharArgument,
    kShortArgument,
    kLongArgument,
    kLongLongArgument,
    kWideArgument,
    kIntmaxArgument,
    kSizeArgument,
    kPtrdiffArgument,
    kLongDoubleArgument,
    kPointerArgument,
};

const PetariChar16 kConversionError = 0xFFFF;
const int kBufferSize = 512;
const int kMaxDigits = 509;

struct PrintFormat {
    Justification justification;
    Sign sign;
    bool precisionSpecified;
    bool alternateForm;
    Argument argument;
    PetariChar16 conversion;
    int fieldWidth;
    int precision;
};

struct Output {
    PetariChar16* dst;
    size_t maxCount;
    size_t written;
};

bool isDigit(PetariChar16 c) {
    return c >= u'0' && c <= u'9';
}

// Matches __wStringWrite: output past the limit is dropped, not an error.
// memmove allows "%ls" arguments that alias the destination, as in
// swprintf(buf, n, L"%ls%s%ls", buf, "\n", name).
void write(Output* out, const PetariChar16* src, size_t count) {
    size_t room = out->maxCount - out->written;
    size_t toWrite = count <= room ? count : room;
    std::memmove(out->dst + out->written, src, toWrite * sizeof(PetariChar16));
    out->written += toWrite;
}

const PetariChar16* parseFormat(const PetariChar16* s, va_list* args, PrintFormat* format) {
    PrintFormat f;
    f.justification = kRightJustification;
    f.sign = kOnlyMinus;
    f.precisionSpecified = false;
    f.alternateForm = false;
    f.argument = kNormalArgument;
    f.fieldWidth = 0;
    f.precision = 0;

    PetariChar16 c = *++s;
    if (c == u'%') {
        f.conversion = c;
        *format = f;
        return s + 1;
    }

    for (;;) {
        bool flagFound = true;
        switch (c) {
        case u'-':
            f.justification = kLeftJustification;
            break;
        case u'+':
            f.sign = kSignAlways;
            break;
        case u' ':
            if (f.sign != kSignAlways) {
                f.sign = kSpaceHolder;
            }
            break;
        case u'#':
            f.alternateForm = true;
            break;
        case u'0':
            if (f.justification != kLeftJustification) {
                f.justification = kZeroFill;
            }
            break;
        default:
            flagFound = false;
            break;
        }

        if (!flagFound) {
            break;
        }
        c = *++s;
    }

    if (c == u'*') {
        if ((f.fieldWidth = va_arg(*args, int)) < 0) {
            f.justification = kLeftJustification;
            f.fieldWidth = -f.fieldWidth;
        }
        c = *++s;
    } else {
        while (isDigit(c)) {
            f.fieldWidth = f.fieldWidth * 10 + (c - u'0');
            c = *++s;
        }
    }

    if (f.fieldWidth > kMaxDigits) {
        f.conversion = kConversionError;
        *format = f;
        return s + 1;
    }

    if (c == u'.') {
        f.precisionSpecified = true;
        if ((c = *++s) == u'*') {
            if ((f.precision = va_arg(*args, int)) < 0) {
                f.precisionSpecified = false;
            }
            c = *++s;
        } else {
            while (isDigit(c)) {
                f.precision = f.precision * 10 + (c - u'0');
                c = *++s;
            }
        }
    }

    bool lengthFound = true;
    switch (c) {
    case u'h':
        f.argument = kShortArgument;
        if (s[1] == u'h') {
            f.argument = kCharArgument;
            c = *++s;
        }
        break;
    case u'l':
        f.argument = kLongArgument;
        if (s[1] == u'l') {
            f.argument = kLongLongArgument;
            c = *++s;
        }
        break;
    case u'L':
        f.argument = kLongDoubleArgument;
        break;
    case u'j':
        f.argument = kIntmaxArgument;
        break;
    case u't':
        f.argument = kPtrdiffArgument;
        break;
    case u'z':
        f.argument = kSizeArgument;
        break;
    default:
        lengthFound = false;
        break;
    }

    if (lengthFound) {
        c = *++s;
    }

    f.conversion = c;
    bool badFloatLength = f.argument == kShortArgument || f.argument == kIntmaxArgument || f.argument == kSizeArgument ||
                          f.argument == kPtrdiffArgument || f.argument == kLongLongArgument;

    switch (c) {
    case u'd':
    case u'i':
    case u'u':
    case u'o':
    case u'x':
    case u'X':
        if (f.argument == kLongDoubleArgument) {
            f.argument = kLongLongArgument;
        }
        if (!f.precisionSpecified) {
            f.precision = 1;
        } else if (f.justification == kZeroFill) {
            f.justification = kRightJustification;
        }
        break;
    case u'f':
    case u'F':
        if (badFloatLength) {
            f.conversion = kConversionError;
            break;
        }
        if (!f.precisionSpecified) {
            f.precision = 6;
        }
        break;
    case u'a':
    case u'A':
        if (!f.precisionSpecified) {
            f.precision = 0xD;
        }
        if (badFloatLength || f.argument == kCharArgument) {
            f.conversion = kConversionError;
        }
        break;
    case u'g':
    case u'G':
        if (!f.precision) {
            f.precision = 1;
        }
        [[fallthrough]];
    case u'e':
    case u'E':
        if (badFloatLength || f.argument == kCharArgument) {
            f.conversion = kConversionError;
            break;
        }
        if (!f.precisionSpecified) {
            f.precision = 6;
        }
        break;
    case u'p':
        // MSL reads a 32-bit long here; native pointers are 64 bits.
        f.argument = kPointerArgument;
        f.alternateForm = true;
        f.conversion = u'x';
        f.precision = 8;
        break;
    case u'c':
        if (f.argument == kLongArgument) {
            f.argument = kWideArgument;
        } else if (f.precisionSpecified || f.argument != kNormalArgument) {
            f.conversion = kConversionError;
        }
        break;
    case u's':
        if (f.argument == kLongArgument) {
            f.argument = kWideArgument;
        } else if (f.argument != kNormalArgument) {
            f.conversion = kConversionError;
        }
        break;
    case u'n':
        if (f.argument == kLongDoubleArgument) {
            f.argument = kLongLongArgument;
        }
        break;
    default:
        f.conversion = kConversionError;
        break;
    }

    *format = f;
    return s + 1;
}

// long2str/longlong2str: builds digits backwards from bufferEnd, which receives
// a terminator. Returns nullptr when the result would exceed 509 characters.
template < typename Signed, typename Unsigned >
PetariChar16* integerToString(Signed num, PetariChar16* bufferEnd, PrintFormat format) {
    Unsigned unsignedNum = static_cast< Unsigned >(num);
    Unsigned base = 10;
    bool minus = false;
    int digits = 0;
    PetariChar16* p = bufferEnd;
    *--p = 0;

    if (!num && !format.precision && !(format.alternateForm && format.conversion == u'o')) {
        return p;
    }

    switch (format.conversion) {
    case u'd':
    case u'i':
        if (num < 0) {
            unsignedNum = Unsigned(0) - unsignedNum;
            minus = true;
        }
        break;
    case u'o':
        base = 8;
        format.sign = kOnlyMinus;
        break;
    case u'u':
        format.sign = kOnlyMinus;
        break;
    case u'x':
    case u'X':
        base = 16;
        format.sign = kOnlyMinus;
        break;
    }

    do {
        int n = static_cast< int >(unsignedNum % base);
        unsignedNum /= base;
        if (n < 10) {
            n += u'0';
        } else {
            n += (format.conversion == u'x' ? u'a' : u'A') - 10;
        }
        *--p = static_cast< PetariChar16 >(n);
        digits++;
    } while (unsignedNum != 0);

    if (base == 8 && format.alternateForm && *p != u'0') {
        *--p = u'0';
        digits++;
    }

    if (format.justification == kZeroFill) {
        format.precision = format.fieldWidth;
        if (minus || format.sign != kOnlyMinus) {
            format.precision--;
        }
        if (base == 16 && format.alternateForm) {
            format.precision -= 2;
        }
    }

    if (bufferEnd - p + format.precision > kMaxDigits) {
        return nullptr;
    }

    while (digits < format.precision) {
        *--p = u'0';
        digits++;
    }

    if (base == 16 && format.alternateForm) {
        *--p = format.conversion;
        *--p = u'0';
    }

    if (minus) {
        *--p = u'-';
    } else if (format.sign == kSignAlways) {
        *--p = u'+';
    } else if (format.sign == kSpaceHolder) {
        *--p = u' ';
    }

    return p;
}

// Formats a floating-point value with the host C library into buffer and
// returns the number of code units, or -1 if it does not fit.
int floatToString(long double value, const PrintFormat& format, PetariChar16* buffer) {
    char spec[16];
    int length = 0;
    spec[length++] = '%';
    if (format.sign == kSignAlways) {
        spec[length++] = '+';
    } else if (format.sign == kSpaceHolder) {
        spec[length++] = ' ';
    }
    if (format.alternateForm) {
        spec[length++] = '#';
    }
    spec[length++] = '.';
    spec[length++] = '*';
    spec[length++] = 'L';
    spec[length++] = static_cast< char >(format.conversion);
    spec[length] = 0;

    char narrow[kBufferSize];
    int count = std::snprintf(narrow, sizeof(narrow), spec, format.precision, value);
    if (count < 0 || count >= kBufferSize) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        buffer[i] = static_cast< unsigned char >(narrow[i]);
    }
    return count;
}

int format(Output* out, const PetariChar16* formatString, va_list* args) {
    const PetariChar16* formatPtr = formatString;
    int charsWritten = 0;
    PetariChar16 buffer[kBufferSize];
    PetariChar16 fillChar = u' ';

    while (*formatPtr) {
        const PetariChar16* currentFormat = petari_utf16_wcschr(formatPtr, u'%');
        if (!currentFormat) {
            int count = static_cast< int >(petari_utf16_wcslen(formatPtr));
            charsWritten += count;
            write(out, formatPtr, count);
            break;
        }

        int numChars = static_cast< int >(currentFormat - formatPtr);
        charsWritten += numChars;
        write(out, formatPtr, numChars);

        PrintFormat fmt;
        formatPtr = parseFormat(currentFormat, args, &fmt);

        const PetariChar16* text = buffer;
        bool conversionError = false;

        switch (fmt.conversion) {
        case u'd':
        case u'i':
        case u'o':
        case u'u':
        case u'x':
        case u'X': {
            bool isSigned = fmt.conversion == u'd' || fmt.conversion == u'i';
            bool wide = fmt.argument == kLongLongArgument || fmt.argument == kIntmaxArgument || fmt.argument == kPointerArgument;
            PetariChar16* result;
            if (wide) {
                long long value;
                if (fmt.argument == kPointerArgument) {
                    value = static_cast< long long >(reinterpret_cast< uintptr_t >(va_arg(*args, void*)));
                } else {
                    value = va_arg(*args, long long);
                }
                result = integerToString< long long, unsigned long long >(value, buffer + kBufferSize, fmt);
            } else {
                // Wii long, size_t and ptrdiff_t are 32 bits.
                int32_t value;
                if (fmt.argument == kSizeArgument) {
                    value = static_cast< int32_t >(va_arg(*args, size_t));
                } else if (fmt.argument == kPtrdiffArgument) {
                    value = static_cast< int32_t >(va_arg(*args, ptrdiff_t));
                } else {
                    value = va_arg(*args, int32_t);
                }
                if (fmt.argument == kShortArgument) {
                    value = isSigned ? static_cast< short >(value) : static_cast< unsigned short >(value);
                }
                result = integerToString< int32_t, uint32_t >(value, buffer + kBufferSize, fmt);
            }

            if (!result) {
                conversionError = true;
                break;
            }
            text = result;
            numChars = static_cast< int >(buffer + kBufferSize - 1 - result);
            break;
        }
        case u'f':
        case u'F':
        case u'e':
        case u'E':
        case u'g':
        case u'G':
        case u'a':
        case u'A': {
            long double value = fmt.argument == kLongDoubleArgument ? va_arg(*args, long double) : va_arg(*args, double);
            numChars = floatToString(value, fmt, buffer);
            if (numChars < 0) {
                conversionError = true;
            }
            break;
        }
        case u's':
            if (fmt.argument == kWideArgument) {
                const PetariChar16* str = va_arg(*args, const PetariChar16*);
                if (!str) {
                    str = u"";
                }
                if (fmt.alternateForm) {
                    // Length-prefixed string: the first code unit's low byte is the length.
                    numChars = static_cast< unsigned char >(*str++);
                    if (fmt.precisionSpecified && numChars > fmt.precision) {
                        numChars = fmt.precision;
                    }
                } else if (fmt.precisionSpecified) {
                    numChars = 0;
                    while (numChars < fmt.precision && str[numChars] != 0) {
                        numChars++;
                    }
                } else {
                    numChars = static_cast< int >(petari_utf16_wcslen(str));
                }
                text = str;
            } else {
                const char* str = va_arg(*args, const char*);
                if (!str) {
                    str = "";
                }
                // MSL reads the length byte through an unrelated pointer here; this
                // uses the string argument, which is the evident intent.
                if (fmt.alternateForm) {
                    numChars = static_cast< unsigned char >(*str++);
                    if (fmt.precisionSpecified && numChars > fmt.precision) {
                        numChars = fmt.precision;
                    }
                } else if (fmt.precisionSpecified) {
                    const void* end = std::memchr(str, 0, fmt.precision);
                    numChars = end ? static_cast< int >(static_cast< const char* >(end) - str) : fmt.precision;
                } else {
                    numChars = static_cast< int >(std::strlen(str));
                }

                // mbstowcs in MSL's "C" locale widens each byte and stops at NUL.
                int converted = 0;
                while (converted < numChars && converted < kBufferSize && str[converted] != 0) {
                    buffer[converted] = static_cast< unsigned char >(str[converted]);
                    converted++;
                }
                numChars = converted;
                text = buffer;
            }
            break;
        case u'n':
            switch (fmt.argument) {
            case kShortArgument:
                *va_arg(*args, short*) = static_cast< short >(charsWritten);
                break;
            case kLongLongArgument:
            case kIntmaxArgument:
                *va_arg(*args, long long*) = charsWritten;
                break;
            case kSizeArgument:
                *va_arg(*args, size_t*) = charsWritten;
                break;
            case kPtrdiffArgument:
                *va_arg(*args, ptrdiff_t*) = charsWritten;
                break;
            case kCharArgument:
                // MSL stores nothing for %hhn.
                (void)va_arg(*args, void*);
                break;
            default:
                // Wii int and long are both 32 bits.
                *va_arg(*args, int32_t*) = charsWritten;
                break;
            }
            continue;
        case u'c':
            if (fmt.argument == kWideArgument) {
                buffer[0] = static_cast< PetariChar16 >(va_arg(*args, int));
                numChars = 1;
            } else {
                // mbtowc of a NUL byte produces no characters.
                unsigned char ch = static_cast< unsigned char >(va_arg(*args, int));
                buffer[0] = ch;
                numChars = ch != 0 ? 1 : 0;
            }
            break;
        case u'%':
            buffer[0] = u'%';
            numChars = 1;
            break;
        default:
            conversionError = true;
            break;
        }

        if (conversionError) {
            // MSL copies the rest of the format, starting at the bad conversion.
            int count = static_cast< int >(petari_utf16_wcslen(currentFormat));
            charsWritten += count;
            write(out, currentFormat, count);
            return charsWritten;
        }

        int fieldWidth = numChars;
        if (fmt.justification != kLeftJustification) {
            fillChar = fmt.justification == kZeroFill ? u'0' : u' ';
            if ((*text == u'+' || *text == u'-' || *text == u' ') && fillChar == u'0' && numChars > 0) {
                write(out, text, 1);
                text++;
                numChars--;
            }
            while (fieldWidth < fmt.fieldWidth) {
                write(out, &fillChar, 1);
                fieldWidth++;
            }
        }

        write(out, text, numChars);

        if (fmt.justification == kLeftJustification) {
            PetariChar16 blank = u' ';
            while (fieldWidth < fmt.fieldWidth) {
                write(out, &blank, 1);
                fieldWidth++;
            }
        }

        charsWritten += fieldWidth;
    }

    return charsWritten;
}
}  // namespace

extern "C" {
size_t petari_utf16_wcslen(const PetariChar16* str) {
    size_t length = 0;
    while (str[length] != 0) {
        length++;
    }
    return length;
}

PetariChar16* petari_utf16_wcscpy(PetariChar16* dst, const PetariChar16* src) {
    PetariChar16* q = dst;
    while ((*q++ = *src++) != 0) {
    }
    return dst;
}

PetariChar16* petari_utf16_wcsncpy(PetariChar16* dst, const PetariChar16* src, size_t num) {
    size_t i = 0;
    for (; i < num && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    for (; i < num; i++) {
        dst[i] = 0;
    }
    return dst;
}

int petari_utf16_wcscmp(const PetariChar16* str1, const PetariChar16* str2) {
    PetariChar16 c1;
    PetariChar16 c2;
    while ((c1 = *str1++) == (c2 = *str2++)) {
        if (!c1) {
            return 0;
        }
    }
    return static_cast< int >(c1) - static_cast< int >(c2);
}

PetariChar16* petari_utf16_wcschr(const PetariChar16* str, PetariChar16 chr) {
    for (;; str++) {
        if (*str == chr) {
            return const_cast< PetariChar16* >(str);
        }
        if (*str == 0) {
            return nullptr;
        }
    }
}

int petari_utf16_vswprintf(PetariChar16* dst, size_t count, const PetariChar16* formatString, va_list args) {
    Output out = {dst, count, 0};
    va_list copy;
    va_copy(copy, args);
    int written = format(&out, formatString, &copy);
    va_end(copy);

    if (written >= 0) {
        if (static_cast< size_t >(written) < count) {
            dst[written] = 0;
        } else {
            // MSL writes s[n - 1] unconditionally; n == 0 leaves dst untouched.
            if (count != 0) {
                dst[count - 1] = 0;
            }
            written = -1;
        }
    }
    return written;
}

int petari_utf16_swprintf(PetariChar16* dst, size_t count, const PetariChar16* formatString, ...) {
    va_list args;
    va_start(args, formatString);
    int written = petari_utf16_vswprintf(dst, count, formatString, args);
    va_end(args);
    return written;
}
}
