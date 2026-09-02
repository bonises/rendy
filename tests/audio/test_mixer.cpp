#include <catch2/catch_test_macros.hpp>

#include <rendy/audio/audio.hpp>

#include <vector>

using namespace rendy;

// Codex review finding 5: public PCM APIs must reject null/garbage instead
// of hitting UB. (The mixer degrades to silent-but-safe without a device,
// so these run fine in headless CI.)
TEST_CASE("createSound validates its inputs", "[audio]") {
    audio::Mixer mixer;
    const std::vector<float> pcm(1024, 0.0f);

    REQUIRE_FALSE(mixer.createSound(nullptr, 1024, 1, 48000).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 0, 1, 48000).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 1024, 0, 48000).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 1024, 9, 48000).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 1024, 1, 0).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 1024, 1, -44100).hasValue());
    REQUIRE_FALSE(mixer.createSound(pcm.data(), 1024, 2, 10000000).hasValue());

    // Valid mono 22.05 kHz resamples fine.
    auto sound = mixer.createSound(pcm.data(), 512, 1, 22050);
    REQUIRE(sound.hasValue());
    REQUIRE(sound.value().valid());
}

TEST_CASE("openStream rejects what it cannot stream", "[audio]") {
    audio::Mixer mixer;
    // Only ogg can stream.
    REQUIRE_FALSE(mixer.openStream("music.wav").hasValue());
    // Missing/invalid files fail cleanly.
    REQUIRE_FALSE(mixer.openStream("/nonexistent/nope.ogg").hasValue());
    // A destroyed mixer with a started feeder thread shuts down cleanly
    // (exercised by the destructor at scope end even on failure paths).
}

TEST_CASE("stale and invalid refs are inert", "[audio]") {
    audio::Mixer mixer;
    // Unknown sound: play returns an invalid voice.
    REQUIRE_FALSE(mixer.play(audio::SoundRef{123}).valid());
    // Invalid voice refs never crash.
    mixer.stop(audio::VoiceRef{});
    mixer.setVolume(audio::VoiceRef{999, 1}, 0.5f);
    REQUIRE_FALSE(mixer.playing(audio::VoiceRef{2, 42}));

    const std::vector<float> pcm(256, 0.0f);
    auto sound = mixer.createSound(pcm.data(), 256, 1, 48000).value();
    mixer.unload(sound);
    // Unloaded sound: ref becomes inert.
    REQUIRE_FALSE(mixer.play(sound).valid());
}
