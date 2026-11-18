# TrueAir
A polished mastering or playback enhancement utility

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
