# audio

Header: `rendy/audio/audio.hpp`, namespace `rendy::audio` — see the
[audio guide](../guides/audio.md).

## Mixer

| Member | Signature | Notes |
|---|---|---|
| ctor | `Mixer()` | opens default output; failure → silent but safe |
| `ok` | `bool ok() const` | |
| `load` | `Result<SoundRef> load(path)` | .wav / .ogg, decoded at load |
| `openStream` | `Result<SoundRef> openStream(path)` | .ogg decoded on the fly (music); single voice, play() restarts |
| `createSound` | `Result<SoundRef> createSound(const float* frames, size_t frameCount, int channels, int sampleRate)` | interleaved PCM |
| `unload` | `void unload(SoundRef)` | stops voices using it and frees the PCM; the ref becomes inert |
| `play` | `VoiceRef play(SoundRef, const PlayOptions& = {})` | invalid ref if all 32 voices busy |
| `stop / stopAll` | | |
| `setPaused` | `void setPaused(VoiceRef, bool)` | |
| `playing` | `bool playing(VoiceRef) const` | |
| `setVolume / setPan` | per voice | pan −1…+1, constant power |
| `setMasterVolume / masterVolume` | | |

`PlayOptions { volume (1), pan (0), loop (false) }`.

`SoundRef`/`VoiceRef` are value handles; stale voice refs are ignored
safely. All control calls are lock-free and callable from the game loop.
