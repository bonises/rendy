#include "rendy/audio/audio.hpp"

#include "rendy/core/log.hpp"

#include <SDL3/SDL.h>

#include <dr_wav.h>

#include "audio/stb_vorbis_decl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace rendy::audio {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr uint32_t kVoiceCount = 32;

// Linear resample + up/down-mix to interleaved stereo at kSampleRate.
std::vector<float> normalizePcm(const float* input, size_t frameCount, int channels,
                                int sampleRate) {
    const size_t outFrames =
        sampleRate == kSampleRate
            ? frameCount
            : static_cast<size_t>(static_cast<double>(frameCount) * kSampleRate / sampleRate);
    std::vector<float> out(outFrames * kChannels);
    const double step = static_cast<double>(sampleRate) / kSampleRate;
    for (size_t i = 0; i < outFrames; ++i) {
        const double srcPos = static_cast<double>(i) * step;
        const size_t i0 = std::min(static_cast<size_t>(srcPos), frameCount - 1);
        const size_t i1 = std::min(i0 + 1, frameCount - 1);
        const float t = static_cast<float>(srcPos - static_cast<double>(i0));
        for (int c = 0; c < kChannels; ++c) {
            const int srcChannel = channels == 1 ? 0 : std::min(c, channels - 1);
            const float a = input[i0 * static_cast<size_t>(channels) + static_cast<size_t>(srcChannel)];
            const float b = input[i1 * static_cast<size_t>(channels) + static_cast<size_t>(srcChannel)];
            out[i * kChannels + static_cast<size_t>(c)] = a + (b - a) * t;
        }
    }
    return out;
}

} // namespace

namespace detail {

// A background-decoded ogg: the feeder thread keeps the SPSC ring topped
// up (writer: feeder, reader: audio callback).
struct Stream {
    stb_vorbis* vorbis = nullptr;
    int srcRate = 0;
    int srcChannels = 0;
    std::atomic<bool> loop{false};
    std::atomic<bool> ended{false};   ///< decode hit EOF and loop is off
    std::atomic<bool> restart{false}; ///< play() wants a rewind

    static constexpr size_t kRingFrames = 32768; // power of two, ~0.68 s
    std::vector<float> ring = std::vector<float>(kRingFrames * kChannels);
    // Monotonic frame counters; masked into the ring on access.
    std::atomic<size_t> writePos{0};
    std::atomic<size_t> readPos{0};

    // Feeder-only resample state: stereo frames at srcRate + fractional
    // read cursor into them.
    std::vector<float> pending;
    double cursor = 0.0;
    std::vector<float> decodeBuf;
    bool tailPadded = false; ///< last source frame duplicated at non-loop EOF

    ~Stream() {
        if (vorbis != nullptr) stb_vorbis_close(vorbis);
    }
};

struct Sound {
    std::vector<float> frames; // interleaved stereo 48 kHz
    std::unique_ptr<Stream> stream; // set instead of frames for openStream
    [[nodiscard]] size_t frameCount() const { return frames.size() / kChannels; }
};

struct Voice {
    std::atomic<bool> active{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> loop{false};
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<uint32_t> generation{0};
    // Non-atomic state: written by play()/unload() only while holding the
    // stream lock (callback excluded), read/advanced by the audio thread.
    const Sound* sound = nullptr;
    size_t position = 0;
};

struct MixerImpl {
    SDL_AudioStream* stream = nullptr;
    bool ownsAudioInit = false;

    // Sounds are only appended; the audio thread reads via raw pointers that
    // stay valid because we store unique_ptrs.
    std::mutex soundsMutex;
    std::vector<std::unique_ptr<Sound>> sounds;

    Voice voices[kVoiceCount];
    std::atomic<float> masterVolume{1.0f};

    // Feeder thread for streamed sounds (started on first openStream).
    std::thread feeder;
    std::mutex feederMutex;
    std::condition_variable feederWake;
    bool feederQuit = false;
    bool feederRunning = false;

    void startFeeder() {
        if (feederRunning) return;
        feederRunning = true;
        feeder = std::thread([this] { feederLoop(); });
    }

    void feederLoop() {
        std::unique_lock lock(feederMutex);
        while (!feederQuit) {
            feederWake.wait_for(lock, std::chrono::milliseconds(30));
            if (feederQuit) break;
            lock.unlock();
            {
                std::lock_guard soundsLock(soundsMutex);
                for (auto& sound : sounds)
                    if (sound != nullptr && sound->stream != nullptr)
                        fillStream(*sound->stream);
            }
            lock.lock();
        }
    }

    /// Decode + resample until the ring is comfortably full. Feeder only.
    void fillStream(Stream& st) {
        if (st.restart.exchange(false)) {
            stb_vorbis_seek_start(st.vorbis);
            st.pending.clear();
            st.cursor = 0.0;
            st.tailPadded = false;
            st.ended.store(false, std::memory_order_relaxed);
            // Empty the ring without racing the callback.
            StreamLock streamLock(stream);
            st.readPos.store(0, std::memory_order_relaxed);
            st.writePos.store(0, std::memory_order_relaxed);
        }
        if (st.ended.load(std::memory_order_relaxed)) return;

        const double step = static_cast<double>(st.srcRate) / kSampleRate;
        while (true) {
            const size_t used = st.writePos.load(std::memory_order_relaxed) -
                                st.readPos.load(std::memory_order_acquire);
            if (used + 1024 > Stream::kRingFrames) return; // comfortably full

            // Ensure source material for at least one output frame.
            while (st.pending.size() / kChannels < st.cursor + 2.0) {
                constexpr int kDecodeFrames = 2048;
                st.decodeBuf.resize(static_cast<size_t>(kDecodeFrames) *
                                    static_cast<size_t>(st.srcChannels));
                const int got = stb_vorbis_get_samples_float_interleaved(
                    st.vorbis, st.srcChannels, st.decodeBuf.data(), kDecodeFrames * st.srcChannels);
                if (got <= 0) {
                    if (st.loop.load(std::memory_order_relaxed)) {
                        stb_vorbis_seek_start(st.vorbis);
                        continue;
                    }
                    // The linear resampler needs two source frames; duplicate
                    // the last one once so the final real frame gets played.
                    if (!st.tailPadded && st.pending.size() >= kChannels) {
                        st.tailPadded = true;
                        const size_t last = st.pending.size() - kChannels;
                        for (int c = 0; c < kChannels; ++c)
                            st.pending.push_back(st.pending[last + static_cast<size_t>(c)]);
                        continue;
                    }
                    st.ended.store(true, std::memory_order_release);
                    return;
                }
                // Down/up-mix to stereo and append.
                const size_t base = st.pending.size();
                st.pending.resize(base + static_cast<size_t>(got) * kChannels);
                for (int i = 0; i < got; ++i) {
                    for (int c = 0; c < kChannels; ++c) {
                        const int srcC = st.srcChannels == 1 ? 0 : std::min(c, st.srcChannels - 1);
                        st.pending[base + static_cast<size_t>(i) * kChannels +
                                   static_cast<size_t>(c)] =
                            st.decodeBuf[static_cast<size_t>(i) *
                                             static_cast<size_t>(st.srcChannels) +
                                         static_cast<size_t>(srcC)];
                    }
                }
            }

            // Resample one output frame into the ring.
            const auto i0 = static_cast<size_t>(st.cursor);
            const float t = static_cast<float>(st.cursor - static_cast<double>(i0));
            const size_t write = st.writePos.load(std::memory_order_relaxed);
            const size_t slot = (write & (Stream::kRingFrames - 1)) * kChannels;
            for (int c = 0; c < kChannels; ++c) {
                const float a = st.pending[i0 * kChannels + static_cast<size_t>(c)];
                const float b = st.pending[(i0 + 1) * kChannels + static_cast<size_t>(c)];
                st.ring[slot + static_cast<size_t>(c)] = a + (b - a) * t;
            }
            st.writePos.store(write + 1, std::memory_order_release);
            st.cursor += step;

            // Trim consumed whole source frames off the front now and then.
            if (st.cursor > 4096.0) {
                const auto whole = static_cast<size_t>(st.cursor) - 1;
                st.pending.erase(st.pending.begin(),
                                 st.pending.begin() +
                                     static_cast<long>(whole * kChannels));
                st.cursor -= static_cast<double>(whole);
            }
        }
    }
    // Fixed mix buffer: the callback mixes in chunks of at most
    // kMaxChunkFrames so it NEVER allocates, whatever SDL requests.
    static constexpr int kMaxChunkFrames = 4096;
    float scratch[kMaxChunkFrames * kChannels];

    // Excludes the audio callback while voice state is rearranged.
    struct StreamLock {
        SDL_AudioStream* stream;
        explicit StreamLock(SDL_AudioStream* s) : stream(s) {
            if (stream != nullptr) SDL_LockAudioStream(stream);
        }
        ~StreamLock() {
            if (stream != nullptr) SDL_UnlockAudioStream(stream);
        }
        StreamLock(const StreamLock&) = delete;
        StreamLock& operator=(const StreamLock&) = delete;
    };

    static void callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int) {
        auto* self = static_cast<MixerImpl*>(userdata);
        constexpr int kFrameBytes = kChannels * static_cast<int>(sizeof(float));
        int frames = additionalAmount / kFrameBytes;
        while (frames > 0) {
            const int chunk = std::min(frames, kMaxChunkFrames);
            std::fill_n(self->scratch, static_cast<size_t>(chunk) * kChannels, 0.0f);
            self->mix(self->scratch, static_cast<size_t>(chunk));
            SDL_PutAudioStreamData(stream, self->scratch, chunk * kFrameBytes);
            frames -= chunk;
        }
    }

    void mix(float* out, size_t frames) {
        const float master = masterVolume.load(std::memory_order_relaxed);
        for (Voice& voice : voices) {
            if (!voice.active.load(std::memory_order_acquire)) continue;
            if (voice.paused.load(std::memory_order_relaxed)) continue;
            const Sound* sound = voice.sound;
            if (sound == nullptr ||
                (sound->stream == nullptr && sound->frameCount() == 0)) {
                voice.active.store(false, std::memory_order_release);
                continue;
            }

            const float volume = voice.volume.load(std::memory_order_relaxed) * master;
            const float pan = std::clamp(voice.pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
            // Constant-power pan.
            const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
            const float leftGain = volume * std::cos(angle);
            const float rightGain = volume * std::sin(angle);
            const bool loop = voice.loop.load(std::memory_order_relaxed);

            if (sound->stream != nullptr) {
                // Streamed: consume the feeder's ring; underruns play
                // silence but keep the voice alive until the decode ends.
                Stream& st = *sound->stream;
                // A pending restart means the ring still holds the previous
                // playback's tail — play silence until the feeder has rewound
                // instead of up to ~0.7 s of stale audio.
                if (st.restart.load(std::memory_order_acquire)) continue;
                size_t read = st.readPos.load(std::memory_order_relaxed);
                const size_t write = st.writePos.load(std::memory_order_acquire);
                const size_t available = write - read;
                const size_t count = std::min(frames, available);
                for (size_t i = 0; i < count; ++i) {
                    const size_t slot = (read & (Stream::kRingFrames - 1)) * kChannels;
                    out[i * kChannels] += st.ring[slot] * leftGain;
                    out[i * kChannels + 1] += st.ring[slot + 1] * rightGain;
                    ++read;
                }
                st.readPos.store(read, std::memory_order_release);
                if (available == 0 && st.ended.load(std::memory_order_acquire))
                    voice.active.store(false, std::memory_order_release);
                (void)loop; // decode-level looping is handled by the feeder
                continue;
            }

            const size_t total = sound->frameCount();
            size_t position = voice.position;
            for (size_t i = 0; i < frames; ++i) {
                if (position >= total) {
                    if (!loop) break;
                    position = 0;
                }
                out[i * kChannels] += sound->frames[position * kChannels] * leftGain;
                out[i * kChannels + 1] += sound->frames[position * kChannels + 1] * rightGain;
                position++;
            }
            voice.position = position;
            if (position >= total && !loop)
                voice.active.store(false, std::memory_order_release);
        }
    }
};

} // namespace detail

using detail::MixerImpl;
using detail::Sound;
using detail::Stream;
using detail::Voice;

Mixer::Mixer() : impl_(std::make_unique<MixerImpl>()) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            log::error("audio: SDL_InitSubSystem failed: {}", SDL_GetError());
            return;
        }
        impl_->ownsAudioInit = true;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, kChannels, kSampleRate};
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                              &MixerImpl::callback, impl_.get());
    if (impl_->stream == nullptr) {
        log::error("audio: failed to open output device: {}", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(impl_->stream);
    log::debug("audio: output open ({} Hz, {} ch)", kSampleRate, kChannels);
}

Mixer::Mixer(Mixer&&) noexcept = default;
Mixer& Mixer::operator=(Mixer&&) noexcept = default;

Mixer::~Mixer() {
    if (impl_ && impl_->feederRunning) {
        {
            std::lock_guard lock(impl_->feederMutex);
            impl_->feederQuit = true;
        }
        impl_->feederWake.notify_all();
        impl_->feeder.join();
    }
    if (impl_ && impl_->stream != nullptr) SDL_DestroyAudioStream(impl_->stream);
    if (impl_ && impl_->ownsAudioInit) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Mixer::ok() const { return impl_->stream != nullptr; }

Result<SoundRef> Mixer::createSound(const float* frames, size_t frameCount, int channels,
                                    int sampleRate) {
    if (frames == nullptr) return err("audio: null PCM pointer");
    if (channels < 1 || channels > 8 || frameCount == 0 || frameCount > (1u << 31))
        return err("audio: invalid PCM ({} channels, {} frames)", channels, frameCount);
    if (sampleRate < 1000 || sampleRate > 768000)
        return err("audio: unsupported sample rate {}", sampleRate);
    // Cap decoded size (~1 GB of stereo float) so a corrupt header can't
    // request an absurd allocation.
    const size_t outFrames =
        static_cast<size_t>(static_cast<double>(frameCount) * kSampleRate / sampleRate);
    if (outFrames > (1u << 27))
        return err("audio: sound too long ({} frames at {} Hz)", frameCount, sampleRate);
    auto sound = std::make_unique<Sound>();
    sound->frames = normalizePcm(frames, frameCount, channels, sampleRate);
    std::lock_guard lock(impl_->soundsMutex);
    impl_->sounds.push_back(std::move(sound));
    return SoundRef{static_cast<uint32_t>(impl_->sounds.size() - 1)};
}

Result<SoundRef> Mixer::load(const std::string& path) {
    const auto dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);

    if (ext == "wav" || ext == "WAV") {
        unsigned int channels = 0;
        unsigned int sampleRate = 0;
        drwav_uint64 frameCount = 0;
        float* pcm = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels,
                                                             &sampleRate, &frameCount, nullptr);
        if (pcm == nullptr) return err("audio: failed to load wav '{}'", path);
        auto result = createSound(pcm, frameCount, static_cast<int>(channels),
                                  static_cast<int>(sampleRate));
        drwav_free(pcm, nullptr);
        return result;
    }
    if (ext == "ogg" || ext == "OGG") {
        int channels = 0;
        int sampleRate = 0;
        short* pcm = nullptr;
        const int frameCount =
            stb_vorbis_decode_filename(path.c_str(), &channels, &sampleRate, &pcm);
        if (frameCount <= 0 || pcm == nullptr)
            return err("audio: failed to load ogg '{}'", path);
        std::vector<float> floatPcm(static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
        for (size_t i = 0; i < floatPcm.size(); ++i)
            floatPcm[i] = static_cast<float>(pcm[i]) / 32768.0f;
        free(pcm);
        return createSound(floatPcm.data(), static_cast<size_t>(frameCount), channels,
                           sampleRate);
    }
    return err("audio: unsupported format '{}' (wav/ogg supported)", path);
}

Result<SoundRef> Mixer::openStream(const std::string& path) {
    const auto dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    if (ext != "ogg" && ext != "OGG")
        return err("audio: only .ogg can be streamed ('{}')", path);

    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_filename(path.c_str(), &error, nullptr);
    if (vorbis == nullptr)
        return err("audio: failed to open ogg stream '{}' (error {})", path, error);
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels < 1 || info.channels > 8 || info.sample_rate < 1000 ||
        info.sample_rate > 768000) {
        stb_vorbis_close(vorbis);
        return err("audio: unsupported ogg stream '{}' ({} ch, {} Hz)", path, info.channels,
                   info.sample_rate);
    }

    auto sound = std::make_unique<Sound>();
    sound->stream = std::make_unique<Stream>();
    sound->stream->vorbis = vorbis;
    sound->stream->srcRate = static_cast<int>(info.sample_rate);
    sound->stream->srcChannels = info.channels;
    std::lock_guard lock(impl_->soundsMutex);
    impl_->sounds.push_back(std::move(sound));
    impl_->startFeeder();
    impl_->feederWake.notify_all();
    return SoundRef{static_cast<uint32_t>(impl_->sounds.size() - 1)};
}

void Mixer::unload(SoundRef sound) {
    std::lock_guard lock(impl_->soundsMutex);
    if (sound.id >= impl_->sounds.size() || impl_->sounds[sound.id] == nullptr) return;
    {
        // The callback must not be mid-mix on this sound while we free it.
        MixerImpl::StreamLock streamLock(impl_->stream);
        for (Voice& voice : impl_->voices)
            if (voice.sound == impl_->sounds[sound.id].get()) {
                voice.active.store(false, std::memory_order_release);
                voice.sound = nullptr;
            }
        impl_->sounds[sound.id].reset(); // slot stays; the ref becomes inert
    }
}

VoiceRef Mixer::play(SoundRef soundRef, const PlayOptions& options) {
    const Sound* sound = nullptr;
    {
        std::lock_guard lock(impl_->soundsMutex);
        if (soundRef.id >= impl_->sounds.size()) return {};
        sound = impl_->sounds[soundRef.id].get();
    }
    if (sound == nullptr) return {}; // unloaded

    // Claim under the stream lock: without it the callback could be mid-mix
    // on this voice (seen as inactive → finished) and clobber our fresh
    // sound/position with its stale write-back.
    MixerImpl::StreamLock streamLock(impl_->stream);
    if (sound->stream != nullptr) {
        // One voice per stream: stop any other voice playing it and ask
        // the feeder to rewind the decode.
        for (Voice& voice : impl_->voices)
            if (voice.sound == sound) voice.active.store(false, std::memory_order_release);
        sound->stream->loop.store(options.loop, std::memory_order_relaxed);
        sound->stream->restart.store(true, std::memory_order_release);
        impl_->feederWake.notify_all();
    }
    for (uint32_t i = 0; i < kVoiceCount; ++i) {
        Voice& voice = impl_->voices[i];
        if (voice.active.load(std::memory_order_acquire)) continue;
        const uint32_t generation = voice.generation.fetch_add(1) + 1;
        voice.sound = sound;
        voice.position = 0;
        voice.volume.store(options.volume, std::memory_order_relaxed);
        voice.pan.store(options.pan, std::memory_order_relaxed);
        voice.loop.store(options.loop, std::memory_order_relaxed);
        voice.paused.store(false, std::memory_order_relaxed);
        voice.active.store(true, std::memory_order_release);
        return VoiceRef{i, generation};
    }
    log::debug("audio: all {} voices busy, dropping sound", kVoiceCount);
    return {};
}

void Mixer::stop(VoiceRef voice) {
    if (!voice.valid() || voice.index >= kVoiceCount) return;
    Voice& v = impl_->voices[voice.index];
    if (v.generation.load() == voice.generation)
        v.active.store(false, std::memory_order_release);
}

void Mixer::stopAll() {
    for (Voice& voice : impl_->voices) voice.active.store(false, std::memory_order_release);
}

void Mixer::setPaused(VoiceRef voice, bool paused) {
    if (!voice.valid() || voice.index >= kVoiceCount) return;
    Voice& v = impl_->voices[voice.index];
    if (v.generation.load() == voice.generation)
        v.paused.store(paused, std::memory_order_relaxed);
}

bool Mixer::playing(VoiceRef voice) const {
    if (!voice.valid() || voice.index >= kVoiceCount) return false;
    const Voice& v = impl_->voices[voice.index];
    return v.generation.load() == voice.generation &&
           v.active.load(std::memory_order_acquire);
}

void Mixer::setVolume(VoiceRef voice, float volume) {
    if (!voice.valid() || voice.index >= kVoiceCount) return;
    Voice& v = impl_->voices[voice.index];
    if (v.generation.load() == voice.generation)
        v.volume.store(volume, std::memory_order_relaxed);
}

void Mixer::setPan(VoiceRef voice, float pan) {
    if (!voice.valid() || voice.index >= kVoiceCount) return;
    Voice& v = impl_->voices[voice.index];
    if (v.generation.load() == voice.generation) v.pan.store(pan, std::memory_order_relaxed);
}

void Mixer::setMasterVolume(float volume) {
    impl_->masterVolume.store(volume, std::memory_order_relaxed);
}
float Mixer::masterVolume() const { return impl_->masterVolume.load(std::memory_order_relaxed); }

} // namespace rendy::audio
