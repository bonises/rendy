#pragma once

/// \file audio.hpp
/// Sound playback: a fixed-pool mixer over the system's default output.
///
///     rendy::audio::Mixer mixer;
///     auto music = mixer.load("theme.ogg").value();
///     mixer.play(music, {.loop = true, .volume = 0.6f});

#include "../core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace rendy::audio {

namespace detail {
struct MixerImpl;
}

struct SoundRef {
    uint32_t id = UINT32_MAX;
    [[nodiscard]] bool valid() const { return id != UINT32_MAX; }
};

/// Handle to one playing instance. Stale handles (finished/stolen voices)
/// are safely ignored by all calls.
struct VoiceRef {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
};

struct PlayOptions {
    float volume = 1.0f;
    float pan = 0.0f; ///< -1 left … +1 right (constant power)
    bool loop = false;
};

class Mixer {
public:
    /// Opens the default playback device (48 kHz float stereo). Failure is
    /// logged and the mixer becomes silent but safe to use.
    Mixer();
    Mixer(Mixer&&) noexcept;
    Mixer& operator=(Mixer&&) noexcept;
    ~Mixer();

    [[nodiscard]] bool ok() const;

    /// Load a .wav or .ogg file, decoded and resampled at load time.
    Result<SoundRef> load(const std::string& path);
    /// Create a sound from raw PCM (interleaved float, any channel count 1/2).
    Result<SoundRef> createSound(const float* frames, size_t frameCount, int channels,
                                 int sampleRate);
    void unload(SoundRef sound);

    VoiceRef play(SoundRef sound, const PlayOptions& options = {});
    void stop(VoiceRef voice);
    void stopAll();
    void setPaused(VoiceRef voice, bool paused);
    [[nodiscard]] bool playing(VoiceRef voice) const;
    void setVolume(VoiceRef voice, float volume);
    void setPan(VoiceRef voice, float pan);

    void setMasterVolume(float volume);
    [[nodiscard]] float masterVolume() const;

private:
    std::unique_ptr<detail::MixerImpl> impl_;
};

} // namespace rendy::audio
