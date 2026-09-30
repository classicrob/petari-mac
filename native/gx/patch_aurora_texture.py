#!/usr/bin/env python3
"""Keep negative GX texture LOD bias conversions defined on the host, and sample
depth-format textures as the texture unit does."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
old = 'static_cast<u8>(32.0f * clampedBias)'
if text.count(old) != 2:
    raise SystemExit('Aurora texture LOD patch anchor mismatch')
text = text.replace(old, 'static_cast<u8>(static_cast<int>(32.0f * clampedBias))')

# The texture unit's format field is 4 bits (init_texobj_common writes
# format & 0xF into image0): GX_TF_Z8, GX_TF_Z16 and GX_TF_Z24X8 (I8, IA8 and
# RGBA8 | _GX_TF_ZTF) are sampled as those base formats; the ZTF bit only
# matters to GXSetZTexture. Aurora's converter knows only the base formats and
# aborted ("unknown texture format 22") when a draw sampled texture map 0 still
# holding the game's 4x4 GX_TF_Z24X8 depth-clear texture (Petari: Good Egg's
# launch star). GXGetTexObjFmt keeps the full format.
old = '  GX_WRITE_U32(obj.format());\n'
if text.count(old) != 1:
    raise SystemExit('Aurora texture depth-format patch anchor mismatch')
text = text.replace(old, '''  u32 sampledFormat = obj.format();
  if ((sampledFormat & (_GX_TF_ZTF | _GX_TF_CTF)) == _GX_TF_ZTF) {
    sampledFormat &= 0xF;
    static bool sLogged[16] = {};
    if (!sLogged[sampledFormat]) {
      sLogged[sampledFormat] = true;
      std::fprintf(stderr, "[gx texture] depth-format texture 0x%x (%ux%u at %p) loaded for sampling; sampled as base format 0x%x\\n",
                   obj.format(), obj.width(), obj.height(), obj.data, sampledFormat);
    }
  }
  GX_WRITE_U32(sampledFormat);
''')
# CPU-written textures. The Wii GPU reads texels from memory: a texture the game
# rewrites in place (SnowFloor, fur density, normal maps, Mario's dissolve mask) and
# stores with DCStoreRange/DCFlushRange shows its new texels at the next draw
# (GXInvalidateTexAll drops the texture cache). Aurora uploads once per texture
# object and texDataVersion, and only GXInitTexObj/GXInitTexObjData change those,
# so a texture object reused over rewritten memory kept its first upload. The host
# store log (native/platform/os/os_cache.cpp) records stores by page; when a loaded
# texture's bytes were stored since this object was last loaded, bump its version
# so the texture is hashed again (and re-uploaded only if the texels changed).
old = 'void GXLoadTexObj(GXTexObj* obj_, GXTexMapID id) {\n'
if text.count(old) != 1:
    raise SystemExit('Aurora texture store-revalidation patch anchor mismatch (GXLoadTexObj)')
text = text.replace(old, '''extern "C" __attribute__((weak)) std::uint64_t petari_dc_store_generation(void) { return 0; }
extern "C" __attribute__((weak)) int petari_dc_stored_since(const void*, std::size_t, std::uint64_t) { return 0; }

namespace {
struct PetariTexStoreSeen {
  std::uint64_t generation = 0;
  std::chrono::steady_clock::time_point lastLoad;
};
std::unordered_map<u32, PetariTexStoreSeen> sPetariTexStoreSeen;
std::uint64_t sPetariTexLoads = 0;
// Aurora drops an object's cached upload after ObjectCacheIdleFrames (600) frames
// unused. Remembering objects for longer than that at any frame rate above 1 fps
// means a texture object seen for the first time has no cached upload that could
// be stale, so it never needs a bump.
constexpr auto kPetariTexForgetAfter = std::chrono::minutes(10);
}  // namespace

// Petari: returns true (and bumps texDataVersion) when the texture's bytes were stored
// since this texture object was last loaded.
extern "C" bool petari_gx_revalidate_texobj(GXTexObj* obj_) {
  // Called on game threads from GXLoadTexObj: keep the bookkeeping map off the game's
  // current JKR heap (it can be the tiny system heap during the logo).
  PetariNative::HostAllocationScope petariHostAllocations;
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  if (obj->data == nullptr || obj->texObjId == 0) {
    return false;
  }
  const std::uint64_t now = petari_dc_store_generation();
  const std::uint64_t load = ++sPetariTexLoads;
  const auto clock = std::chrono::steady_clock::now();
  bool stale = false;
  auto it = sPetariTexStoreSeen.find(obj->texObjId);
  if (it == sPetariTexStoreSeen.end()) {
    it = sPetariTexStoreSeen.emplace(obj->texObjId, PetariTexStoreSeen{now, clock}).first;
  } else if (it->second.generation != now) {
    const u32 size = GXGetTexBufferSize(static_cast<u16>(obj->width()), static_cast<u16>(obj->height()), obj->format(),
                                        obj->has_mips(), obj->has_mips() ? 11 : 0);
    stale = petari_dc_stored_since(obj->data, size, it->second.generation) != 0;
  }
  it->second.generation = now;
  it->second.lastLoad = clock;
  if ((load & 0x3FFF) == 0) {
    for (auto sweep = sPetariTexStoreSeen.begin(); sweep != sPetariTexStoreSeen.end();) {
      sweep = clock - sweep->second.lastLoad > kPetariTexForgetAfter ? sPetariTexStoreSeen.erase(sweep) : std::next(sweep);
    }
  }
  if (stale) {
    ++obj->texDataVersion;
    static int sLogged = 0;
    if (sLogged < 16) {
      ++sLogged;
      std::fprintf(stderr, "[gx texture] texels stored since the last load; reloading texture object %u (%ux%u format 0x%x at %p)\\n",
                   obj->texObjId, obj->width(), obj->height(), obj->format(), obj->data);
    }
  }
  return stale;
}

void GXLoadTexObj(GXTexObj* obj_, GXTexMapID id) {
  petari_gx_revalidate_texobj(obj_);
''')
if '#include <petari/host_allocation.hpp>' not in text:
    text = '#include <petari/host_allocation.hpp>\n' + text
if '#include <unordered_map>' not in text:
    text = text.replace('#include <algorithm>\n', '#include <algorithm>\n#include <chrono>\n#include <cstddef>\n#include <cstdint>\n#include <unordered_map>\n', 1)

if '#include <cstdio>' not in text:
    text = text.replace('#include <algorithm>\n', '#include <algorithm>\n#include <cstdio>\n', 1)
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
