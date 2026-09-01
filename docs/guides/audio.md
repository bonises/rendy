# Audio

`audio::Mixer` opens the default output (48 kHz float stereo) and mixes up
to 32 simultaneous voices. It is independent of `App` — create it whenever
you want sound.

```cpp
audio::Mixer mixer;

auto music = mixer.load("theme.ogg").value();
auto blip  = mixer.load("blip.wav").value();

auto musicVoice = mixer.play(music, {.loop = true, .volume = 0.6f});
mixer.play(blip, {.pan = -0.4f});          // fire and forget
```

## Sounds

- `load(path)` decodes **WAV** (dr_wav) and **OGG Vorbis** (stb_vorbis)
  fully at load time, resampling to 48 kHz stereo. Fine for effects and
  music of ordinary length; streaming for very long files is a later
  feature.
- `createSound(frames, frameCount, channels, sampleRate)` takes raw
  interleaved float PCM — procedural audio, synthesis, or your own decoder.
  (`06_breakout` and `07_audio` synthesize all their sounds this way.)

## Voices

`play` returns a `VoiceRef` — keep it if you want to control the instance:

```cpp
mixer.setVolume(musicVoice, 0.3f);
mixer.setPan(musicVoice, 0.5f);       // -1 left … +1 right, constant power
mixer.setPaused(musicVoice, true);
mixer.stop(musicVoice);
bool alive = mixer.playing(musicVoice);
```

Stale refs (voice finished or reused) are ignored safely — no lifetime
bookkeeping needed. When all 32 voices are busy, new sounds are dropped
(logged at debug level).

`setMasterVolume` scales everything.

## Threading model

Mixing happens on SDL's audio thread; control calls are lock-free atomics,
so you can call everything from your game loop without latency spikes. Pan
uses a constant-power law so sounds don't dip in the middle.

Positional audio tip: for a quick 2D/3D panning effect, set
`pan = clamp(worldX / halfScreenWidth, -1, 1)` and scale volume by distance
— see the brick sounds in `06_breakout`.
