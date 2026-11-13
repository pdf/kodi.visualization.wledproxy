# visualization.wledproxy

A Kodi visualization add-on that runs another installed visualization (projectM by default) and passes everything through to it unchanged. On the way, it analyses the audio the way WLED's audio-reactive firmware does and sends WLED audio sync packets over UDP, so WLED devices, and anything that decodes the 44-byte packet, can react to the music.

- Packet format and analysis details: [docs/wled-sync.md](docs/wled-sync.md)
- Design and work order: [docs/plan.md](docs/plan.md)

Targets Kodi 22 (Piers) on 64-bit ARM Linux (CoreELEC 22, `Amlogic-no.aarch64`).

## Limits

- Packets flow only while the visualization is on screen and music plays. Leaving the visualization screen, pausing or stopping ends the stream; there is no "stopped" packet.
- Passthrough audio (DTS/AC3 bitstreams) has no PCM, so no packets.
- Values follow WLED's processing (0-255 levels after WLED-style scaling and auto-gain, bands from about 43 Hz to 9.3 kHz); they are not calibrated measurements.
- A change of the wrapped visualization applies the next time the visualization opens.

## Settings

| Setting | Default | |
|---|---|---|
| Visualization | projectM | the visualization to wrap |
| Configure visualization | | saves and opens the wrapped visualization's own settings |
| Send audio sync packets | on | |
| Destination | multicast | multicast (239.0.0.1), broadcast (default-route subnet) or a single receiver |
| Receiver address | | for a single receiver |
| UDP port | 11988 | WLED's default |

## Building

The build runs in a Debian trixie container with Docker (glibc 2.41, GCC 14 and the aarch64 cross compiler), so the library runs on CoreELEC 22's newer glibc and libstdc++:

```bash
make docker
```

This builds and runs the tests natively in the container, then cross-builds for aarch64, checks the library (`tools/check_elf.sh`: only the three Kodi entry points exported, glibc and libstdc++ symbol versions, ELF64 AArch64) and writes the packages:

- `build-aarch64/visualization.wledproxy-<version>.tar.gz`, to unpack into `/storage/.kodi/addons/`;
- `build-aarch64/visualization.wledproxy-<version>.zip`, for Kodi's "Install from zip file".

The kodi-dev-kit headers come from upstream `xbmc/xbmc` at tag `22.0rc2-Piers`, the Kodi that CoreELEC 22 builds (stable and nightly), fetched by CMake at configure time (sparse checkout, a few megabytes). `addon.xml` declares their minimum API versions as `minversion`, so the add-on also runs on Piers 22.0 RC1 (tested on CoreELEC 22.0-RC1). The version lives in `CMakeLists.txt` (`project(... VERSION ...)`); `addon.xml` is generated from it.

Tests on the build machine itself, without Docker:

```bash
make test
```

The tests cover the resampler and WLED chain (tones in the expected channels, silence, peak frequency, auto-gain), packet layout, the target settings store and `.po` reader against fixture files, packaging, and a loader harness that plays Kodi's part against the real library and a mock target. The multicast test skips itself on hosts whose firewall drops looped-back multicast; it runs in the container.

## Checking packets

On any machine on the same network, while music plays with the visualization on screen:

```bash
python3 tools/recv_sync.py
```

It joins 239.0.0.1:11988 and prints one decoded packet per second with the rate and any gaps (`--all` prints every packet).

## Licence

MIT. The WLED analysis is reimplemented from the behaviour and published constants of WLED's EUPL-1.2 audio-reactive usermod; no WLED code is included. pugixml (MIT) is vendored in `third_party/pugixml`.
