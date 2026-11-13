#include "sender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace wledproxy
{
namespace
{

void PutFloat(uint8_t* p, float v)
{
  uint32_t u;
  std::memcpy(&u, &v, 4);
  p[0] = static_cast<uint8_t>(u);
  p[1] = static_cast<uint8_t>(u >> 8);
  p[2] = static_cast<uint8_t>(u >> 16);
  p[3] = static_cast<uint8_t>(u >> 24);
}

// Interface name of the default IPv4 route, from /proc/net/route.
bool DefaultRouteInterface(std::string& name)
{
  std::ifstream f("/proc/net/route");
  std::string line;
  std::getline(f, line); // header
  unsigned best_metric = ~0u;
  bool found = false;
  while (std::getline(f, line))
  {
    std::istringstream ss(line);
    std::string iface, dest, gw, flags, refcnt, use, metric, mask;
    if (!(ss >> iface >> dest >> gw >> flags >> refcnt >> use >> metric >> mask))
      continue;
    const unsigned long fl = std::strtoul(flags.c_str(), nullptr, 16);
    if (dest != "00000000" || mask != "00000000" || !(fl & 0x1)) // RTF_UP
      continue;
    const unsigned m = static_cast<unsigned>(std::strtoul(metric.c_str(), nullptr, 10));
    if (!found || m < best_metric)
    {
      name = iface;
      best_metric = m;
      found = true;
    }
  }
  return found;
}

} // namespace

void EncodePacket(const SyncFrame& frame, uint8_t frame_counter, uint8_t out[kPacketSize])
{
  std::memset(out, 0, kPacketSize);
  std::memcpy(out, "00002", 6); // includes the terminating NUL

  // pressure: 8.8 fixed point, integer part saturating at 255.
  const float p = frame.pressure > 0.0f ? frame.pressure : 0.0f;
  const uint32_t fixed = static_cast<uint32_t>(p * 256.0f);
  uint32_t integer = fixed / 256;
  if (integer > 255)
    integer = 255;
  out[6] = static_cast<uint8_t>(integer);
  out[7] = static_cast<uint8_t>(fixed % 256);

  PutFloat(out + 8, frame.sampleRaw);
  PutFloat(out + 12, frame.sampleSmth);
  out[16] = frame.samplePeak;
  out[17] = frame_counter;
  std::memcpy(out + 18, frame.fftResult, 16);
  out[34] = static_cast<uint8_t>(frame.zeroCrossingCount);
  out[35] = static_cast<uint8_t>(frame.zeroCrossingCount >> 8);
  PutFloat(out + 36, frame.FFT_Magnitude);
  PutFloat(out + 40, frame.FFT_MajorPeak);
}

bool DefaultRouteBroadcast(in_addr& out, std::string& error)
{
  std::string iface;
  if (!DefaultRouteInterface(iface))
  {
    error = "no default IPv4 route";
    return false;
  }
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0)
  {
    error = std::string("getifaddrs: ") + std::strerror(errno);
    return false;
  }
  bool found = false;
  for (ifaddrs* a = list; a; a = a->ifa_next)
  {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name)
      continue;
    const auto* addr = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
    const auto* mask = reinterpret_cast<const sockaddr_in*>(a->ifa_netmask);
    if (!mask)
      continue;
    out.s_addr = addr->sin_addr.s_addr | ~mask->sin_addr.s_addr;
    found = true;
    break;
  }
  freeifaddrs(list);
  if (!found)
    error = "no IPv4 address on " + iface;
  return found;
}

Sender::~Sender()
{
  Close();
}

void Sender::Close()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_fd >= 0)
    ::close(m_fd);
  m_fd = -1;
}

bool Sender::Configure(const SenderConfig& config, std::string& error)
{
  Close();
  if (!config.enabled)
    return true;

  if (config.port <= 0 || config.port > 65535)
  {
    error = "invalid port " + std::to_string(config.port);
    return false;
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(config.port));
  switch (config.destination)
  {
    case Destination::Multicast:
      inet_pton(AF_INET, kMulticastGroup, &addr.sin_addr);
      break;
    case Destination::Broadcast:
      if (!DefaultRouteBroadcast(addr.sin_addr, error))
        return false;
      break;
    case Destination::Unicast:
      if (config.host.empty() || inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1)
      {
        error = "invalid receiver address '" + config.host + "'";
        return false;
      }
      break;
  }

  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
  {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  if (config.destination == Destination::Multicast)
  {
    const unsigned char ttl = 1;
    const unsigned char loop = config.multicast_loop ? 1 : 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  }
  else if (config.destination == Destination::Broadcast)
  {
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0)
    {
      error = std::string("SO_BROADCAST: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  m_fd = fd;
  m_addr = addr;
  return true;
}

void Sender::Send(const SyncFrame& frame)
{
  std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
  if (!lock.owns_lock() || m_fd < 0)
    return;
  if (m_reset_counter.exchange(false, std::memory_order_relaxed))
    m_counter = 0;
  m_counter = NextFrameCounter(m_counter);

  uint8_t packet[kPacketSize];
  EncodePacket(frame, m_counter, packet);
  const ssize_t n = ::sendto(m_fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL,
                             reinterpret_cast<const sockaddr*>(&m_addr), sizeof(m_addr));
  if (n != static_cast<ssize_t>(sizeof(packet)))
  {
    m_last_errno.store(errno, std::memory_order_relaxed);
    m_errors.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace wledproxy
