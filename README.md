# TrueAir
A polished mastering or playback enhancement utility

## What it does

TrueAir restores the high-frequency "air" that lossy codecs (MP3, AAC, SBC) cut off with a
steep low-pass, typically somewhere between 15 and 20 kHz.

1. **Cutoff detection**: an FFT of the input looks for a codec-style cliff, a drop of at least 24 dB
   within a few hundred Hz. Natural high-frequency roll-off in lossless recordings is much
   gentler, so it doesn't trigger detection. With no cliff, restoration fades out and the
   audio passes through bit-exact (only delayed by the reported latency).
2. **Harmonic synthesis**, at 4x oversampling (2x at 88.2/96 kHz): the octave below the cutoff
   goes through a waveshaper with its linear part removed, so only new harmonics come out.
   An 8th-order high-pass at the cutoff then keeps only the content above it.
3. **Envelope matching** at the host rate gives the new air the dynamics of the band below it.
   **Brightness** sets its level relative to that band.

Parameters: **Amount**, **Brightness**, **Drive** (adds odd harmonics), **Auto** / manual **Cutoff**,
**Output Gain**.

## Building

Requires CMake 3.22+, Ninja and a C++20 compiler. JUCE 8.0.10 is fetched automatically on first configure.
On Linux, install JUCE's dependencies first (ALSA, JACK, X11, freetype, fontconfig, GTK3, WebKitGTK).

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Outputs land in `build/release/TrueAir_artefacts/Release/` (VST3, Standalone, and AU on macOS).
Use the `debug` preset for development builds.
