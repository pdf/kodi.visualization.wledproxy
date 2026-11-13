// Loader harness: plays Kodi's part against the real proxy library.
//
// Fills an AddonGlobalInterface with recording stubs, loads the proxy with
// dlopen (RTLD_LAZY, as Kodi's SoLoader does), and drives ADDON_Create,
// create_instance, audio_start, audio_data, destroy_instance and destroy
// against a mock target library installed in a fake Kodi tree.

#include "mock_target.h"
#include "test.h"

#include <kodi/c-api/addon-instance/visualization.h>
#include <kodi/c-api/addon_base.h>
#include <kodi/c-api/filesystem.h>
#include <kodi/c-api/general.h>
#include <kodi/versions.h>

#include <arpa/inet.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Fake Kodi tree.
// ---------------------------------------------------------------------------

std::string g_root;

void MkdirP(const std::string& path)
{
  std::string cur;
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/'))
  {
    if (part.empty())
    {
      cur = "/";
      continue;
    }
    cur += (cur.empty() || cur.back() == '/') ? part : "/" + part;
    mkdir(cur.c_str(), 0755);
  }
}

void CopyFile(const std::string& from, const std::string& to)
{
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary);
  out << in.rdbuf();
}

void WriteFile(const std::string& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string ReadFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string AddonXml(const std::string& id, const std::string& lib)
{
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<addon id=\"" + id +
         "\" version=\"1.0.0\" name=\"Mock\">\n  <extension point=\"xbmc.player.musicviz\" "
         "library_linux=\"" +
         lib + "\"/>\n</addon>\n";
}

// Installs a mock target. system=true puts addon.xml under xbmc/addons and the
// library under xbmcbin/addons, like a distribution's system add-on.
void InstallMock(const std::string& id, const std::string& so, bool system)
{
  const std::string folder = g_root + (system ? "/xbmc/addons/" : "/home/addons/") + id;
  const std::string libdir = system ? g_root + "/xbmcbin/addons/" + id : folder;
  MkdirP(folder + "/resources/language/resource.language.en_gb");
  MkdirP(libdir);
  WriteFile(folder + "/addon.xml", AddonXml(id, "mock.so"));
  CopyFile(so, libdir + "/mock.so");
  CopyFile(std::string(WLEDPROXY_FIXTURES) + "/defs_v1.xml", folder + "/resources/settings.xml");
  CopyFile(std::string(WLEDPROXY_FIXTURES) + "/strings.po",
           folder + "/resources/language/resource.language.en_gb/strings.po");
  const std::string data = g_root + "/home/userdata/addon_data/" + id;
  MkdirP(data);
  CopyFile(std::string(WLEDPROXY_FIXTURES) + "/values_v2.xml", data + "/settings.xml");
}

std::string Translate(const std::string& in)
{
  static const std::pair<const char*, const char*> kMap[] = {
      {"special://home/", "/home/"},
      {"special://xbmc/", "/xbmc/"},
      {"special://xbmcbin/", "/xbmcbin/"},
      {"special://profile/", "/home/userdata/"},
  };
  if (in.rfind("special://xbmcaltbinaddons/", 0) == 0)
    return "";
  for (const auto& [from, to] : kMap)
    if (in.rfind(from, 0) == 0)
      return g_root + to + in.substr(std::strlen(from));
  return in;
}

// ---------------------------------------------------------------------------
// Kodi stubs.
// ---------------------------------------------------------------------------

int g_kodi_base_tag;
std::map<std::string, std::string> g_settings; // the proxy's own settings
std::map<std::string, int> g_calls;
std::vector<std::string> g_log;
std::vector<std::string> g_notifications;

void* KodiBase()
{
  return &g_kodi_base_tag;
}

bool CheckBase(const void* hdl)
{
  if (hdl == KodiBase())
    return true;
  ++g_calls["wrong_base"];
  return false;
}

void free_string(const KODI_ADDON_BACKEND_HDL, char* str)
{
  ++g_calls["free_string"];
  free(str);
}

void free_string_array(const KODI_ADDON_BACKEND_HDL, char** arr, int n)
{
  for (int i = 0; i < n; ++i)
    free(arr[i]);
  free(arr);
}

void addon_log_msg(const KODI_ADDON_BACKEND_HDL hdl, const int level, const char* msg)
{
  CheckBase(hdl);
  g_log.push_back(msg);
  if (std::getenv("HARNESS_VERBOSE"))
    std::printf("    log[%d] %s\n", level, msg);
}

char* get_addon_path(const KODI_ADDON_BACKEND_HDL hdl)
{
  CheckBase(hdl);
  ++g_calls["get_addon_path"];
  return strdup((g_root + "/home/addons/visualization.wledproxy").c_str());
}

char* get_lib_path(const KODI_ADDON_BACKEND_HDL hdl)
{
  CheckBase(hdl);
  ++g_calls["get_lib_path"];
  return strdup("kodi-lib-path");
}

char* get_user_path(const KODI_ADDON_BACKEND_HDL hdl)
{
  CheckBase(hdl);
  ++g_calls["get_user_path"];
  return strdup((g_root + "/home/userdata/addon_data/visualization.wledproxy/").c_str());
}

char* get_temp_path(const KODI_ADDON_BACKEND_HDL hdl)
{
  CheckBase(hdl);
  ++g_calls["get_temp_path"];
  return strdup("kodi-temp-path");
}

char* get_localized_string(const KODI_ADDON_BACKEND_HDL hdl, long id)
{
  CheckBase(hdl);
  ++g_calls["get_localized_string"];
  return strdup(("kodi:" + std::to_string(id)).c_str());
}

bool open_settings_dialog(const KODI_ADDON_BACKEND_HDL hdl)
{
  CheckBase(hdl);
  ++g_calls["open_settings_dialog"];
  return true;
}

bool is_setting_using_default(const KODI_ADDON_BACKEND_HDL hdl, const char*)
{
  CheckBase(hdl);
  ++g_calls["is_setting_using_default"];
  return false;
}

bool get_setting_bool(const KODI_ADDON_BACKEND_HDL hdl, const char* id, bool* v)
{
  CheckBase(hdl);
  ++g_calls["get_setting_bool"];
  auto it = g_settings.find(id);
  if (it == g_settings.end())
    return false;
  *v = it->second == "true";
  return true;
}

bool get_setting_int(const KODI_ADDON_BACKEND_HDL hdl, const char* id, int* v)
{
  CheckBase(hdl);
  ++g_calls["get_setting_int"];
  auto it = g_settings.find(id);
  if (it == g_settings.end())
    return false;
  *v = std::atoi(it->second.c_str());
  return true;
}

bool get_setting_float(const KODI_ADDON_BACKEND_HDL hdl, const char* id, float* v)
{
  CheckBase(hdl);
  ++g_calls["get_setting_float"];
  auto it = g_settings.find(id);
  if (it == g_settings.end())
    return false;
  *v = std::strtof(it->second.c_str(), nullptr);
  return true;
}

bool get_setting_string(const KODI_ADDON_BACKEND_HDL hdl, const char* id, char** v)
{
  CheckBase(hdl);
  ++g_calls["get_setting_string"];
  auto it = g_settings.find(id);
  if (it == g_settings.end())
    return false;
  *v = strdup(it->second.c_str());
  return true;
}

bool set_setting_bool(const KODI_ADDON_BACKEND_HDL hdl, const char*, bool)
{
  CheckBase(hdl);
  ++g_calls["set_setting_bool"];
  return true;
}
bool set_setting_int(const KODI_ADDON_BACKEND_HDL hdl, const char*, int)
{
  CheckBase(hdl);
  ++g_calls["set_setting_int"];
  return true;
}
bool set_setting_float(const KODI_ADDON_BACKEND_HDL hdl, const char*, float)
{
  CheckBase(hdl);
  ++g_calls["set_setting_float"];
  return true;
}
bool set_setting_string(const KODI_ADDON_BACKEND_HDL hdl, const char*, const char*)
{
  CheckBase(hdl);
  ++g_calls["set_setting_string"];
  return true;
}

char* get_addon_info(const KODI_ADDON_BACKEND_HDL hdl, const char* id)
{
  CheckBase(hdl);
  ++g_calls["get_addon_info"];
  return strdup((std::string("proxy-") + id).c_str());
}

char* get_type_version(const KODI_ADDON_BACKEND_HDL, int)
{
  return strdup("0.0.0");
}

void* get_interface(const KODI_ADDON_BACKEND_HDL, const char*, const char*)
{
  return nullptr;
}

char* get_language(void* hdl, int, bool)
{
  CheckBase(hdl);
  return strdup("en-gb");
}

bool queue_notification(void* hdl,
                        int,
                        const char* header,
                        const char* message,
                        const char*,
                        unsigned int,
                        bool,
                        unsigned int)
{
  CheckBase(hdl);
  g_notifications.push_back(std::string(header) + ": " + message);
  return true;
}

bool directory_exists(void* hdl, const char* path)
{
  CheckBase(hdl);
  struct stat st;
  return stat(Translate(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_exists(void* hdl, const char* path, bool)
{
  CheckBase(hdl);
  struct stat st;
  return stat(Translate(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

char* translate_special_protocol(void* hdl, const char* path)
{
  CheckBase(hdl);
  return strdup(Translate(path).c_str());
}

// Kodi's side of one add-on and its visualization instance, as
// CAddonDll/Interface_Base and CVisualization set them up.
struct KodiSide
{
  AddonToKodiFuncTable_kodi_addon kodi_addon{};
  AddonToKodiFuncTable_kodi kodi{};
  AddonToKodiFuncTable_kodi_filesystem filesystem{};
  AddonToKodiFuncTable_Addon to_kodi{};
  KodiToAddonFuncTable_Addon to_addon{};
  AddonGlobalInterface ifc{};

  KODI_ADDON_INSTANCE_FUNC_CB instance_cb{};
  KODI_ADDON_INSTANCE_INFO info{};
  KODI_ADDON_INSTANCE_FUNC instance_funcs{};
  KodiToAddonFuncTable_Visualization viz_to_addon{};
  AddonToKodiFuncTable_Visualization viz_to_kodi{};
  AddonInstance_Visualization viz{};
  KODI_ADDON_INSTANCE_STRUCT instance{};
  int instance_kodi_tag = 0;

  KodiSide()
  {
    kodi_addon.get_addon_path = get_addon_path;
    kodi_addon.get_lib_path = get_lib_path;
    kodi_addon.get_user_path = get_user_path;
    kodi_addon.get_temp_path = get_temp_path;
    kodi_addon.get_localized_string = get_localized_string;
    kodi_addon.open_settings_dialog = open_settings_dialog;
    kodi_addon.is_setting_using_default = is_setting_using_default;
    kodi_addon.get_setting_bool = get_setting_bool;
    kodi_addon.get_setting_int = get_setting_int;
    kodi_addon.get_setting_float = get_setting_float;
    kodi_addon.get_setting_string = get_setting_string;
    kodi_addon.set_setting_bool = set_setting_bool;
    kodi_addon.set_setting_int = set_setting_int;
    kodi_addon.set_setting_float = set_setting_float;
    kodi_addon.set_setting_string = set_setting_string;
    kodi_addon.get_addon_info = get_addon_info;
    kodi_addon.get_type_version = get_type_version;
    kodi_addon.get_interface = get_interface;
    kodi.get_language = get_language;
    kodi.queue_notification = queue_notification;
    filesystem.directory_exists = directory_exists;
    filesystem.file_exists = file_exists;
    filesystem.translate_special_protocol = translate_special_protocol;

    to_kodi.kodiBase = KodiBase();
    to_kodi.free_string = free_string;
    to_kodi.free_string_array = free_string_array;
    to_kodi.addon_log_msg = addon_log_msg;
    to_kodi.kodi = &kodi;
    to_kodi.kodi_addon = &kodi_addon;
    to_kodi.kodi_filesystem = &filesystem;

    ifc.toKodi = &to_kodi;
    ifc.toAddon = &to_addon;
    ifc.firstKodiInstance = &instance;

    info.type = ADDON_INSTANCE_VISUALIZATION;
    info.id = "visualization.wledproxy";
    info.version = "0.1.0";
    info.kodi = &instance_kodi_tag;
    info.first_instance = true;
    info.functions = &instance_cb;
    viz.toAddon = &viz_to_addon;
    viz.toKodi = &viz_to_kodi;
    instance.info = &info;
    instance.functions = &instance_funcs;
    instance.visualization = &viz;
  }
};

// ---------------------------------------------------------------------------
// Proxy library.
// ---------------------------------------------------------------------------

struct Proxy
{
  void* handle = nullptr;
  ADDON_STATUS (*create)(void*) = nullptr;
  const char* (*type_version)(int) = nullptr;
  const char* (*type_min_version)(int) = nullptr;

  bool Load()
  {
    handle = dlopen(WLEDPROXY_SO, RTLD_LAZY);
    if (!handle)
    {
      std::printf("  dlopen proxy: %s\n", dlerror());
      return false;
    }
    create = reinterpret_cast<ADDON_STATUS (*)(void*)>(dlsym(handle, "ADDON_Create"));
    type_version = reinterpret_cast<const char* (*)(int)>(dlsym(handle, "ADDON_GetTypeVersion"));
    type_min_version =
        reinterpret_cast<const char* (*)(int)>(dlsym(handle, "ADDON_GetTypeMinVersion"));
    return create && type_version && type_min_version;
  }

  void Unload()
  {
    if (handle)
      dlclose(handle);
    handle = nullptr;
  }
};

std::string MockLibPath(const std::string& id, bool system = false)
{
  return g_root + (system ? "/xbmcbin/addons/" : "/home/addons/") + id + "/mock.so";
}

MockState* Mock(const std::string& id, bool system = false)
{
  // RTLD_NOLOAD finds the copy the proxy loaded, without loading another.
  void* h = dlopen(MockLibPath(id, system).c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (!h)
    return nullptr;
  auto fn = reinterpret_cast<MockStateFn>(dlsym(h, "mock_state"));
  MockState* s = fn ? fn() : nullptr;
  dlclose(h); // drop the reference RTLD_NOLOAD took
  return s;
}

bool MockLoaded(const std::string& id, bool system = false)
{
  void* h = dlopen(MockLibPath(id, system).c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (h)
    dlclose(h);
  return h != nullptr;
}

// ---------------------------------------------------------------------------
// UDP receiving.
// ---------------------------------------------------------------------------

struct Receiver
{
  int fd = -1;
  int port = 0;

  bool Open(bool multicast)
  {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
      return false;
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    if (multicast)
    {
      ip_mreq m{};
      inet_pton(AF_INET, "239.0.0.1", &m.imr_multiaddr);
      m.imr_interface.s_addr = htonl(INADDR_ANY);
      if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0)
        return false;
    }
    return true;
  }

  std::vector<std::vector<uint8_t>> Drain(int timeout_ms = 300)
  {
    std::vector<std::vector<uint8_t>> out;
    pollfd p{fd, POLLIN, 0};
    while (poll(&p, 1, timeout_ms) > 0)
    {
      uint8_t buf[256];
      const ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n < 0)
        break;
      out.emplace_back(buf, buf + n);
      timeout_ms = 50;
    }
    return out;
  }

  ~Receiver()
  {
    if (fd >= 0)
      close(fd);
  }
};

// One second of a music-like chord, as Kodi would deliver it.
std::vector<float> Audio(double seconds)
{
  const size_t frames = static_cast<size_t>(seconds * 44100);
  std::vector<float> v(frames * 2);
  for (size_t i = 0; i < frames; ++i)
  {
    const double t = i / 44100.0;
    const double env = 0.25 + 0.75 * std::pow(0.5 + 0.5 * std::cos(2 * M_PI * 2 * t), 4);
    double x = 0;
    for (double f : {110.0, 220.0, 440.0, 880.0, 1760.0})
      x += std::sin(2 * M_PI * f * t) / 5.0;
    v[2 * i] = v[2 * i + 1] = static_cast<float>(0.25 * env * x);
  }
  return v;
}

size_t Play(KodiSide& k, double seconds)
{
  const std::vector<float> audio = Audio(seconds);
  size_t calls = 0;
  for (size_t off = 0; off < audio.size(); off += 1024)
  {
    const size_t n = std::min<size_t>(1024, audio.size() - off);
    k.viz_to_addon.audio_data(k.instance.hdl, audio.data() + off, n);
    ++calls;
  }
  return calls;
}

void ResetKodi(const std::string& target)
{
  g_settings = {{"target", target},
                {"enabled", "true"},
                {"destination", "2"},
                {"udp_host", "127.0.0.1"},
                {"udp_port", "11988"}};
  g_calls.clear();
  g_log.clear();
  g_notifications.clear();
}

void CheckPackets(const std::vector<std::vector<uint8_t>>& packets, size_t min_count)
{
  CHECK_MSG(packets.size() >= min_count, "%zu packets", packets.size());
  uint8_t expected = 1;
  for (const auto& p : packets)
  {
    CHECK(p.size() == 44);
    if (p.size() != 44)
      continue;
    CHECK(std::memcmp(p.data(), "00002", 6) == 0);
    CHECK_MSG(p[17] == expected, "frameCounter %d, want %d", p[17], expected);
    expected = expected == 255 ? 1 : expected + 1;
  }
}

std::string Joined(const std::vector<std::string>& v)
{
  std::string s;
  for (const auto& x : v)
    s += x + "\n";
  return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

TEST(proxy_reports_its_own_api_versions)
{
  Proxy p;
  CHECK(p.Load());
  CHECK(std::strcmp(p.type_version(ADDON_GLOBAL_MAIN), ADDON_GLOBAL_VERSION_MAIN) == 0);
  CHECK(std::strcmp(p.type_version(ADDON_GLOBAL_GENERAL), ADDON_GLOBAL_VERSION_GENERAL) == 0);
  CHECK(std::strcmp(p.type_version(ADDON_GLOBAL_FILESYSTEM), ADDON_GLOBAL_VERSION_FILESYSTEM) ==
        0);
  CHECK(std::strcmp(p.type_version(ADDON_INSTANCE_VISUALIZATION),
                    ADDON_INSTANCE_VERSION_VISUALIZATION) == 0);
  // Types the proxy doesn't use report "0.0.0", which Kodi skips.
  CHECK(std::strcmp(p.type_version(ADDON_GLOBAL_GUI), "0.0.0") == 0);
  CHECK(std::strcmp(p.type_version(ADDON_GLOBAL_AUDIOENGINE), "0.0.0") == 0);
  p.Unload();
}

TEST(full_lifecycle_unicast)
{
  ResetKodi("visualization.mock");
  Receiver rx;
  CHECK(rx.Open(false));
  g_settings["udp_port"] = std::to_string(rx.port);

  Proxy p;
  CHECK(p.Load());
  KodiSide k;
  const ADDON_STATUS status = p.create(&k.ifc);
  CHECK_MSG(status == ADDON_STATUS_OK, "status %d\n%s", status, Joined(g_log).c_str());
  if (status != ADDON_STATUS_OK)
    return;

  MockState* m = Mock("visualization.mock");
  CHECK(m != nullptr);
  if (!m)
    return;

  // Kodi's struct carries the target's handle; create already ran.
  CHECK(k.ifc.addonBase == &m->base_tag);
  CHECK(k.to_addon.create == nullptr);
  CHECK(k.to_addon.destroy && k.to_addon.create_instance && k.to_addon.destroy_instance);
  CHECK(m->create_calls == 1);
  CHECK(m->to_kodi_base == KodiBase());
  CHECK(m->first_instance == &k.instance);

  // Redirected for the target.
  CHECK(m->addon_path == g_root + "/home/addons/visualization.mock");
  CHECK(m->user_path == g_root + "/home/userdata/addon_data/visualization.mock/");
  CHECK(m->string_30000 == "General");
  CHECK(m->string_30001 == "Quoted \"word\" and \\ backslash");
  CHECK(m->info_id == "visualization.mock");
  CHECK(m->info_path == g_root + "/home/addons/visualization.mock");
  CHECK(m->info_profile == "special://profile/addon_data/visualization.mock/");
  CHECK(!m->open_dialog);
  CHECK(m->got_enabled && m->enabled);
  CHECK(m->got_count && m->count == 13);
  CHECK(m->got_ratio && m->ratio == 0.5f);
  CHECK(m->got_name && m->name == "caf\xc3\xa9 \"x\"");
  CHECK(m->set_name_ok);
  CHECK(!m->count_default);
  CHECK(!m->got_unknown);
  const std::string values =
      ReadFile(g_root + "/home/userdata/addon_data/visualization.mock/settings.xml");
  CHECK(values.find("written by mock") != std::string::npos);
  // Kodi's own settings were not touched by the target.
  CHECK(g_calls["set_setting_string"] == 0);
  CHECK(g_calls["open_settings_dialog"] == 0);
  CHECK(g_calls["get_addon_path"] == 0);
  CHECK(g_calls["get_user_path"] == 0);

  // Forwarded unchanged.
  CHECK(m->lib_path == "kodi-lib-path");
  CHECK(m->temp_path == "kodi-temp-path");
  CHECK(m->string_core == "kodi:186");
  CHECK(m->info_name == "proxy-name");
  CHECK(g_calls["get_lib_path"] == 1);
  CHECK(g_calls["get_temp_path"] == 1);
  CHECK(g_calls["wrong_base"] == 0);

  // Instance creation reaches the target with its handle.
  CHECK(k.to_addon.create_instance(k.ifc.addonBase, &k.instance) == ADDON_STATUS_OK);
  CHECK(m->create_instance_calls == 1);
  CHECK(m->create_instance_hdl == &m->base_tag);
  CHECK(k.instance.hdl == &m->instance_tag);
  CHECK(k.viz_to_addon.render != nullptr);
  CHECK(k.viz_to_addon.audio_start != nullptr && k.viz_to_addon.audio_data != nullptr);

  // Rendering goes straight to the target.
  k.viz_to_addon.render(k.instance.hdl);
  CHECK(m->render_calls == 1);

  // Audio passes through and packets come out.
  CHECK(k.viz_to_addon.audio_start(k.instance.hdl, 2, 44100, 32));
  CHECK(m->audio_start_calls == 1 && m->audio_start_hdl == &m->instance_tag);
  CHECK(m->audio_start_channels == 2);
  const size_t calls = Play(k, 2.0);
  CHECK(m->audio_data_calls == static_cast<int>(calls));
  CHECK(m->audio_data_hdl == &m->instance_tag);
  CHECK(m->audio_data_floats == 2 * 88200);
  auto packets = rx.Drain();
  CheckPackets(packets, 85); // 2 s at 43 frames per second
  // Music arrives as non-zero bands after the first frames.
  bool any_band = false;
  for (const auto& pk : packets)
    for (int i = 18; i < 34; ++i)
      any_band |= pk.size() == 44 && pk[i] > 0;
  CHECK(any_band);

  // A port change applies immediately.
  Receiver rx2;
  CHECK(rx2.Open(false));
  g_settings["udp_port"] = std::to_string(rx2.port);
  CHECK(k.to_addon.setting_change_integer(k.ifc.addonBase, "udp_port", rx2.port) ==
        ADDON_STATUS_OK);
  Play(k, 0.5);
  CHECK(rx2.Drain().size() >= 20);
  CHECK(rx.Drain(100).empty());

  // Kodi reports every setting when its dialog saves: no reopen without a change.
  size_t opened = 0;
  for (const auto& line : g_log)
    opened += line.find("sending audio sync packets") != std::string::npos;
  k.to_addon.setting_change_boolean(k.ifc.addonBase, "enabled", true);
  k.to_addon.setting_change_integer(k.ifc.addonBase, "destination", 2);
  k.to_addon.setting_change_string(k.ifc.addonBase, "udp_host", "127.0.0.1");
  size_t opened_after = 0;
  for (const auto& line : g_log)
    opened_after += line.find("sending audio sync packets") != std::string::npos;
  CHECK_MSG(opened_after == opened, "reopened %zu times", opened_after - opened);

  // Disabling stops packets; audio still reaches the target.
  g_settings["enabled"] = "false";
  CHECK(k.to_addon.setting_change_boolean(k.ifc.addonBase, "enabled", false) == ADDON_STATUS_OK);
  const int before = m->audio_data_calls;
  Play(k, 0.5);
  CHECK(m->audio_data_calls > before);
  CHECK(rx2.Drain(100).empty());

  // The target setting is accepted and kept for next time.
  CHECK(k.to_addon.setting_change_string(k.ifc.addonBase, "target", "visualization.other") ==
        ADDON_STATUS_OK);
  CHECK(m->setting_calls == 0); // the proxy's settings don't reach the target

  // Teardown in Kodi's order.
  k.to_addon.destroy_instance(k.ifc.addonBase, &k.instance);
  CHECK(m->destroy_instance_calls == 1 && m->destroy_instance_hdl == &m->base_tag);
  int destroy_calls = 0;
  void* destroy_hdl = nullptr;
  void* const base = &m->base_tag;
  m->destroy_calls_out = &destroy_calls;
  m->destroy_hdl_out = &destroy_hdl;
  k.to_addon.destroy(k.ifc.addonBase);
  m = nullptr; // unloaded with the target
  CHECK(destroy_calls == 1 && destroy_hdl == base);
  CHECK(!MockLoaded("visualization.mock"));
  p.Unload();
}

TEST(restart_resets_frame_counter_and_reloads)
{
  // Every opening of the visualization runs ADDON_Create again.
  for (int round = 0; round < 2; ++round)
  {
    ResetKodi("visualization.mock");
    Receiver rx;
    CHECK(rx.Open(false));
    g_settings["udp_port"] = std::to_string(rx.port);
    Proxy p;
    CHECK(p.Load());
    KodiSide k;
    CHECK(p.create(&k.ifc) == ADDON_STATUS_OK);
    CHECK(k.to_addon.create_instance(k.ifc.addonBase, &k.instance) == ADDON_STATUS_OK);
    MockState* m = Mock("visualization.mock");
    CHECK(m && m->create_calls == 1);
    CHECK(k.viz_to_addon.audio_start(k.instance.hdl, 2, 44100, 32));
    Play(k, 0.5);
    CheckPackets(rx.Drain(), 20);
    // audio_start again (a new track) restarts the counter at 1.
    CHECK(k.viz_to_addon.audio_start(k.instance.hdl, 2, 44100, 32));
    Play(k, 0.5);
    CheckPackets(rx.Drain(), 20);
    k.to_addon.destroy_instance(k.ifc.addonBase, &k.instance);
    k.to_addon.destroy(k.ifc.addonBase);
    p.Unload();
  }
}

// Whether this host delivers looped-back multicast at all (a host firewall
// can drop it), checked with plain sockets.
bool MulticastLoopWorks(Receiver& rx)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  const unsigned char ttl = 1, loop = 1;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(rx.port));
  inet_pton(AF_INET, "239.0.0.1", &a.sin_addr);
  const char probe[4] = {'p', 'r', 'b', '\0'};
  sendto(fd, probe, sizeof(probe), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  close(fd);
  return !rx.Drain(300).empty();
}

TEST(multicast_with_loopback)
{
  ResetKodi("visualization.mock");
  Receiver rx;
  if (!rx.Open(true))
  {
    std::printf("  SKIPPED: can't join 239.0.0.1 on this host\n");
    return;
  }
  if (!MulticastLoopWorks(rx))
  {
    std::printf("  SKIPPED: this host drops looped-back multicast (plain socket probe)\n");
    return;
  }
  g_settings["destination"] = "0";
  g_settings["udp_port"] = std::to_string(rx.port);
  setenv("WLEDPROXY_MULTICAST_LOOP", "1", 1);

  Proxy p;
  CHECK(p.Load());
  KodiSide k;
  CHECK(p.create(&k.ifc) == ADDON_STATUS_OK);
  CHECK(k.to_addon.create_instance(k.ifc.addonBase, &k.instance) == ADDON_STATUS_OK);
  CHECK(k.viz_to_addon.audio_start(k.instance.hdl, 2, 44100, 32));
  Play(k, 1.0);
  CheckPackets(rx.Drain(), 40);
  k.to_addon.destroy_instance(k.ifc.addonBase, &k.instance);
  k.to_addon.destroy(k.ifc.addonBase);
  p.Unload();
  unsetenv("WLEDPROXY_MULTICAST_LOOP");
}

TEST(system_addon_layout)
{
  // addon.xml under special://xbmc/addons, library under special://xbmcbin.
  ResetKodi("visualization.mocksys");
  Proxy p;
  CHECK(p.Load());
  KodiSide k;
  const ADDON_STATUS status = p.create(&k.ifc);
  CHECK_MSG(status == ADDON_STATUS_OK, "status %d\n%s", status, Joined(g_log).c_str());
  MockState* m = Mock("visualization.mocksys", true);
  CHECK(m && m->addon_path == g_root + "/xbmc/addons/visualization.mocksys");
  if (status == ADDON_STATUS_OK)
    k.to_addon.destroy(k.ifc.addonBase);
  CHECK(!MockLoaded("visualization.mocksys", true));
  p.Unload();
}

TEST(version_mismatch_fails_cleanly)
{
  ResetKodi("visualization.mockold");
  Proxy p;
  CHECK(p.Load());
  KodiSide k;
  CHECK(p.create(&k.ifc) == ADDON_STATUS_PERMANENT_FAILURE);
  CHECK(k.to_addon.destroy == nullptr && k.to_addon.create_instance == nullptr);
  CHECK(k.ifc.addonBase == nullptr);
  CHECK(!MockLoaded("visualization.mockold"));
  CHECK(g_notifications.size() == 1);
  bool logged = false;
  for (const auto& line : g_log)
    logged |= line.find("4.0.0") != std::string::npos &&
              line.find(ADDON_INSTANCE_VERSION_VISUALIZATION) != std::string::npos;
  CHECK_MSG(logged, "log:\n%s", Joined(g_log).c_str());
  p.Unload();
}

TEST(bad_targets_fail_cleanly)
{
  for (const char* target : {"", "visualization.wledproxy", "visualization.missing"})
  {
    ResetKodi(target);
    Proxy p;
    CHECK(p.Load());
    KodiSide k;
    CHECK(p.create(&k.ifc) == ADDON_STATUS_PERMANENT_FAILURE);
    CHECK(k.to_addon.destroy == nullptr);
    CHECK(g_notifications.size() == 1);
    p.Unload();
  }
}

int main()
{
  std::string tmpl = std::string(WLEDPROXY_TEST_TMP) + "/harness-XXXXXX";
  MkdirP(WLEDPROXY_TEST_TMP);
  if (!mkdtemp(tmpl.data()))
  {
    std::perror("mkdtemp");
    return 1;
  }
  g_root = tmpl;
  InstallMock("visualization.mock", MOCK_TARGET_SO, false);
  InstallMock("visualization.mocksys", MOCK_TARGET_SO, true);
  InstallMock("visualization.mockold", MOCK_TARGET_OLD_SO, false);
  return test::RunAll();
}
