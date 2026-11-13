// A visualization add-on library written against the C ABI, recording what
// it sees. Loaded by the proxy in the harness.

#include "mock_target.h"

#include <kodi/c-api/addon-instance/visualization.h>
#include <kodi/versions.h>

#include <cstdlib>

#ifndef MOCK_VIS_VERSION
#define MOCK_VIS_VERSION ADDON_INSTANCE_VERSION_VISUALIZATION
#endif

namespace
{

MockState g;
AddonGlobalInterface* g_ifc = nullptr;

std::string Take(char* s)
{
  std::string r = s ? s : "(null)";
  if (s)
    g_ifc->toKodi->free_string(g_ifc->toKodi->kodiBase, s);
  return r;
}

void Destroy(const KODI_ADDON_HDL hdl)
{
  ++g.destroy_calls;
  g.destroy_hdl = hdl;
  if (g.destroy_calls_out)
    ++*g.destroy_calls_out;
  if (g.destroy_hdl_out)
    *g.destroy_hdl_out = hdl;
}

bool AudioStart(const KODI_ADDON_VISUALIZATION_HDL hdl, int channels, int, int)
{
  ++g.audio_start_calls;
  g.audio_start_hdl = hdl;
  g.audio_start_channels = channels;
  return true;
}

void AudioData(const KODI_ADDON_VISUALIZATION_HDL hdl, const float* data, size_t length)
{
  ++g.audio_data_calls;
  g.audio_data_hdl = hdl;
  g.audio_data_floats += length;
  g.last_audio_ptr = data;
}

void Render(const KODI_ADDON_VISUALIZATION_HDL)
{
  ++g.render_calls;
}

ADDON_STATUS CreateInstance(const KODI_ADDON_HDL hdl, KODI_ADDON_INSTANCE_STRUCT* instance)
{
  ++g.create_instance_calls;
  g.create_instance_hdl = hdl;
  if (instance->info->type != ADDON_INSTANCE_VISUALIZATION)
    return ADDON_STATUS_NOT_IMPLEMENTED;
  instance->hdl = &g.instance_tag;
  KodiToAddonFuncTable_Visualization* t = instance->visualization->toAddon;
  t->audio_start = AudioStart;
  t->audio_data = AudioData;
  t->render = Render;
  return ADDON_STATUS_OK;
}

void DestroyInstance(const KODI_ADDON_HDL hdl, KODI_ADDON_INSTANCE_STRUCT*)
{
  ++g.destroy_instance_calls;
  g.destroy_instance_hdl = hdl;
}

ADDON_STATUS SettingString(const KODI_ADDON_HDL, const char*, const char*)
{
  ++g.setting_calls;
  return ADDON_STATUS_OK;
}

} // namespace

extern "C" __attribute__((visibility("default"))) MockState* mock_state()
{
  return &g;
}

extern "C" __attribute__((visibility("default"))) ADDON_STATUS ADDON_Create(void* ifc_ptr)
{
  g = MockState{};
  g_ifc = static_cast<AddonGlobalInterface*>(ifc_ptr);
  ++g.create_calls;
  AddonToKodiFuncTable_Addon* k = g_ifc->toKodi;
  void* base = k->kodiBase;
  g.to_kodi_base = base;
  g.first_instance = g_ifc->firstKodiInstance;

  AddonToKodiFuncTable_kodi_addon* a = k->kodi_addon;
  g.addon_path = Take(a->get_addon_path(base));
  g.user_path = Take(a->get_user_path(base));
  g.lib_path = Take(a->get_lib_path(base));
  g.temp_path = Take(a->get_temp_path(base));
  g.string_30000 = Take(a->get_localized_string(base, 30000));
  g.string_30001 = Take(a->get_localized_string(base, 30001));
  g.string_core = Take(a->get_localized_string(base, 186));
  g.info_id = Take(a->get_addon_info(base, "id"));
  g.info_path = Take(a->get_addon_info(base, "path"));
  g.info_profile = Take(a->get_addon_info(base, "PROFILE"));
  g.info_name = Take(a->get_addon_info(base, "name"));
  g.open_dialog = a->open_settings_dialog(base);
  g.got_enabled = a->get_setting_bool(base, "enabled", &g.enabled);
  g.got_count = a->get_setting_int(base, "count", &g.count);
  g.got_ratio = a->get_setting_float(base, "ratio", &g.ratio);
  char* name = nullptr;
  g.got_name = a->get_setting_string(base, "name", &name);
  g.name = Take(name);
  g.set_name_ok = a->set_setting_string(base, "name", "written by mock");
  g.count_default = a->is_setting_using_default(base, "count");
  int dummy = 0;
  g.got_unknown = a->get_setting_int(base, "no_such_setting", &dummy);

  g_ifc->addonBase = &g.base_tag;
  g_ifc->toAddon->destroy = Destroy;
  g_ifc->toAddon->create_instance = CreateInstance;
  g_ifc->toAddon->destroy_instance = DestroyInstance;
  g_ifc->toAddon->setting_change_string = SettingString;
  return ADDON_STATUS_OK;
}

extern "C" __attribute__((visibility("default"))) const char* ADDON_GetTypeVersion(int type)
{
  switch (type)
  {
    case ADDON_GLOBAL_MAIN:
      return ADDON_GLOBAL_VERSION_MAIN;
    case ADDON_GLOBAL_GENERAL:
      return ADDON_GLOBAL_VERSION_GENERAL;
    case ADDON_INSTANCE_VISUALIZATION:
      return MOCK_VIS_VERSION;
    default:
      return nullptr;
  }
}

extern "C" __attribute__((visibility("default"))) const char* ADDON_GetTypeMinVersion(int type)
{
  return ADDON_GetTypeVersion(type);
}
