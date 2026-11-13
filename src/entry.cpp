// Exported symbols and the load/unload sequence.

#include "proxy.h"
#include "po.h"
#include "version_check.h"

#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// The kodi-dev-kit helpers used for the proxy's own settings, logging and
// notifications read this. It points at Kodi's real interface.
AddonGlobalInterface* kodi::addon::CPrivateBase::m_interface = nullptr;

namespace wledproxy
{
namespace
{

ProxyState* g_state = nullptr;

constexpr const char* kAddonId = WLEDPROXY_ADDON_ID;
constexpr int64_t kErrorLogIntervalMs = 60000;

// Proxy strings (resources/language/.../strings.po).
constexpr int kStrNoTarget = 30100;
constexpr int kStrTargetLoad = 30101;
constexpr int kStrTargetVersion = 30102;
constexpr int kStrSocket = 30103;

int64_t MonotonicMs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string Localized(int id, const char* fallback)
{
  std::string s = kodi::addon::GetLocalizedString(id, fallback);
  return s.empty() ? fallback : s;
}

// ---------------------------------------------------------------------------
// Proxy settings.
// ---------------------------------------------------------------------------

SenderConfig ReadSenderConfig()
{
  SenderConfig c;
  c.enabled = kodi::addon::GetSettingBoolean("enabled", true);
  const int dest = kodi::addon::GetSettingInt("destination", 0);
  c.destination = (dest == 1)   ? Destination::Broadcast
                  : (dest == 2) ? Destination::Unicast
                                : Destination::Multicast;
  c.host = kodi::addon::GetSettingString("udp_host", "");
  c.port = kodi::addon::GetSettingInt("udp_port", kDefaultPort);
  const char* loop = std::getenv("WLEDPROXY_MULTICAST_LOOP");
  c.multicast_loop = loop && std::strcmp(loop, "1") == 0;
  return c;
}

void ApplySenderConfig(const SenderConfig& c, bool notify)
{
  ProxyState& s = State();
  // Kodi reports every setting when its dialog saves; reopen only on change.
  if (s.sender_configured && c == s.sender_config)
    return;
  s.sender_config = c;
  s.sender_configured = true;
  std::string error;
  s.sending.store(false, std::memory_order_relaxed);
  if (!s.sender.Configure(c, error))
  {
    Log(ADDON_LOG_ERROR, "can't set up audio sync sender: %s", error.c_str());
    if (notify)
      Notify(Localized(kStrSocket, "Can't send audio sync packets") + ": " + error);
    return;
  }
  if (c.enabled)
  {
    static const char* const kNames[] = {"multicast", "broadcast", "unicast"};
    Log(ADDON_LOG_INFO, "sending audio sync packets (%s, port %d%s%s)",
        kNames[static_cast<int>(c.destination)], c.port,
        c.destination == Destination::Unicast ? ", host " : "",
        c.destination == Destination::Unicast ? c.host.c_str() : "");
  }
  else
    Log(ADDON_LOG_INFO, "audio sync packets disabled");
  s.sending.store(c.enabled, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Audio wrappers.
// ---------------------------------------------------------------------------

WrappedInstance* FindInstance(KODI_ADDON_INSTANCE_HDL hdl)
{
  if (!g_state || !hdl)
    return nullptr;
  for (WrappedInstance& w : g_state->instances)
    if (w.hdl.load(std::memory_order_acquire) == hdl)
      return &w;
  return nullptr;
}

bool WrapAudioStart(const KODI_ADDON_VISUALIZATION_HDL hdl,
                    int channels,
                    int samples_per_sec,
                    int bits_per_sample)
{
  WrappedInstance* w = FindInstance(hdl);
  auto fn = w ? w->audio_start.load(std::memory_order_acquire) : nullptr;
  const bool result = fn ? fn(hdl, channels, samples_per_sec, bits_per_sample) : false;
  if (g_state)
  {
    if (channels != 2 || samples_per_sec != 44100)
      Log(ADDON_LOG_WARNING, "unexpected visualization audio format: %d channels, %d Hz",
          channels, samples_per_sec);
    g_state->reset_analysis.store(true, std::memory_order_release);
  }
  return result;
}

void Analyse(ProxyState& s, const float* data, size_t length)
{
  if (s.reset_analysis.exchange(false, std::memory_order_acq_rel))
  {
    s.resampler.Reset();
    s.analyzer.Reset();
    s.sender.ResetCounter();
  }

  constexpr size_t kChunkFrames = 1024;
  float mono[kChunkFrames / 2 + 1];
  SyncFrame frames[4];
  size_t frames_total = length / 2;
  while (frames_total > 0)
  {
    const size_t n = frames_total < kChunkFrames ? frames_total : kChunkFrames;
    const size_t m = s.resampler.Process(data, n, mono);
    data += 2 * n;
    frames_total -= n;
    const size_t done = s.analyzer.Push(mono, m, frames, 4);
    for (size_t i = 0; i < done; ++i)
      s.sender.Send(frames[i]);
  }

  const unsigned errors = s.sender.TakeErrorCount();
  if (errors > 0)
  {
    const int64_t now = MonotonicMs();
    if (s.last_error_log_ms == 0 || now - s.last_error_log_ms >= kErrorLogIntervalMs)
    {
      s.last_error_log_ms = now;
      Log(ADDON_LOG_WARNING, "%u audio sync packet(s) not sent: %s", errors,
          std::strerror(s.sender.LastErrno()));
    }
  }
}

void WrapAudioData(const KODI_ADDON_VISUALIZATION_HDL hdl,
                   const float* audio_data,
                   size_t audio_data_length)
{
  WrappedInstance* w = FindInstance(hdl);
  auto fn = w ? w->audio_data.load(std::memory_order_acquire) : nullptr;
  if (fn)
    fn(hdl, audio_data, audio_data_length);
  if (g_state && audio_data && g_state->sending.load(std::memory_order_relaxed))
    Analyse(*g_state, audio_data, audio_data_length);
}

// ---------------------------------------------------------------------------
// Kodi to add-on calls.
// ---------------------------------------------------------------------------

ADDON_STATUS ProxyCreateInstance(const KODI_ADDON_HDL hdl, KODI_ADDON_INSTANCE_STRUCT* instance)
{
  ProxyState& s = State();
  if (!s.target_to_addon.create_instance)
    return ADDON_STATUS_NOT_IMPLEMENTED;
  const ADDON_STATUS status = s.target_to_addon.create_instance(hdl, instance);
  if (status != ADDON_STATUS_OK || !instance || !instance->info)
    return status;
  if (instance->info->type != ADDON_INSTANCE_VISUALIZATION || !instance->visualization ||
      !instance->visualization->toAddon)
    return status;

  KodiToAddonFuncTable_Visualization* t = instance->visualization->toAddon;
  WrappedInstance* slot = FindInstance(instance->hdl);
  if (!slot)
  {
    for (WrappedInstance& w : s.instances)
    {
      if (!w.hdl.load(std::memory_order_relaxed))
      {
        slot = &w;
        break;
      }
    }
  }
  if (!slot)
  {
    Log(ADDON_LOG_WARNING, "too many visualization instances; audio sync not attached");
    return status;
  }
  // Keep the target's functions; never save our own wrappers.
  if (t->audio_start != WrapAudioStart)
    slot->audio_start.store(t->audio_start, std::memory_order_relaxed);
  if (t->audio_data != WrapAudioData)
    slot->audio_data.store(t->audio_data, std::memory_order_relaxed);
  slot->hdl.store(instance->hdl, std::memory_order_release);
  t->audio_start = WrapAudioStart;
  t->audio_data = WrapAudioData;
  return status;
}

void ProxyDestroyInstance(const KODI_ADDON_HDL hdl, KODI_ADDON_INSTANCE_STRUCT* instance)
{
  ProxyState& s = State();
  if (s.target_to_addon.destroy_instance)
    s.target_to_addon.destroy_instance(hdl, instance);
  if (instance)
  {
    if (WrappedInstance* w = FindInstance(instance->hdl))
    {
      w->hdl.store(nullptr, std::memory_order_release);
      w->audio_start.store(nullptr, std::memory_order_relaxed);
      w->audio_data.store(nullptr, std::memory_order_relaxed);
    }
  }
}

void FreeState()
{
  if (!g_state)
    return;
  g_state->sending.store(false);
  g_state->sender.Close();
  UnloadTarget(g_state->target);
  delete g_state;
  g_state = nullptr;
}

void ProxyDestroy(const KODI_ADDON_HDL hdl)
{
  if (!g_state)
    return;
  if (g_state->target_to_addon.destroy)
    g_state->target_to_addon.destroy(hdl);
  Log(ADDON_LOG_DEBUG, "target %s unloaded", g_state->target.id.c_str());
  FreeState();
}

// The proxy's own settings. Kodi passes the target's handle; it's ignored.
ADDON_STATUS OnSettingChanged(const char* name)
{
  if (!g_state || !name)
    return ADDON_STATUS_OK;
  const std::string id = name;
  if (id == "enabled" || id == "destination" || id == "udp_host" || id == "udp_port")
    ApplySenderConfig(ReadSenderConfig(), true);
  // "target" applies the next time the visualization opens.
  return ADDON_STATUS_OK;
}

ADDON_STATUS ProxySettingString(const KODI_ADDON_HDL, const char* name, const char*)
{
  return OnSettingChanged(name);
}
ADDON_STATUS ProxySettingBool(const KODI_ADDON_HDL, const char* name, bool)
{
  return OnSettingChanged(name);
}
ADDON_STATUS ProxySettingInt(const KODI_ADDON_HDL, const char* name, int)
{
  return OnSettingChanged(name);
}
ADDON_STATUS ProxySettingFloat(const KODI_ADDON_HDL, const char* name, float)
{
  return OnSettingChanged(name);
}

std::string LanguageFolder()
{
  std::string lang = kodi::GetLanguage(LANG_FMT_ISO_639_1, true);
  for (char& c : lang)
  {
    if (c == '-')
      c = '_';
    else if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return lang;
}

void LoadTargetStrings(ProxyState& s)
{
  const std::string base = s.target.folder + "/resources/language/resource.language.";
  // English first, then the GUI language on top of it.
  LoadPoFile(base + "en_gb/strings.po", s.strings);
  const std::string lang = LanguageFolder();
  if (!lang.empty() && lang != "en_gb")
    LoadPoFile(base + lang + "/strings.po", s.strings);
}

ADDON_STATUS Create(AddonGlobalInterface* ifc)
{
  kodi::addon::CPrivateBase::m_interface = ifc;
  if (!ifc || !ifc->toKodi || !ifc->toKodi->kodi_addon || !ifc->toAddon)
    return ADDON_STATUS_PERMANENT_FAILURE;

  FreeState(); // a previous Create that failed half way
  g_state = new ProxyState;
  ProxyState& s = *g_state;
  s.kodi = ifc;
  s.kodi_addon = ifc->toKodi->kodi_addon;

  // 1-2. Proxy settings and the target.
  const std::string target = kodi::addon::GetSettingString("target", "");
  if (target.empty() || target == kAddonId)
  {
    Log(ADDON_LOG_ERROR, "no visualization to wrap (target '%s')", target.c_str());
    Notify(Localized(kStrNoTarget, "Choose a visualization to wrap in the add-on settings"));
    FreeState();
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  // 3-4. Locate and load its library.
  std::string error;
  if (!LoadTarget(target, s.target, error))
  {
    Log(ADDON_LOG_ERROR, "can't load %s: %s", target.c_str(), error.c_str());
    Notify(Localized(kStrTargetLoad, "Can't load the wrapped visualization") + ": " + target);
    FreeState();
    return ADDON_STATUS_PERMANENT_FAILURE;
  }
  Log(ADDON_LOG_INFO, "wrapping %s (%s)", target.c_str(), s.target.lib_path.c_str());

  // 5. API versions, with Kodi's rule.
  if (!CheckTargetVersions(s.target.get_type_version, s.target.get_type_min_version, error))
  {
    Log(ADDON_LOG_ERROR, "%s: %s", target.c_str(), error.c_str());
    Notify(Localized(kStrTargetVersion, "Wrapped visualization uses an incompatible API") +
           ": " + target);
    FreeState();
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  // Redirect context for the target's own paths, settings and strings.
  s.target_profile = "special://profile/addon_data/" + target + "/";
  s.target_user_path = kodi::vfs::TranslateSpecialProtocol(s.target_profile);
  if (!s.target_user_path.empty() && s.target_user_path.back() != '/')
    s.target_user_path += '/';
  s.settings.Open(s.target.folder + "/resources/settings.xml",
                  s.target_user_path + "settings.xml");
  LoadTargetStrings(s);

  // 6. The target's interface: Kodi's, with its own tables.
  s.target_ifc = *ifc;
  s.target_ifc.addonBase = nullptr;
  s.target_ifc.globalSingleInstance = nullptr;
  s.target_to_addon = KodiToAddonFuncTable_Addon{};
  s.target_ifc.toAddon = &s.target_to_addon;
  s.target_to_kodi = *ifc->toKodi;
  BuildRedirectTable();
  s.target_to_kodi.kodi_addon = &s.target_kodi_addon;
  s.target_ifc.toKodi = &s.target_to_kodi;

  // 7. Create the target.
  ADDON_STATUS status = s.target.create(&s.target_ifc);
  if (status == ADDON_STATUS_OK && s.target_to_addon.create)
    status = s.target_to_addon.create(s.target_ifc.firstKodiInstance, &s.target_ifc.addonBase);
  if (status != ADDON_STATUS_OK)
  {
    Log(ADDON_LOG_ERROR, "%s failed to start (status %d)", target.c_str(), status);
    if (s.target_to_addon.destroy && s.target_ifc.addonBase)
      s.target_to_addon.destroy(s.target_ifc.addonBase);
    FreeState();
    return status;
  }

  // 8. Kodi's base calls now carry the target's handle.
  ifc->addonBase = s.target_ifc.addonBase;
  ifc->globalSingleInstance = s.target_ifc.globalSingleInstance;

  // 9. Kodi's table: lifetime through the proxy, settings to the proxy.
  KodiToAddonFuncTable_Addon* t = ifc->toAddon;
  t->create = nullptr; // the target's create already ran above
  t->destroy = ProxyDestroy;
  t->create_instance = ProxyCreateInstance;
  t->destroy_instance = ProxyDestroyInstance;
  t->setting_change_string = ProxySettingString;
  t->setting_change_boolean = ProxySettingBool;
  t->setting_change_integer = ProxySettingInt;
  t->setting_change_float = ProxySettingFloat;

  // 10. Socket.
  ApplySenderConfig(ReadSenderConfig(), true);
  return ADDON_STATUS_OK;
}

} // namespace

ProxyState& State()
{
  return *g_state;
}

void Log(ADDON_LOG level, const char* fmt, ...)
{
  AddonGlobalInterface* ifc = kodi::addon::CPrivateBase::m_interface;
  if (!ifc || !ifc->toKodi || !ifc->toKodi->addon_log_msg)
    return;
  char buf[1024];
  int n = std::snprintf(buf, sizeof(buf), "proxy: ");
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, args);
  va_end(args);
  ifc->toKodi->addon_log_msg(ifc->toKodi->kodiBase, level, buf);
}

void Notify(const std::string& message)
{
  AddonGlobalInterface* ifc = kodi::addon::CPrivateBase::m_interface;
  if (!ifc || !ifc->toKodi || !ifc->toKodi->kodi || !ifc->toKodi->kodi->queue_notification)
    return;
  kodi::QueueNotification(QUEUE_ERROR, "WLED visualization proxy", message);
}

} // namespace wledproxy

extern "C" ATTR_DLL_EXPORT ADDON_STATUS ADDON_Create(KODI_HANDLE addonInterface)
{
  return wledproxy::Create(static_cast<AddonGlobalInterface*>(addonInterface));
}

extern "C" ATTR_DLL_EXPORT const char* ADDON_GetTypeVersion(int type)
{
  return kodi::addon::GetTypeVersion(type);
}

extern "C" ATTR_DLL_EXPORT const char* ADDON_GetTypeMinVersion(int type)
{
  return kodi::addon::GetTypeMinVersion(type);
}
