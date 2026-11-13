// Locating, loading and validating the target library.

#include "proxy.h"

#include <kodi/Filesystem.h>
#include <pugixml.hpp>

#include <dlfcn.h>

namespace wledproxy
{
namespace
{

std::string StripSlash(std::string s)
{
  while (s.size() > 1 && s.back() == '/')
    s.pop_back();
  return s;
}

bool FindTargetFolder(const std::string& id, std::string& folder)
{
  for (const char* base : {"special://home/addons/", "special://xbmc/addons/"})
  {
    const std::string special = std::string(base) + id + "/";
    if (kodi::vfs::DirectoryExists(special))
    {
      folder = StripSlash(kodi::vfs::TranslateSpecialProtocol(special));
      return true;
    }
  }
  return false;
}

bool ReadLibraryName(const std::string& folder, std::string& name, std::string& error)
{
  const std::string path = folder + "/addon.xml";
  pugi::xml_document doc;
  const pugi::xml_parse_result r = doc.load_file(path.c_str());
  if (!r)
  {
    error = "can't read " + path + ": " + r.description();
    return false;
  }
  for (const pugi::xpath_node& xn : doc.select_nodes("/addon/extension"))
  {
    const pugi::xml_node ext = xn.node();
    const char* lib = ext.attribute("library_linux").as_string();
    if (!*lib)
      lib = ext.attribute("library").as_string();
    if (*lib)
    {
      name = lib;
      return true;
    }
  }
  error = path + " names no library";
  return false;
}

bool Exists(const std::string& path)
{
  return !path.empty() && kodi::vfs::FileExists(path, false);
}

// Same search as Kodi's CAddonDll::GetDllPath: the add-on folder, then the
// alternative binary folder, then the binary tree mirroring special://xbmc/
// (system add-ons keep addon.xml under share/ and the library under lib/).
bool FindLibrary(const std::string& id,
                 const std::string& folder,
                 const std::string& name,
                 std::string& lib_path)
{
  const std::string in_folder = folder + "/" + name;
  if (Exists(in_folder))
  {
    lib_path = in_folder;
    return true;
  }
  const std::string altbin = kodi::vfs::TranslateSpecialProtocol("special://xbmcaltbinaddons/");
  if (!altbin.empty())
  {
    for (const std::string& p : {StripSlash(altbin) + "/" + name,
                                 StripSlash(altbin) + "/" + id + "/" + name})
    {
      if (Exists(p))
      {
        lib_path = p;
        return true;
      }
    }
  }
  const std::string xbmc = StripSlash(kodi::vfs::TranslateSpecialProtocol("special://xbmc/"));
  const std::string xbmcbin =
      StripSlash(kodi::vfs::TranslateSpecialProtocol("special://xbmcbin/"));
  if (!xbmc.empty() && in_folder.compare(0, xbmc.size(), xbmc) == 0)
  {
    const std::string p = xbmcbin + in_folder.substr(xbmc.size());
    if (Exists(p))
    {
      lib_path = p;
      return true;
    }
  }
  return false;
}

} // namespace

bool LoadTarget(const std::string& id, TargetLibrary& lib, std::string& error)
{
  lib = TargetLibrary{};
  lib.id = id;
  if (!FindTargetFolder(id, lib.folder))
  {
    error = "add-on folder of " + id + " not found";
    return false;
  }
  std::string name;
  if (!ReadLibraryName(lib.folder, name, error))
    return false;
  if (!FindLibrary(id, lib.folder, name, lib.lib_path))
  {
    error = "library " + name + " of " + id + " not found";
    return false;
  }

  dlerror();
  lib.handle = dlopen(lib.lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib.handle)
  {
    const char* e = dlerror();
    error = "dlopen " + lib.lib_path + ": " + (e ? e : "unknown error");
    return false;
  }
  lib.create = reinterpret_cast<ADDON_STATUS (*)(void*)>(dlsym(lib.handle, "ADDON_Create"));
  lib.get_type_version =
      reinterpret_cast<const char* (*)(int)>(dlsym(lib.handle, "ADDON_GetTypeVersion"));
  lib.get_type_min_version =
      reinterpret_cast<const char* (*)(int)>(dlsym(lib.handle, "ADDON_GetTypeMinVersion"));
  if (!lib.create || !lib.get_type_version)
  {
    error = lib.lib_path + " lacks ADDON_Create or ADDON_GetTypeVersion";
    UnloadTarget(lib);
    return false;
  }
  return true;
}

void UnloadTarget(TargetLibrary& lib)
{
  if (lib.handle)
    dlclose(lib.handle);
  lib.handle = nullptr;
  lib.create = nullptr;
  lib.get_type_version = nullptr;
  lib.get_type_min_version = nullptr;
}

} // namespace wledproxy
