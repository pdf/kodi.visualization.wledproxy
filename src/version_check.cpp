#include "version_check.h"

#include <kodi/versions.h>

#include <cstdlib>
#include <cstring>

namespace wledproxy
{
namespace
{

struct TypeVersions
{
  int type;
  const char* name;
  const char* version;
  const char* min_version;
};

// The versions Kodi itself reports for each type. These come straight from
// the macros (not kodi::addon::GetTypeVersion), because the proxy is built
// with BUILD_KODI_ADDON, which hides the types the proxy itself doesn't use.
const TypeVersions kKodiTypes[] = {
    {ADDON_GLOBAL_MAIN, "main", ADDON_GLOBAL_VERSION_MAIN, ADDON_GLOBAL_VERSION_MAIN_MIN},
    {ADDON_GLOBAL_GUI, "gui", ADDON_GLOBAL_VERSION_GUI, ADDON_GLOBAL_VERSION_GUI_MIN},
    {ADDON_GLOBAL_AUDIOENGINE, "audioengine", ADDON_GLOBAL_VERSION_AUDIOENGINE,
     ADDON_GLOBAL_VERSION_AUDIOENGINE_MIN},
    {ADDON_GLOBAL_GENERAL, "general", ADDON_GLOBAL_VERSION_GENERAL,
     ADDON_GLOBAL_VERSION_GENERAL_MIN},
    {ADDON_GLOBAL_NETWORK, "network", ADDON_GLOBAL_VERSION_NETWORK,
     ADDON_GLOBAL_VERSION_NETWORK_MIN},
    {ADDON_GLOBAL_FILESYSTEM, "filesystem", ADDON_GLOBAL_VERSION_FILESYSTEM,
     ADDON_GLOBAL_VERSION_FILESYSTEM_MIN},
    {ADDON_GLOBAL_TOOLS, "tools", ADDON_GLOBAL_VERSION_TOOLS, ADDON_GLOBAL_VERSION_TOOLS_MIN},
    {ADDON_INSTANCE_VISUALIZATION, "visualization", ADDON_INSTANCE_VERSION_VISUALIZATION,
     ADDON_INSTANCE_VERSION_VISUALIZATION_MIN},
};

std::string Str(const char* s)
{
  return (s && *s) ? s : "(none)";
}

// Kodi's CAddonVersion::empty(): no version, or "0.0.0", which is what
// kodi::addon::GetTypeVersion reports for a type the add-on doesn't use.
bool IsEmpty(const char* v)
{
  return !v || !*v || std::strcmp(v, "0.0.0") == 0;
}

} // namespace

int CompareVersions(const char* a, const char* b)
{
  const bool a_empty = !a || !*a;
  const bool b_empty = !b || !*b;
  if (a_empty || b_empty)
    return (a_empty ? 0 : 1) - (b_empty ? 0 : 1);

  while (*a || *b)
  {
    char* end_a = nullptr;
    char* end_b = nullptr;
    const unsigned long va = *a ? std::strtoul(a, &end_a, 10) : 0;
    const unsigned long vb = *b ? std::strtoul(b, &end_b, 10) : 0;
    if (va != vb)
      return va < vb ? -1 : 1;
    a = (*a) ? end_a : a;
    b = (*b) ? end_b : b;
    // Skip the separator; stop at anything that isn't a dotted number.
    if (*a == '.')
      ++a;
    else if (*a)
      a = "";
    if (*b == '.')
      ++b;
    else if (*b)
      b = "";
  }
  return 0;
}

bool CheckTargetVersions(TypeVersionFn version, TypeVersionFn min_version, std::string& error)
{
  for (const auto& t : kKodiTypes)
  {
    const char* addon_version = version ? version(t.type) : nullptr;
    const char* addon_min = min_version ? min_version(t.type) : addon_version;

    // Kodi: a global type the add-on doesn't use reports an empty version.
    if (t.type <= ADDON_GLOBAL_MAX && IsEmpty(addon_version))
      continue;

    if (CompareVersions(t.min_version, addon_version) > 0 ||
        CompareVersions(addon_min, t.version) > 0)
    {
      error = std::string("incompatible API version for type '") + t.name + "': Kodi " +
              t.version + " (min " + t.min_version + "), target " + Str(addon_version) +
              " (min " + Str(addon_min) + ")";
      return false;
    }
  }
  return true;
}

} // namespace wledproxy
