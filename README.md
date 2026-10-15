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

Requirements: CMake 3.21+, Ninja, a C++20 compiler, and Qt 6.5+ with Qt Quick for the app
(Debian/Ubuntu: `qt6-declarative-dev`). Without Qt Quick the app is skipped and the
libraries still build.

Already cloned without `--recursive`: `git submodule update --init --recursive`.

## License

[MIT](LICENSE)
