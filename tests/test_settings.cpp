// Target settings store and strings.po reader.

#include "po.h"
#include "target_settings.h"
#include "test.h"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace wledproxy;

namespace
{

std::string Fixture(const char* name)
{
  return std::string(WLEDPROXY_FIXTURES) + "/" + name;
}

std::string TempDir()
{
  mkdir(WLEDPROXY_TEST_TMP, 0755);
  std::string tmpl = std::string(WLEDPROXY_TEST_TMP) + "/settings-XXXXXX";
  const char* d = mkdtemp(tmpl.data());
  return d ? d : WLEDPROXY_TEST_TMP;
}

void CopyFile(const std::string& from, const std::string& to)
{
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary);
  out << in.rdbuf();
}

std::string ReadFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A values file in a fresh folder: <tmp>/addon_data/settings.xml.
std::string ValuesCopy(const char* fixture)
{
  const std::string dir = TempDir() + "/addon_data";
  mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/settings.xml";
  if (fixture)
    CopyFile(Fixture(fixture), path);
  return path;
}

} // namespace

TEST(definitions_v1_defaults)
{
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), "/nonexistent/settings.xml");
  CHECK(s.DefinitionCount() == 5); // the action button carries no value
  bool b = false;
  int i = 0;
  float f = 0;
  std::string str;
  CHECK(s.GetBool("enabled", b) && b);
  CHECK(s.GetInt("count", i) && i == 42);
  CHECK(s.GetFloat("ratio", f) && f == 0.5f);
  CHECK(s.GetString("name", str) && str == "Tom & Jerry");
  CHECK(s.GetString("empty", str) && str.empty());
  CHECK(!s.GetString("missing", str));
  CHECK(!s.GetString("button", str));
  CHECK(s.IsUsingDefault("count"));
}

TEST(definitions_legacy_defaults)
{
  TargetSettings s;
  s.Open(Fixture("defs_legacy.xml"), "/nonexistent/settings.xml");
  CHECK(s.DefinitionCount() == 4);
  bool b = false;
  int i = 0;
  float f = 0;
  std::string str;
  CHECK(s.GetBool("enabled", b) && b);
  CHECK(s.GetInt("count", i) && i == 2);
  CHECK(s.GetFloat("ratio", f) && f == 0.25f);
  CHECK(s.GetString("name", str) && str == "it's");
}

TEST(values_v0)
{
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), Fixture("values_v0.xml"));
  bool b = true;
  int i = 0;
  float f = 0;
  std::string str;
  CHECK(s.GetBool("enabled", b) && !b);
  CHECK(s.GetInt("count", i) && i == 7);
  CHECK(s.GetFloat("ratio", f) && f == 0.5f); // missing: definition default
  CHECK(s.GetString("name", str) && str == "a <b> c");
  CHECK(!s.IsUsingDefault("count"));
  CHECK(s.IsUsingDefault("ratio"));
}

TEST(values_v1)
{
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), Fixture("values_v1.xml"));
  bool b = true;
  int i = 0;
  float f = 0;
  CHECK(s.GetBool("enabled", b) && !b);
  CHECK(s.GetInt("count", i) && i == 42);
  CHECK(s.GetFloat("ratio", f) && f == 0.75f);
  CHECK(s.IsUsingDefault("count"));
}

TEST(values_v2_and_entities)
{
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), Fixture("values_v2.xml"));
  int i = 0;
  std::string str;
  CHECK(s.GetInt("count", i) && i == 13);
  CHECK(s.GetString("name", str) && str == "caf\xc3\xa9 \"x\"");
  CHECK(s.GetString("empty", str) && str.empty());
  // Undefined keys present in the file are readable (projectM's last preset).
  CHECK(s.GetString("last_preset", str) && str == "Some Preset.milk");
}

TEST(missing_values_file)
{
  TargetSettings s;
  const std::string path = ValuesCopy(nullptr);
  s.Open(Fixture("defs_v1.xml"), path);
  int i = 0;
  CHECK(s.GetInt("count", i) && i == 42);
  CHECK(s.IsUsingDefault("count"));
}

TEST(write_saves_version_2)
{
  const std::string path = ValuesCopy("values_v0.xml");
  {
    TargetSettings s;
    s.Open(Fixture("defs_v1.xml"), path);
    CHECK(s.SetFloat("ratio", 0.125f));
    CHECK(s.SetString("name", "x & y"));
    CHECK(s.SetInt("count", 42));
    CHECK(!s.SetInt("unknown", 1)); // not defined, not in the file
  }
  const std::string text = ReadFile(path);
  CHECK(text.find("version=\"2\"") != std::string::npos);
  CHECK(text.find("<setting id=\"ratio\">0.125</setting>") != std::string::npos);
  CHECK(text.find("<setting id=\"count\" default=\"true\">42</setting>") != std::string::npos);
  CHECK(text.find("<setting id=\"enabled\">false</setting>") != std::string::npos);
  CHECK(text.find("x &amp; y") != std::string::npos);
  CHECK(text.find("<setting id=\"empty\" default=\"true\" />") != std::string::npos);
  CHECK(access((path + ".tmp").c_str(), F_OK) != 0);

  // And it reads back.
  TargetSettings s2;
  s2.Open(Fixture("defs_v1.xml"), path);
  float f = 0;
  std::string str;
  CHECK(s2.GetFloat("ratio", f) && f == 0.125f);
  CHECK(s2.GetString("name", str) && str == "x & y");
}

TEST(write_creates_folder)
{
  const std::string dir = TempDir() + "/new_addon_data";
  const std::string path = dir + "/settings.xml";
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), path);
  CHECK(s.SetBool("enabled", false));
  bool b = true;
  CHECK(s.GetBool("enabled", b) && !b);
  CHECK(access(path.c_str(), F_OK) == 0);
}

TEST(reloads_when_file_changes)
{
  const std::string path = ValuesCopy("values_v1.xml");
  TargetSettings s;
  s.Open(Fixture("defs_v1.xml"), path);
  float f = 0;
  CHECK(s.GetFloat("ratio", f) && f == 0.75f);

  // Another writer (Kodi's settings dialog) replaces the file.
  {
    std::ofstream out(path + ".new");
    out << "<settings version=\"2\"><setting id=\"ratio\">0.875</setting></settings>\n";
  }
  // Make sure the modification time differs even on coarse clocks.
  struct timespec times[2] = {{0, UTIME_OMIT}, {1, 0}};
  utimensat(AT_FDCWD, (path + ".new").c_str(), times, 0);
  CHECK(std::rename((path + ".new").c_str(), path.c_str()) == 0);
  CHECK(s.GetFloat("ratio", f) && f == 0.875f);
}

TEST(float_format_is_c_locale)
{
  CHECK(FormatFloat(0.5f) == "0.5");
  CHECK(FormatFloat(1.0f) == "1");
  CHECK(FormatFloat(-2.25f) == "-2.25");
  CHECK(FormatFloat(0.1f) == "0.1");
}

TEST(po_reader)
{
  std::unordered_map<uint32_t, std::string> m;
  CHECK(LoadPoFile(Fixture("strings.po"), m));
  CHECK(m.size() == 5);
  CHECK(m[30000] == "General");
  CHECK(m[30001] == "Quoted \"word\" and \\ backslash");
  CHECK(m[30002] == "Multi-line text");
  CHECK(m[30003] == "Translated");
  CHECK(m[30004] == "Line\nbreak");
  CHECK(!LoadPoFile("/nonexistent.po", m));
}

TEST(po_reader_crlf)
{
  std::unordered_map<uint32_t, std::string> m;
  ParsePo("msgctxt \"#30010\"\r\nmsgid \"Windows\"\r\nmsgstr \"\"\r\n", m);
  CHECK(m.size() == 1 && m[30010] == "Windows");
}

TEST_MAIN()
