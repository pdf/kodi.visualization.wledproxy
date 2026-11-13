# WLED audio sync packets from visualization.wledproxy

The proxy sends WLED "audio sync" V2 packets, the format WLED's audio-reactive usermod uses to share its analysis between devices. WLED devices set to receive audio sync react to them directly; anything else can decode the 44 bytes described here.

The definition this follows is the `audioSyncPacket` struct of the MoonModules "Audioreactive V2" usermod, `audio_reactive.h` at commit [`a4338f2`](https://github.com/MoonModules/WLED-AudioReactive-Usermod/blob/a4338f2ae26e303653c8fffbbb9af975401cefc5/audio_reactive.h#L1352-L1364). Line references below (`L123`) are to that file at that commit.

## Transport

| | Value | WLED source |
|---|---|---|
| Port | 11988 by default (setting `udp_port`) | L1401 |
| Multicast | group 239.0.0.1, TTL 1, loopback off (default) | L2390 |
| Broadcast | subnet broadcast address of the interface holding the default route | L1917-L1919 |
| Unicast | one IPv4 address (setting `udp_host`) | - |
| Rate | one packet per analysis frame: 512 samples at 22050 Hz, about 43 per second (23.2 ms) | L479, L507 |

Packets flow only while the visualization is on screen and Kodi plays audio with PCM output. There is no "stopped" packet: on pause, stop, or leaving the visualization screen, the stream simply ends. Receivers should treat a gap (WLED uses about a second) as silence.

Unlike WLED, the proxy keeps sending during long silences (WLED stops after 30 s of silence, L234); the values are then zero.

## Layout

44 bytes, packed, little-endian:

| Offset | Size | Field | Type | Range | Meaning |
|---|---|---|---|---|---|
| 0 | 6 | `header` | char[6] | `"00002\0"` | protocol version 2 |
| 6 | 2 | `pressure` | uint8[2] | 0.0-255.996 | sound pressure estimate, 8.8 fixed point: `[0]` integer part (saturates at 255), `[1]` fraction in 1/256 |
| 8 | 4 | `sampleRaw` | float | 0-255 | volume, lightly smoothed, after auto-gain |
| 12 | 4 | `sampleSmth` | float | 0-255 | volume, smoothed, after auto-gain |
| 16 | 1 | `samplePeak` | uint8 | 0 or 1 | 1 if a peak was detected since the previous packet |
| 17 | 1 | `frameCounter` | uint8 | 1-255 | rolling counter, starts at 1 when the visualization starts or a track starts, wraps 255 to 1 |
| 18 | 16 | `fftResult` | uint8[16] | 0-255 | levels of the 16 frequency channels below |
| 34 | 2 | `zeroCrossingCount` | uint16 | 0-340 | zero crossings in the frame, times 2/3 |
| 36 | 4 | `FFT_Magnitude` | float | >= 0 | magnitude of the strongest frequency, scaled by the auto-gain |
| 40 | 4 | `FFT_MajorPeak` | float | 1-11025 | strongest frequency, Hz |

`frameCounter` never carries 0, which WLED receivers read as "legacy sender" and always accept. WLED receivers drop a frame unless `(int8_t)(new - last) > 0` (L1954), which the 255-to-1 wrap passes.

Decoding in Python:

```python
import struct

header, pressure, sample_raw, sample_smth, peak, counter, fft, zero_crossings, magnitude, major_peak = struct.unpack("<6s2sffBB16sHff", data)
assert len(data) == 44 and header == b"00002\0"
levels = list(fft)                        # 16 values, 0-255
pressure = pressure[0] + pressure[1] / 256
```

`tools/recv_sync.py` joins the group and prints decoded packets.

## Frequency channels

WLED's default mapping (`freqDist` 0, L1015-L1032). FFT bins are 22050 / 512 = 43.07 Hz wide; a channel is the RMS of its bins.

| Channel | Bins | Hz | Extra factor |
|---|---|---|---|
| 0 | 1 | 43-86 | |
| 1 | 2 | 86-129 | |
| 2 | 3-4 | 129-216 | |
| 3 | 5-6 | 216-301 | |
| 4 | 7-9 | 301-430 | |
| 5 | 10-12 | 430-560 | |
| 6 | 13-18 | 560-818 | |
| 7 | 19-25 | 818-1120 | |
| 8 | 26-32 | 1120-1421 | |
| 9 | 33-43 | 1421-1895 | |
| 10 | 44-55 | 1895-2412 | |
| 11 | 56-69 | 2412-3015 | |
| 12 | 70-85 | 3015-3704 | |
| 13 | 86-103 | 3704-4479 | |
| 14 | 104-164 | 4479-7106 | 0.88 |
| 15 | 165-215 | 7106-9259 | 0.70 |

Values are 0-255 after WLED's scaling and auto-gain. They are not calibrated measurements: a loud passage and a quiet one look alike once the auto-gain has settled.

## How the proxy computes the fields

Kodi hands visualizations stereo, interleaved float samples at 44100 Hz (resampled by its audio engine). The proxy:

1. Averages the two channels, low-pass filters with a 63-tap half-band FIR (Kaiser window, about 80 dB stopband from 12.75 kHz, flat to 9.3 kHz) and keeps every second sample: mono at 22050 Hz, WLED's rate. Samples are scaled to the signed 16-bit range (see "Input level").
2. Collects 512-sample frames and runs WLED's chain on each, as the V2 usermod does on a classic dual-core ESP32 build (no `WLEDMM_FASTPATH`, so no sliding window, L131; with `FFT_MAJORPEAK_HUMAN_EAR`, L152), with its default settings.
3. Encodes and sends one packet per frame.

WLED splits the work between an FFT task (once per 512 samples) and its main loop, which runs the volume filters and auto-gain about once per millisecond (L2499-L2525). The proxy simulates the loop in 1 ms steps on a clock driven by the sample count, so its results don't depend on how Kodi batches the audio. Per frame it runs the loop steps for the frame's 23.2 ms, then the FFT task, then sends.

### FFT task (once per frame)

| Step | Proxy | WLED source |
|---|---|---|
| Frame peak | largest absolute sample, ignoring samples within 1024 of full scale (clipping artefacts) | L862 |
| Zero crossings | sign changes between consecutive samples, times 2/3 | L877 |
| Noise gate | the FFT runs only if the loop's `volumeSmth` > 0.25; otherwise bins are cleared, `FFT_MajorPeak` = 1, `FFT_Magnitude` = 0.001 and channels decay by 0.85 per frame (below 4 they become 0) | L905, L1065 |
| DC removal | subtract the frame's mean (no input filter: `useInputFilter` 0) | L372, L908 |
| Window | 4-term Blackman-Harris (`fftWindow` 0), correction factor 1.0 | L382, L932 |
| FFT | 512-point, magnitudes; bin 0 set to 0 | L507, L941 |
| Major peak | strongest local maximum of the magnitudes weighted by `sqrt(f) / 23.66` (bin 0 halved, folded above 0.42 x 22050 Hz), with parabolic interpolation; below one bin: 1 Hz and magnitude 0; above 0.42 x 22050 Hz: previous value kept; magnitude scaled back up by the weight when the weight is below 1; clamped to 1-11025 Hz | L521, L722-L724, L955-L977 |
| Bin scaling | magnitudes divided by 16 | L996 |
| Channels | RMS of the bins in the table above (`averageByRMS`) | L379, L546, L1015-L1032 |
| Frequency profile | channel factors of profile 1, "Line-In": 2.35, 1.32, 1.32, 1.40, 1.48, 1.57, 1.68, 1.80, 1.89, 1.95, 2.14, 2.26, 2.50, 2.90, 4.20, 6.50 | L419 |
| Gain | times 0.40 (`FFT_DOWNSCALE`), times the auto-gain factor | L512, L1161-L1164 |
| Dynamics limiter | rising: `avg += 0.78 (x - avg)`; falling: `avg += 0.33 (x - avg)` (decay time 300 ms); both clamped to 0-1023 | L303, L311, L1171-L1185 |
| Square-root scaling | `r = avg x 0.38 - 6`; `r = sqrt(r)` if r > 1, else 0; `r x= 0.85 + channel / 4.5`; `fftResult = round(r x 255 / 16)` clamped to 0-255 (`FFTScalingMode` 3; GEQ post-gain 1 at input level 128) | L341, L1221-L1246 |
| Bin peak | `samplePeak` set if `sampleAvg` > 1, bin 8 > 31 and the last peak is over 100 ms old | L319-L320, L1261 |

`FFT_MajorPeak` uses `f = (bin + delta) x 22050 / 512`; the arduinoFFT library WLED uses divides by 511 instead, a difference of at most 0.2 % (under 19 Hz at 9 kHz). `FFT_Magnitude` is the interpolated apex of the parabola through the three bins around the peak.

### Main loop (every simulated millisecond)

Volume (`getSample`, L1649-L1741):

| Step | Value | WLED source |
|---|---|---|
| Level floor `micLev` | follows the frame peak with weight 1/12288 per step; when the peak falls 0.24 below it, `micLev = (31 micLev + peak) / 32`; during sound it is then frozen until 4 s of silence (`micLevelMethod` 1) | L375, L1662-L1697 |
| Envelope `expAdjF` | of `abs(peak - micLev)`: weight 0.073 when rising above the squelch, 0.18 otherwise; 0 at or below squelch 10 | L330, L1652-L1683 |
| `sampleRaw` (internal) | `expAdjF x 60/40 x 128/128 + expAdjF / 16`, clamped to 0-255 (gain 60, input level 128) | L335, L328, L1700-L1710 |
| `sampleAvg` | `(15 sampleAvg + sampleRaw) / 16` | L1737 |
| `sampleMax` | rises halfway to `expAdjF`; otherwise decays by 0.9994 per step, or follows quickly when the gain puts it above 336 | L1714-L1728 |
| Volume peak | `samplePeak` set when `sampleMax` rises, `sampleAvg` > 1 and the last peak is over 80 ms old | L1268 |
| Peak reset | WLED clears its local peak after 50 ms; the packet's `samplePeak` is cleared after each send | L1283, L1899 |

Auto-gain (`agcAvg`, preset "normal", L353-L367, L1558-L1646), updated when more than 2 ms have passed:

| Constant | Value |
|---|---|
| set-points | 112 while the gained signal is at or below 88, else 220 |
| gain limits | 1/64 to 32 |
| integrator | `+= error x 0.002 x 0.25` when 0.085 < target gain < 6.5 and below zone stop 336; `x 0.9` otherwise; `x 0.91` (to 0) when squelched |
| PI gains | Kp 0.6, Ki 1.7 |
| follow rate | 1/192 outside the zone 42-240 (squelch + 32 to 240), 1/6144 inside |
| `rawSampleAgc` (`sampleRaw` field) | `0.8 x gained + 0.2 x previous`, as an integer |
| `sampleAgc` (`sampleSmth` field) | `+= (gained - sampleAgc) / 12`; halves towards 0 when the gained value is below 1 |

Other loop values:

| Field | Value | WLED source |
|---|---|---|
| `FFT_Magnitude` (packet) | FFT magnitude times the auto-gain factor; 0.001 when `volumeSmth` < 1 | L2509-L2511 |
| `pressure` | every 13 ms: `pressure += 0.38 (estimate - pressure)`, with the line-in estimate: `v = (expAdjF / 11)^2`, 0 at or below 2.3, 255 at or above 26623, else `256 (ln v - ln 2.3) / (ln 26623 - ln 2.3)` | L1767-L1791, L2518-L2522 |
| `volumeSmth` (internal, gates the FFT) | `sampleAgc`, limited to rise at most 196 per 50 ms and fall at most 196 per 300 ms | L310-L311, L1799-L1820 |

### Input level

A 0 dBFS sine reaches the chain as +-32767, the full 16-bit range of WLED's I2S input. The auto-gain then normalises the volume: in the tests, a music-like chord with a 2 Hz beat gives `sampleSmth` of about 37-39 from -40 dBFS to -6 dBFS peaks.

The gain cannot go below 1/64, so signals whose frame peaks stay above about -6 dBFS (a single loud tone, or heavily compressed music) give higher `sampleSmth` (about 58 at -6 dBFS and 96 at 0 dBFS for a single tone) and more channels at 255. To test once a WLED controller is available: compare the proxy's packets with that controller's own line-in analysis of the same audio, and lower the input scale (`kInputScale` in `src/resample.h`) if Kodi's levels sit above WLED's range.
