#include "G3DLocaleCore.h"

#include "G3DLocaleEn.h"
#include "G3DLocaleZhCN.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace
{
namespace fs = std::filesystem;

//----------------------------------------------------------------------------
std::string ToLower(std::string str)
{
  std::transform(str.begin(), str.end(), str.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return str;
}

//----------------------------------------------------------------------------
// Parse a flat JSON object (key -> string) into the provided catalog map.
// Non-string values and parse errors are ignored (best effort).
void ParseInto(std::map<std::string, std::string>& catalog, const char* data, std::size_t size)
{
  try
  {
    const nlohmann::json doc = nlohmann::json::parse(data, data + size);
    if (!doc.is_object())
    {
      return;
    }
    for (auto it = doc.begin(); it != doc.end(); ++it)
    {
      if (it.value().is_string())
      {
        catalog[it.key()] = it.value().get<std::string>();
      }
    }
  }
  catch (const nlohmann::json::exception&)
  {
    // Ignore malformed catalog, callers fall back to other languages / the key.
  }
}

// --- ICU MessageFormat subset formatter -------------------------------------

//----------------------------------------------------------------------------
// Index of the '}' matching the '{' at position `open` (s[open] == '{'),
// honoring nesting; npos when unbalanced.
std::size_t MatchBrace(const std::string& s, std::size_t open)
{
  int depth = 0;
  for (std::size_t i = open; i < s.size(); ++i)
  {
    if (s[i] == '{')
    {
      ++depth;
    }
    else if (s[i] == '}')
    {
      if (--depth == 0)
      {
        return i;
      }
    }
  }
  return std::string::npos;
}

//----------------------------------------------------------------------------
std::string Trim(const std::string& s)
{
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
  {
    ++b;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
  {
    --e;
  }
  return s.substr(b, e - b);
}

//----------------------------------------------------------------------------
const std::string* FindArg(const G3DLocaleCore::Args& args, const std::string& name)
{
  const auto it = std::find_if(
    args.begin(), args.end(), [&name](const auto& pair) { return pair.first == name; });
  return it == args.end() ? nullptr : &it->second;
}

//----------------------------------------------------------------------------
bool ParseInteger(const std::string& s, long long& out)
{
  const std::string t = Trim(s);
  if (t.empty())
  {
    return false;
  }
  std::size_t i = 0;
  bool neg = false;
  if (t[i] == '+' || t[i] == '-')
  {
    neg = t[i] == '-';
    ++i;
  }
  if (i >= t.size())
  {
    return false;
  }
  long long value = 0;
  for (; i < t.size(); ++i)
  {
    if (t[i] < '0' || t[i] > '9')
    {
      return false;
    }
    value = value * 10 + (t[i] - '0');
  }
  out = neg ? -value : value;
  return true;
}

//----------------------------------------------------------------------------
// Thousands grouping separator for a language (default ","). Future: de=".",
// fr=narrow no-break space.
std::string GroupingSeparator(const std::string& /*lang*/)
{
  return ",";
}

//----------------------------------------------------------------------------
std::string FormatNumber(long long n, const std::string& lang)
{
  const std::string sep = GroupingSeparator(lang);
  const bool neg = n < 0;
  // Magnitude via unsigned to stay correct at the negative extreme.
  const unsigned long long mag =
    neg ? 0ull - static_cast<unsigned long long>(n) : static_cast<unsigned long long>(n);
  const std::string digits = std::to_string(mag);
  std::string out;
  const std::size_t len = digits.size();
  for (std::size_t i = 0; i < len; ++i)
  {
    if (i != 0 && (len - i) % 3 == 0)
    {
      out += sep;
    }
    out += digits[i];
  }
  return neg ? "-" + out : out;
}

//----------------------------------------------------------------------------
// Parse a plural/select body into ordered (selector, sub-message) pairs.
// Body looks like: ` =0 {…} one {…} other {…} `.
std::vector<std::pair<std::string, std::string>> ParseSubmessages(const std::string& body)
{
  std::vector<std::pair<std::string, std::string>> subs;
  std::size_t i = 0;
  while (i < body.size())
  {
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
    {
      ++i;
    }
    if (i >= body.size())
    {
      break;
    }
    const std::size_t selStart = i;
    while (
      i < body.size() && body[i] != '{' && !std::isspace(static_cast<unsigned char>(body[i])))
    {
      ++i;
    }
    const std::string selector = body.substr(selStart, i - selStart);
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
    {
      ++i;
    }
    if (i >= body.size() || body[i] != '{')
    {
      break; // malformed; stop best-effort
    }
    const std::size_t close = MatchBrace(body, i);
    if (close == std::string::npos)
    {
      break;
    }
    subs.emplace_back(selector, body.substr(i + 1, close - i - 1));
    i = close + 1;
  }
  return subs;
}

//----------------------------------------------------------------------------
const std::string* PickSubmessage(
  const std::vector<std::pair<std::string, std::string>>& subs, const std::string& selector)
{
  for (const auto& pair : subs)
  {
    if (pair.first == selector)
    {
      return &pair.second;
    }
  }
  return nullptr;
}

// Mutually recursive: a placeholder may expand to a sub-message that contains
// further placeholders.
void FormatInto(const std::string& tmpl, const G3DLocaleCore::Args& args,
  const std::string& lang, const std::string* hash, std::string& out);

//----------------------------------------------------------------------------
// Render a single `{...}` placeholder given its inner text (without the braces).
void FormatPlaceholder(const std::string& inner, const G3DLocaleCore::Args& args,
  const std::string& lang, std::string& out)
{
  const std::size_t comma1 = inner.find(',');
  if (comma1 == std::string::npos)
  {
    // Simple {name}: substitute, or preserve "{name}" literally when unknown.
    if (const std::string* value = FindArg(args, Trim(inner)))
    {
      out += *value;
    }
    else
    {
      out += '{';
      out += inner;
      out += '}';
    }
    return;
  }

  const std::string name = Trim(inner.substr(0, comma1));
  const std::string rest = inner.substr(comma1 + 1);
  const std::size_t comma2 = rest.find(',');
  const std::string type = Trim(rest.substr(0, comma2));
  const std::string* value = FindArg(args, name);

  if (type == "number")
  {
    long long n = 0;
    if (value && ParseInteger(*value, n))
    {
      out += FormatNumber(n, lang);
    }
    else if (value)
    {
      out += *value; // not an integer: emit raw
    }
    else
    {
      out += '{';
      out += inner;
      out += '}';
    }
    return;
  }

  if ((type == "plural" || type == "select") && comma2 != std::string::npos)
  {
    const std::vector<std::pair<std::string, std::string>> subs =
      ParseSubmessages(rest.substr(comma2 + 1));
    const std::string* chosen = nullptr;
    if (type == "plural")
    {
      long long n = 0;
      const bool ok = value && ParseInteger(*value, n);
      std::string formattedNumber;
      if (ok)
      {
        formattedNumber = FormatNumber(n, lang);
        chosen = PickSubmessage(subs, "=" + std::to_string(n));
        if (!chosen)
        {
          chosen = PickSubmessage(subs, G3DLocaleCore::PluralCategory(lang, n));
        }
      }
      if (!chosen)
      {
        chosen = PickSubmessage(subs, "other");
      }
      if (chosen)
      {
        FormatInto(*chosen, args, lang, ok ? &formattedNumber : nullptr, out);
      }
    }
    else // select
    {
      if (value)
      {
        chosen = PickSubmessage(subs, *value);
      }
      if (!chosen)
      {
        chosen = PickSubmessage(subs, "other");
      }
      if (chosen)
      {
        FormatInto(*chosen, args, lang, nullptr, out);
      }
    }
    return;
  }

  // Unknown argument type: preserve the placeholder verbatim.
  out += '{';
  out += inner;
  out += '}';
}

//----------------------------------------------------------------------------
void FormatInto(const std::string& tmpl, const G3DLocaleCore::Args& args,
  const std::string& lang, const std::string* hash, std::string& out)
{
  for (std::size_t i = 0; i < tmpl.size();)
  {
    const char c = tmpl[i];
    if (c == '#' && hash != nullptr)
    {
      out += *hash;
      ++i;
    }
    else if (c == '{')
    {
      const std::size_t close = MatchBrace(tmpl, i);
      if (close == std::string::npos)
      {
        out += c; // stray '{': treat literally
        ++i;
      }
      else
      {
        FormatPlaceholder(tmpl.substr(i + 1, close - i - 1), args, lang, out);
        i = close + 1;
      }
    }
    else
    {
      out += c;
      ++i;
    }
  }
}
}

//----------------------------------------------------------------------------
G3DLocaleCore& G3DLocaleCore::GetInstance()
{
  static G3DLocaleCore instance;
  return instance;
}

//----------------------------------------------------------------------------
G3DLocaleCore::G3DLocaleCore()
{
  ParseInto(this->Catalogs["en"], reinterpret_cast<const char*>(G3DLocaleEn), sizeof(G3DLocaleEn));
  ParseInto(
    this->Catalogs["zh-CN"], reinterpret_cast<const char*>(G3DLocaleZhCN), sizeof(G3DLocaleZhCN));
}

//----------------------------------------------------------------------------
std::vector<std::string> G3DLocaleCore::SupportedLanguages()
{
  return { "en", "zh-CN" };
}

//----------------------------------------------------------------------------
void G3DLocaleCore::SetLanguage(const std::string& lang)
{
  const std::vector<std::string> supported = G3DLocaleCore::SupportedLanguages();
  if (std::find(supported.begin(), supported.end(), lang) != supported.end())
  {
    this->Language = lang;
  }
  else
  {
    this->Language = "en";
  }
}

//----------------------------------------------------------------------------
const std::string& G3DLocaleCore::GetLanguage() const
{
  return this->Language;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::NormalizeLocale(const std::string& raw)
{
  const std::string lower = ToLower(raw);
  // Match common spellings: "zh", "zh_cn", "zh-hans", "chinese (simplified)_china", ...
  if (lower.find("zh") != std::string::npos || lower.find("chinese") != std::string::npos ||
    lower.find("hans") != std::string::npos)
  {
    return "zh-CN";
  }
  return "en";
}

//----------------------------------------------------------------------------
const std::string* G3DLocaleCore::Find(const std::string& lang, const std::string& key) const
{
  const auto catalogIt = this->Catalogs.find(lang);
  if (catalogIt == this->Catalogs.end())
  {
    return nullptr;
  }
  const auto entryIt = catalogIt->second.find(key);
  if (entryIt == catalogIt->second.end())
  {
    return nullptr;
  }
  return &entryIt->second;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::Translate(const std::string& key) const
{
  if (const std::string* hit = this->Find(this->Language, key))
  {
    return *hit;
  }
  if (this->Language != "en")
  {
    if (const std::string* fallback = this->Find("en", key))
    {
      return *fallback;
    }
  }
  // Visible marker: an untranslated key returns itself.
  return key;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::Translate(const std::string& key, const Args& args) const
{
  // Format in the language whose catalog actually supplies the text, so plural
  // rules and number grouping match what is shown: the active language when it
  // has the key, otherwise English (the fallback, or the key itself).
  const std::string lang = this->Find(this->Language, key) ? this->Language : "en";
  return G3DLocaleCore::FormatMessage(this->Translate(key), args, lang);
}

//----------------------------------------------------------------------------
bool G3DLocaleCore::NeedsCJK() const
{
  return this->Language.rfind("zh", 0) == 0 || this->Language.rfind("ja", 0) == 0 ||
    this->Language.rfind("ko", 0) == 0;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::GetActiveCatalogText() const
{
  std::string text;
  const auto catalogIt = this->Catalogs.find(this->Language);
  if (catalogIt != this->Catalogs.end())
  {
    for (const auto& entry : catalogIt->second)
    {
      text += entry.second;
    }
  }
  return text;
}

//----------------------------------------------------------------------------
void G3DLocaleCore::SetCJKFontPath(const std::string& path)
{
  this->CJKFontPath = path;
}

//----------------------------------------------------------------------------
const std::string& G3DLocaleCore::GetCJKFontPath() const
{
  return this->CJKFontPath;
}

//----------------------------------------------------------------------------
void G3DLocaleCore::LoadFromDirectory(const std::string& dir)
{
  std::error_code ec;
  const fs::path dirPath(dir);
  if (!fs::is_directory(dirPath, ec))
  {
    return;
  }

  for (const std::string& lang : G3DLocaleCore::SupportedLanguages())
  {
    const fs::path file = dirPath / (lang + ".json");
    if (!fs::is_regular_file(file, ec))
    {
      continue;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
      continue;
    }
    const std::string content(
      (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    // External entries override the embedded defaults key by key.
    ParseInto(this->Catalogs[lang], content.data(), content.size());
  }
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::FormatNamed(const std::string& tmpl, const Args& args)
{
  if (args.empty() || tmpl.find('{') == std::string::npos)
  {
    return tmpl;
  }

  std::string result;
  result.reserve(tmpl.size());
  for (std::size_t i = 0; i < tmpl.size();)
  {
    if (tmpl[i] == '{')
    {
      const std::size_t close = tmpl.find('}', i + 1);
      if (close != std::string::npos)
      {
        const std::string name = tmpl.substr(i + 1, close - i - 1);
        const auto argIt = std::find_if(
          args.begin(), args.end(), [&name](const auto& pair) { return pair.first == name; });
        if (argIt != args.end())
        {
          result += argIt->second;
          i = close + 1;
          continue;
        }
      }
    }
    result += tmpl[i];
    ++i;
  }
  return result;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::FormatMessage(
  const std::string& tmpl, const Args& args, const std::string& lang)
{
  if (args.empty() || tmpl.find('{') == std::string::npos)
  {
    return tmpl;
  }
  std::string result;
  result.reserve(tmpl.size());
  FormatInto(tmpl, args, lang, nullptr, result);
  return result;
}

//----------------------------------------------------------------------------
std::string G3DLocaleCore::PluralCategory(const std::string& lang, long long n)
{
  // Absolute value of the operand (CLDR uses |n|), without UB at the extreme.
  const unsigned long long an =
    n < 0 ? 0ull - static_cast<unsigned long long>(n) : static_cast<unsigned long long>(n);
  // Map a language to its plural family here when adding it.
  const std::string base = lang.substr(0, lang.find('-')); // "zh-CN" -> "zh"
  if (base == "zh" || base == "ja" || base == "ko")
  {
    return "other"; // east-asian: no cardinal plural distinction
  }
  if (base == "fr")
  {
    return (an == 0 || an == 1) ? "one" : "other"; // french: 0 and 1 are "one"
  }
  // germanic / default (en, de, es, nl, sv, ...): only 1 is "one".
  return an == 1 ? "one" : "other";
}
