#pragma once

#include <string>

namespace wledproxy
{

using TypeVersionFn = const char* (*)(int type);

// Compares dotted version strings ("2.0.4"). An empty or null string is lower
// than any version. Returns <0, 0 or >0.
int CompareVersions(const char* a, const char* b);

// Applies Kodi's CheckAPIVersion rule (xbmc/addons/binary-addons/AddonDll.cpp)
// to the target library, using the versions this proxy was compiled with as
// Kodi's: every global type, plus the visualization instance type.
// min_version may be null (the export is optional, as in Kodi).
// On failure, fills error with both versions and returns false.
bool CheckTargetVersions(TypeVersionFn version, TypeVersionFn min_version, std::string& error);

} // namespace wledproxy
