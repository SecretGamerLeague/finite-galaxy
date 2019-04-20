// PlayerInfoPanel.h

#ifndef PLAYER_INFO_PANEL_H_
#define PLAYER_INFO_PANEL_H_

#include "Panel.h"

#include "ClickZone.h"
#include "Point.h"

#include <set>
#include <vector>

class PlayerInfo;
class Rectangle;



// This panel displays detailed information about the player and their fleet. If
// the player is landed on a planet, it also allows them to reorder the ships in
// their fleet (including changing which one is the flagship).
class PlayerInfoPanel : public Panel {
public:
  explicit PlayerInfoPanel(PlayerInfo &player);
  
  virtual void Step() override;
  virtual void Draw() override;
  
  
protected:
  // Only override the ones you need; the default action is to return false.
  virtual bool KeyDown(SDL_Keycode key, Uint16 mod, const Command &command, bool isNewPress) override;
  virtual bool Click(int x, int y, int clicks) override;
  virtual bool Hover(int x, int y) override;
  virtual bool Drag(double dx, double dy) override;
  virtual bool Release(int x, int y) override;
  virtual bool Scroll(double dx, double dy) override;
  
  
private:
  // Draw the two subsections of this panel.
  void DrawPlayer(const Rectangle &bounds);
  void DrawFleet(const Rectangle &bounds);
  
  // Handle mouse hover (also including hover during drag actions):
  bool Hover(const Point &point);
  // Adjust the scroll by the given amount. Return true if it changed.
  bool Scroll(int distance);
  
  
private:
  PlayerInfo &player;
  
  std::vector<ClickZone<int>> zones;
  // Keep track of which ship the mouse is hovering over, which ship was most
  // recently selected, which ship is currently being dragged, and all ships
  // that are currently selected.
  int hoverIndex = -1;
  int selectedIndex = -1;
  std::set<int> allSelected;
  // This is the index of the ship at the top of the fleet listing.
  int scroll = 0;
  Point hoverPoint;
  bool canEdit = false;
  bool isDragging = false;
};



#endif
