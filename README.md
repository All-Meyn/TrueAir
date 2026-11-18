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

4. **Transient vs tonal split**: harmonic air suits sustained, pitched sounds but can make
   cymbals and consonants ring or sound metallic. When the band below the cutoff rises
   sharply (fast vs slow RMS), the air crossfades to a noise burst that follows the
   transient's fast envelope, then returns to harmonic air.

Parameters: **Amount**, **Brightness**, **Drive** (adds odd harmonics), **Transients** (how strongly
transients use the noise burst; 0% = harmonic air only), **Auto** / manual **Cutoff**,
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

### Windows

Requires Visual Studio 2022 or newer with the "Desktop development with C++" workload, and CMake 3.22+.
From a Developer PowerShell or a normal terminal:

```powershell
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
```

The VST3 lands in `build\windows\TrueAir_artefacts\Release\VST3\TrueAir.vst3`. Copy it to
`C:\Program Files\Common Files\VST3\`. It links the C++ runtime statically, so users don't need
the Visual C++ Redistributable.

CI (`.github/workflows/windows-vst3.yml`) builds and tests this on every pull request and push to
`main`, and uploads the VST3 as the `TrueAir-VST3-Windows-x64` artifact.
