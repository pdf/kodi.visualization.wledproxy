#include "po.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace wledproxy
{
namespace
{

// Decodes one quoted C-style string starting at s[pos] == '"'. Appends to out.
void AppendQuoted(const std::string& s, size_t pos, std::string& out)
{
  for (size_t i = pos + 1; i < s.size(); ++i)
  {
    const char c = s[i];
    if (c == '"')
      return;
    if (c != '\\' || i + 1 >= s.size())
    {
      out += c;
      continue;
    }
    const char e = s[++i];
    switch (e)
    {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      default:
        out += e; // \" \\ and anything else
        break;
    }
  }
}

enum class Field
{
  None,
  Ctxt,
  Id,
  Str
};

struct Entry
{
  std::string ctxt, id, str;
  bool any = false;
};

void Commit(Entry& e, std::unordered_map<uint32_t, std::string>& strings)
{
  if (e.any && e.ctxt.size() > 1 && e.ctxt[0] == '#')
  {
    char* end = nullptr;
    const unsigned long n = std::strtoul(e.ctxt.c_str() + 1, &end, 10);
    if (end && *end == '\0')
      strings[static_cast<uint32_t>(n)] = e.str.empty() ? e.id : e.str;
  }
  e = Entry{};
}

} // namespace

void ParsePo(const std::string& text, std::unordered_map<uint32_t, std::string>& strings)
{
  std::istringstream in(text);
  std::string line;
  Entry entry;
  Field field = Field::None;

  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
    {
      Commit(entry, strings);
      field = Field::None;
      continue;
    }
    if (line[start] == '#')
      continue;

    auto keyword = [&](const char* kw) {
      const size_t n = std::char_traits<char>::length(kw);
      return line.compare(start, n, kw) == 0 &&
             (line.size() == start + n || line[start + n] == ' ' || line[start + n] == '\t');
    };

    std::string* target = nullptr;
    if (keyword("msgctxt"))
    {
      // A new msgctxt starts a new entry.
      Commit(entry, strings);
      field = Field::Ctxt;
      target = &entry.ctxt;
    }
    else if (keyword("msgid"))
    {
      if (field == Field::Str)
        Commit(entry, strings);
      field = Field::Id;
      target = &entry.id;
    }
    else if (keyword("msgstr"))
    {
      field = Field::Str;
      target = &entry.str;
    }
    else if (line[start] == '"')
    {
      // Continuation line of the current field.
      switch (field)
      {
        case Field::Ctxt:
          target = &entry.ctxt;
          break;
        case Field::Id:
          target = &entry.id;
          break;
        case Field::Str:
          target = &entry.str;
          break;
        case Field::None:
          break;
      }
    }
    if (!target)
      continue;
    const size_t q = line.find('"', start);
    if (q == std::string::npos)
      continue;
    entry.any = true;
    AppendQuoted(line, q, *target);
  }
  Commit(entry, strings);
}

bool LoadPoFile(const std::string& path, std::unordered_map<uint32_t, std::string>& strings)
{
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  ParsePo(ss.str(), strings);
  return true;
}

} // namespace wledproxy
