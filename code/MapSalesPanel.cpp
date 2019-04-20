// MapSalesPanel.cpp

#include "MapSalesPanel.h"

#include "Command.h"
#include "Dialogue.h"
#include "FillShader.h"
#include "Font.h"
#include "FontSet.h"
#include "GameData.h"
#include "Government.h"
#include "ItemInfoDisplay.h"
#include "Outfit.h"
#include "PlayerInfo.h"
#include "Point.h"
#include "PointerShader.h"
#include "Preferences.h"
#include "RingShader.h"
#include "Screen.h"
#include "Ship.h"
#include "Sprite.h"
#include "SpriteSet.h"
#include "SpriteShader.h"
#include "StellarObject.h"
#include "System.h"
#include "UI.h"

#include <algorithm>

using namespace std;

const double MapSalesPanel::ICON_HEIGHT = 90.;
const double MapSalesPanel::PAD = 1.;
const int MapSalesPanel::WIDTH = 300;



MapSalesPanel::MapSalesPanel(PlayerInfo &player, bool isOutfitters)
  : MapPanel(player, SHOW_SPECIAL),
   categories(GameData::Categories(isOutfitters ? "outfit" : "ship")), 
  isOutfitters(isOutfitters),
  collapsed(player.Collapsed(isOutfitters ? "outfitter map" : "shipyard map"))
{
  if(!isOutfitters)
    swizzle = GameData::PlayerGovernment()->GetSwizzle();
}



MapSalesPanel::MapSalesPanel(const MapPanel &panel, bool isOutfitters)
  : MapPanel(panel),
   categories(GameData::Categories(isOutfitters ? "outfit" : "ship")), 
  isOutfitters(isOutfitters),
  collapsed(player.Collapsed(isOutfitters ? "outfitter map" : "shipyard map"))
{
  commodity = SHOW_SPECIAL;
  if(!isOutfitters)
    swizzle = GameData::PlayerGovernment()->GetSwizzle();
}



void MapSalesPanel::Draw()
{
  MapPanel::Draw();
  
  zones.clear();
  hidPrevious = true;
  
  // Adjust the scroll amount if for some reason the display has changed so
  // that no items are visible.
  scroll = min(0., max(-maxScroll, scroll));
  
  DrawKey();
  DrawPanel();
  DrawItems();
  DrawButtons(isOutfitters ? "is outfitters" : "is shipyards");
  DrawInfo();
}



bool MapSalesPanel::KeyDown(SDL_Keycode key, Uint16 mod, const Command &command, bool isNewPress)
{
  if(key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)
  {
    scroll += static_cast<double>((Screen::Height() - 100) * ((key == SDLK_PAGEUP) - (key == SDLK_PAGEDOWN)));
    scroll = min(0., max(-maxScroll, scroll));
  }
  else if((key == SDLK_DOWN || key == SDLK_UP) && !zones.empty())
  {
    selected += (key == SDLK_DOWN) - (key == SDLK_UP);
    if(selected < 0)
      selected = zones.size() - 1;
    else if(selected > static_cast<int>(zones.size() - 1))
      selected = 0;
    
    Compare(compare = -1);
    Select(selected);
    ScrollTo(selected);
  }
  else if(key == 'f')
    GetUI()->Push(new Dialogue(
      this, &MapSalesPanel::DoFind, "Search for:"));
  else
    return MapPanel::KeyDown(key, mod, command, isNewPress);
  
  return true;
}



bool MapSalesPanel::Click(int x, int y, int clicks)
{
  if(x < Screen::Left() + WIDTH)
  {
    Point point(x, y);
    
    bool isCompare = (SDL_GetModState() & KMOD_SHIFT);
    
    for(const ClickZone<int> &zone : zones)
      if(zone.Contains(point))
      {
        if(isCompare)
        {
          if(zone.Value() != selected)
            Compare(compare = zone.Value());
        }
        else
        {
          Select(selected = zone.Value());
          Compare(compare = -1);
        }
        break;
      }
  }
  else if(x >= Screen::Left() + WIDTH + 30 && x < Screen::Left() + WIDTH + 190 && y < Screen::Top() + 70)
  {
    // This click was in the map key.
    onlyShowSoldHere = (!onlyShowSoldHere && y >= Screen::Top() + 42 && y < Screen::Top() + 62);
  }
  else
    return MapPanel::Click(x, y, clicks);
  
  return true;
}



// Check to see if the mouse is over the scrolling pane.
bool MapSalesPanel::Hover(int x, int y)
{
  isDragging = (x < Screen::Left() + WIDTH);
  
  return isDragging ? true : MapPanel::Hover(x, y);
}



bool MapSalesPanel::Drag(double dx, double dy)
{
  if(isDragging)
    scroll = min(0., max(-maxScroll, scroll + dy));
  else
    return MapPanel::Drag(dx, dy);
  
  return true;
}



bool MapSalesPanel::Scroll(double dx, double dy)
{
  if(isDragging)
    scroll = min(0., max(-maxScroll, scroll + dy * 2.5 * Preferences::ScrollSpeed()));
  else
    return MapPanel::Scroll(dx, dy);
  
  return true;
}



void MapSalesPanel::DrawKey() const
{
  const Sprite *back = SpriteSet::Get("ui/sales key");
  SpriteShader::Draw(back, Screen::TopLeft() + Point(WIDTH + 10, 0) + .5 * Point(back->Width(), back->Height()));
  
  Colour bright(.6f, .6f);
  Colour dim(.3f, .3f);
  const Font &font = FontSet::Get(18);
  
  Point pos(Screen::Left() + 50. + WIDTH, Screen::Top() + 12.);
  Point textOff(10., -.5 * font.Height());
  
  static const double VALUE[] = {
    -.5,
    0.,
    1.
  };
  
  double selectedValue = (selectedSystem ? SystemValue(selectedSystem) : -1.);
  for(int i = 0; i < 3; ++i)
  {
    bool isSelected = (VALUE[i] == selectedValue);
    RingShader::Draw(pos, OUTER, INNER, MapColour(VALUE[i]));
    font.Draw(KeyLabel(i), pos + textOff, isSelected ? bright : dim);
    if(onlyShowSoldHere && i == 2)
    {
      // If we're filtering out items not sold here, draw a pointer.
      PointerShader::Draw(pos + Point(-7., 0.), Point(1., 0.), 10.f, 10.f, 0.f, bright);
    }
    pos.Y() += 20.;
  }
}



void MapSalesPanel::DrawPanel() const
{
  const Colour &back = *GameData::Colours().Get("map side panel background");
  FillShader::Fill(
    Point(Screen::Width() * -.5 + WIDTH * .5, 0.),
    Point(WIDTH, Screen::Height()), 
    back);
  
  const Sprite *edgeSprite = SpriteSet::Get("ui/right edge");
  if(edgeSprite->Height())
  {
    int steps = Screen::Height() / edgeSprite->Height();
    for(int y = -steps; y <= steps; ++y)
    {
      Point pos(
        Screen::Width() * -.5f + WIDTH + .5f * edgeSprite->Width(),
        y * edgeSprite->Height());
      SpriteShader::Draw(edgeSprite, pos);
    }
  }
}



void MapSalesPanel::DrawInfo() const
{
  if(selected >= 0)
  {
    const Sprite *left = SpriteSet::Get("ui/left edge");
    const Sprite *bottom = SpriteSet::Get(compare >= 0 ? "ui/bottom edges" : "ui/bottom edge");
    const Sprite *box = SpriteSet::Get(compare >= 0 ? "ui/thumb boxes" : "ui/thumb box");
    
    const ItemInfoDisplay &selectedInfo = SelectedInfo();
    const ItemInfoDisplay &compareInfo = CompareInfo();
    int height = max<int>(selectedInfo.AttributesHeight(), box->Height());
    int width = selectedInfo.PanelWidth();
    if(compare >= 0)
    {
      height = max(height, compareInfo.AttributesHeight());
      width += box->Width() + compareInfo.PanelWidth();
    }
    
    const Colour &back = *GameData::Colours().Get("map side panel background");
    Point size(width, height);
    Point topLeft(Screen::Right() - size.X(), Screen::Top());
    FillShader::Fill(topLeft + .5 * size, size, back);
    
    Point leftPos = topLeft + Point(
      -.5 * left->Width(),
      size.Y() - .5 * left->Height());
    SpriteShader::Draw(left, leftPos);
    // The top left corner of the bottom sprite should be 10 x units right
    // of the bottom left corner of the left edge sprite.
    Point bottomPos = leftPos + Point(
      10. + .5 * (bottom->Width() - left->Width()),
      .5 * (left->Height() + bottom->Height()));
    SpriteShader::Draw(bottom, bottomPos);
    
    if(compare >= 0)
    {
      compareInfo.DrawAttributes(topLeft);
      topLeft.X() += compareInfo.PanelWidth() + box->Width();
      
      SpriteShader::Draw(box, topLeft + Point(-50., 100.));
      DrawSprite(topLeft + Point(-95., 5.), SelectedSprite());
      DrawSprite(topLeft + Point(-95., 105.), CompareSprite());
    }
    else
    {
      SpriteShader::Draw(box, topLeft + Point(-60., 50.));
      DrawSprite(topLeft + Point(-95., 5.), SelectedSprite());
    }
    selectedInfo.DrawAttributes(topLeft);
  }
}



bool MapSalesPanel::DrawHeader(Point &corner, const string &category)
{
  bool hide = collapsed.count(category);
  if(!hidPrevious)
    corner.Y() += 50.;
  hidPrevious = hide;
  
  const Sprite *arrow = SpriteSet::Get(hide ? "ui/collapsed" : "ui/expanded");
  SpriteShader::Draw(arrow, corner + Point(15., 25.));
  
  const Colour &textColour = *GameData::Colours().Get(hide ? "medium" : "bright");
  const Font &bigFont = FontSet::Get(24);
  bigFont.Draw(category, corner + Point(30., 10.), textColour);
  AddZone(Rectangle::FromCorner(corner, Point(WIDTH, 40.)), [this, category](){ ClickCategory(category); });
  corner.Y() += 40.;
  
  return hide;
}



void MapSalesPanel::DrawSprite(const Point &corner, const Sprite *sprite) const
{
  if(sprite)
  {
    Point iconOffset(.5 * ICON_HEIGHT, .5 * ICON_HEIGHT);
    double scale = min(.5, min((ICON_HEIGHT - 2.) / sprite->Height(), (ICON_HEIGHT - 2.) / sprite->Width()));
    SpriteShader::Draw(sprite, corner + iconOffset, scale, swizzle);
  }
}



void MapSalesPanel::Draw(Point &corner, const Sprite *sprite, bool isForSale, bool isSelected,
    const string &name, const string &price, const string &info)
{
  const Font &font = FontSet::Get(18);
  Colour selectionColour(0.f, .3f);
  
  Point nameOffset(ICON_HEIGHT, .5 * ICON_HEIGHT - PAD - 1.5 * font.Height());
  Point priceOffset(ICON_HEIGHT, nameOffset.Y() + font.Height() + PAD);
  Point infoOffset(ICON_HEIGHT, priceOffset.Y() + font.Height() + PAD);
  Point blockSize(WIDTH, ICON_HEIGHT);

  if(corner.Y() < Screen::Bottom() && corner.Y() + ICON_HEIGHT >= Screen::Top())
  {
    if(isSelected)
      FillShader::Fill(corner + .5 * blockSize, blockSize, selectionColour);
    
    DrawSprite(corner, sprite);
    
    const Colour &textColour = *GameData::Colours().Get(isForSale ? "medium" : "dim");
    font.Draw(name, corner + nameOffset, textColour);
    font.Draw(price, corner + priceOffset, textColour);
    font.Draw(info, corner + infoOffset, textColour);
  }
  zones.emplace_back(corner + .5 * blockSize, blockSize, zones.size());
  corner.Y() += ICON_HEIGHT;
}



void MapSalesPanel::DoFind(const string &text)
{
  int index = FindItem(text);
  if(index >= 0 && index < static_cast<int>(zones.size()))
  {
    Compare(compare = -1);
    Select(selected = index);
    ScrollTo(index);
  }
}



void MapSalesPanel::ScrollTo(int index)
{
  if(index < 0 || index >= static_cast<int>(zones.size()))
    return;
  
  const ClickZone<int> &it = zones[selected];
  if(it.Bottom() > Screen::Bottom())
    scroll += Screen::Bottom() - it.Bottom();
  if(it.Top() < Screen::Top())
    scroll += Screen::Top() - it.Top();
}



void MapSalesPanel::ClickCategory(const string &name)
{
  bool isHidden = collapsed.count(name);
  if(SDL_GetModState() & KMOD_SHIFT)
  {
    // If the shift key is held down, hide or show all categories.
    if(isHidden)
      collapsed.clear();
    else
      for(const string &category : categories)
        collapsed.insert(category);
  }
  else if(isHidden)
    collapsed.erase(name);
  else
    collapsed.insert(name);
}

