#pragma once

#include <revolution/gx/GXStruct.h>

namespace nw4r {
    namespace ut {
        struct Color : public GXColor {
        public:
            static const int ALPHA_MAX = 255;

            static const u32 WHITE = 0xFFFFFFFF;

            Color() {
                *this = 0xFFFFFFFF;
            }

            Color(u32 color) {
                *this = color;
            }

            Color(const GXColor& color) {
                *this = color;
            }

#ifdef PETARI_NATIVE
            // A packed color is 0xRRGGBBAA, the byte order of the Wii's big-endian
            // u32 view of the r, g, b, a bytes.
            Color& operator=(u32 color) {
                r = static_cast< u8 >(color >> 24);
                g = static_cast< u8 >(color >> 16);
                b = static_cast< u8 >(color >> 8);
                a = static_cast< u8 >(color);
                return *this;
            }

            Color& operator=(const GXColor& color) {
                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
                return *this;
            }

            operator u32() const {
                return static_cast< u32 >(r) << 24 | static_cast< u32 >(g) << 16 | static_cast< u32 >(b) << 8 | a;
            }
#else
            Color& operator=(u32 color) {
                ToU32ref() = color;
                return *this;
            }

            Color& operator=(const GXColor& color) {
                return operator=(*reinterpret_cast< const u32* >(&color));
            }

            operator u32() const {
                return ToU32ref();
            }
#endif

            ~Color() {
            }

            u32& ToU32ref() {
                return *reinterpret_cast< u32* >(this);
            }
            const u32& ToU32ref() const {
                return *reinterpret_cast< const u32* >(this);
            }
        } ATTRIBUTE_ALIGN(4);
    };  // namespace ut
};  // namespace nw4r
