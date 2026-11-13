#pragma once

#include <cstddef>

namespace wledproxy
{

// Input scale: samples reach the WLED chain as signed 16-bit values, a 0 dBFS
// sine peaking at +-kInputScale. See docs/wled-sync.md, "Input level".
constexpr float kInputScale = 32767.0f;

// Stereo interleaved float at 44100 Hz to mono at 22050 Hz.
//
// The two channels are averaged, low-pass filtered with a half-band FIR
// (Kaiser window, about 80 dB stopband from 12.75 kHz, passband to 9.3 kHz,
// so nothing aliases into WLED's bands) and decimated by 2. Output is scaled
// by `scale`, so a 0 dBFS sine peaks at +-scale. No allocation.
class Resampler
{
public:
  static constexpr int kTaps = 63; // half-band length: 4k + 3

  explicit Resampler(float scale = kInputScale);

  void Reset();

  // Consumes `frames` stereo frames. Writes at most (frames + 1) / 2 mono
  // samples to out and returns how many were written.
  size_t Process(const float* stereo, size_t frames, float* out);

  float Scale() const { return m_scale; }

private:
  float m_coeffs[kTaps];
  float m_history[2 * kTaps]; // doubled ring buffer for contiguous reads
  int m_pos = 0;
  bool m_odd = false;
  float m_scale;
};

} // namespace wledproxy
