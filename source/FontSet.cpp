// FontSet.cpp

#include "FontSet.h"

#include "Files.h"
#include "Font.h"

#include <cstdlib>
#include <map>
#include <fontconfig/fontconfig.h>

using namespace std;

namespace {
  map<int, Font> fonts;

  const string defaultFontDescription("Linux Libertine Display");
  string fontDescription = defaultFontDescription;
  string fontLanguage("en");
  char envBackend[] = "PANGOCAIRO_BACKEND=fc";
}



void FontSet::Add(const std::string &fontsDir)
{
  const FcChar8* fontsDirFc8 = reinterpret_cast<const FcChar8*>(fontsDir.c_str());
  const string cfgFile = fontsDir + "fonts.conf";
  const FcChar8* cfgFileFc8 = reinterpret_cast<const FcChar8*>(cfgFile.c_str());
  FcConfig *fcConfig = FcConfigGetCurrent();
  if(!FcConfigAppFontAddDir(fcConfig, fontsDirFc8))
    Files::LogError("Warning: Fail to load fonts in \"" + fontsDir + "\".");
  if(!FcConfigParseAndLoad(fcConfig, cfgFileFc8, FcFalse))
    Files::LogError("Warning: Parse error in \"" + cfgFile + "\".");
}



const Font &FontSet::Get(int size)
{
  if(!fonts.count(size))
  {
    putenv(envBackend);
    Font &font = fonts[size];
    font.SetFontDescription(fontDescription);
    font.SetPixelSize(size);
    font.SetLanguage(fontLanguage);
  }
  return fonts[size];
}



void FontSet::SetFontDescription(const string &desc)
{
  fontDescription = desc.empty() ? defaultFontDescription : desc;
  for(auto &it : fonts)
    it.second.SetFontDescription(fontDescription);
}



void FontSet::SetLanguage(const string &lang)
{
  fontLanguage = lang;
  for(auto &it : fonts)
    it.second.SetLanguage(lang);
}

