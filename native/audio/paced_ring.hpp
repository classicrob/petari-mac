#pragma once
// Paced AI output for a host audio device (sdl_sink.cpp), kept free of SDL so
// it can be tested against a simulated device clock (audio_sdl_tests.cpp).
//
// The AI DMA engine (Platform::Audio::pull) must advance about in real time:
// each block start raises the DMA interrupt, and the game's audio thread then
// prepares the next block. A device that requests large bursts must therefore
// not pull them straight from AI. A producer thread pulls small quanta into
// this ring, and the device callback only drains it.
//
// Pacing is level-driven: each producer tick pulls what the ring lacks
// relative to its target level. That locks the long-run pull rate to the
// device's consumption (its clock) rather than the host's, so rate drift
// never accumulates. How much a tick may pull is bounded by real time
// (Pacer), with at most 5 ms of credit, so late or coalesced ticks cannot
// starve the ring and a stall is never made up in a burst.
//
// The bound is also the game's deadline. The DMA engine advances only as the
// producer pulls, and JAudio2 must register the next block before the current
// one (560 frames, 17.5 ms at 32 kHz) has been pulled through. A device that
// takes large bursts leaves the ring short by a whole burst each time; refilled
// at 2x, the engine alternated between double speed and a halt, so a block
// could end 8.75 ms after it started and the game's audio thread, answering in
// 9-16 ms, had blocks replayed (observatory-3, story-9 logs). The engine
// therefore runs at most 5% fast (a block lasts at least 16.7 ms); only below
// the low-water mark, where the next device burst would underrun, may it catch
// up faster, at 1.25x (14 ms blocks). Not 2x: when the level is low because the
// game was late (the engine waited for it), doubling the pace halves the
// deadline of a game already behind, and it falls further behind.
// Pulled audio is never discarded: the producer only pulls what fits.
//
// Single producer, single consumer.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace PetariNative::AudioSDL::Detail {

class PacedRing {
public:
    static constexpr std::uint64_t kCapacity = 8192;  // frames

    // Frames of silence queued before the first pull, and the lowest target
    // level (latency floor).
    void reset(std::uint64_t prebuffer) {
        mSamples.fill(0);
        mRead.store(0, std::memory_order_relaxed);
        mWrite.store(prebuffer, std::memory_order_relaxed);
        mMinTarget = prebuffer;
        mLargestRequest.store(0, std::memory_order_relaxed);
        mUnderrun.store(0, std::memory_order_relaxed);
    }

    std::uint64_t level() const {
        return mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire);
    }

    // The level the producer maintains: the largest device request plus the
    // prebuffer, so that even right before a device burst the ring holds the
    // prebuffer's worth (the host stall it rides out).
    static constexpr std::uint64_t kHeadroom = 1024;  // never target a full ring

    std::uint64_t target() const {
        const std::uint64_t wanted = mMinTarget + mLargestRequest.load(std::memory_order_relaxed);
        return std::min<std::uint64_t>(wanted, kCapacity - kHeadroom);
    }

    // Below this level the next device burst would (nearly) underrun: the
    // largest request plus 8 ms at 32 kHz.
    static constexpr std::uint64_t kLowWaterMargin = 256;
    std::uint64_t lowWater() const { return mLargestRequest.load(std::memory_order_relaxed) + kLowWaterMargin; }

    // Producer: frames to pull this tick, at most maxChunk.
    std::size_t wanted(std::size_t maxChunk) const {
        const std::uint64_t have = level();
        const std::uint64_t goal = target();
        if (have >= goal) {
            return 0;
        }
        const std::uint64_t space = kCapacity - have;
        return static_cast<std::size_t>(std::min<std::uint64_t>({goal - have, maxChunk, space}));
    }

    // Producer: append interleaved stereo frames (count <= space; wanted()
    // guarantees it).
    void write(const std::int16_t* frames, std::size_t count) {
        const std::uint64_t w = mWrite.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t at = (w + i) % kCapacity;
            mSamples[at * 2] = frames[i * 2];
            mSamples[at * 2 + 1] = frames[i * 2 + 1];
        }
        mWrite.store(w + count, std::memory_order_release);
    }

    // Consumer: the device's whole request (it may then be read in pieces);
    // the target covers the largest one.
    void noteRequest(std::size_t count) {
        if (count > mLargestRequest.load(std::memory_order_relaxed)) {
            mLargestRequest.store(count, std::memory_order_relaxed);
        }
    }

    // Consumer: fill count frames, zero-filling what is missing (an
    // underrun). Returns the frames that came from the ring.
    std::size_t read(std::int16_t* out, std::size_t count) {
        noteRequest(count);
        const std::uint64_t r = mRead.load(std::memory_order_relaxed);
        const std::uint64_t available = mWrite.load(std::memory_order_acquire) - r;
        const std::size_t copied = static_cast<std::size_t>(std::min<std::uint64_t>(count, available));
        for (std::size_t i = 0; i < copied; ++i) {
            const std::uint64_t at = (r + i) % kCapacity;
            out[i * 2] = mSamples[at * 2];
            out[i * 2 + 1] = mSamples[at * 2 + 1];
        }
        std::fill(out + copied * 2, out + count * 2, 0);
        mRead.store(r + copied, std::memory_order_release);
        if (copied < count) {
            mUnderrun.fetch_add(count - copied, std::memory_order_relaxed);
        }
        return copied;
    }

    std::size_t largestRequest() const { return mLargestRequest.load(std::memory_order_relaxed); }
    std::uint64_t underrunFrames() const { return mUnderrun.load(std::memory_order_relaxed); }
    std::uint64_t takeUnderrunFrames() { return mUnderrun.exchange(0, std::memory_order_relaxed); }

private:
    std::array<std::int16_t, kCapacity * 2> mSamples{};
    std::atomic<std::uint64_t> mRead{0}, mWrite{0};
    std::uint64_t mMinTarget = 0;
    std::atomic<std::size_t> mLargestRequest{0};
    std::atomic<std::uint64_t> mUnderrun{0};
};

// Real-time bound on the pull rate: a tick may pull the audio time elapsed
// since the previous tick (at most 5 ms of it) times kTrim, or times kCatchUp
// while the ring is below its low-water mark. Fractions of a frame carry over
// to the next tick, so the average speed is exact. Producer thread only.
class Pacer {
public:
    explicit Pacer(std::uint32_t rate) : mRate(rate) {}
    // elapsedSeconds: real time since the previous tick.
    std::size_t allowance(double elapsedSeconds, const PacedRing& ring) {
        const double credited = std::min(std::max(elapsedSeconds, 0.0), kMaxCreditSeconds);
        const double speed = ring.level() < ring.lowWater() ? kCatchUp : kTrim;
        const double frames = speed * mRate * credited + mCarry;
        const std::size_t whole = static_cast<std::size_t>(frames);
        mCarry = frames - static_cast<double>(whole);
        return std::max<std::size_t>(1, whole);
    }
    static constexpr double kTrim = 1.05;
    static constexpr double kCatchUp = 1.25;
    static constexpr double kMaxCreditSeconds = 0.005;

private:
    std::uint32_t mRate;
    double mCarry = 0.0;
};

// One producer tick: pulls what the ring wants, in pieces of at most quantum
// frames. pull(out, frames) is Platform::Audio::pull in the sink and returns
// the frames it produced; fewer than asked means the DMA engine is waiting for
// the game's next block, and the tick ends. Returns the frames pulled.
template <class Pull>
std::size_t produceTick(PacedRing& ring, std::size_t quantum, std::size_t maxChunk, Pull&& pull) {
    std::array<std::int16_t, 2 * 256> block;
    std::size_t total = 0;
    std::size_t wanted = ring.wanted(maxChunk);
    while (wanted > 0) {
        const std::size_t n = std::min({wanted, quantum, block.size() / 2});
        const std::size_t got = std::min<std::size_t>(pull(block.data(), n), n);
        ring.write(block.data(), got);
        total += got;
        if (got < n) {
            break;
        }
        wanted -= n;
    }
    return total;
}

}  // namespace PetariNative::AudioSDL::Detail
