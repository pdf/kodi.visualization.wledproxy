// Resampler, WLED analysis chain, packet encoding and version checks.

#include "resample.h"
#include "sender.h"
#include "test.h"
#include "version_check.h"
#include "wled_analysis.h"

#include <kodi/versions.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace wledproxy;

namespace
{

std::vector<float> StereoSine(double freq, double dbfs, double seconds, double phase = 0)
{
  const size_t frames = static_cast<size_t>(seconds * 44100);
  const double amp = std::pow(10.0, dbfs / 20.0);
  std::vector<float> v(frames * 2);
  for (size_t i = 0; i < frames; ++i)
  {
    const float s = static_cast<float>(amp * std::sin(2 * M_PI * freq * i / 44100.0 + phase));
    v[2 * i] = s;
    v[2 * i + 1] = s;
  }
  return v;
}

// Music-like test signal: WLED's level tracker (micLev) settles on the
// quietest frame level and the AGC follows the peaks above it, so a steady
// sine slowly fades out. Real music has dynamics; this gives a tone (or a
// six-note chord when freq is 0) a 2 Hz beat envelope between 0.25 and 1.
std::vector<float> Beat(double freq, double dbfs, double seconds)
{
  const size_t frames = static_cast<size_t>(seconds * 44100);
  const double amp = std::pow(10.0, dbfs / 20.0);
  std::vector<float> v(frames * 2);
  for (size_t i = 0; i < frames; ++i)
  {
    const double t = i / 44100.0;
    const double env = 0.25 + 0.75 * std::pow(0.5 + 0.5 * std::cos(2 * M_PI * 2 * t), 4);
    double x = 0;
    if (freq > 0)
      x = std::sin(2 * M_PI * freq * t);
    else
      for (double f : {110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0})
        x += std::sin(2 * M_PI * f * t + f) / 6.0;
    const float s = static_cast<float>(amp * env * x);
    v[2 * i] = s;
    v[2 * i + 1] = s;
  }
  return v;
}

std::vector<float> Resample(const std::vector<float>& stereo, float scale = kInputScale)
{
  Resampler r(scale);
  std::vector<float> out(stereo.size() / 4 + 2);
  const size_t n = r.Process(stereo.data(), stereo.size() / 2, out.data());
  out.resize(n);
  return out;
}

float PeakAfter(const std::vector<float>& v, size_t skip)
{
  float p = 0;
  for (size_t i = skip; i < v.size(); ++i)
    p = std::max(p, std::fabs(v[i]));
  return p;
}

// Feeds Kodi-like stereo audio through resampler and analyzer in chunks of
// 1024 floats, as Kodi's audio engine does. Returns all frames.
std::vector<SyncFrame> Analyse(WledAnalyzer& a, Resampler& r, const std::vector<float>& stereo)
{
  std::vector<SyncFrame> frames;
  float mono[600];
  SyncFrame f[4];
  for (size_t off = 0; off < stereo.size(); off += 1024)
  {
    const size_t len = std::min<size_t>(1024, stereo.size() - off);
    const size_t m = r.Process(stereo.data() + off, len / 2, mono);
    const size_t n = a.Push(mono, m, f, 4);
    frames.insert(frames.end(), f, f + n);
  }
  return frames;
}

std::vector<SyncFrame> AnalyseTone(double freq, double dbfs, double seconds)
{
  WledAnalyzer a;
  Resampler r(kInputScale);
  return Analyse(a, r, StereoSine(freq, dbfs, seconds));
}

std::vector<SyncFrame> AnalyseBeat(double freq, double dbfs, double seconds)
{
  WledAnalyzer a;
  Resampler r(kInputScale);
  return Analyse(a, r, Beat(freq, dbfs, seconds));
}

float MeanSmth(const std::vector<SyncFrame>& frames, size_t tail)
{
  float sum = 0;
  for (size_t i = frames.size() - tail; i < frames.size(); ++i)
    sum += frames[i].sampleSmth;
  return sum / tail;
}

int MaxChannel(const SyncFrame& f)
{
  return static_cast<int>(std::max_element(f.fftResult, f.fftResult + 16) - f.fftResult);
}

float GetFloat(const uint8_t* p)
{
  uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

} // namespace

TEST(resampler_keeps_1khz_level)
{
  const auto out = Resample(StereoSine(1000, 0, 0.5), 32767.0f);
  const float peak = PeakAfter(out, 200);
  CHECK_MSG(std::fabs(peak - 32767.0f) < 32767.0f * 0.01f, "peak %.1f", peak);
}

TEST(resampler_keeps_9khz_level)
{
  // Top of WLED's highest band.
  const auto out = Resample(StereoSine(9000, -6, 0.5), 32767.0f);
  const float peak = PeakAfter(out, 200);
  const float expected = 32767.0f * std::pow(10.0f, -6.0f / 20.0f);
  CHECK_MSG(std::fabs(peak - expected) < expected * 0.02f, "peak %.1f want %.1f", peak, expected);
}

TEST(resampler_suppresses_15khz)
{
  const auto out = Resample(StereoSine(15000, 0, 0.5), 32767.0f);
  const float peak = PeakAfter(out, 200);
  const float db = 20.0f * std::log10(peak / 32767.0f);
  CHECK_MSG(db < -70.0f, "15 kHz at %.1f dB", db);
}

TEST(resampler_mixes_to_mono)
{
  // Opposite channels cancel.
  std::vector<float> v = StereoSine(1000, 0, 0.1);
  for (size_t i = 0; i < v.size(); i += 2)
    v[i + 1] = -v[i];
  const auto out = Resample(v, 32767.0f);
  CHECK(PeakAfter(out, 0) < 1e-3f);
}

TEST(tone_100hz_in_channel_1)
{
  const auto frames = AnalyseBeat(100, -18, 10);
  const SyncFrame& f = frames.back();
  CHECK_MSG(MaxChannel(f) == 1, "max channel %d", MaxChannel(f));
  CHECK(f.fftResult[1] > 0);
}

TEST(tone_1khz_in_channel_7)
{
  const auto frames = AnalyseBeat(1000, -18, 10);
  const SyncFrame& f = frames.back();
  CHECK_MSG(MaxChannel(f) == 7, "max channel %d", MaxChannel(f));
  CHECK(f.fftResult[7] > 0);
}

TEST(tone_5khz_in_channel_14)
{
  const auto frames = AnalyseBeat(5000, -18, 10);
  const SyncFrame& f = frames.back();
  CHECK_MSG(MaxChannel(f) == 14, "max channel %d", MaxChannel(f));
  CHECK(f.fftResult[14] > 0);
}

TEST(major_peak_within_one_bin)
{
  for (double freq : {100.0, 440.0, 1000.0, 2500.0, 5000.0, 8000.0})
  {
    const auto frames = AnalyseTone(freq, -12, 3);
    const float peak = frames.back().FFT_MajorPeak;
    CHECK_MSG(std::fabs(peak - freq) < 43.07, "%.0f Hz reported as %.1f Hz", freq, peak);
  }
}

TEST(silence_is_zero)
{
  WledAnalyzer a;
  Resampler r(kInputScale);
  const auto frames = Analyse(a, r, std::vector<float>(44100 * 3 * 2, 0.0f));
  CHECK(frames.size() > 100);
  for (const SyncFrame& f : frames)
  {
    for (uint8_t c : f.fftResult)
      CHECK(c == 0);
    CHECK(f.sampleSmth == 0.0f);
    CHECK(f.sampleRaw == 0.0f);
    CHECK(f.samplePeak == 0);
    CHECK(f.pressure == 0.0f);
  }
}

TEST(silence_after_music_decays_to_zero)
{
  WledAnalyzer a;
  Resampler r(kInputScale);
  Analyse(a, r, StereoSine(440, -12, 3));
  const auto frames = Analyse(a, r, std::vector<float>(44100 * 3 * 2, 0.0f));
  const SyncFrame& f = frames.back();
  for (uint8_t c : f.fftResult)
    CHECK(c == 0);
  CHECK(f.sampleSmth < 1.0f);
}

TEST(frame_rate_is_43_per_second)
{
  const auto frames = AnalyseTone(440, -12, 10);
  // 10 s at 22050 Hz / 512 = 430.6 frames.
  CHECK_MSG(frames.size() == 430, "%zu frames", frames.size());
}

TEST(agc_normalises_volume)
{
  // The same music-like chord 24 dB apart: after the AGC settles (WLED's
  // "normal" preset needs several seconds), sampleSmth should be close.
  const float q = MeanSmth(AnalyseBeat(0, -30, 40), 200);
  const float l = MeanSmth(AnalyseBeat(0, -6, 40), 200);
  std::printf("  sampleSmth: -30 dBFS %.1f, -6 dBFS %.1f\n", q, l);
  CHECK(q > 20.0f);
  CHECK(l > 20.0f);
  CHECK_MSG(q / l > 0.8f && q / l < 1.25f, "ratio %.2f", q / l);
}

TEST(zero_crossings_follow_frequency)
{
  // 1 kHz: about 2 * 1000 * 0.0232 = 46 crossings per frame, times 2/3.
  const auto frames = AnalyseTone(1000, -12, 1);
  const int zc = frames.back().zeroCrossingCount;
  CHECK_MSG(zc >= 28 && zc <= 33, "zeroCrossingCount %d", zc);
}

TEST(packet_layout)
{
  SyncFrame f;
  f.sampleRaw = 12.5f;
  f.sampleSmth = 100.25f;
  f.samplePeak = 1;
  for (int i = 0; i < 16; ++i)
    f.fftResult[i] = static_cast<uint8_t>(i * 10 + 5);
  f.zeroCrossingCount = 0x1234;
  f.FFT_Magnitude = 4096.5f;
  f.FFT_MajorPeak = 440.0f;
  f.pressure = 200.5f;

  uint8_t p[kPacketSize];
  EncodePacket(f, 7, p);
  static_assert(kPacketSize == 44, "WLED V2 packet is 44 bytes");
  CHECK(std::memcmp(p, "00002\0", 6) == 0);
  CHECK(p[6] == 200 && p[7] == 128);
  CHECK(GetFloat(p + 8) == 12.5f);
  CHECK(GetFloat(p + 12) == 100.25f);
  CHECK(p[16] == 1);
  CHECK(p[17] == 7);
  for (int i = 0; i < 16; ++i)
    CHECK(p[18 + i] == i * 10 + 5);
  CHECK(p[34] == 0x34 && p[35] == 0x12);
  CHECK(GetFloat(p + 36) == 4096.5f);
  CHECK(GetFloat(p + 40) == 440.0f);
}

TEST(packet_pressure_saturates)
{
  SyncFrame f;
  f.pressure = 400.0f;
  uint8_t p[kPacketSize];
  EncodePacket(f, 1, p);
  CHECK(p[6] == 255);
  f.pressure = -3.0f;
  EncodePacket(f, 1, p);
  CHECK(p[6] == 0 && p[7] == 0);
}

TEST(frame_counter_wraps_to_1)
{
  uint8_t c = 0;
  c = NextFrameCounter(c);
  CHECK(c == 1);
  for (int i = 0; i < 254; ++i)
    c = NextFrameCounter(c);
  CHECK(c == 255);
  c = NextFrameCounter(c);
  CHECK(c == 1);
  // WLED receivers accept the wrap: (int8)(1 - 255) > 0.
  CHECK(static_cast<int8_t>(static_cast<uint8_t>(1 - 255)) > 0);
}

TEST(version_compare)
{
  CHECK(CompareVersions("2.0.4", "2.0.4") == 0);
  CHECK(CompareVersions("2.0.3", "2.0.4") < 0);
  CHECK(CompareVersions("2.1.0", "2.0.9") > 0);
  CHECK(CompareVersions("5.15.1", "5.9.0") > 0);
  CHECK(CompareVersions("", "1.0.0") < 0);
  CHECK(CompareVersions(nullptr, "") == 0);
  CHECK(CompareVersions("1.0", "1.0.0") == 0);
}

namespace
{
const char* g_version_override = nullptr;
int g_override_type = -1;

const char* MatchingVersion(int type)
{
  if (type == g_override_type)
    return g_version_override;
  switch (type)
  {
    case ADDON_GLOBAL_MAIN:
      return ADDON_GLOBAL_VERSION_MAIN;
    case ADDON_GLOBAL_GENERAL:
      return ADDON_GLOBAL_VERSION_GENERAL;
    case ADDON_INSTANCE_VISUALIZATION:
      return ADDON_INSTANCE_VERSION_VISUALIZATION;
    case ADDON_GLOBAL_AUDIOENGINE:
      return "0.0.0"; // unused, as kodi::addon::GetTypeVersion reports it
    default:
      return nullptr; // unused global type
  }
}
} // namespace

TEST(version_check_accepts_matching_target)
{
  std::string error;
  g_override_type = -1;
  CHECK(CheckTargetVersions(MatchingVersion, MatchingVersion, error));
  CHECK(CheckTargetVersions(MatchingVersion, nullptr, error));
}

TEST(version_check_rejects_old_and_new)
{
  std::string error;
  g_override_type = ADDON_INSTANCE_VISUALIZATION;
  g_version_override = "4.0.0";
  CHECK(!CheckTargetVersions(MatchingVersion, nullptr, error));
  CHECK(error.find("visualization") != std::string::npos);
  g_version_override = "6.0.0";
  CHECK(!CheckTargetVersions(MatchingVersion, nullptr, error));
  g_version_override = nullptr; // visualization must be present
  CHECK(!CheckTargetVersions(MatchingVersion, nullptr, error));
  g_version_override = "0.0.0";
  CHECK(!CheckTargetVersions(MatchingVersion, nullptr, error));
  g_override_type = ADDON_GLOBAL_GUI;
  g_version_override = "1.0.0";
  CHECK(!CheckTargetVersions(MatchingVersion, nullptr, error));
  g_override_type = -1;
}

TEST_MAIN()
