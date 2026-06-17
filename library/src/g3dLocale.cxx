#include "g3dLocale.h"

#include "G3DLocaleCore.h"

namespace g3d
{
//----------------------------------------------------------------------------
void locale::setLanguage(const std::string& lang)
{
  G3DLocaleCore::GetInstance().SetLanguage(lang);
}

//----------------------------------------------------------------------------
std::string locale::getLanguage()
{
  return G3DLocaleCore::GetInstance().GetLanguage();
}

//----------------------------------------------------------------------------
std::string locale::normalizeLocale(const std::string& raw)
{
  return G3DLocaleCore::NormalizeLocale(raw);
}

//----------------------------------------------------------------------------
std::vector<std::string> locale::supportedLanguages()
{
  return G3DLocaleCore::SupportedLanguages();
}

//----------------------------------------------------------------------------
void locale::loadFromDirectory(const std::string& dir)
{
  G3DLocaleCore::GetInstance().LoadFromDirectory(dir);
}

//----------------------------------------------------------------------------
std::string locale::translate(const std::string& key)
{
  return G3DLocaleCore::GetInstance().Translate(key);
}

//----------------------------------------------------------------------------
std::string locale::translate(const std::string& key, const Args& args)
{
  return G3DLocaleCore::GetInstance().Translate(key, args);
}

//----------------------------------------------------------------------------
bool locale::needsCJK()
{
  return G3DLocaleCore::GetInstance().NeedsCJK();
}

//----------------------------------------------------------------------------
void locale::setCJKFontPath(const std::string& path)
{
  G3DLocaleCore::GetInstance().SetCJKFontPath(path);
}
}
