#pragma once

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wledproxy
{

// Settings store for the wrapped (target) add-on.
//
// Definitions and defaults come from the target's resources/settings.xml, in
// the version 1 format (<setting id type><default>) or the legacy format
// (<setting id type default="...">). Values come from the target's
// addon_data settings.xml (versions 0, 1 and 2), cached and reloaded when the
// file's modification time changes. Writes save the whole file in version 2
// format. All methods are thread-safe.
class TargetSettings
{
public:
  bool Open(const std::string& definitions_path, const std::string& values_path);

  bool GetBool(const std::string& id, bool& value);
  bool GetInt(const std::string& id, int& value);
  bool GetFloat(const std::string& id, float& value);
  bool GetString(const std::string& id, std::string& value);

  bool SetBool(const std::string& id, bool value);
  bool SetInt(const std::string& id, int value);
  bool SetFloat(const std::string& id, float value);
  bool SetString(const std::string& id, const std::string& value);

  bool IsUsingDefault(const std::string& id);

  // Number of settings defined by the target.
  size_t DefinitionCount() const;

private:
  struct Definition
  {
    std::string type;
    std::string default_value;
  };

  void LoadDefinitions();
  void ReloadValuesIfChanged();
  bool Lookup(const std::string& id, std::string& value);
  bool Store(const std::string& id, const std::string& value);
  bool Save();

  mutable std::mutex m_mutex;
  std::string m_definitions_path;
  std::string m_values_path;
  std::map<std::string, Definition> m_definitions;
  std::vector<std::string> m_order; // definition order, for writing
  std::map<std::string, std::string> m_values;
  bool m_have_mtime = false;
  struct timespec m_mtime = {};
  off_t m_size = 0;
};

// Formats a float the way Kodi stores it: shortest round-trip form, C locale.
std::string FormatFloat(float value);

} // namespace wledproxy
