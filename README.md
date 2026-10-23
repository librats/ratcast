# ratcast

P2P voice, video and screen sharing built on [librats](https://github.com/librats/librats) —
made for watching movies and games together: one person streams, everyone else watches.

- **libratcast** — a media transport library over librats (in the spirit of librist / SRT):
  deadline-bound delivery, FEC, congestion-driven rate control, one-to-many fan-out.
- **ratcast** — a Qt 6 desktop app for calls and streams (Windows, Linux, macOS; Android later).

Early development. See [docs/PLAN.md](docs/PLAN.md) for the design and roadmap.

## Building

```bash
git clone --recursive https://github.com/librats/ratcast.git
cd ratcast
cmake --preset debug          # or: core (libraries only, no Qt), release, asan
cmake --build --preset debug
ctest --preset debug
```

Requirements: CMake 3.22+, Ninja, a C++20 compiler, and for the app Qt 6.6+ with Qt Quick, Qt
Shader Tools and QtGui's private headers, for QRhi (Debian/Ubuntu: `qt6-declarative-dev
qt6-shadertools-dev qt6-base-private-dev`; the Qt installer: add the Qt Shader Tools module).
Without them the app is skipped and the libraries still build.

### Windows

Visual Studio 2022 (MSVC) and the Windows SDK; the media libraries come from
[vcpkg](https://vcpkg.io), WebRTC's AudioProcessing (echo cancellation) through the overlay port
in this repository:

```powershell
vcpkg install ffmpeg[avcodec,avformat,swscale,nvcodec,amf,qsv,dav1d,svt-av1,aom,openh264] `
    opus pkgconf webrtc-audio-processing sdl3 --overlay-ports=cmake/vcpkg-ports --triplet x64-windows
```

FFmpeg's features are every encoder the engine may pick: NVENC, AMF and Quick Sync for the GPUs
(their runtimes come with the drivers), SVT-AV1, libaom and OpenH264 where there is none. Never
add `gpl` or `version3`: the app is MIT and ships FFmpeg as LGPL 2.1.

Then configure with vcpkg's toolchain and Qt's prefix, e.g. in a `CMakeUserPresets.json`:

```json
{
  "version": 6,
  "configurePresets": [{
    "name": "msvc", "generator": "Visual Studio 17 2022", "architecture": "x64",
    "binaryDir": "${sourceDir}/build/${presetName}",
    "toolchainFile": "C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
    "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Qt/6.10.3/msvc2022_64" }
  }],
  "buildPresets": [{ "name": "msvc-debug", "configurePreset": "msvc", "configuration": "Debug" }],
  "testPresets": [{ "name": "msvc-debug", "configurePreset": "msvc", "configuration": "Debug" }]
}
```

Screens and windows are captured through Windows.Graphics.Capture (Windows 10 1903+), sound
through WASAPI (the applications' sound without the call's own needs Windows 10 2004+), cameras
through Media Foundation.

`cmake --install` lays out a folder that runs on a bare Windows: the programs, the DLLs they
import, Qt's (windeployqt) and MSVC's runtime. That folder, zipped, is what
[the release workflow](.github/workflows/release.yml) publishes for a `v*` tag (one matching
`project(VERSION)`), and leaves as an artifact for every push to main — beside a Linux AppImage
(the app with FFmpeg's same encoders, Qt and the rest, but PipeWire and GLib, which are the
machine's: glibc 2.39 and PipeWire 1.0 or newer, as on Ubuntu 24.04, Debian 13, Fedora 40).
Both keep themselves at the latest release (`-DRATCAST_UPDATES=ON`, which only that workflow
sets): a little after the start and every six hours the app looks at GitHub's latest release,
downloads its package for the system — never while a call is on — checks it against the SHA-256
GitHub gives, and puts it in place. The AppImage is replaced at once; the Windows folder at the
next start. It starts with the next launch, or at once with "Restart now"; "Update
automatically", unticked, leaves it to the user.

Already cloned without `--recursive`: `git submodule update --init --recursive`.

## C API

libratcast has a C API, [`ratcast/ratcast.h`](libratcast/include/ratcast/ratcast.h), on top of
librats' own ([`rats.h`](external/librats/src/librats/bindings/rats.h)): librats makes the node
and connects it, ratcast carries the media.

```c
rats_config_t config = {.protocol = RATCAST_PROTOCOL};  /* reaches ratcast-send */
rats_t node = rats_create(&config);
ratcast_t media;
ratcast_handlers_t handlers = {.user = app, .stream_announced = on_stream,
                               .frame_received = on_frame};
ratcast_attach(node, &handlers, NULL, &media);  /* before rats_start() */
rats_start(node);
rats_connect(node, "sender.example", 7700);

/* on_stream: subscribe to what the sender offers; a feed in order, 200 ms behind */
ratcast_subscribe(media, peer_id, stream->id, 200000);
```

A sender offers streams with `ratcast_add_stream()` and sends encoded frames with
`ratcast_send_frame()`. [`test_capi_c.c`](libratcast/tests/test_capi_c.c) is a whole program.

## License

[MIT](LICENSE)
