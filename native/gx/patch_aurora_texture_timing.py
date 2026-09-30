#!/usr/bin/env python3
"""Frame telemetry for first-use GX textures (petari/frame_telemetry.hpp): time spent
hashing texture sources and creating/converting/uploading static textures."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
if source.parent.name == 'gx':
    anchor = ('TextureKeys hash_texture_source(const GXTexObj_& obj, const GXTlutObj_* tlut, bool buildSourceKey) {\n'
              '  ZoneScoped;\n')
    counter = 'TextureHash'
elif source.parent.name == 'gfx':
    anchor = ('TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,\n'
              '                                    bool tlut, const char* label) noexcept {\n'
              '  ZoneScoped;\n')
    counter = 'TextureUpload'
else:
    raise SystemExit(f'Unsupported Aurora texture timing source: {source}')
if text.count(anchor) != 1:
    raise SystemExit(f'Aurora texture timing patch anchor mismatch in {source}')
text = text.replace(anchor, anchor + f'  PetariNative::FrameTelemetry::Scope petariTiming{{PetariNative::FrameTelemetry::{counter}}};\n')
if source.parent.name == 'gx':
    # These counters run on the serialized GX resolver; end_frame reads them
    # after draining the FIFO. No unbounded identity history is added for profiling.
    profile = r'''namespace {
struct PetariTextureProfile {
  bool enabled = std::getenv("PETARI_TEXTURE_PROFILE") != nullptr;
  FILE* file = nullptr;
  std::uint64_t hashes = 0, stableHashes = 0, uploads = 0, stableUploads = 0;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~PetariTextureProfile() { if (file) std::fclose(file); }
  void frame(std::uint64_t frame, std::size_t objects, double sweepUs, double sweepCpuUs) {
    if (!enabled) return;
    PetariNative::HostAllocationScope allocations;
    if (!file) {
      file = std::fopen(std::getenv("PETARI_TEXTURE_PROFILE"), "w");
      if (!file) { enabled = false; return; }
      std::fprintf(file, "frame,seconds,objects,hashes,stable_hashes,uploads,stable_uploads,sweep_us,sweep_cpu_us,thread_id,on_main_thread\n");
    }
    std::uint64_t threadId = 0;
    pthread_threadid_np(nullptr, &threadId);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(file, "%llu,%.6f,%zu,%llu,%llu,%llu,%llu,%.3f,%.3f,%llu,%d\n",
                 static_cast<unsigned long long>(frame), seconds, objects,
                 static_cast<unsigned long long>(hashes), static_cast<unsigned long long>(stableHashes),
                 static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(stableUploads), sweepUs, sweepCpuUs,
                 static_cast<unsigned long long>(threadId), pthread_main_np());
    std::fflush(file);  // _Exit on normal game shutdown skips destructors and stdio flushing.
    hashes = stableHashes = uploads = stableUploads = 0;
  }
};
PetariTextureProfile sPetariTextureProfile;
std::uint64_t petariTextureCpuNs() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}
}  // namespace
'''
    text = profile + text
    # A remembered object with an upload is a stable identity. A conservative
    # revalidation usually hashes to a content-cache hit, which is NOT an upload.
    hash_anchor = anchor + f'  PetariNative::FrameTelemetry::Scope petariTiming{{PetariNative::FrameTelemetry::{counter}}};\n'
    text = text.replace(hash_anchor, hash_anchor + '''  if (sPetariTextureProfile.enabled) {
    ++sPetariTextureProfile.hashes;
    const auto it = s_textureObjectCaches.find(obj.texObjId);
    if (it != s_textureObjectCaches.end() && it->second.handle) ++sPetariTextureProfile.stableHashes;
  }
''')
    upload_anchor = '      ++s_stats.misses;\n'
    if text.count(upload_anchor) != 2:
        raise SystemExit('Aurora texture upload profile anchor mismatch')
    text = text.replace(upload_anchor, upload_anchor + '''      if (sPetariTextureProfile.enabled) {
        ++sPetariTextureProfile.uploads;
        const auto it = s_textureObjectCaches.find(obj.texObjId);
        if (it != s_textureObjectCaches.end() && it->second.handle) ++sPetariTextureProfile.stableUploads;
      }
''')
    sweep_anchor = '  sweep_object_caches();\n'
    if text.count(sweep_anchor) != 1:
        raise SystemExit('Aurora texture sweep profile anchor mismatch')
    text = text.replace(sweep_anchor, '''  const auto petariSweepStart = sPetariTextureProfile.enabled ? std::chrono::steady_clock::now()
                                                              : std::chrono::steady_clock::time_point{};
  const auto petariSweepCpuStart = sPetariTextureProfile.enabled ? petariTextureCpuNs() : 0;
  sweep_object_caches();
  if (sPetariTextureProfile.enabled) {
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - petariSweepStart).count();
    const double cpuUs = (petariTextureCpuNs() - petariSweepCpuStart) / 1000.0;
    sPetariTextureProfile.frame(s_frameCount, s_textureObjectCaches.size(), us, cpuUs);
  }
''')
    text = '#include <chrono>\n#include <time.h>\n#include <cstdio>\n#include <cstdlib>\n#include <cstdint>\n#include <pthread.h>\n#include <petari/host_allocation.hpp>\n' + text
text = '#include <petari/frame_telemetry.hpp>\n' + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
