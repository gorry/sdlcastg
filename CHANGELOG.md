# Changelog

Versions follow the form `YYYY.MMDD.N`.

## 2026.0926.1

- Video frames are now accepted on a wall-clock schedule, so the stream gets the requested frame rate. Before, a 60 fps stream received only about 41 fps (28 fps while no audio was playing), because two frames often fell into one slot and the next slot stayed empty.
- The main documents are now in Japanese (README.md, BUILD.md), with English versions in README.en.md and BUILD.en.md.

## 2026.0925.1 (first release)

- Discovery (mDNS), Cast V2 control over TLS, and the Default Media Receiver.
- Live streaming of RGBA video and PCM audio as WebM (VP8 + Opus) from a built-in HTTP server.
- SDL2 layer: renderer readback with GPU downscaling, `SDL_AudioSpec` conversion, and the Android multicast lock.
- Tested on Windows 11 x64 and Android 8.0 – 17 (arm64-v8a, armeabi-v7a), streaming to a TV with Chromecast built-in.
