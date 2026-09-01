// 07_audio: mixer playground. Synthesized sounds (no asset files needed),
// optional wav/ogg from the command line, pan/volume/loop controls via UI.

#include <rendy/rendy.hpp>

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace rendy;

namespace {

// A short synthesized tone with a soft envelope.
audio::SoundRef makeTone(audio::Mixer& mixer, float frequency, float seconds,
                         float vibrato = 0.0f) {
    const int rate = 48000;
    const auto frames = static_cast<size_t>(seconds * rate);
    std::vector<float> pcm(frames);
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / rate;
        const float envelope =
            std::min(1.0f, t * 40.0f) * std::exp(-3.0f * t / seconds);
        const float f = frequency + vibrato * std::sin(t * 6.0f * TwoPi);
        pcm[i] = 0.5f * envelope * std::sin(t * f * TwoPi);
    }
    return mixer.createSound(pcm.data(), frames, 1, rate).valueOr(audio::SoundRef{});
}

// A loopable pad chord.
audio::SoundRef makePad(audio::Mixer& mixer) {
    const int rate = 48000;
    const auto frames = static_cast<size_t>(4.0f * rate);
    std::vector<float> pcm(frames * 2);
    const float chord[] = {220.0f, 277.18f, 329.63f}; // A major
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / rate;
        float sample = 0.0f;
        for (float f : chord)
            sample += std::sin(t * f * TwoPi) + 0.3f * std::sin(t * f * 2.0f * TwoPi);
        sample *= 0.06f;
        // Slow stereo movement, seamless at the loop point (4 s period).
        const float sweep = std::sin(t * 0.25f * TwoPi);
        pcm[i * 2] = sample * (0.7f + 0.3f * sweep);
        pcm[i * 2 + 1] = sample * (0.7f - 0.3f * sweep);
    }
    return mixer.createSound(pcm.data(), frames, 2, rate).valueOr(audio::SoundRef{});
}

} // namespace

int main(int argc, char** argv) {
    auto appResult = App::create({.title = "rendy — audio", .size = {700, 480}});
    if (!appResult) {
        log::error("failed to start: {}", appResult.error().message);
        return 1;
    }
    auto app = std::move(appResult).value();

    audio::Mixer mixer;

    auto blip = makeTone(mixer, 880.0f, 0.15f);
    auto laser = makeTone(mixer, 1400.0f, 0.3f, 300.0f);
    auto thud = makeTone(mixer, 90.0f, 0.4f);
    auto pad = makePad(mixer);
    audio::VoiceRef padVoice;

    audio::SoundRef fileSound;
    if (argc > 1) {
        if (auto loaded = mixer.load(argv[1]))
            fileSound = loaded.value();
        else
            log::warn("{}", loaded.error().message);
    }

    ui::Context context(app);
    context.addStylesheet(R"css(
        root { background-color: #11111b; color: #cdd6f4; padding: 20px; gap: 10px; }
        .title { font-size: 20px; color: #f5e0dc; margin-bottom: 8px; }
        .row { flex-direction: row; gap: 8px; align-items: center; }
        button { padding: 10px 18px; border-radius: 8px; background-color: #45475a; }
        button:hover { background-color: #585b70; }
        button:active { background-color: #89b4fa; color: #11111b; }
        .label { width: 120px; color: #a6adc8; font-size: 14px; }
        .value { color: #f9e2af; font-size: 14px; width: 60px; }
        .hint { color: #6c7086; font-size: 13px; margin-top: 12px; }
    )css");

    auto root = context.root();
    root.addChild("div", {.classes = "title", .text = "rendy mixer"});

    auto soundRow = root.addChild("div", {.classes = "row"});
    soundRow.addChild("div", {.classes = "label", .text = "Ljud (1/2/3)"});
    soundRow.addChild("button", {.text = "Blip"}).onClick([&](ui::Element) {
        mixer.play(blip, {.pan = -0.5f});
    });
    soundRow.addChild("button", {.text = "Laser"}).onClick([&](ui::Element) {
        mixer.play(laser, {.pan = 0.5f});
    });
    soundRow.addChild("button", {.text = "Thud"}).onClick([&](ui::Element) {
        mixer.play(thud, {});
    });
    if (fileSound.valid())
        soundRow.addChild("button", {.text = "Fil"}).onClick([&](ui::Element) {
            mixer.play(fileSound, {});
        });

    auto padRow = root.addChild("div", {.classes = "row"});
    padRow.addChild("div", {.classes = "label", .text = "Pad-loop"});
    padRow.addChild("button", {.text = "Starta / stoppa"}).onClick([&](ui::Element) {
        if (mixer.playing(padVoice)) {
            mixer.stop(padVoice);
        } else {
            padVoice = mixer.play(pad, {.volume = 0.8f, .loop = true});
        }
    });

    auto volumeRow = root.addChild("div", {.classes = "row"});
    volumeRow.addChild("div", {.classes = "label", .text = "Mastervolym"});
    auto volumeValue = volumeRow.addChild("div", {.classes = "value", .text = "100%"});
    auto setVolume = [&, volumeValue](float delta) mutable {
        const float volume = std::clamp(mixer.masterVolume() + delta, 0.0f, 1.5f);
        mixer.setMasterVolume(volume);
        volumeValue.setText(fmt::format("{:.0f}%", volume * 100.0f));
    };
    volumeRow.addChild("button", {.text = "−"}).onClick([setVolume](ui::Element) mutable {
        setVolume(-0.1f);
    });
    volumeRow.addChild("button", {.text = "+"}).onClick([setVolume](ui::Element) mutable {
        setVolume(0.1f);
    });

    root.addChild("div", {.classes = "hint",
                          .text = "tangenter: 1 blip, 2 laser, 3 thud, space pad, esc avslutar"});

    while (app.pollEvents()) {
        const Input& input = app.input();
        if (input.keyPressed(Key::Escape)) app.quit();
        if (std::getenv("RENDY_AUTOQUIT") != nullptr && app.time() > 2.5) app.quit();
        if (input.keyPressed(Key::Num1)) mixer.play(blip, {.pan = -0.5f});
        if (input.keyPressed(Key::Num2)) mixer.play(laser, {.pan = 0.5f});
        if (input.keyPressed(Key::Num3)) mixer.play(thud, {});
        if (input.keyPressed(Key::Space)) {
            if (mixer.playing(padVoice))
                mixer.stop(padVoice);
            else
                padVoice = mixer.play(pad, {.volume = 0.8f, .loop = true});
        }

        auto frame = app.beginFrame({});
        context.update();
        context.paint(frame.canvas());
        frame.present();
    }
    return 0;
}
