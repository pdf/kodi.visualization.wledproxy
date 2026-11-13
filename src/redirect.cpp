// The kodi_addon callbacks the target sees.
//
// Kodi resolves paths, settings and add-on strings by the calling add-on's
// identity, which for the target is the proxy's. These replacements answer
// for the target instead; everything else is Kodi's own function.
// Returned strings are malloc'd: the target releases them through Kodi's
// free_string, which calls free().

#include "proxy.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace wledproxy
{
namespace
{

constexpr long kAddonStringBase = 30000;

char* Dup(const std::string& s)
{
  return strdup(s.c_str());
}

char* get_addon_path(const KODI_ADDON_BACKEND_HDL)
{
  return Dup(State().target.folder);
}

char* get_user_path(const KODI_ADDON_BACKEND_HDL)
{
  return Dup(State().target_user_path);
}

char* get_localized_string(const KODI_ADDON_BACKEND_HDL hdl, long label_id)
{
  ProxyState& s = State();
  if (label_id < kAddonStringBase)
    return s.kodi_addon->get_localized_string(hdl, label_id);
  auto it = s.strings.find(static_cast<uint32_t>(label_id));
  return Dup(it != s.strings.end() ? it->second : std::string());
}

bool open_settings_dialog(const KODI_ADDON_BACKEND_HDL)
{
  return false;
}

bool is_setting_using_default(const KODI_ADDON_BACKEND_HDL, const char* id)
{
  return id && State().settings.IsUsingDefault(id);
}

bool get_setting_bool(const KODI_ADDON_BACKEND_HDL, const char* id, bool* value)
{
  return id && value && State().settings.GetBool(id, *value);
}

bool get_setting_int(const KODI_ADDON_BACKEND_HDL, const char* id, int* value)
{
  return id && value && State().settings.GetInt(id, *value);
}

bool get_setting_float(const KODI_ADDON_BACKEND_HDL, const char* id, float* value)
{
  return id && value && State().settings.GetFloat(id, *value);
}

bool get_setting_string(const KODI_ADDON_BACKEND_HDL, const char* id, char** value)
{
  if (!id || !value)
    return false;
  std::string v;
  if (!State().settings.GetString(id, v))
    return false;
  *value = Dup(v);
  return true;
}

bool set_setting_bool(const KODI_ADDON_BACKEND_HDL, const char* id, bool value)
{
  return id && State().settings.SetBool(id, value);
}

bool set_setting_int(const KODI_ADDON_BACKEND_HDL, const char* id, int value)
{
  return id && State().settings.SetInt(id, value);
}

bool set_setting_float(const KODI_ADDON_BACKEND_HDL, const char* id, float value)
{
  return id && State().settings.SetFloat(id, value);
}

bool set_setting_string(const KODI_ADDON_BACKEND_HDL, const char* id, const char* value)
{
  return id && State().settings.SetString(id, value ? value : "");
}

char* get_addon_info(const KODI_ADDON_BACKEND_HDL hdl, const char* id)
{
  ProxyState& s = State();
  if (id)
  {
    if (strcasecmp(id, "id") == 0)
      return Dup(s.target.id);
    if (strcasecmp(id, "path") == 0)
      return Dup(s.target.folder);
    if (strcasecmp(id, "profile") == 0)
      return Dup(s.target_profile);
  }
  return s.kodi_addon->get_addon_info(hdl, id);
}

} // namespace

void BuildRedirectTable()
{
  ProxyState& s = State();
  AddonToKodiFuncTable_kodi_addon& t = s.target_kodi_addon;
  t = *s.kodi_addon;
  t.get_addon_path = get_addon_path;
  t.get_user_path = get_user_path;
  t.get_localized_string = get_localized_string;
  t.open_settings_dialog = open_settings_dialog;
  t.is_setting_using_default = is_setting_using_default;
  t.get_setting_bool = get_setting_bool;
  t.get_setting_int = get_setting_int;
  t.get_setting_float = get_setting_float;
  t.get_setting_string = get_setting_string;
  t.set_setting_bool = set_setting_bool;
  t.set_setting_int = set_setting_int;
  t.set_setting_float = set_setting_float;
  t.set_setting_string = set_setting_string;
  t.get_addon_info = get_addon_info;
}

} // namespace wledproxy
