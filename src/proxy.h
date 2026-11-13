#pragma once

// Shared state of the proxy add-on. One copy per loaded library; Kodi loads
// the library once per add-on and runs ADDON_Create each time it does.

#include "resample.h"
#include "sender.h"
#include "target_settings.h"
#include "wled_analysis.h"

#include <kodi/c-api/addon-instance/visualization.h>
#include <kodi/c-api/addon_base.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace wledproxy
{

// The wrapped add-on's library.
struct TargetLibrary
{
  std::string id;
  std::string folder;   // real path of the add-on folder, no trailing slash
  std::string lib_path; // real path of the shared library
  void* handle = nullptr;
  ADDON_STATUS (*create)(void* addon_interface) = nullptr;
  const char* (*get_type_version)(int type) = nullptr;
  const char* (*get_type_min_version)(int type) = nullptr; // optional
};

// One visualization instance whose audio callbacks are wrapped.
struct WrappedInstance
{
  std::atomic<KODI_ADDON_INSTANCE_HDL> hdl{nullptr};
  std::atomic<PFN_KODI_ADDON_VISUALIZATION_AUDIO_START_V1> audio_start{nullptr};
  std::atomic<PFN_KODI_ADDON_VISUALIZATION_AUDIO_DATA_V1> audio_data{nullptr};
};

struct ProxyState
{
  // Kodi's struct for this add-on (also kodi::addon::CPrivateBase::m_interface).
  AddonGlobalInterface* kodi = nullptr;
  // Kodi's own kodi_addon callbacks, kept for forwarding.
  AddonToKodiFuncTable_kodi_addon* kodi_addon = nullptr;

  TargetLibrary target;

  // The interface the target sees: a copy of Kodi's with its own tables.
  AddonGlobalInterface target_ifc = {};
  KodiToAddonFuncTable_Addon target_to_addon = {};
  AddonToKodiFuncTable_Addon target_to_kodi = {};
  AddonToKodiFuncTable_kodi_addon target_kodi_addon = {};
  bool target_created = false;

  // Redirect context.
  std::string target_user_path; // translated, trailing slash
  std::string target_profile;   // special://profile/addon_data/<id>/
  TargetSettings settings;
  std::unordered_map<uint32_t, std::string> strings;

  static constexpr int kMaxInstances = 4;
  WrappedInstance instances[kMaxInstances];

  // Audio path.
  Resampler resampler{kInputScale};
  WledAnalyzer analyzer;
  Sender sender;
  SenderConfig sender_config;
  bool sender_configured = false;
  std::atomic<bool> reset_analysis{true};
  std::atomic<bool> sending{false};
  int64_t last_error_log_ms = 0;
};

ProxyState& State();

// redirect.cpp: fills State().target_kodi_addon from Kodi's table.
void BuildRedirectTable();

// target.cpp
bool LoadTarget(const std::string& id, TargetLibrary& lib, std::string& error);
void UnloadTarget(TargetLibrary& lib);

// Logging and notification helpers (entry.cpp).
void Log(ADDON_LOG level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Notify(const std::string& message);

} // namespace wledproxy
