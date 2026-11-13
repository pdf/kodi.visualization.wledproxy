#pragma once

#include "wled_analysis.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>

namespace wledproxy
{

constexpr size_t kPacketSize = 44;
constexpr uint16_t kDefaultPort = 11988;
constexpr const char* kMulticastGroup = "239.0.0.1";

// Encodes one WLED audio sync V2 packet (44 bytes, little-endian).
void EncodePacket(const SyncFrame& frame, uint8_t frame_counter, uint8_t out[kPacketSize]);

// Next frameCounter: 1..255, wrapping 255 to 1 (0 means "legacy sender").
inline uint8_t NextFrameCounter(uint8_t c)
{
  return c >= 255 ? 1 : static_cast<uint8_t>(c + 1);
}

enum class Destination
{
  Multicast = 0,
  Broadcast = 1,
  Unicast = 2
};

struct SenderConfig
{
  bool enabled = true;
  Destination destination = Destination::Multicast;
  std::string host;
  int port = kDefaultPort;
  // Test hook: deliver multicast to local sockets too. Off in normal use;
  // set by WLEDPROXY_MULTICAST_LOOP=1 in the environment.
  bool multicast_loop = false;

  bool operator==(const SenderConfig& o) const
  {
    return enabled == o.enabled && destination == o.destination && host == o.host &&
           port == o.port && multicast_loop == o.multicast_loop;
  }
  bool operator!=(const SenderConfig& o) const { return !(*this == o); }
};

// Socket, addressing and sending.
//
// Configure() runs on the GUI thread; Send() runs on the audio thread and
// never blocks: if a reconfiguration holds the lock, that packet is dropped.
class Sender
{
public:
  ~Sender();

  // Opens (or closes, when disabled) the socket for this configuration.
  // Returns false and fills error if the destination can't be set up.
  bool Configure(const SenderConfig& config, std::string& error);
  void Close();

  // Encodes and sends one frame. Audio thread only.
  void Send(const SyncFrame& frame);

  // Resets the frame counter, so the next packet carries 1.
  void ResetCounter() { m_reset_counter.store(true, std::memory_order_relaxed); }

  // Send errors since the last call (for rate-limited logging).
  unsigned TakeErrorCount() { return m_errors.exchange(0, std::memory_order_relaxed); }
  int LastErrno() const { return m_last_errno.load(std::memory_order_relaxed); }

private:
  std::mutex m_mutex;
  int m_fd = -1;
  sockaddr_in m_addr = {};
  uint8_t m_counter = 0;
  std::atomic<bool> m_reset_counter{true};
  std::atomic<unsigned> m_errors{0};
  std::atomic<int> m_last_errno{0};
};

// Subnet broadcast address of the interface holding the default IPv4 route.
bool DefaultRouteBroadcast(in_addr& out, std::string& error);

} // namespace wledproxy
