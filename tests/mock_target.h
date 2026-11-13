#pragma once

// State recorded by the mock target library, read by the harness through
// the exported mock_state().

#include <kodi/c-api/addon_base.h>

#include <cstddef>
#include <string>

struct MockState
{
  // Handles the mock hands out.
  int base_tag = 0;
  int instance_tag = 0;

  // What ADDON_Create saw.
  int create_calls = 0;
  void* to_kodi_base = nullptr; // kodiBase in the table the mock got
  void* first_instance = nullptr;
  std::string addon_path;
  std::string user_path;
  std::string lib_path;
  std::string temp_path;
  std::string string_30000;
  std::string string_30001;
  std::string string_core;
  std::string info_id;
  std::string info_path;
  std::string info_profile;
  std::string info_name;
  bool open_dialog = true;
  bool got_enabled = false;
  bool enabled = false;
  bool got_count = false;
  int count = 0;
  bool got_ratio = false;
  float ratio = 0;
  bool got_name = false;
  std::string name;
  bool set_name_ok = false;
  bool count_default = false;
  bool got_unknown = true;

  // Calls from Kodi through the proxy.
  int destroy_calls = 0;
  void* destroy_hdl = nullptr;
  // Set by the harness: destroy also records here, since the library is
  // unloaded right after.
  int* destroy_calls_out = nullptr;
  void** destroy_hdl_out = nullptr;
  int create_instance_calls = 0;
  void* create_instance_hdl = nullptr;
  int destroy_instance_calls = 0;
  void* destroy_instance_hdl = nullptr;
  int setting_calls = 0;
  int audio_start_calls = 0;
  void* audio_start_hdl = nullptr;
  int audio_start_channels = 0;
  int audio_data_calls = 0;
  void* audio_data_hdl = nullptr;
  size_t audio_data_floats = 0;
  const float* last_audio_ptr = nullptr;
  int render_calls = 0;
};

using MockStateFn = MockState* (*)();
