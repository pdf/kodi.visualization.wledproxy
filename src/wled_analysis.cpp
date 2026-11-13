// WLED-compatible audio analysis.
//
// Reimplemented from the behaviour and published constants of the
// MoonModules "Audioreactive V2" usermod (audio_reactive.h); no WLED code is
// used. docs/wled-sync.md lists each constant with its source line.

#include "wled_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wledproxy
{
namespace
{

constexpr int N = WledAnalyzer::kFftSize;
constexpr float kSampleRate = static_cast<float>(WledAnalyzer::kSampleRate);
constexpr float kBinWidth = kSampleRate / N; // 43.07 Hz
constexpr double kFrameMs = 1000.0 * N / WledAnalyzer::kSampleRate;

// User settings at their defaults.
constexpr float kSoundSquelch = 10.0f;
constexpr float kSampleGain = 60.0f;
constexpr float kInputLevel = 128.0f;
constexpr int kPinkIndex = 1;       // line-in profile
constexpr float kDecayTimeMs = 300; // dynamics limiter decay
constexpr float kAttackTimeMs = 50; // dynamics limiter attack
constexpr int kBinNum = 8;          // peak detection bin
constexpr float kMaxVol = 31;       // peak detection threshold
constexpr double kMinShowDelayMs = 50;

// AGC preset "normal".
constexpr double kAgcSampleDecay = 0.9994;
constexpr float kAgcZoneLow = 32;
constexpr float kAgcZoneHigh = 240;
constexpr float kAgcZoneStop = 336;
constexpr float kAgcTarget0 = 112;
constexpr float kAgcTarget0Up = 88;
constexpr float kAgcTarget1 = 220;
constexpr double kAgcFollowFast = 1.0 / 192.0;
constexpr double kAgcFollowSlow = 1.0 / 6144.0;
constexpr double kAgcKp = 0.6;
constexpr double kAgcKi = 1.7;
constexpr float kAgcSampleSmooth = 1.0f / 12.0f;

// Frequency profile 1 ("Line-In CS5343 + DC blocker").
constexpr float kPink[16] = {2.35f, 1.32f, 1.32f, 1.40f, 1.48f, 1.57f, 1.68f, 1.80f,
                             1.89f, 1.95f, 2.14f, 2.26f, 2.50f, 2.90f, 4.20f, 6.50f};

constexpr float kFftDownscale = 0.40f; // RMS band averaging
constexpr float kPinkCenter = 23.66f;  // sqrt(560 Hz)

float Clamp(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

float MapF(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

} // namespace

WledAnalyzer::WledAnalyzer()
{
  for (int i = 0; i < N; ++i)
  {
    // 4-term Blackman-Harris, symmetric over N samples.
    const double r = static_cast<double>(i) / (N - 1);
    m_window[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(2 * M_PI * r) +
                                     0.14128 * std::cos(4 * M_PI * r) -
                                     0.01168 * std::cos(6 * M_PI * r));

    // Pink-noise weighting used only to pick the major peak.
    float f = i * kBinWidth + kBinWidth / 2.0f;
    if (f > kSampleRate * 0.42f)
      f = kSampleRate * 0.42f - 0.25f * (f - kSampleRate * 0.42f);
    m_pinkFactors[i] = std::sqrt(f) / kPinkCenter;
  }
  m_pinkFactors[0] *= 0.5f;

  for (int i = 0; i < N / 2; ++i)
  {
    m_cos[i] = static_cast<float>(std::cos(2 * M_PI * i / N));
    m_sin[i] = static_cast<float>(-std::sin(2 * M_PI * i / N));
  }
  int bits = 0;
  while ((1 << bits) < N)
    ++bits;
  for (int i = 0; i < N; ++i)
  {
    int r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b))
        r |= 1 << (bits - 1 - b);
    m_bitrev[i] = static_cast<uint16_t>(r);
  }
  Reset();
}

void WledAnalyzer::Reset()
{
  std::memset(m_input, 0, sizeof(m_input));
  m_fill = 0;
  std::memset(m_vReal, 0, sizeof(m_vReal));
  std::memset(m_vImag, 0, sizeof(m_vImag));
  m_now = 0;
  m_lastTick = 0;
  m_micDataReal = 0;
  std::memset(m_fftCalc, 0, sizeof(m_fftCalc));
  std::memset(m_fftAvg, 0, sizeof(m_fftAvg));
  std::memset(m_lastFftCalc, 0, sizeof(m_lastFftCalc));
  std::memset(m_fftResult, 0, sizeof(m_fftResult));
  m_FFT_MajorPeak = 1;
  m_FFT_Magnitude = 0;
  m_FFT_MajPeakSmth = 1;
  m_zeroCrossingCount = 0;
  m_micLev = 0;
  m_expAdjF = 0;
  m_sampleReal = 0;
  m_sampleRaw = 0;
  m_sampleAvg = 0;
  m_sampleMax = 0;
  m_multAgc = 1;
  m_controlIntegrated = 0;
  m_sampleAgc = 0;
  m_rawSampleAgc = 0;
  m_agcLastTime = 0;
  m_isFrozen = false;
  m_haveSilence = true;
  m_lastSoundTime = 0;
  m_volumeSmth = 0;
  m_lastVolumeSmth = 0;
  m_limiterLastTime = 0;
  m_myMagnitude = 0;
  m_soundPressure = 0;
  m_lastEstimate = 0;
  m_samplePeak = false;
  m_udpSamplePeak = false;
  m_timeOfPeak = 0;
}

size_t WledAnalyzer::Push(const float* samples, size_t count, SyncFrame* frames,
                          size_t max_frames)
{
  size_t done = 0;
  while (count > 0)
  {
    const size_t take = std::min(count, static_cast<size_t>(N - m_fill));
    std::memcpy(m_input + m_fill, samples, take * sizeof(float));
    m_fill += static_cast<int>(take);
    samples += take;
    count -= take;
    if (m_fill < N)
      break;
    m_fill = 0;
    SyncFrame scratch;
    RunFrame(done < max_frames ? frames[done] : scratch);
    ++done;
  }
  return std::min(done, max_frames);
}

void WledAnalyzer::RunFrame(SyncFrame& out)
{
  // The main loop runs while the FFT task waits for this batch of samples.
  m_now += kFrameMs;
  while (m_lastTick + 1.0 <= m_now)
  {
    m_lastTick += 1.0;
    LoopTick(m_lastTick);
  }

  std::memcpy(m_vReal, m_input, sizeof(m_vReal));
  RunFft();

  // transmitAudioData()
  out.sampleRaw = m_rawSampleAgc;
  out.sampleSmth = m_sampleAgc;
  out.samplePeak = m_udpSamplePeak ? 1 : 0;
  m_udpSamplePeak = false;
  std::memcpy(out.fftResult, m_fftResult, sizeof(out.fftResult));
  out.zeroCrossingCount = m_zeroCrossingCount;
  out.FFT_Magnitude = m_myMagnitude;
  out.FFT_MajorPeak = m_FFT_MajorPeak;
  out.pressure = m_soundPressure;
}

void WledAnalyzer::RunFft()
{
  std::memset(m_vImag, 0, sizeof(m_vImag));

  // Highest sample (ignoring near-clipping artefacts) and zero crossings.
  float maxSample = 0.0f;
  unsigned crossings = 0;
  for (int i = 0; i < N; ++i)
  {
    const float v = m_vReal[i];
    if (v <= 32767.0f - 1024.0f && v >= -32768.0f + 1024.0f)
      maxSample = std::max(maxSample, std::fabs(v));
    if (i < N - 1 && std::signbit(v) != std::signbit(m_vReal[i + 1]))
      ++crossings;
  }
  m_zeroCrossingCount = static_cast<uint16_t>((crossings * 2) / 3);
  m_micDataReal = maxSample;

  const bool gate_open = std::fabs(m_volumeSmth) > 0.25f;
  if (gate_open)
  {
    // DC removal: subtract the mean.
    double mean = 0;
    for (int i = 0; i < N; ++i)
      mean += m_vReal[i];
    mean /= N;
    for (int i = 0; i < N; ++i)
      m_vReal[i] = (m_vReal[i] - static_cast<float>(mean)) * m_window[i];

    Fft();
    for (int i = 0; i < N; ++i)
      m_vReal[i] = std::sqrt(m_vReal[i] * m_vReal[i] + m_vImag[i] * m_vImag[i]);
    m_vReal[0] = 0;

    const float last_peak = m_FFT_MajorPeak;
    const float last_mag = m_FFT_Magnitude;

    // Major peak on pink-weighted magnitudes, with parabolic interpolation.
    for (int i = 0; i < N; ++i)
      m_vReal[i] *= m_pinkFactors[i];
    int peak = 0;
    float peak_val = 0;
    for (int i = 1; i <= N / 2; ++i)
    {
      if (m_vReal[i - 1] < m_vReal[i] && m_vReal[i] > m_vReal[i + 1] && m_vReal[i] > peak_val)
      {
        peak_val = m_vReal[i];
        peak = i;
      }
    }
    if (peak > 0)
    {
      const float a = m_vReal[peak - 1];
      const float b = m_vReal[peak];
      const float c = m_vReal[peak + 1];
      const float denom = a - 2.0f * b + c;
      const float delta = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;
      m_FFT_MajorPeak = (peak + delta) * kBinWidth;
      m_FFT_Magnitude = b - 0.25f * (a - c) * delta;
    }
    else
    {
      m_FFT_MajorPeak = 0;
      m_FFT_Magnitude = 0;
    }
    if (m_FFT_MajorPeak < kSampleRate / N)
    {
      m_FFT_MajorPeak = 1.0f;
      m_FFT_Magnitude = 0;
    }
    if (m_FFT_MajorPeak > 0.42f * kSampleRate)
    {
      m_FFT_MajorPeak = last_peak;
      m_FFT_Magnitude = last_mag;
    }
    for (int i = 0; i < N; ++i)
      m_vReal[i] /= m_pinkFactors[i];
    if (m_FFT_MajorPeak > kBinWidth / 1.25f && m_FFT_MajorPeak < kSampleRate / 2.2f &&
        m_FFT_Magnitude > 4.0f)
    {
      const int bin = std::clamp(static_cast<int>((m_FFT_MajorPeak + kBinWidth / 2.0f) / kBinWidth),
                                 0, N - 1);
      m_FFT_Magnitude *= std::max(1.0f / m_pinkFactors[bin], 1.0f);
    }
    m_FFT_MajorPeak = Clamp(m_FFT_MajorPeak, 1.0f, 11025.0f);
    m_FFT_MajPeakSmth += 0.42f * (m_FFT_MajorPeak - m_FFT_MajPeakSmth);
  }
  else
  {
    std::memset(m_vReal, 0, sizeof(m_vReal));
    m_FFT_MajorPeak = 1;
    m_FFT_Magnitude = 0.001f;
  }

  for (int i = 0; i < N; ++i)
    m_vReal[i] = std::fabs(m_vReal[i]) / 16.0f;

  if (gate_open)
  {
    // freqDist 0: bins to 16 channels (bin width 43 Hz).
    m_fftCalc[0] = BandAvg(1, 1);              //   43 -   86 Hz
    m_fftCalc[1] = BandAvg(2, 2);              //   86 -  129
    m_fftCalc[2] = BandAvg(3, 4);              //  129 -  216
    m_fftCalc[3] = BandAvg(5, 6);              //  216 -  301
    m_fftCalc[4] = BandAvg(7, 9);              //  301 -  430
    m_fftCalc[5] = BandAvg(10, 12);            //  430 -  560
    m_fftCalc[6] = BandAvg(13, 18);            //  560 -  818
    m_fftCalc[7] = BandAvg(19, 25);            //  818 - 1120
    m_fftCalc[8] = BandAvg(26, 32);            // 1120 - 1421
    m_fftCalc[9] = BandAvg(33, 43);            // 1421 - 1895
    m_fftCalc[10] = BandAvg(44, 55);           // 1895 - 2412
    m_fftCalc[11] = BandAvg(56, 69);           // 2412 - 3015
    m_fftCalc[12] = BandAvg(70, 85);           // 3015 - 3704
    m_fftCalc[13] = BandAvg(86, 103);          // 3704 - 4479
    m_fftCalc[14] = BandAvg(104, 164) * 0.88f; // 4479 - 7106
    m_fftCalc[15] = BandAvg(165, 215) * 0.70f; // 7106 - 9259
  }
  else
  {
    for (float& c : m_fftCalc)
    {
      c *= 0.85f;
      if (c < 4.0f)
        c = 0.0f;
    }
  }
  std::memcpy(m_lastFftCalc, m_fftCalc, sizeof(m_lastFftCalc));

  PostProcess(gate_open);
  AutoResetPeak();
  DetectSamplePeak();
}

void WledAnalyzer::Fft()
{
  for (int i = 0; i < N; ++i)
  {
    const int j = m_bitrev[i];
    if (j > i)
    {
      std::swap(m_vReal[i], m_vReal[j]);
      std::swap(m_vImag[i], m_vImag[j]);
    }
  }
  for (int len = 2; len <= N; len <<= 1)
  {
    const int half = len / 2;
    const int step = N / len;
    for (int start = 0; start < N; start += len)
    {
      for (int k = 0; k < half; ++k)
      {
        const float wr = m_cos[k * step];
        const float wi = m_sin[k * step];
        const int a = start + k;
        const int b = a + half;
        const float tr = m_vReal[b] * wr - m_vImag[b] * wi;
        const float ti = m_vReal[b] * wi + m_vImag[b] * wr;
        m_vReal[b] = m_vReal[a] - tr;
        m_vImag[b] = m_vImag[a] - ti;
        m_vReal[a] += tr;
        m_vImag[a] += ti;
      }
    }
  }
}

float WledAnalyzer::BandAvg(int from, int to) const
{
  if (from == to)
    return m_vReal[from];
  double sum = 0;
  for (int i = from; i <= to; ++i)
    sum += static_cast<double>(m_vReal[i]) * m_vReal[i];
  return std::sqrt(static_cast<float>(sum / (to - from + 1)));
}

void WledAnalyzer::PostProcess(bool gate_open)
{
  for (int i = 0; i < 16; ++i)
  {
    if (gate_open)
    {
      m_fftCalc[i] *= kPink[i];
      m_fftCalc[i] *= kFftDownscale;
      m_fftCalc[i] *= static_cast<float>(m_multAgc);
      if (m_fftCalc[i] < 0)
        m_fftCalc[i] = 0;
    }

    // Dynamics limiter: rise fast, fall at the rate for a 300 ms decay time.
    if (m_fftCalc[i] > m_fftAvg[i])
      m_fftAvg[i] += 0.78f * (m_fftCalc[i] - m_fftAvg[i]);
    else
      m_fftAvg[i] += 0.33f * (m_fftCalc[i] - m_fftAvg[i]);

    m_fftCalc[i] = Clamp(m_fftCalc[i], 0.0f, 1023.0f);
    m_fftAvg[i] = Clamp(m_fftAvg[i], 0.0f, 1023.0f);

    // Square-root scaling to 0..255.
    float r = m_fftAvg[i];
    r *= 0.38f;
    r -= 6.0f;
    r = r > 1.0f ? std::sqrt(r) : 0.0f;
    r *= 0.85f + static_cast<float>(i) / 4.5f;
    r = MapF(r, 0.0f, 16.0f, 0.0f, 255.0f);
    // GEQ post-gain is 1.0 at the default input level of 128.
    m_fftResult[i] = static_cast<uint8_t>(std::max(std::min(static_cast<int>(r + 0.5f), 255), 0));
  }
}

void WledAnalyzer::DetectSamplePeak()
{
  if (m_sampleAvg > 1 && kMaxVol > 0 && kBinNum > 4 && m_vReal[kBinNum] > kMaxVol &&
      (m_now - m_timeOfPeak) > 100)
  {
    m_samplePeak = true;
    m_timeOfPeak = m_now;
    m_udpSamplePeak = true;
  }
}

void WledAnalyzer::AutoResetPeak()
{
  if (m_now - m_timeOfPeak > kMinShowDelayMs)
    m_samplePeak = false; // udpSamplePeak is reset by the sender
}

void WledAnalyzer::LoopTick(double now)
{
  GetSample(now);
  AgcAvg(now);

  m_volumeSmth = m_sampleAgc;
  m_myMagnitude = m_FFT_Magnitude * static_cast<float>(m_multAgc);
  if (m_volumeSmth < 1)
    m_myMagnitude = 0.001f;

  if (now - m_lastEstimate > 12)
  {
    m_lastEstimate = now;
    m_soundPressure += 0.38f * (EstimatePressure() - m_soundPressure);
  }
  LimitSampleDynamics(now);

  // AutoResetPeak() at loop time.
  if (now - m_timeOfPeak > kMinShowDelayMs)
    m_samplePeak = false;
}

void WledAnalyzer::GetSample(double now)
{
  constexpr float kWeighting = 0.18f;
  constexpr float kWeighting2 = 0.073f;

  // micLevelMethod 1 ("freeze"): the level floor stops rising during sound.
  if (!m_isFrozen)
    m_micLev += (m_micDataReal - m_micLev) / 12288.0;
  if (m_micDataReal < m_micLev - 0.24)
  {
    m_micLev = (m_micLev * 31.0 + m_micDataReal) / 32.0;
    if (!m_haveSilence)
      m_isFrozen = true;
  }

  float micInNoDC = static_cast<float>(std::fabs(m_micDataReal - m_micLev));
  if (micInNoDC > m_expAdjF && m_expAdjF > kSoundSquelch)
    m_expAdjF = kWeighting2 * micInNoDC + (1.0f - kWeighting2) * m_expAdjF;
  else
    m_expAdjF = kWeighting * micInNoDC + (1.0f - kWeighting) * m_expAdjF;
  m_expAdjF = std::fabs(m_expAdjF);

  if (m_expAdjF <= kSoundSquelch)
  {
    m_expAdjF = 0.0f;
    micInNoDC = 0.0f;
  }
  if (m_expAdjF <= 0.5f)
    m_haveSilence = true;
  else
  {
    m_lastSoundTime = now;
    m_haveSilence = false;
  }
  if (m_isFrozen && m_haveSilence && (now - m_lastSoundTime) > 4000)
    m_isFrozen = false;

  const float tmpSample = m_expAdjF;
  float sampleAdj = tmpSample * kSampleGain / 40.0f * kInputLevel / 128.0f + tmpSample / 16.0f;
  m_sampleReal = tmpSample;
  sampleAdj = std::fmax(std::fmin(sampleAdj, 255.0f), 0.0f);
  m_sampleRaw = static_cast<int16_t>(sampleAdj);

  if (m_sampleMax < m_sampleReal && m_sampleReal > 0.5f)
  {
    m_sampleMax += 0.5 * (m_sampleReal - m_sampleMax);
    if ((now - m_timeOfPeak) > 80 && m_sampleAvg > 1)
    {
      m_samplePeak = true;
      m_timeOfPeak = now;
      m_udpSamplePeak = true;
    }
  }
  else
  {
    if (m_multAgc * m_sampleMax > kAgcZoneStop)
      m_sampleMax += 0.5 * (m_sampleReal - m_sampleMax);
    else
      m_sampleMax *= kAgcSampleDecay;
  }
  if (m_sampleMax < 0.5)
    m_sampleMax = 0.0;

  m_sampleAvg = (m_sampleAvg * 15.0f + sampleAdj) / 16.0f;
  m_sampleAvg = std::fabs(m_sampleAvg);
}

void WledAnalyzer::AgcAvg(double now)
{
  const double lastMultAgc = m_multAgc;
  double multAgcTemp = m_multAgc;
  double tmpAgc;

  if (now - m_agcLastTime > 2)
  {
    m_agcLastTime = now;
    tmpAgc = m_sampleReal * m_multAgc;
    if (std::fabs(m_sampleReal) < 2.0f || m_sampleMax < 1.0)
    {
      // Squelched: keep the gain, spin down the integrator.
      if (std::fabs(m_controlIntegrated) < 0.01)
        m_controlIntegrated = 0.0;
      else
        m_controlIntegrated *= 0.91;
    }
    else
    {
      multAgcTemp = (tmpAgc <= kAgcTarget0Up) ? kAgcTarget0 / m_sampleMax
                                              : kAgcTarget1 / m_sampleMax;
    }
    multAgcTemp = std::clamp(multAgcTemp, 1.0 / 64.0, 32.0);

    const double control_error = multAgcTemp - lastMultAgc;
    if (multAgcTemp > 0.085 && multAgcTemp < 6.5 && m_multAgc * m_sampleMax < kAgcZoneStop)
      m_controlIntegrated += control_error * 0.002 * 0.25;
    else
      m_controlIntegrated *= 0.9;

    tmpAgc = m_sampleReal * lastMultAgc;
    const double follow = (tmpAgc > kAgcZoneHigh || tmpAgc < kSoundSquelch + kAgcZoneLow)
                              ? kAgcFollowFast
                              : kAgcFollowSlow;
    multAgcTemp = lastMultAgc + follow * kAgcKp * control_error;
    multAgcTemp += follow * kAgcKi * m_controlIntegrated;
    multAgcTemp = std::clamp(multAgcTemp, 1.0 / 64.0, 32.0);
  }

  float out = static_cast<float>(m_sampleReal * multAgcTemp);
  if (std::fabs(m_sampleReal) < 2.0f)
    out = 0.0f;
  if (out > 255)
    out = 255.0f;
  if (out < 1)
    out = 0.0f;

  m_multAgc = multAgcTemp;
  m_rawSampleAgc = static_cast<int16_t>(0.8f * out + 0.2f * static_cast<float>(m_rawSampleAgc));
  if (std::fabs(out) < 1.0f)
    m_sampleAgc = 0.5f * out + 0.5f * m_sampleAgc;
  else
    m_sampleAgc += kAgcSampleSmooth * (out - m_sampleAgc);
  m_sampleAgc = std::fabs(m_sampleAgc);
}

float WledAnalyzer::EstimatePressure() const
{
  constexpr float kLogMin = 0.8329091229351f; // ln(2.3)
  constexpr float kMin = 2.3f;
  constexpr float kLogMax = 10.1895683436f; // ln(32767 - 6144)
  constexpr float kMax = 32767.0f - 6144.0f;

  // Line-in correction (WLED dmType 4): scale down, then square.
  float v = std::fabs(m_sampleReal) / 11.0f;
  v *= v;
  if (v <= kMin)
    return 0.0f;
  if (v >= kMax)
    return 255.0f;
  const float scaled = (std::log(v) - kLogMin) / (kLogMax - kLogMin);
  return std::fmin(std::fmax(256.0f * scaled, 0.0f), 255.0f);
}

void WledAnalyzer::LimitSampleDynamics(double now)
{
  constexpr float kBigChange = 196;
  const double dt = std::clamp(now - m_limiterLastTime, 1.0, 1000.0);
  float delta = m_volumeSmth - m_lastVolumeSmth;
  const float maxAttack = kBigChange * static_cast<float>(dt) / kAttackTimeMs;
  const float maxDecay = -kBigChange * static_cast<float>(dt) / kDecayTimeMs;
  delta = std::clamp(delta, maxDecay, maxAttack);
  m_volumeSmth = m_lastVolumeSmth + delta;
  m_lastVolumeSmth = m_volumeSmth;
  m_limiterLastTime = now;
}

} // namespace wledproxy
