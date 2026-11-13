#pragma once

#include <cstddef>
#include <cstdint>

namespace wledproxy
{

// One analysis result, with the fields of WLED's audio sync V2 packet.
struct SyncFrame
{
  float sampleRaw = 0;     // rawSampleAgc (AGC on)
  float sampleSmth = 0;    // sampleAgc (AGC on)
  uint8_t samplePeak = 0;  // 1 if a peak was detected since the last frame
  uint8_t fftResult[16] = {};
  uint16_t zeroCrossingCount = 0;
  float FFT_Magnitude = 0; // FFT_Magnitude scaled by the AGC gain (my_magnitude)
  float FFT_MajorPeak = 1; // Hz
  float pressure = 0;      // soundPressure, 0..255
};

// WLED-compatible analysis of mono 22050 Hz samples in the signed 16-bit range.
//
// A reimplementation of the processing the MoonModules "Audioreactive V2"
// usermod (audio_reactive.h) does on a classic dual-core ESP32 build, with
// its defaults: DC removal, Blackman-Harris window, 512-point FFT, pink-noise
// peak weighting, freqDist 0 band mapping, frequency profile 1 (line-in),
// square-root scaling, dynamics limiter on, AGC preset "normal", squelch 10,
// gain 60. See docs/wled-sync.md for every constant and its source.
//
// WLED runs its volume filters and AGC in the main loop, about once per
// millisecond, and the FFT every 512 samples. Here the loop is simulated in
// 1 ms steps on a clock driven by the sample count, so results don't depend
// on how Kodi batches the audio. No allocation after construction.
class WledAnalyzer
{
public:
  static constexpr int kSampleRate = 22050;
  static constexpr int kFftSize = 512;

  WledAnalyzer();

  void Reset();

  // Adds samples. For every completed 512-sample frame, fills frames[k]
  // (up to max_frames) and returns the number of frames completed.
  size_t Push(const float* samples, size_t count, SyncFrame* frames, size_t max_frames);

  // Internal state, for tests.
  float MultAgc() const { return static_cast<float>(m_multAgc); }
  float VolumeSmth() const { return m_volumeSmth; }

private:
  void RunFrame(SyncFrame& out);
  void RunFft(); // FFT + band mapping + post-processing, on m_vReal
  void Fft();    // in-place radix-2 on m_vReal/m_vImag
  float BandAvg(int from, int to) const;
  void PostProcess(bool gate_open);
  void DetectSamplePeak();
  void AutoResetPeak();
  void LoopTick(double now_ms);
  void GetSample(double now_ms);
  void AgcAvg(double now_ms);
  float EstimatePressure() const;
  void LimitSampleDynamics(double now_ms);

  // Input collection.
  float m_input[kFftSize];
  int m_fill = 0;

  // Constant tables.
  float m_window[kFftSize];
  float m_pinkFactors[kFftSize];
  float m_cos[kFftSize / 2];
  float m_sin[kFftSize / 2];
  uint16_t m_bitrev[kFftSize];

  // FFT work buffers.
  float m_vReal[kFftSize];
  float m_vImag[kFftSize];

  // Simulated clock (ms since Reset) and loop scheduling.
  double m_now = 0;
  double m_lastTick = 0;

  // FFT task state.
  float m_micDataReal = 0;
  float m_fftCalc[16];
  float m_fftAvg[16];
  float m_lastFftCalc[16];
  uint8_t m_fftResult[16];
  float m_FFT_MajorPeak = 1;
  float m_FFT_Magnitude = 0;
  float m_FFT_MajPeakSmth = 1;
  uint16_t m_zeroCrossingCount = 0;

  // getSample() / agcAvg() state.
  double m_micLev = 0;
  float m_expAdjF = 0;
  float m_sampleReal = 0;
  int16_t m_sampleRaw = 0;
  float m_sampleAvg = 0;
  double m_sampleMax = 0;
  double m_multAgc = 1;
  double m_controlIntegrated = 0;
  float m_sampleAgc = 0;
  int16_t m_rawSampleAgc = 0;
  double m_agcLastTime = 0;
  bool m_isFrozen = false;
  bool m_haveSilence = true;
  double m_lastSoundTime = 0;

  // Effects-facing values (loop).
  float m_volumeSmth = 0;
  float m_lastVolumeSmth = 0;
  double m_limiterLastTime = 0;
  float m_myMagnitude = 0;
  float m_soundPressure = 0;
  double m_lastEstimate = 0;

  // Peaks.
  bool m_samplePeak = false;
  bool m_udpSamplePeak = false;
  double m_timeOfPeak = 0;
};

} // namespace wledproxy
