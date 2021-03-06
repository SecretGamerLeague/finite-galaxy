// Information.cpp

#include "Information.h"

#include "Sprite.h"

using namespace std;



void Information::SetSprite(const string &name, const Sprite *sprite, const Point &unit, float frame)
{
  sprites[name] = sprite;
  spriteUnits[name] = unit;
  spriteFrames[name] = frame;
}



const Sprite *Information::GetSprite(const string &name) const
{
  static const Sprite empty;
  
  auto it = sprites.find(name);
  return (it == sprites.end()) ? &empty : it->second;
}



const Point &Information::GetSpriteUnit(const string &name) const
{
  static const Point up(0., -1.);
  
  auto it = spriteUnits.find(name);
  return (it == spriteUnits.end()) ? up : it->second;
}



float Information::GetSpriteFrame(const string &name) const
{
  auto it = spriteFrames.find(name);
  return (it == spriteFrames.end()) ? 0.f : it->second;
}



void Information::SetString(const string &name, const string &value)
{
  strings[name] = value;
}



const string &Information::GetString(const string &name) const
{
  static const string empty;
  
  auto it = strings.find(name);
  return (it == strings.end()) ? empty : it->second;
}



void Information::SetBar(const string &name, double value, double segments)
{
  bars[name] = value;
  barSegments[name] = static_cast<double>(segments);
}



double Information::BarValue(const string &name) const
{
  auto it = bars.find(name);
  
  return (it == bars.end()) ? 0. : it->second;
}



double Information::BarSegments(const string &name) const
{
  auto it = barSegments.find(name);
  
  return (it == barSegments.end()) ? 1. : it->second;
}


  
void Information::SetCondition(const string &condition)
{
  conditions.insert(condition);
}



bool Information::HasCondition(const string &condition) const
{
  if(condition.empty())
    return true;
  
  if(condition.front() == '!')
    return !HasCondition(condition.substr(1));
  
  return conditions.count(condition);
}


  
void Information::SetOutlineColour(const Colour &colour)
{
  outlineColour = colour;
}



const Colour &Information::GetOutlineColour() const
{
  return outlineColour;
}
