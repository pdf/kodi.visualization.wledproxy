# Plan: visualization.wledproxy

## Goal

`visualization.wledproxy` is a Kodi visualization add-on that loads any other installed visualization (projectM by default) and passes everything through to it unchanged. On the way through, it taps the audio samples, analyses them the way WLED's audio-reactive firmware does, and sends WLED "audio sync" packets over UDP. The add-on knows nothing about what consumes the packets. Using WLED's format means WLED devices can react directly, and anything else can decode a documented 44-byte packet.

The first consumer is a Home Assistant custom integration that drives Matter lights and a Hyperion.NG ambilight (WS2801 strip behind an Adalight Arduino) while Kodi plays music. It lives in its own repository and installs through HACS.

Constraints carried over from the brief: music only, no extra hardware, Kodi's audio output path is not touched, one solution.

## Known limits (by design)

- Data flows only while a visualization is on screen. Kodi creates the visualization instance only while audio plays and the visualization control is visible, and frees it when hidden (`xbmc/guilib/GUIVisualisationControl.cpp`, `Process` and `UpdateVisibility`). Leaving the visualization screen, pausing or stopping all end the packet stream; the format has no "stopped" packet.
- Passthrough audio (DTS/AC3 bitstreams) produces no PCM, so no data.
- Values follow WLED's processing: 0-255 band levels after WLED-style scaling and auto-gain, bands from about 43 Hz to 9.3 kHz. They are not calibrated measurements.
- Target settings changed while the visualization runs take effect the next time the visualization opens.
- If the target's own settings dialog is open while the target saves state (for projectM: last preset), the dialog's save can overwrite that state. Configuration values are not affected.

## Facts established

All Kodi references are to the Piers branch. CoreELEC 22 builds Kodi from `CoreELEC/xbmc` commit `d4ea7ceb56a21609f7be8614921a468fa12ba0c6`; its `versions.h`, `c-api/addon_base.h`, `c-api/addon-instance/visualization.h` and `c-api/filesystem.h` are byte-identical to upstream Piers. WLED references are to `MoonModules/WLED-AudioReactive-Usermod` (`audio_reactive.h`, branch `main`), the "Audioreactive V2" usermod that WLED is moving to; the in-tree `wled/WLED` `usermods/audioreactive_v1` is marked deprecated.

| Topic | Fact | Source |
|---|---|---|
| Architecture | CoreELEC 22 ships only `Amlogic-no.aarch64` images: 64-bit userspace | CoreELEC 22.0-Piers_rc2 release assets |
| Box libraries | glibc 2.44, GCC 16.2 (libstdc++) | `CoreELEC` branch `coreelec-22`, `packages/devel/glibc`, `packages/lang/gcc` |
| projectM | CoreELEC packages `visualization.projectm` 22.3.2-Piers rev 1, matching the installed 22.3.2.1 | `packages/mediacenter/kodi-binary-addons/visualization.projectm/package.mk` |
| API versions | main 2.0.4, general 1.0.6, gui 5.15.1, filesystem 1.1.11, instance visualization 5.0.0 | `kodi-dev-kit/include/kodi/versions.h` |
| Library loading | Kodi loads add-on libraries with `RTLD_LAZY` (local scope) and unloads them when the add-on is destroyed | `cores/DllLoader/SoLoader.cpp:111`, `addons/binary-addons/AddonDll.cpp:230` |
| Library lifetime | The add-on library is created when the first instance is needed and destroyed when the last one is released, so every opening of the visualization runs `ADDON_Create` again | `addons/binary-addons/BinaryAddonBase.cpp:35-64` |
| Base calls | Kodi calls every base function with its own struct's `addonBase` handle | `AddonDll.cpp:202, 271, 289, 411-488` |
| `toAddon` tables | Both the global and the per-instance `toAddon` tables are allocated by Kodi; the add-on fills them in | `AddonBase.h:802-809`, `kodi-dev-kit/include/kodi/addon-instance/Visualization.h:783-805`, `addons/Visualization.cpp:52` |
| Per-call dispatch | Kodi reads `instance->visualization->toAddon->audio_data` on every call | `addons/Visualization.cpp:125` |
| No FFT from Kodi | Kodi stopped passing frequency data to visualizations in Kodi 20 (commit `2a953d5807`); add-ons get samples only | `c-api/addon-instance/visualization.h:68` |
| Audio format | Visualization audio is always stereo, interleaved float, 44100 Hz (resampled by the audio engine) | `cores/AudioEngine/Engines/ActiveAE/ActiveAE.cpp:1477-1480` |
| Audio thread | `audio_data` is called from the audio engine thread (`CActiveAE::RunStages`); `audio_start` from the GUI thread | `ActiveAE.cpp:2410`, `GUIVisualisationControl.cpp:178` |
| Self-only callbacks | Settings, paths and localized strings resolve by the calling add-on's identity; no API takes another add-on's ID | `addons/interfaces/AddonBase.cpp:259, 394-424` |
| No other settings access | No binary API runs builtins; JSON-RPC `Settings.*` covers core and skin settings only | `c-api/general.h`, `interfaces/json-rpc/schema/methods.json` |
| Settings value files | `addon_data/<id>/settings.xml`: version 1+ is `<setting id="x" [default="true"]>value</setting>`; version 0 is `<setting id="x" value="..."/>`. Missing keys fall back to the definitions | `addons/settings/AddonSettings.cpp:309-410, 1493`, `settings/SettingsValueXmlSerializer.cpp:87-91` |
| Settings dialog | Kodi creates a fresh add-on object for each lookup, so a target's settings dialog reads its file from disk when opened | `addons/AddonManager.cpp:647` |
| Add-on picker | A setting of type `addon` with `<constraints><addontype>` gives a picker of installed add-ons of that type (the proxy itself included) | `settings/SettingAddon.cpp:60-74`, `addons/gui/GUIWindowAddonBrowser.cpp:486` |
| Action buttons | An `action` setting's data runs as a builtin after replacing `$CWD` and `$ID`; `$INFO[...]` is not expanded. `<close>true</close>` saves and closes the dialog first | `addons/settings/AddonSettings.cpp:179-220`, `interfaces/builtins/Builtins.cpp:145` |
| `RunScript` | Accepts a script file path when the argument is not an add-on ID | `interfaces/builtins/AddonBuiltins.cpp:246-285` |
| Filesystem API | `translate_special_protocol`, `directory_exists`, `file_exists` are available | `c-api/filesystem.h:260-278` |
| WLED sync packet | 44 bytes, packed, little-endian: `header[6]` = `"00002"`, `pressure[2]` (8.8 fixed point), `float sampleRaw`, `float sampleSmth`, `uint8 samplePeak`, `uint8 frameCounter`, `uint8 fftResult[16]`, `uint16 zeroCrossingCount`, `float FFT_Magnitude`, `float FFT_MajorPeak`. V2 fills `pressure`, `frameCounter` and `zeroCrossingCount` in bytes V1 left reserved, so V1 receivers still accept it | `audio_reactive.h:1350-1364`; `audioreactive_v1/audio_reactive.cpp:800-815` |
| WLED transport | Default port 11988, multicast group 239.0.0.1, optional broadcast; sent after each FFT frame (about 23 ms, 25 ms fallback); receivers accept `frameCounter` 0 and otherwise drop frames that aren't newer (8-bit rollover-safe) | `audio_reactive.h:1401, 1887-1931, 1949-1962, 2390, 2641` |
| WLED analysis | 22050 Hz, 512-point FFT; default band mapping (`freqDist` 0) from about 43 Hz to 9259 Hz; default scaling mode 3 (square root to 0-255); pink-noise correction profiles (1 = line-in); auto-gain presets with set-points 112/144/164 | `audio_reactive.h:341-357, 416, 479-512, 995-1063, 1200-1230` |
| WLED licence | `wled/WLED` and the V2 usermod are EUPL-1.2 | GitHub licence metadata |
| Hyperion audio | Hyperion.NG's audio grabber reads a local ALSA capture device only; it has no network audio input | `libsrc/grabber/audio/AudioGrabberLinux.cpp` |
| Hyperion JSON API | Port 19444; `image` takes `priority` (1-253), `origin`, `duration` (ms), `imagewidth`, `imageheight`, base64 `imagedata`; `clear` removes a priority | `libsrc/api/JSONRPC_schema/schema-image.json`, `schema-clear.json`, `libsrc/hyperion/schema/schema-jsonServer.json` |
| Hyperion priorities | Lower number wins. Capture defaults: system 250, USB/V4L 240, audio 230. Home Assistant's Hyperion integration uses 128 | `libsrc/hyperion/schema/schema-instCapture.json`; `homeassistant/components/hyperion/const.py:11` |
| HA Kodi player | Home Assistant's Kodi media player registers websocket callbacks for Kodi player events (`Player.OnPause` and others), so play, pause and stop reach Home Assistant as pushed events | `homeassistant/components/kodi/media_player.py:181-225, 324-335` |
| HA Matter lights | `light.turn_on` supports `transition` (tenths of a second) except on blocklisted devices; colour and brightness are sent as two separate Matter commands | `homeassistant/components/matter/light.py:119-190, 318-334, 405-412` |

## Components

1. `visualization.wledproxy`: the Kodi add-on (C++ shared library plus add-on files), in `src/` and `addon/`.
2. `docs/wled-sync.md`: how the proxy fills each field of WLED's audio sync packet, with a link to WLED's definition.
3. `kodi_audio_lights`: the first consumer, a Home Assistant custom integration (Python) that drives lights and Hyperion. It lives in its own repository, `pdf/ha-kodi-audio-lights`, and installs through HACS.
4. `tools/recv_sync.py`: a small receiver that joins the multicast group and prints decoded packets, for checking the stream from any host.

## 1. Kodi add-on

### Files

```
visualization.wledproxy/
  addon.xml
  visualization.wledproxy.so
  resources/
    settings.xml
    language/resource.language.en_gb/strings.po
    lib/configure_target.py
```

`addon.xml`: extension point `xbmc.player.musicviz`, `library_linux="visualization.wledproxy.so"`, imports `kodi.binary.global.main` 2.0.4, `kodi.binary.global.general` 1.0.6, `kodi.binary.global.filesystem` 1.1.11 and `kodi.binary.instance.visualization` 5.0.0.

### Settings

| ID | Type | Default | Purpose |
|---|---|---|---|
| `target` | `addon`, addontype `xbmc.player.musicviz` | `visualization.projectm` | Visualization to wrap |
| `configure_target` | `action`, `RunScript($CWD/resources/lib/configure_target.py)`, button with `<close>true</close>` | - | Opens the chosen target's own settings |
| `enabled` | boolean | true | Send audio sync packets |
| `destination` | list: multicast, broadcast, unicast | multicast | How packets are addressed |
| `udp_host` | string (IP), shown for unicast only | empty | Receiver host |
| `udp_port` | integer | 11988 | Receiver UDP port (WLED's default) |

Multicast goes to 239.0.0.1 with TTL 1 and loopback off. Broadcast goes to the subnet broadcast address of the default-route interface.

`configure_target.py` reads `target` with `xbmcaddon.Addon('visualization.wledproxy').getSetting('target')` and runs `xbmc.executebuiltin('Addon.OpenSettings(<target>)')`. The button closes (and saves) the proxy's dialog first, so a just-changed target is used.

### Code structure

C++17, using the `kodi-dev-kit` C++ helpers for the proxy's own settings, logging and notifications. The proxy does not use `ADDONCREATOR`; it defines `kodi::addon::CPrivateBase::m_interface` itself, pointing at Kodi's real `AddonGlobalInterface`, and exports its own `ADDON_Create`, `ADDON_GetTypeVersion` and `ADDON_GetTypeMinVersion`. Built with `-fvisibility=hidden` and a linker version script that exports only those three symbols.

Source files:

- `src/entry.cpp`: exported symbols, load and unload sequence.
- `src/target.cpp`: locating, loading and validating the target library.
- `src/redirect.cpp`: the redirected `kodi_addon` callbacks.
- `src/target_settings.cpp`: target settings store (pugixml).
- `src/po.cpp`: `strings.po` reader.
- `src/resample.cpp`: stereo 44100 Hz to mono 22050 Hz.
- `src/wled_analysis.cpp`: the WLED-compatible analysis chain.
- `src/sender.cpp`: socket, addressing and packet encoding.
- `third_party/pugixml/` (v1.16, vendored).

### Load sequence (`ADDON_Create`)

1. Store Kodi's interface pointer in `CPrivateBase::m_interface`. Read the proxy's own settings.
2. Read `target`. If it is empty or equals the proxy's own ID, show a notification and return `ADDON_STATUS_PERMANENT_FAILURE`.
3. Find the target folder: `special://home/addons/<target>`, then `special://xbmc/addons/<target>`, translated with `translate_special_protocol` and checked with `directory_exists`.
4. Read `library_linux` from the target's `addon.xml` (pugixml) and `dlopen` it with `RTLD_NOW | RTLD_LOCAL`.
5. Version check: for every add-on type, call the target's `ADDON_GetTypeVersion` and `ADDON_GetTypeMinVersion` and apply the same rule as Kodi's `CheckAPIVersion` (`AddonDll.cpp:519`) against the versions the proxy was compiled with. On mismatch: log both versions, notify, `dlclose`, fail.
6. Build the target's interface:
   - copy Kodi's `AddonGlobalInterface`;
   - give the copy its own zeroed `KodiToAddonFuncTable_Addon`;
   - give the copy its own `AddonToKodiFuncTable_Addon`, a field-by-field copy of Kodi's (same `kodiBase` and sub-tables) except that `kodi_addon` points at the proxy's own table: Kodi's `kodi_addon` entries with the redirected ones replaced.
7. Call the target's `ADDON_Create` with the copy. On failure, `dlclose` and return its status.
8. Copy `copy.addonBase` into Kodi's struct, so Kodi's base calls carry the target's handle.
9. Fill Kodi's `toAddon` table: `create`, `create_instance`, `destroy_instance` and `destroy` point to the proxy; the four `setting_change_*` point to the proxy's own handlers (they receive the target's handle, which the proxy ignores).
10. Open the UDP socket (non-blocking) if `enabled`.

### Kodi to add-on calls

- `create_instance`: forward to the target's. If the instance type is visualization, save the target's `audio_start` and `audio_data` from `instance->visualization->toAddon` and replace them with proxy wrappers.
- Wrappers forward first, then:
  - `audio_start` (GUI thread): set an atomic "reset analysis" flag.
  - `audio_data` (audio thread): resample, analyse, and send one packet per completed FFT frame.
- `destroy_instance`: forward.
- `destroy`: forward; `dlclose` the target; close the socket; free the copied tables.
- `setting_change_*` (the proxy's own settings): `enabled`, `destination`, `udp_host` and `udp_port` apply immediately (the socket is reopened). `target` returns OK and applies the next time the visualization opens.

### Redirected `kodi_addon` callbacks (target only)

| Callback | Behaviour |
|---|---|
| `get_addon_path` | Target's add-on folder |
| `get_user_path` | `special://profile/addon_data/<target>/`, translated |
| `get_setting_bool/int/float/string` | From the target settings store |
| `set_setting_bool/int/float/string` | Write through the target settings store |
| `is_setting_using_default` | From the store |
| `get_localized_string` | IDs 30000 and up from the target's `strings.po`; lower IDs forwarded to Kodi |
| `open_settings_dialog` | Returns false |
| `get_addon_info` | `id`, `path` and `profile` answered for the target; other keys forwarded |
| everything else | Forwarded unchanged |

Returned strings are allocated with `malloc`, because the C++ helpers release them through `free_string`, which Kodi implements with `free`.

### Target settings store

- Definitions and defaults from the target's `resources/settings.xml`: the version 1 format (`<setting id type><default>`) and the legacy format (`<setting id type default="...">`).
- Values from `special://profile/addon_data/<target>/settings.xml`, versions 0, 1 and 2.
- Values are cached and reloaded when the file's modification time changes.
- Writes update the cache and save the whole file in version 2 format (`default="true"` on unchanged keys), written to a temporary file and renamed. Floats are formatted with the C locale.

### Audio analysis (WLED-compatible)

All in the audio thread, with no locks and no allocation after setup.

1. Mix stereo to mono and downsample 44100 Hz to 22050 Hz with a half-band low-pass filter, scaling samples to the signed 16-bit range.
2. Collect 512-sample frames (about 23 ms at 22050 Hz) and run WLED's chain on each, with the V2 usermod's defaults: DC removal, its default window, 512-point FFT, `freqDist` 0 band mapping to 16 channels, pink-noise profile 1 (line-in, since the input is line-level digital audio), square-root scaling to 0-255, and its "normal" auto-gain preset.
3. Volume (`sampleRaw`, `sampleSmth`), `samplePeak`, `FFT_Magnitude`, `FFT_MajorPeak`, `zeroCrossingCount` and `pressure` are computed as the V2 usermod defines them.
4. Send one packet per frame with `frameCounter` starting at 1 and incrementing (wrapping 255 to 1, since 0 means "legacy sender" to receivers).

Licence: WLED is EUPL-1.2 and this repository is MIT, so the chain is reimplemented from WLED's documented behaviour and the constants it publishes; no WLED code is copied. `docs/wled-sync.md` lists every constant with its WLED source line.

Input scale: a 0 dBFS sine maps to full 16-bit range. Auto-gain then normalises volume, so the scale affects mainly the noise gate. To test once a WLED controller exists: compare the proxy's packets with that controller's own line-in analysis of the same audio, and adjust the scale and constants if needed.

`sendto` on a non-blocking socket; errors are counted and logged at most once per minute.

## 2. Packet format (`docs/wled-sync.md`)

The proxy emits WLED audio sync V2 packets exactly as specified in the facts table: 44 bytes, header `"00002"`, port 11988, one per analysis frame. The document covers:

- each field, its range and how the proxy computes it;
- the band edges of the 16 channels;
- that there is no stop packet: the stream simply ends on pause, stop or leaving the visualization;
- decoding in Python: `struct.unpack("<6s2sffBB16sHff", data)`.

## 3. First consumer: Home Assistant integration (separate repository, installed with HACS)

### Why a separate repository

HACS's rules for integrations (`hacs/documentation`, `source/docs/publish/start.md` and `integration.md`) make sharing this repository a poor fit:

- **Location:** the integration must sit at `custom_components/<domain>/` in the repository root, and HACS manages only one integration per repository.
- **Versions:** HACS takes the integration's version from the repository's latest GitHub release. Here every Kodi add-on release would show up in Home Assistant as an integration update, and an integration fix would need a Kodi release tag.
- **Repository metadata:** HACS shows the repository's description, topics and README, which here describe the Kodi add-on.

Decision: a new public repository, `pdf/ha-kodi-audio-lights`, with its own releases. It refers to `docs/wled-sync.md` in this repository for the packet format.

### Repository layout

```
custom_components/kodi_audio_lights/
  __init__.py
  manifest.json
  config_flow.py
  const.py
  receiver.py
  gate.py
  lights.py
  hyperion.py
  switch.py
  strings.json
  translations/en.json
  brand/icon.png
tests/
hacs.json
README.md
LICENSE (MIT, as here)
.github/workflows/validate.yml
.github/workflows/release.yml
```

- `manifest.json`: `domain`, `name`, `version`, `documentation`, `issue_tracker`, `codeowners` (all required by HACS), plus `config_flow: true`, `iot_class: local_push` and `integration_type: service`. No extra Python requirements: the Hyperion JSON API is spoken directly over `asyncio` TCP.
- `hacs.json`: `name` and `homeassistant` (minimum version, set to the Home Assistant version the tests run against). Not `zip_release`: HACS downloads the folder from the release tag.
- `brand/icon.png`: HACS requires brand assets in the integration folder or in `home-assistant/brands`; shipping them in the folder avoids a PR to that repository.
- GitHub metadata HACS requires: a description, topics (`home-assistant`, `hacs`, `kodi`, `wled`, `hyperion`, `homeassistant-integration`) and issues enabled.

### Set-up in the Home Assistant UI

Config flow, so neither installing nor configuring touches files on the Home Assistant host. Options flow with the same fields; the integration reloads when they change.

- **Input:** UDP port (default 11988); join multicast group 239.0.0.1 (default on); source address filter, filled in from the selected media player's Kodi config entry when there is one.
- **Gating:** media player (optional `media_player` entity selector).
- **Lights:** light entities (multiple, `light` domain), minimum and maximum brightness, update rate (default 3 per second), restore lights afterwards (default off).
- **Hyperion:** on/off, host, port (default 19444), priority (default 150).

Entity: a switch, "Music reactive" (state restored across restarts), so automations and dashboards can turn the whole integration on or off.

### Active state

The integration drives outputs only while all of these hold:

1. the "Music reactive" switch is on;
2. the selected media player is `playing` (skipped if none is selected);
3. a valid packet arrived in the last 1 s.

Condition 2 comes from a state-change listener on the media player, which Home Assistant's Kodi integration updates from Kodi's pushed player events. Pause and stop end the active state as soon as Home Assistant sees them, the same moment the owner's own player-state automations run. Condition 3 only covers leaving the visualization screen while music keeps playing.

When the active state ends: pending light commands are cancelled, Hyperion's priority is cleared, and nothing else changes unless "restore lights afterwards" is on. Light state before and after music stays with the owner's automations. A command already sent can still land up to about 300 ms after an automation's own command; the bulb then ends in the automation's state, because a new command replaces a running transition.

### Receiving

- One `asyncio` datagram endpoint on the configured port, joined to 239.0.0.1 if enabled.
- Packets that are not 44 bytes with header `"00002"`, or come from a filtered-out source, are ignored (counted, logged at most once per minute).
- `frameCounter` handling matches WLED's receiver: 0 is always accepted; otherwise a frame is dropped unless `(int8)(new - last) > 0`.

### Light output

- Per update interval (default 1/3 s), average the received frames:
  - level: `sampleSmth / 255`;
  - bass, mid, treble: means of `fftResult` channels 0-3 (43-301 Hz), 4-10 (301-2412 Hz) and 11-15 (2412-9259 Hz), divided by 255.
- Brightness: `min + level * (max - min)` percent. Colour: hue from the circular mean of red (bass), green (mid) and blue (treble) weighted by band level; saturation 100.
- Each light has its own worker that always sends the latest wanted state: if a call is still running, intermediate states are skipped. Calls use `light.turn_on` with `transition` equal to the update interval.
- Deadband: skip a send if brightness changed less than 3 points and hue less than 10 degrees.
- If "restore lights afterwards" is on: `scene.create` with `snapshot_entities` when the active state starts, and `scene.turn_on` with a 1 s transition when it ends.

### Hyperion output

- One persistent TCP connection to the Hyperion JSON API, reconnecting with backoff. Replies are read and discarded.
- For every packet while active, send an `image` command: 16x9 pixels, `priority` as configured, `origin` "Kodi Audio Lights", `duration` 500 ms. If the previous write hasn't drained, that frame is skipped.
- Picture: the 16 channels run along the image's border from bottom centre (channel 0) up both sides to top centre (channel 15), mirrored left and right. Each channel's hue steps from red to violet, and its brightness is its `fftResult` value. Interior pixels take the colour of the nearest border pixel. Hyperion maps the image onto the LED layout, so the integration needs no layout of its own.
- When the active state ends, send `clear` for the priority. The 500 ms `duration` also makes Hyperion fall back to its capture by itself if Home Assistant stops.
- Priority 150 is above Hyperion's capture (230-250), so music replaces the screen picture, and below Home Assistant's Hyperion integration (128), so a colour set by hand still wins.

### Releases and checks

- `validate.yml` on every push and PR: HACS Action (`hacs/action`, category `integration`), Hassfest (`home-assistant/actions/hassfest`) and `pytest`.
- `release.yml` on a pushed tag `v<version>`: fails if the tag differs from `manifest.json`'s `version`, runs the same checks, and publishes a GitHub release. HACS needs a full release, not just a tag.

### Install for users

1. HACS > three-dot menu > Custom repositories: add `https://github.com/pdf/ha-kodi-audio-lights`, type Integration.
2. Download "Kodi Audio Lights" in HACS and restart Home Assistant.
3. Settings > Devices & services > Add integration > Kodi Audio Lights.

The README also carries a my.home-assistant.io link that opens step 1 with the repository filled in. After that, HACS offers updates like any other integration.

The integration is distributed as a HACS custom repository only; it is not submitted to the HACS default store.

## Build

- Headers: `xbmc/addons/kodi-dev-kit/include` from `CoreELEC/xbmc` at `d4ea7ceb56a21609f7be8614921a468fa12ba0c6`, fetched by CMake at configure time.
- Toolchain: a `debian:trixie` container (glibc 2.41, GCC 14) with `g++-aarch64-linux-gnu`, run with Docker on the build host. Both libraries are older than the box's (glibc 2.44, GCC 16), so the result runs there.
- CMake with a toolchain file for `aarch64-linux-gnu`; flags `-O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden`, version script for the three exports.
- Post-build checks (scripted): `readelf --dyn-syms` shows only the three exports; the highest `GLIBC_` and `GLIBCXX_` versions required are at or below 2.41 and 3.4.33; `readelf -h` shows `AArch64`, ELF64.
- `make package` produces, from the same staged add-on folder:
  - `visualization.wledproxy-<version>.tar.gz`, for installing over SSH;
  - `visualization.wledproxy-<version>.zip`, installable with Kodi's "Install from zip file" (add-on folder at the zip root, as Kodi requires).
- The add-on version lives in one place (CMake `project(VERSION)`), and `addon.xml` is generated from it.
- `addon.xml` metadata declares `<platform>linux-aarch64</platform>`, so Kodi offers it only on 64-bit ARM Linux (`addons/addoninfo/AddonInfoBuilder.cpp:824-851`).

## Release: Kodi repository and zips

Produced only after the on-box validation in the work order passes.

### Repository add-on

`repository.visualization.wledproxy`, built from `repo/repository.visualization.wledproxy/` in this repository. Its `addon.xml` (extension point `xbmc.addon.repository`, `<platform>all</platform>`) has one `<dir>`:

```xml
<dir minversion="21.90.0" maxversion="22.89.0">
  <info>https://pdf.github.io/kodi.visualization.wledproxy/piers/addons.xml</info>
  <checksum verify="sha256">https://pdf.github.io/kodi.visualization.wledproxy/piers/addons.xml.sha256</checksum>
  <datadir>https://pdf.github.io/kodi.visualization.wledproxy/piers</datadir>
  <hashes>sha256</hashes>
</dir>
```

- Kodi compares `minversion`/`maxversion` with its `xbmc.addon` version (`addons/Repository.cpp:104-117`). Piers and CoreELEC 22 report 21.90.902 (Omega was 21.3.0), so this range covers Piers pre-releases and every 22.x release, and stops before the next major. A future Kodi major gets its own `<dir>` and folder, because the binary API changes between majors.
- `checksum verify="sha256"`: Kodi checks the index's SHA-256 against the content of the checksum URL (`Repository.cpp:221-223`).
- `hashes sha256`: Kodi verifies every downloaded zip, reading `<zip>.sha256` when the server doesn't send a `content-sha256` header (`Repository.cpp:57-85`). GitHub Pages doesn't, so each zip gets a `.sha256` file.

### Published layout (GitHub Pages)

```
piers/
  addons.xml
  addons.xml.sha256
  visualization.wledproxy/visualization.wledproxy-<version>.zip (+ .sha256)
  repository.visualization.wledproxy/repository.visualization.wledproxy-<version>.zip (+ .sha256)
repository.visualization.wledproxy.zip
index.html
```

- The `<id>/<id>-<version>.zip` paths are Kodi's default download location (`AddonInfoBuilder.cpp:396`).
- `addons.xml` lists both add-ons, so the repository updates itself as well as the proxy.
- `repository.visualization.wledproxy.zip` at the top is a fixed-name copy of the latest repository zip, which is what users install first. `index.html` links it for people browsing the site. To test: whether Kodi's file manager, given the Pages URL as an HTTP file source, can browse that page and install from it, which would save downloading the zip by hand.
- Old proxy versions stay in `piers/visualization.wledproxy/` so Kodi's "Versions" list can roll back.

### Generator and publishing

- `tools/make_repo.py` (Python standard library only) takes the built zips and the existing published tree. It adds the new zips, regenerates `addons.xml` from each zip's `addon.xml`, and writes all `.sha256` files. It refuses to overwrite an existing version.
- `.github/workflows/release.yml` runs on a pushed tag `v<version>`:
  - fails if the tag differs from the CMake version;
  - builds with the same Docker script as local builds, and runs the tests;
  - downloads the current Pages content, runs `make_repo.py`, and deploys to Pages;
  - attaches the add-on zip, the tar.gz and the repository zip to a GitHub release.
- The repository is public and Pages is not enabled yet. Before the first release, in GitHub's repository settings:
  - rename the repository from `pdf/kodi.visualization.proxy` to `pdf/kodi.visualization.wledproxy` (the Pages URLs above depend on the name; GitHub redirects the old URL for git);
  - set Pages to deploy from GitHub Actions.

### Install for users

1. Settings > System > Add-ons > Unknown sources: on.
2. Download `https://pdf.github.io/kodi.visualization.wledproxy/repository.visualization.wledproxy.zip` and use Add-ons > Install from zip file.
3. Add-ons > Install from repository > WLED visualization proxy repository > Visualisations > WLED visualization proxy > Install.

Installing the add-on zip directly (step 2 with `visualization.wledproxy-<version>.zip`) also works, but then Kodi won't update it.

## Tests

On the build host (x86-64, native build of the same sources):

1. Analysis:
   - resampler: a 1 kHz sine keeps its level; a 15 kHz sine is suppressed;
   - WLED chain: sine tones land in the expected `fftResult` channel (for example 100 Hz in channel 1, 1 kHz in channel 7, 5 kHz in channel 14); silence keeps all channels and volume at 0; `FFT_MajorPeak` reports the tone's frequency within one bin; auto-gain brings a -30 dBFS and a -6 dBFS signal to similar `sampleSmth`;
   - packets: 44 bytes, header `"00002"`, field offsets as in the facts table, `frameCounter` wrapping 255 to 1.
2. Settings store with fixture files in each format (definitions version 1 and legacy; values versions 0, 1, 2; missing values file; entities in values) and the `.po` reader.
3. Loader harness: a test program that plays Kodi's part (fills an `AddonGlobalInterface` with recording stubs, loads the proxy with `dlopen`, calls `ADDON_Create`, `create_instance`, `audio_start`, `audio_data`, `destroy_instance`, `destroy`) against a mock target library. It checks:
   - Kodi's struct ends up with the target's `addonBase`;
   - every call reaches the mock target with the right handles;
   - the target's path, user path, settings and strings calls are redirected and other calls are not;
   - a version mismatch fails cleanly;
   - packets arrive on a local socket, for unicast and for multicast.
4. Packaging: the zip has the add-on folder at its root and the generated `addon.xml` matches the CMake version. `make_repo.py` against a fixture tree: index contents, checksums, refusing to overwrite an existing version, and keeping old versions.
5. HA integration (in its own repository): `pytest` with `pytest-homeassistant-custom-component` covering:
   - packet decoding, filtering and `frameCounter` handling;
   - the active state: switch, media player `playing`/`paused`/`idle`, and the 1 s packet timeout;
   - band grouping, colour mapping, deadband and the latest-wins light worker, including cancellation when the active state ends;
   - Hyperion: `image` payloads against a fake JSON API server, `clear` on deactivation, reconnect, skipped frames under back-pressure;
   - optional snapshot and restore;
   - config and options flows.
   Also HACS Action and Hassfest.

On the box and network, after the above pass: the install steps below, one at a time.

## Install and first run (validation, over SSH)

1. On the build host, copy the package to the box:
   ```bash
   scp build-aarch64/visualization.wledproxy-0.1.0.tar.gz root@<box>:/storage/
   ```
2. On the box, unpack and restart Kodi:
   ```bash
   tar -xzf /storage/visualization.wledproxy-0.1.0.tar.gz -C /storage/.kodi/addons/ && systemctl restart kodi
   ```
3. In Kodi: Add-ons > My add-ons > Visualisations > WLED visualization proxy > Enable (Kodi adds manually copied add-ons disabled).
4. In Kodi: Settings > Player > Music > Visualisation > WLED visualization proxy. The defaults (multicast, port 11988) need no change.
5. On the build host (same network), check packets arrive while music plays with the visualization on screen, and keep it running for 10 minutes to rule out IGMP snooping without a querier on the switch:
   ```bash
   python3 tools/recv_sync.py
   ```
6. Install the integration through HACS as in its "Install for users" steps (before its first release, choose the default branch in HACS's version picker), select the Kodi media player, the lights and the Hyperion host.
7. Check with the existing player-state automations: play, pause, resume and stop, and confirm the lights end in the automations' states and the ambilight returns to screen capture on pause.

Troubleshooting starts from `/storage/.kodi/temp/kodi.log`; every proxy log line is prefixed `visualization.wledproxy:`.

## Work order

1. Repository skeleton: CMake, header fetch, pugixml, Docker build script, local `make test`.
2. Loader harness and mock target, then `entry.cpp`, `target.cpp` and `redirect.cpp` until the harness passes.
3. Target settings store and `.po` reader with their tests.
4. Resampler, WLED analysis chain and sender with their tests; `docs/wled-sync.md`; `tools/recv_sync.py`.
5. Add-on files (`addon.xml`, `settings.xml`, strings, configure script) and packaging (tar.gz and zip).
6. On-box install over SSH and test with projectM (install steps 1-5); then with one other installed visualization.
7. HA integration in `pdf/ha-kodi-audio-lights` (the owner creates the empty public repository): code, tests, both workflows, then install through HACS on the owner's Home Assistant (install steps 6-7).
8. Repository add-on, `make_repo.py` with its tests, and the release workflow.
9. Remove the SSH-installed copy from the box, then do the user install above from the published repository, to check the repository end to end.
