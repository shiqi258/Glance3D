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
  return G3DLocaleCore::FormatNamed(this->Translate(key), args);
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
