#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wledproxy
{

// Reads the numbered entries of a Kodi strings.po file: msgctxt "#<id>".
// The text is msgstr, or msgid when msgstr is empty (as Kodi does for
// en_gb). Returns false if the file can't be read.
bool LoadPoFile(const std::string& path, std::unordered_map<uint32_t, std::string>& strings);

// Same, from file contents already in memory.
void ParsePo(const std::string& text, std::unordered_map<uint32_t, std::string>& strings);

} // namespace wledproxy
