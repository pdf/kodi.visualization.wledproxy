#include "resample.h"

#include <cmath>
#include <cstring>

namespace wledproxy
{
namespace
{

// Zeroth-order modified Bessel function of the first kind (series).
double BesselI0(double x)
{
  double sum = 1.0;
  double term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 50; ++k)
  {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < 1e-12 * sum)
      break;
  }
  return sum;
}

} // namespace

Resampler::Resampler(float scale) : m_scale(scale)
{
  // Half-band windowed sinc: h[n] = 0.5 * sinc((n - M) / 2) * kaiser[n].
  constexpr int M = (kTaps - 1) / 2;
  constexpr double kBeta = 7.857; // Kaiser beta for about 80 dB attenuation
  const double i0beta = BesselI0(kBeta);
  double sum = 0.0;
  double h[kTaps];
  for (int n = 0; n < kTaps; ++n)
  {
    const int k = n - M;
    double sinc;
    if (k == 0)
      sinc = 0.5;
    else if (k % 2 == 0)
      sinc = 0.0; // half-band: every other tap is zero
    else
      sinc = std::sin(M_PI * k / 2.0) / (M_PI * k);
    const double r = static_cast<double>(k) / M;
    const double w = BesselI0(kBeta * std::sqrt(1.0 - r * r)) / i0beta;
    h[n] = sinc * w;
    sum += h[n];
  }
  for (int n = 0; n < kTaps; ++n)
    m_coeffs[n] = static_cast<float>(h[n] / sum); // unity gain at DC
  Reset();
}

void Resampler::Reset()
{
  std::memset(m_history, 0, sizeof(m_history));
  m_pos = 0;
  m_odd = false;
}

size_t Resampler::Process(const float* stereo, size_t frames, float* out)
{
  size_t written = 0;
  for (size_t i = 0; i < frames; ++i)
  {
    const float mono = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
    m_history[m_pos] = mono;
    m_history[m_pos + kTaps] = mono;
    m_pos = (m_pos + 1) % kTaps;

    m_odd = !m_odd;
    if (!m_odd)
      continue; // keep every second sample

    // Oldest sample is at m_pos; the newest at m_pos + kTaps - 1.
    const float* x = m_history + m_pos;
    float acc = m_coeffs[(kTaps - 1) / 2] * x[(kTaps - 1) / 2];
    for (int n = 0; n < kTaps; n += 2) // the odd taps are zero
      acc += m_coeffs[n] * x[n];
    out[written++] = acc * m_scale;
  }
  return written;
}

} // namespace wledproxy
