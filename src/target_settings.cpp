#include "target_settings.h"

#include <pugixml.hpp>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wledproxy
{
namespace
{

bool ParseBool(const std::string& s, bool& out)
{
  if (s == "true" || s == "1")
    out = true;
  else if (s == "false" || s == "0")
    out = false;
  else
    return false;
  return true;
}

bool ParseInt(const std::string& s, int& out)
{
  const char* b = s.data();
  const char* e = b + s.size();
  while (b < e && (*b == ' ' || *b == '\t'))
    ++b;
  if (b < e && *b == '+')
    ++b;
  auto r = std::from_chars(b, e, out);
  if (r.ec == std::errc() && r.ptr == e)
    return true;
  // Kodi writes integer sliders and numbers without a fraction, but accept
  // "3.0" from hand-edited files.
  double d = 0;
  auto rd = std::from_chars(b, e, d);
  if (rd.ec != std::errc() || rd.ptr != e)
    return false;
  out = static_cast<int>(d);
  return true;
}

bool ParseFloat(const std::string& s, float& out)
{
  const char* b = s.data();
  const char* e = b + s.size();
  while (b < e && (*b == ' ' || *b == '\t'))
    ++b;
  if (b < e && *b == '+')
    ++b;
  auto r = std::from_chars(b, e, out);
  return r.ec == std::errc() && r.ptr == e;
}

bool IsStructural(const std::string& type)
{
  // Legacy layout elements and buttons carry no value.
  return type == "lsep" || type == "sep" || type == "action";
}

} // namespace

std::string FormatFloat(float value)
{
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), value);
  return std::string(buf, r.ptr);
}

bool TargetSettings::Open(const std::string& definitions_path, const std::string& values_path)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_definitions_path = definitions_path;
  m_values_path = values_path;
  m_definitions.clear();
  m_order.clear();
  m_values.clear();
  m_have_mtime = false;
  LoadDefinitions();
  ReloadValuesIfChanged();
  return true;
}

size_t TargetSettings::DefinitionCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_definitions.size();
}

void TargetSettings::LoadDefinitions()
{
  pugi::xml_document doc;
  if (!doc.load_file(m_definitions_path.c_str()))
    return;

  // Both formats: every <setting id=...> anywhere below <settings>.
  for (const pugi::xpath_node& xn : doc.select_nodes("/settings//setting[@id]"))
  {
    const pugi::xml_node node = xn.node();
    Definition def;
    def.type = node.attribute("type").as_string();
    if (IsStructural(def.type))
      continue;
    if (pugi::xml_attribute legacy = node.attribute("default"))
      def.default_value = legacy.as_string();
    else if (pugi::xml_node d = node.child("default"))
      def.default_value = d.text().as_string();
    const std::string id = node.attribute("id").as_string();
    if (id.empty())
      continue;
    if (m_definitions.emplace(id, def).second)
      m_order.push_back(id);
  }
}

void TargetSettings::ReloadValuesIfChanged()
{
  struct stat st = {};
  if (stat(m_values_path.c_str(), &st) != 0)
  {
    if (m_have_mtime)
      m_values.clear();
    m_have_mtime = false;
    return;
  }
  if (m_have_mtime && st.st_mtim.tv_sec == m_mtime.tv_sec &&
      st.st_mtim.tv_nsec == m_mtime.tv_nsec && st.st_size == m_size)
    return;

  m_have_mtime = true;
  m_mtime = st.st_mtim;
  m_size = st.st_size;
  m_values.clear();

  pugi::xml_document doc;
  if (!doc.load_file(m_values_path.c_str()))
    return;
  const pugi::xml_node root = doc.child("settings");
  const int version = root.attribute("version").as_int(0);
  for (pugi::xml_node s = root.child("setting"); s; s = s.next_sibling("setting"))
  {
    const std::string id = s.attribute("id").as_string();
    if (id.empty())
      continue;
    if (version >= 1)
      m_values[id] = s.text().as_string();
    else
      m_values[id] = s.attribute("value").as_string();
  }
}

bool TargetSettings::Lookup(const std::string& id, std::string& value)
{
  ReloadValuesIfChanged();
  auto v = m_values.find(id);
  if (v != m_values.end())
  {
    value = v->second;
    return true;
  }
  auto d = m_definitions.find(id);
  if (d != m_definitions.end())
  {
    value = d->second.default_value;
    return true;
  }
  return false;
}

bool TargetSettings::GetBool(const std::string& id, bool& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string s;
  return Lookup(id, s) && ParseBool(s, value);
}

bool TargetSettings::GetInt(const std::string& id, int& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string s;
  return Lookup(id, s) && ParseInt(s, value);
}

bool TargetSettings::GetFloat(const std::string& id, float& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string s;
  return Lookup(id, s) && ParseFloat(s, value);
}

bool TargetSettings::GetString(const std::string& id, std::string& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return Lookup(id, value);
}

bool TargetSettings::IsUsingDefault(const std::string& id)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ReloadValuesIfChanged();
  auto d = m_definitions.find(id);
  auto v = m_values.find(id);
  if (v == m_values.end())
    return true;
  return d != m_definitions.end() && v->second == d->second.default_value;
}

bool TargetSettings::SetBool(const std::string& id, bool value)
{
  return SetString(id, value ? "true" : "false");
}

bool TargetSettings::SetInt(const std::string& id, int value)
{
  return SetString(id, std::to_string(value));
}

bool TargetSettings::SetFloat(const std::string& id, float value)
{
  return SetString(id, FormatFloat(value));
}

bool TargetSettings::SetString(const std::string& id, const std::string& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return Store(id, value);
}

bool TargetSettings::Store(const std::string& id, const std::string& value)
{
  ReloadValuesIfChanged();
  if (m_definitions.find(id) == m_definitions.end() && m_values.find(id) == m_values.end())
    return false; // Kodi refuses unknown settings too
  m_values[id] = value;
  return Save();
}

bool TargetSettings::Save()
{
  pugi::xml_document doc;
  pugi::xml_node root = doc.append_child("settings");
  root.append_attribute("version") = "2";

  auto write = [&](const std::string& id, const std::string& value) {
    pugi::xml_node s = root.append_child("setting");
    s.append_attribute("id") = id.c_str();
    auto d = m_definitions.find(id);
    if (d != m_definitions.end() && d->second.default_value == value)
      s.append_attribute("default") = "true";
    if (!value.empty())
      s.text() = value.c_str();
  };

  for (const std::string& id : m_order)
  {
    auto v = m_values.find(id);
    write(id, v != m_values.end() ? v->second : m_definitions[id].default_value);
  }
  // Values the definitions don't know (kept as Kodi keeps them).
  for (const auto& [id, value] : m_values)
    if (m_definitions.find(id) == m_definitions.end())
      write(id, value);

  // Create the folder if this is the target's first saved setting.
  const size_t slash = m_values_path.rfind('/');
  if (slash != std::string::npos && slash > 0)
    mkdir(m_values_path.substr(0, slash).c_str(), 0755);

  const std::string tmp = m_values_path + ".tmp";
  if (!doc.save_file(tmp.c_str(), "    ", pugi::format_default, pugi::encoding_utf8))
    return false;
  if (std::rename(tmp.c_str(), m_values_path.c_str()) != 0)
  {
    std::remove(tmp.c_str());
    return false;
  }

  struct stat st = {};
  if (stat(m_values_path.c_str(), &st) == 0)
  {
    m_have_mtime = true;
    m_mtime = st.st_mtim;
    m_size = st.st_size;
  }
  return true;
}

} // namespace wledproxy
