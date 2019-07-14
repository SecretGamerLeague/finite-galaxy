// Engine.cpp

#include "Engine.h"

#include "Audio.h"
#include "Effect.h"
#include "FillShader.h"
#include "Fleet.h"
#include "Flotsam.h"
#include "Font.h"
#include "FontSet.h"
#include "Format.h"
#include "FrameTimer.h"
#include "GameData.h"
#include "Government.h"
#include "Interface.h"
#include "MapPanel.h"
#include "Mask.h"
#include "Messages.h"
#include "Minable.h"
#include "Mission.h"
#include "NPC.h"
#include "OutlineShader.h"
#include "Person.h"
#include "Planet.h"
#include "PlanetLabel.h"
#include "PlayerInfo.h"
#include "PointerShader.h"
#include "Politics.h"
#include "Preferences.h"
#include "Projectile.h"
#include "Random.h"
#include "RingShader.h"
#include "Screen.h"
#include "Ship.h"
#include "ShipEvent.h"
#include "Sprite.h"
#include "SpriteSet.h"
#include "SpriteShader.h"
#include "StarField.h"
#include "StartConditions.h"
#include "StellarObject.h"
#include "System.h"
#include "Visual.h"
#include "WrappedText.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace std;

namespace {
  int RadarType(const Ship &ship, int step)
  {
    if(ship.GetPersonality().IsTarget() && !ship.IsDestroyed())
    {
      // If a ship is a "target," double-blink it a few times per second.
      int count = (step / 6) % 7;
      if(count == 0 || count == 2)
        return Radar::BLINK;
    }
    if(ship.IsDisabled() || (ship.IsOverheated() && ((step / 20) % 2)))
      return Radar::INACTIVE;
    if(ship.IsYours() || (ship.GetPersonality().IsEscort() && !ship.GetGovernment()->IsEnemy()))
      return Radar::PLAYER;
    if(!ship.GetGovernment()->IsEnemy())
      return Radar::FRIENDLY;
    const auto &target = ship.GetTargetShip();
    if(target && target->IsYours())
      return Radar::HOSTILE;
    return Radar::UNFRIENDLY;
  }
  
  template <class Type>
  void Prune(vector<Type> &objects)
  {
    // First, erase any of the old objects that should be removed.
    typename vector<Type>::iterator in = objects.begin();
    while(in != objects.end() && !in->ShouldBeRemoved())
      ++in;
    
    typename vector<Type>::iterator out = in;
    while(in != objects.end())
    {
      if(!in->ShouldBeRemoved())
        *out++ = std::move(*in);
      ++in;
    }
    if(out != objects.end())
      objects.erase(out, objects.end());
  }
  
  template <class Type>
  void Prune(list<shared_ptr<Type>> &objects)
  {
    for(auto it = objects.begin(); it != objects.end(); )
    {
      if((*it)->ShouldBeRemoved())
        it = objects.erase(it);
      else
        ++it;
    }
  }
  
  template <class Type>
  void Append(vector<Type> &objects, vector<Type> &added)
  {
    objects.insert(objects.end(), make_move_iterator(added.begin()), make_move_iterator(added.end()));
    added.clear();
  }
  
  bool CanSendHail(const shared_ptr<const Ship> &ship, const System *playerSystem)
  {
    if(!ship || !playerSystem)
      return false;
    
    // Make sure this ship is in the same system as the player.
    if(ship->GetSystem() != playerSystem)
      return false;
    
    // Player ships shouldn't send hails.
    if(!ship->GetGovernment() || ship->IsYours())
      return false;
    
    // Make sure this ship is able to send a hail.
    if(ship->IsDisabled() || !ship->Crew()
        || ship->Cloaking() >= 1. || ship->GetPersonality().IsMute())
      return false;
    
    return true;
  }
  
  // Author the given message from the given ship.
  void SendMessage(const shared_ptr<const Ship> &ship, const string &message)
  {
    if(message.empty())
      return;
    
    // If this ship has no name, show its model name instead.
    string tag;
    const string &gov = ship->GetGovernment()->GetName();
    if(!ship->Name().empty())
      tag = gov + " " + ship->Noun() + " \"" + ship->Name() + "\": ";
    else
      tag = ship->ModelName() + " (" + gov + "): ";
    
    Messages::Add(tag + message);
  }
  
  const double RADAR_SCALE = .025;
}



Engine::Engine(PlayerInfo &player)
  : player(player), ai(ships, asteroids.Minables(), flotsam),
  shipCollisions(256u, 32u)
{
  zoom = Preferences::ViewZoom();
  
  // Start the thread for doing calculations.
  calcThread = thread(&Engine::ThreadEntryPoint, this);
  
  if(!player.IsLoaded() || !player.GetSystem())
    return;
  
  // Preload any landscapes for this system.
  for(const StellarObject &object : player.GetSystem()->Objects())
    if(object.GetPlanet())
      GameData::Preload(object.GetPlanet()->Landscape());
  
  // Figure out what planet the player is landed on, if any.
  const StellarObject *object = player.GetStellarObject();
  if(object)
    centre = object->Position();
  
  // Now we know the player's current position. Draw the planets.
  draw[calcTickTock].Clear(step, zoom);
  draw[calcTickTock].SetCentre(centre);
  radar[calcTickTock].SetCentre(centre);
  const Ship *flagship = player.Flagship();
  for(const StellarObject &object : player.GetSystem()->Objects())
    if(object.HasSprite())
    {
      draw[calcTickTock].Add(object);
      
      double r = max(2., object.Radius() * .03 + .5);
      radar[calcTickTock].Add(object.RadarType(flagship), object.Position(), r, r - 1.);
    }
  
  // Add all neighbouring systems to the radar.
  const System *targetSystem = flagship ? flagship->GetTargetSystem() : nullptr;
  const set<const System *> &links = (flagship && flagship->Attributes().Get("jump drive")) ?
    player.GetSystem()->Neighbours() : player.GetSystem()->Links();
  for(const System *system : links)
    radar[calcTickTock].AddPointer(
      (system == targetSystem) ? Radar::SPECIAL : Radar::INACTIVE,
      system->Position() - player.GetSystem()->Position());
  
  GameData::SetHaze(player.GetSystem()->Haze());
}



Engine::~Engine()
{
  {
    unique_lock<mutex> lock(swapMutex);
    terminate = true;
  }
  condition.notify_all();
  calcThread.join();
}



void Engine::Place()
{
  ships.clear();
  ai.ClearOrders();
  
  EnterSystem();
  
  // Add the player's flagship and escorts to the list of ships. The TakeOff()
  // code already took care of loading up fighters and assigning parents.
  for(const shared_ptr<Ship> &ship : player.Ships())
    if(!ship->IsParked() && ship->GetSystem())
      ships.push_back(ship);
  
  // Add NPCs to the list of ships. Fighters have to be assigned to carriers,
  // and all but "uninterested" ships should follow the player.
  shared_ptr<Ship> flagship = player.FlagshipPtr();
  for(const Mission &mission : player.Missions())
    Place(mission.NPCs(), flagship); 
  
  // Get the coordinates of the planet the player is leaving.
  const Planet *planet = player.GetPlanet();
  Point planetPos;
  double planetRadius = 0.;
  const StellarObject *object = player.GetStellarObject();
  if(object)
  {
    planetPos = object->Position();
    planetRadius = object->Radius();
  }
  
  // Give each special ship we just added a random heading and position.
  for(const shared_ptr<Ship> &ship : ships)
  {
    Point pos;
    Angle angle = Angle::Random();
    // Any ships in the same system as the player should be either
    // taking off from a specific planet or nearby.
    if(ship->GetSystem() == player.GetSystem() && !ship->IsDisabled())
    {
      const Personality &person = ship->GetPersonality();
      bool hasOwnPlanet = ship->GetPlanet();
      bool launchesWithPlayer = (ship->IsYours() || planet->CanLand(*ship))
          && !(person.IsStaying() || person.IsWaiting() || hasOwnPlanet);
      const StellarObject *object = hasOwnPlanet ?
          ship->GetSystem()->FindStellar(ship->GetPlanet()) : nullptr;
      // Default to the player's planet in the case of data definition errors.
      if(person.IsLaunching() || launchesWithPlayer || (hasOwnPlanet && !object))
      {
        if(planet)
          ship->SetPlanet(planet);
        pos = planetPos + angle.Unit() * Random::Real() * planetRadius;
      }
      else if(hasOwnPlanet)
        pos = object->Position() + angle.Unit() * Random::Real() * object->Radius();
    }
    // If the position is still (0, 0), the special ship is in a different
    // system, disabled, or otherwise unable to land on viable planets in
    // the player's system: place it "in flight".
    if(!pos)
    {
      ship->SetPlanet(nullptr);
       Fleet::Place(*ship->GetSystem(), *ship);
    }
    
    // This ship is taking off from a planet.
    else
      ship->Place(pos, angle.Unit(), angle);
  }
  // Move any ships that were randomly spawned into the main list, now
  // that all special ships have been repositioned.
  ships.splice(ships.end(), newShips);
  
  player.SetPlanet(nullptr);
}



// Add NPC ships to the known ships. These may have been freshly instantiated
// from an accepted assisting/boarding mission, or from existing missions when
// the player departs from a planet.
void Engine::Place(const list<NPC> &npcs, shared_ptr<Ship> flagship)
{
  for(const NPC &npc : npcs)
  {
    set<Ship *> carriers;
    for(const shared_ptr<Ship> &ship : npc.Ships())
    {
      // Skip ships that have been destroyed.
      if(ship->IsDestroyed() || ship->IsDisabled())
        continue;
      
      // Redo the loading up of fighters.
      if(ship->HasBays())
      {
        ship->UnloadBays();
        carriers.insert(&*ship);
      }
    }
    
    shared_ptr<Ship> npcFlagship;
    for(const shared_ptr<Ship> &ship : npc.Ships())
    {
      // Skip ships that have been destroyed.
      if(ship->IsDestroyed())
        continue;
      
      // Avoid the exploit where the player can wear down a NPC's
      // crew by attrition over the course of many days.
      ship->AddCrew(max(0, ship->RequiredCrew() - ship->Crew()));
      if(!ship->IsDisabled())
        ship->Recharge();
      
      if(ship->CanBeCarried())
      {
        bool docked = false;
        for(auto &it : carriers)
          if(it->Carry(ship))
          {
            docked = true;
            break;
          }
        if(docked)
          continue;
      }
      
      ships.push_back(ship);
      // The first (alive) ship in a NPC block
      // serves as the flagship of the group.
      if(!npcFlagship)
        npcFlagship = ship;
      
      // Only the flagship of a NPC considers the
      // player: the rest of the NPC track it.
      if(npcFlagship && ship != npcFlagship)
        ship->SetParent(npcFlagship);
      else if(!ship->GetPersonality().IsUninterested())
        ship->SetParent(flagship);
      else
        ship->SetParent(nullptr);
    }
  }
}



// Wait for the previous calculations (if any) to be done.
void Engine::Wait()
{
  unique_lock<mutex> lock(swapMutex);
  while(calcTickTock != drawTickTock)
    condition.wait(lock);
}



// Begin the next step of calculations.
void Engine::Step(bool isActive)
{
  events.swap(eventQueue);
  eventQueue.clear();
  
  // The calculation thread is now paused, so it is safe to access things.
  const shared_ptr<Ship> flagship = player.FlagshipPtr();
  const StellarObject *object = player.GetStellarObject();
  if(object)
  {
    centre = object->Position();
    centreVelocity = Point();
  }
  else if(flagship)
  {
    centre = flagship->Position();
    centreVelocity = flagship->Velocity();
    if(doEnter && flagship->Zoom() == 1. && !flagship->IsHyperspacing())
    {
      doEnter = false;
      events.emplace_back(flagship, flagship, ShipEvent::JUMP);
    }
    if(flagship->IsEnteringHyperspace() || flagship->Commands().Has(Command::JUMP))
    {
      if(jumpCount < 100)
        ++jumpCount;
      const System *from = flagship->GetSystem();
      const System *to = flagship->GetTargetSystem();
      if(from && to && from != to)
      {
        jumpInProgress[0] = from;
        jumpInProgress[1] = to;
      }
    }
    else if(jumpCount > 0)
      --jumpCount;
  }
  ai.UpdateEvents(events);
  ai.UpdateKeys(player, clickCommands, isActive && wasActive);
  wasActive = isActive;
  Audio::Update(centre);
  
  // Smoothly zoom in and out.
  if(isActive)
  {
    double zoomTarget = Preferences::ViewZoom();
    if(zoom < zoomTarget)
      zoom = min(zoomTarget, zoom * 1.03);
    else if(zoom > zoomTarget)
      zoom = max(zoomTarget, zoom * .97);
  }
    
  // Draw a highlight to distinguish the flagship from other ships.
  if(flagship && !flagship->IsDestroyed() && Preferences::Has("Highlight player's flagship"))
  {
    highlightSprite = flagship->GetSprite();
    highlightUnit = flagship->Unit() * zoom;
    highlightFrame = flagship->GetFrame();
  }
  else
    highlightSprite = nullptr;
    
  // Any of the player's ships that are in system are assumed to have
  // landed along with the player.
  if(flagship && flagship->GetPlanet() && isActive)
    player.SetPlanet(flagship->GetPlanet());
  
  const System *currentSystem = player.GetSystem();
  // Update this here, for thread safety.
  if(!player.HasTravelPlan() && flagship && flagship->GetTargetSystem())
    player.TravelPlan().push_back(flagship->GetTargetSystem());
  if(player.HasTravelPlan() && currentSystem == player.TravelPlan().back())
    player.PopTravel();
  if(doFlash)
  {
    flash = .4;
    doFlash = false;
  }
  else if(flash)
    flash = max(0., flash * .99 - .002);
  
  targets.clear();
  
  // Update the player's ammo amounts.
  ammo.clear();
  if(flagship)
    for(const auto &it : flagship->Outfits())
    {
      if(!it.first->Icon())
        continue;
      
      if(it.first->Ammo())
        ammo.emplace_back(it.first,
          flagship->OutfitCount(it.first->Ammo()));
      else if(it.first->FiringFuel())
      {
        double remaining = flagship->Fuel()
          * flagship->Attributes().Get("fuel capacity");
        ammo.emplace_back(it.first,
          remaining / it.first->FiringFuel());
      }
      else
        ammo.emplace_back(it.first, -1);
    }
  
  // Display escort information for all ships of the "Escort" government,
  // and all ships with the "escort" personality, except for fighters that
  // are not owned by the player.
  escorts.Clear();
  bool fleetIsJumping = (flagship && flagship->Commands().Has(Command::JUMP));
  for(const auto &it : ships)
    if(it->GetGovernment()->IsPlayer() || it->GetPersonality().IsEscort())
      if(!it->IsYours() && !it->CanBeCarried())
      {
        bool isSelected = (flagship && flagship->GetTargetShip() == it);
        escorts.Add(*it, it->GetSystem() == currentSystem, fleetIsJumping, isSelected);
      }
  for(const shared_ptr<Ship> &escort : player.Ships())
    if(!escort->IsParked() && escort != flagship && !escort->IsDestroyed())
    {
      // Check if this escort is selected.
      bool isSelected = false;
      for(const weak_ptr<Ship> &ptr : player.SelectedShips())
        if(ptr.lock() == escort)
        {
          isSelected = true;
          break;
        }
      escorts.Add(*escort, escort->GetSystem() == currentSystem, fleetIsJumping, isSelected);
    }
  
  // Create the status overlays.
  statuses.clear();
  if(isActive && Preferences::Has("Show status overlays"))
    for(const auto &it : ships)
    {
      if(!it->GetGovernment() || it->GetSystem() != currentSystem || it->Cloaking() == 1.)
        continue;
      // Don't show status for dead ships.
      if(it->IsDestroyed())
        continue;
      
      bool isEnemy = it->GetGovernment()->IsEnemy();
      if(isEnemy || it->IsYours() || it->GetPersonality().IsEscort())
      {
        double width = min(it->Width(), it->Height());
        statuses.emplace_back(it->Position() - centre, it->Shields(), it->Hull(),
          max(20., width * .5), isEnemy);
      }
    }
  
  // Create the planet labels.
  labels.clear();
  if(currentSystem && Preferences::Has("Show planet labels"))
  {
    for(const StellarObject &object : currentSystem->Objects())
    {
      if(!object.GetPlanet() || !object.GetPlanet()->IsAccessible(flagship.get()))
        continue;
      
      Point pos = object.Position() - centre;
      if(pos.Length() - object.Radius() < 600. / zoom)
        labels.emplace_back(pos, object, currentSystem, zoom);
    }
  }
  
  if(flagship && flagship->IsOverheated())
    Messages::Add("Your ship has overheated.");
  
  // Clear the HUD information from the previous frame.
  info = Information();
  if(flagship && flagship->Hull())
  {
    Point shipFacingUnit(0., -1.);
    if(Preferences::Has("Rotate flagship in HUD"))
      shipFacingUnit = flagship->Facing().Unit();
    
    info.SetSprite("player sprite", flagship->GetSprite(), shipFacingUnit, flagship->GetFrame(step));
  }
  if(currentSystem)
    info.SetString("location", currentSystem->Name());
  info.SetString("date", player.GetDate().ToString());
  if(flagship)
  {
    info.SetBar("fuel", flagship->Fuel(), 10.);
    info.SetBar("energy", flagship->Energy(), 10.);
    double heat = flagship->Heat();
    info.SetBar("heat", min(1., heat), 10.);
    // If heat is above 100%, draw a second overlaid bar to indicate the total heat level.
    if(heat > 1.)
      info.SetBar("overheat", min(1., heat - 1.), 10.);
    if(flagship->IsOverheated() && (step / 20) % 2)
      info.SetBar("overheat blink", min(1., heat));
    info.SetBar("shields", flagship->Shields());
    info.SetBar("hull", flagship->Hull(), 20.);
    info.SetBar("disabled hull", min(flagship->Hull(), flagship->DisabledHull()), 20.);
  }
  info.SetString("credits",
    Format::Credits(player.Accounts().Credits()) + " credits");
  bool isJumping = flagship && (flagship->Commands().Has(Command::JUMP) || flagship->IsEnteringHyperspace());
  if(flagship && flagship->GetTargetStellar() && !isJumping)
  {
    const StellarObject *object = flagship->GetTargetStellar();
    string navigationMode = flagship->Commands().Has(Command::LAND) ? "Landing on:" :
      object->GetPlanet() && object->GetPlanet()->CanLand(*flagship) ? "Can land on:" :
      "Cannot land on:";
    info.SetString("navigation mode", navigationMode);
    const string &name = object->Name();
    info.SetString("destination", name);
    
    targets.push_back({
      object->Position() - centre,
      object->Facing(),
      object->Radius(),
      object->GetPlanet()->CanLand() ? Radar::FRIENDLY : Radar::HOSTILE,
      5});
  }
  else if(flagship && flagship->GetTargetSystem())
  {
    info.SetString("navigation mode", "Hyperspace:");
    if(player.HasVisited(flagship->GetTargetSystem()))
      info.SetString("destination", flagship->GetTargetSystem()->Name());
    else
      info.SetString("destination", "unexplored system");
  }
  else
  {
    info.SetString("navigation mode", "Navigation:");
    info.SetString("destination", "no destination");
  }
  // Use the radar that was just populated. (The draw tick-tock has not
  // yet been toggled, but it will be at the end of this function.)
  shared_ptr<const Ship> target;
  shared_ptr<const Minable> targetAsteroid;
  targetVector = Point();
  if(flagship)
  {
    target = flagship->GetTargetShip();
    targetAsteroid = flagship->GetTargetAsteroid();
    // Record that the player knows this type of asteroid is available here.
    if(targetAsteroid)
      for(const auto &it : targetAsteroid->Payload())
        player.Harvest(it.first);
  }
  if(!target)
    targetSwizzle = -1;
  if(!target && !targetAsteroid)
    info.SetString("target name", "no target");
  else if(!target)
  {
    info.SetSprite("target sprite",
      targetAsteroid->GetSprite(),
      targetAsteroid->Facing().Unit(),
      targetAsteroid->GetFrame(step));
    info.SetString("target name", Format::Capitalize(targetAsteroid->Name()) + " Asteroid");
    
    targetVector = targetAsteroid->Position() - centre;
    
//    if(flagship->Attributes().Get("tactical scan power"))
    {
      info.SetCondition("range display");
      int targetRange = round(targetAsteroid->Position().Distance(flagship->Position()));
      info.SetString("target range", to_string(targetRange));
    }
  }
  else
  {
    if(target->GetSystem() == player.GetSystem() && target->Cloaking() < 1.)
      targetUnit = target->Facing().Unit();
    info.SetSprite("target sprite", target->GetSprite(), targetUnit, target->GetFrame(step));
    const Font::Layout layout(Font::TRUNC_MIDDLE, 150);
    info.SetString("target name", target->Name(), layout);
    info.SetString("target type", target->ModelName());
    if(!target->GetGovernment())
      info.SetString("target government", "No Government");
    else
      info.SetString("target government", target->GetGovernment()->GetName());
    targetSwizzle = target->GetSwizzle();
    info.SetString("mission target", target->GetPersonality().IsTarget() ? "(mission target)" : "");
    
    int targetType = RadarType(*target, step);
    info.SetOutlineColour(Radar::GetColour(targetType));
    if(target->GetSystem() == player.GetSystem() && target->IsTargetable())
    {
      // The target area will be a square, with sides proportional to the average
      // of the width and the height of the sprite.
      double size = (target->Width() + target->Height()) * .35;
      targets.push_back({
        target->Position() - centre,
        Angle(45.) + target->Facing(),
        size,
        targetType,
        4});
      
      targetVector = target->Position() - centre;
      
      // Check if the target is close enough to show tactical information.
      double tacticalRange = 100. * sqrt(flagship->Attributes().Get("tactical scan power"));
      double targetRange = target->Position().Distance(flagship->Position());
//      if(tacticalRange)
      {
        info.SetCondition("range display");
        info.SetString("target range", to_string(static_cast<int>(round(targetRange))));
      }
      // Actual tactical information requires a scrutable
      // target that is within the tactical scanner range.
      if((targetRange <= tacticalRange && !target->Attributes().Get("inscrutable"))
          || (tacticalRange && target->IsYours()))
      {
        info.SetBar("target shields", target->Shields());
        info.SetBar("target hull", target->Hull(), 20.);
        info.SetBar("target disabled hull", min(target->Hull(), target->DisabledHull()), 20.);

        info.SetCondition("tactical display");
        info.SetString("target crew", to_string(target->Crew()));
        int fuel = round(target->Fuel() * target->Attributes().Get("fuel capacity"));
        info.SetString("target fuel", to_string(fuel));
        int energy = round(target->Energy() * target->Attributes().Get("energy capacity"));
        info.SetString("target energy", to_string(energy));
        int heat = round(100. * target->Heat());
        info.SetString("target heat", to_string(heat) + "%");
      }
    }
  }
  if(target && target->IsTargetable() && target->GetSystem() == currentSystem
    && (flagship->CargoScanFraction() || flagship->OutfitScanFraction()))
  {
    double width = max(target->Width(), target->Height());
    Point pos = target->Position() - centre;
    statuses.emplace_back(pos, flagship->OutfitScanFraction(), flagship->CargoScanFraction(),
      10. + max(20., width * .5), 2, Angle(pos).Degrees() + 180.);
  }
  // Handle any events that change the selected ships.
  if(groupSelect >= 0)
  {
    // This has to be done in Step() to avoid race conditions.
    if(hasControl)
      player.SetGroup(groupSelect);
    else
      player.SelectGroup(groupSelect, hasShift);
    groupSelect = -1;
  }
  if(doClickNextStep)
  {
    // If a click command is issued, always wait until the next step to act
    // on it, to avoid race conditions.
    doClick = true;
    doClickNextStep = false;
  }
  else
    doClick = false;
  
  if(doClick && !isRightClick)
  {
    doClick = !player.SelectShips(clickBox, hasShift);
    if(doClick)
    {
      const vector<const Ship *> &stack = escorts.Click(clickPoint);
      if(!stack.empty())
        doClick = !player.SelectShips(stack, hasShift);
      else
        clickPoint /= isRadarClick ? RADAR_SCALE : zoom;
    }
  }
  
  // Draw crosshairs on all the selected ships.
  for(const weak_ptr<Ship> &selected : player.SelectedShips())
  {
    shared_ptr<Ship> ship = selected.lock();
    if(ship && ship != target && !ship->IsParked() && ship->GetSystem() == player.GetSystem()
        && !ship->IsDestroyed() && ship->Zoom() > 0.)
    {
      double size = (ship->Width() + ship->Height()) * .35;
      targets.push_back({
        ship->Position() - centre,
        Angle(45.) + ship->Facing(),
        size,
        Radar::PLAYER,
        4});
    }
  }
  
  // Draw crosshairs on any minables in range of the flagship's scanners.
  double scanRange = flagship ? 100. * sqrt(flagship->Attributes().Get("asteroid scan power")) : 0.;
  if(flagship && scanRange && !flagship->IsHyperspacing())
    for(const shared_ptr<Minable> &minable : asteroids.Minables())
    {
      Point offset = minable->Position() - centre;
      if(offset.Length() > scanRange)
        continue;
      
      targets.push_back({
        offset,
        minable->Facing(),
        .8 * minable->Radius(),
        minable == flagship->GetTargetAsteroid() ? Radar::SPECIAL : Radar::INACTIVE,
        3});
    }
}



// Begin the next step of calculations.
void Engine::Go()
{
  {
    unique_lock<mutex> lock(swapMutex);
    ++step;
    drawTickTock = !drawTickTock;
  }
  condition.notify_all();
}



// Pass the list of game events to MainPanel for handling by the player, and any
// UI element generation.
list<ShipEvent> &Engine::Events()
{
  return events;
}



// Draw a frame.
void Engine::Draw() const
{
  GameData::Background().Draw(centre, centreVelocity, zoom);
  static const Set<Colour> &colours = GameData::Colours();
  const Interface *interface = GameData::Interfaces().Get("hud");
  
  // Draw any active planet labels.
  for(const PlanetLabel &label : labels)
    label.Draw();
  
  draw[drawTickTock].Draw();
  batchDraw[drawTickTock].Draw();
  
  for(const auto &it : statuses)
  {
    static const Colour colour[6] = {
      *colours.Get("overlay friendly shields"),
      *colours.Get("overlay hostile shields"),
      *colours.Get("overlay outfit scan"),
      *colours.Get("overlay friendly hull"),
      *colours.Get("overlay hostile hull"),
      *colours.Get("overlay cargo scan")
    };
    Point pos = it.position * zoom;
    double radius = it.radius * zoom;
    if(it.outer > 0.)
      RingShader::Draw(pos, radius + 3., 1.5f, it.outer, colour[it.type], 0.f, it.angle);
    double dashes = (it.type >= 2) ? 0. : 20. * min(1., zoom);
    if(it.inner > 0.)
      RingShader::Draw(pos, radius, 1.5f, it.inner, colour[3 + it.type], dashes, it.angle);
  }
  
  // Draw the flagship highlight, if any.
  if(highlightSprite)
  {
    Point size(highlightSprite->Width(), highlightSprite->Height());
    const Colour &colour = *colours.Get("flagship highlight");
    // The flagship is always in the dead centre of the screen.
    OutlineShader::Draw(highlightSprite, Point(), size, colour, highlightUnit, highlightFrame);
  }
  
  if(flash)
    FillShader::Fill(Point(), Point(Screen::Width(), Screen::Height()), Colour(flash, flash));
  
  // Draw messages. Draw the most recent messages first, as some messages
  // may be wrapped onto multiple lines.
  const Font &font = FontSet::Get(18);
  const vector<Messages::Entry> &messages = Messages::Get(step);
  Rectangle messageBox = interface->GetBox("messages");
  WrappedText messageLine(font);
  messageLine.SetWrapWidth(messageBox.Width());
  messageLine.SetParagraphBreak(0.);
  Point messagePoint = Point(messageBox.Left(), messageBox.Bottom());
  for(auto it = messages.rbegin(); it != messages.rend(); ++it)
  {
    messageLine.Wrap(it->message);
    messagePoint.Y() -= messageLine.Height();
    if(messagePoint.Y() < messageBox.Top())
      break;
    float alpha = (it->step + 1000 - step) * .001f;
    Colour colour(alpha, 0.f);
    messageLine.Draw(messagePoint, colour);
  }
  
  // Draw crosshairs around anything that is targeted.
  for(const Target &target : targets)
  {
    Angle a = target.angle;
    Angle da(360. / target.count);
    
    for(int i = 0; i < target.count; ++i)
    {
      PointerShader::Draw(target.centre * zoom, a.Unit(), 12.f, 14.f, -target.radius * zoom,
        Radar::GetColour(target.type));
      a += da;
    }
  }
  
  // Draw the heads-up display.
  interface->Draw(info);
  if(interface->HasPoint("radar"))
  {
    radar[drawTickTock].Draw(
      interface->GetPoint("radar"),
      RADAR_SCALE,
      interface->GetValue("radar radius"),
      interface->GetValue("radar pointer radius"));
  }
  if(interface->HasPoint("target") && targetVector.Length() > 20.)
  {
    Point centre = interface->GetPoint("target");
    double radius = interface->GetValue("target radius");
    PointerShader::Draw(centre, targetVector.Unit(), 10.f, 10.f, radius, Colour(1.));
  }
  
  // Draw the faction markers.
  if(targetSwizzle >= 0 && interface->HasPoint("faction markers"))
  {
    int width = font.Width(info.GetString("target government").first);
    Point centre = interface->GetPoint("faction markers");
    
    const Sprite *mark[2] = {SpriteSet::Get("ui/faction left"), SpriteSet::Get("ui/faction right")};
    // Round the x offsets to whole numbers so the icons are sharp.
    double dx[2] = {(width + mark[0]->Width() + 1) / -2, (width + mark[1]->Width() + 1) / 2};
    for(int i = 0; i < 2; ++i)
      SpriteShader::Draw(mark[i], centre + Point(dx[i], 0.), 1., targetSwizzle);
  }
  if(jumpCount && Preferences::Has("Show mini-map"))
    MapPanel::DrawMiniMap(player, .5f * min(1.f, jumpCount / 30.f), jumpInProgress, step);
  
  // Draw ammo status.
  static const double ICON_SIZE = 30.;
  static const double AMMO_WIDTH = 80.;
  Rectangle ammoBox = interface->GetBox("ammo");
  // Pad the ammo list by the same amount on all four sides.
  double ammoPad = .5 * (ammoBox.Width() - AMMO_WIDTH);
  const Sprite *selectedSprite = SpriteSet::Get("ui/ammo selected");
  const Sprite *unselectedSprite = SpriteSet::Get("ui/ammo unselected");
  Colour selectedColour = *colours.Get("bright");
  Colour unselectedColour = *colours.Get("dim");
  
  // This is the top left corner of the ammo display.
  Point pos(ammoBox.Left() + ammoPad, ammoBox.Bottom() - ammoPad);
  // These offsets are relative to that corner.
  Point boxOff(AMMO_WIDTH - .5 * selectedSprite->Width(), .5 * ICON_SIZE);
  Point textOff(AMMO_WIDTH - .5 * ICON_SIZE, .5 * (ICON_SIZE - font.Height()));
  Point iconOff(.5 * ICON_SIZE, .5 * ICON_SIZE);
  for(const pair<const Outfit *, int> &it : ammo)
  {
    pos.Y() -= ICON_SIZE;
    if(pos.Y() < ammoBox.Top() + ammoPad)
      break;
    
    bool isSelected = it.first == player.SelectedWeapon();
    
    SpriteShader::Draw(it.first->Icon(), pos + iconOff);
    SpriteShader::Draw(isSelected ? selectedSprite : unselectedSprite, pos + boxOff);
    
    // Some secondary weapons may not have limited ammo. In that case, just
    // show the icon without a number.
    if(it.second < 0)
      continue;
    
    string amount = to_string(it.second);
    Point textPos = pos + textOff + Point(-font.Width(amount), 0.);
    font.Draw(amount, textPos, isSelected ? selectedColour : unselectedColour);
  }
  
  // Draw escort status.
  escorts.Draw(interface->GetBox("escorts"));
  
  // Upload any preloaded sprites that are now available. This is to avoid
  // filling the entire backlog of sprites before landing on a planet.
  GameData::Progress();
  
  if(Preferences::Has("Show CPU / GPU load"))
  {
    string loadString = to_string(lround(load * 100.)) + "% CPU";
    Colour colour = *colours.Get("medium");
    font.Draw(loadString,
      Point(-10 - font.Width(loadString), Screen::Height() * -.5 + 5.), colour);
  }
}



// Select the object the player clicked on.
void Engine::Click(const Point &from, const Point &to, bool hasShift)
{
  // First, see if this is a click on an escort icon.
  doClickNextStep = true;
  this->hasShift = hasShift;
  isRightClick = false;
  
  // Determine if the left-click was within the radar display.
  const Interface *interface = GameData::Interfaces().Get("hud");
  Point radarCentre = interface->GetPoint("radar");
  double radarRadius = interface->GetValue("radar radius");
  if(Preferences::Has("Clickable radar display") && (from - radarCentre).Length() <= radarRadius)
    isRadarClick = true;
  else
    isRadarClick = false;
  
  clickPoint = isRadarClick ? from - radarCentre : from;
  if(isRadarClick)
    clickBox = Rectangle::WithCorners(
      (from - radarCentre) / RADAR_SCALE + centre,
      (to - radarCentre) / RADAR_SCALE  + centre);
  else
    clickBox = Rectangle::WithCorners(from / zoom + centre, to / zoom + centre);
}



void Engine::RClick(const Point &point)
{
  doClickNextStep = true;
  hasShift = false;
  isRightClick = true;
  
  // Determine if the right-click was within the radar display, and if so, rescale.
  const Interface *interface = GameData::Interfaces().Get("hud");
  Point radarCentre = interface->GetPoint("radar");
  double radarRadius = interface->GetValue("radar radius");
  if(Preferences::Has("Clickable radar display") && (point - radarCentre).Length() <= radarRadius)
    clickPoint = (point - radarCentre) / RADAR_SCALE;
  else
    clickPoint = point / zoom;
}



void Engine::SelectGroup(int group, bool hasShift, bool hasControl)
{
  groupSelect = group;
  this->hasShift = hasShift;
  this->hasControl = hasControl;
}



void Engine::EnterSystem()
{
  ai.Clean();
  
  Ship *flagship = player.Flagship();
  if(!flagship)
    return;
  
  doEnter = true;
  player.IncrementDate();
  const Date &today = player.GetDate();
  
  const System *system = flagship->GetSystem();
  Audio::PlayMusic(system->MusicName());
  GameData::SetHaze(system->Haze());  
  
  Messages::Add("Entering the " + system->Name() + " system on "
    + today.ToString() + (system->IsInhabited(flagship) ?
      "." : ". No inhabited planets detected."));
  
  // Preload landscapes and determine if the player used a wormhole.
  const StellarObject *usedWormhole = nullptr;
  for(const StellarObject &object : system->Objects())
    if(object.GetPlanet())
    {
      GameData::Preload(object.GetPlanet()->Landscape());
      if(object.GetPlanet()->IsWormhole() && !usedWormhole
          && flagship->Position().Distance(object.Position()) < 1.)
        usedWormhole = &object;
    }
  
  // Advance the positions of every StellarObject and update politics.
  // Remove expired bribes, clearance, and grace periods from past fines.
  GameData::SetDate(today);
  GameData::StepEconomy();
  // SetDate() clears any bribes from yesterday, so restore any auto-clearance.
  for(const Mission &mission : player.Missions())
    if(mission.ClearanceMessage() == "auto")
    {
      mission.Destination()->Bribe(mission.HasFullClearance());
      for(const Planet *planet : mission.Stopovers())
        planet->Bribe(mission.HasFullClearance());
    }
  
  if(usedWormhole)
  {
    // If ships use a wormhole, they are emitted from its centre in
    // its destination system. Player travel causes a date change,
    // thus the wormhole's new position should be used.
    flagship->SetPosition(usedWormhole->Position());
    if(player.HasTravelPlan())
    {
      // Wormhole travel generally invalidates travel plans
      // unless it was planned. For valid travel plans, the
      // next system will be this system, or accessible.
      const System *to = player.TravelPlan().back();
      if(system != to && !flagship->JumpFuel(to))
        player.TravelPlan().clear();
    }
  }
  
  asteroids.Clear();
  for(const System::Asteroid &a : system->Asteroids())
  {
    // Check whether this is a minable or an ordinary asteroid.
    if(a.Type())
      asteroids.Add(a.Type(), a.Count(), a.Energy(), system->AsteroidBelt());
    else
      asteroids.Add(a.Name(), a.Count(), a.Energy());
  }
  
  // Place five seconds worth of fleets. Check for undefined fleets by not
  // trying to create anything with no government set.
  for(int i = 0; i < 5; ++i)
    for(const System::FleetProbability &fleet : system->Fleets())
      if(fleet.Get()->GetGovernment() && Random::Int(fleet.Period()) < 60)
        fleet.Get()->Place(*system, newShips);
  
  const Fleet *raidFleet = system->GetGovernment()->RaidFleet();
  const Government *raidGovernment = raidFleet ? raidFleet->GetGovernment() : nullptr;
  if(raidGovernment && raidGovernment->IsEnemy())
  {
    pair<double, double> factors = player.RaidFleetFactors();
    double attraction = .005 * (factors.first - factors.second - 2.);
    if(attraction > 0.)
      for(int i = 0; i < 10; ++i)
        if(Random::Real() < attraction)
        {
          raidFleet->Place(*system, newShips);
          Messages::Add("Your fleet has attracted the interest of a "
              + raidGovernment->GetName() + " raiding party.");
        }
  }
  
  grudge.clear();
  
  projectiles.clear();
  visuals.clear();
  flotsam.clear();
  // Cancel any projectiles, visuals, or flotsam created by ships this step.
  newProjectiles.clear();
  newVisuals.clear();
  newFlotsam.clear();
  
  // Help message for new players. Show this message for the first four days,
  // since the new player ships can make at most four jumps before landing.
  if(today <= GameData::Start().GetDate() + 4)
  {
    Messages::Add(GameData::HelpMessage("basics 1"));
    Messages::Add(GameData::HelpMessage("basics 2"));
  }
}



// Thread entry point.
void Engine::ThreadEntryPoint()
{
  while(true)
  {
    {
      unique_lock<mutex> lock(swapMutex);
      while(calcTickTock == drawTickTock && !terminate)
        condition.wait(lock);
    
      if(terminate)
        break;
    }
    
    // Do all the calculations.
    CalculateStep();
    
    {
      unique_lock<mutex> lock(swapMutex);
      calcTickTock = drawTickTock;
    }
    condition.notify_one();
  }
}



void Engine::CalculateStep()
{
  FrameTimer loadTimer;
  
  // Clear the list of objects to draw.
  draw[calcTickTock].Clear(step, zoom);
  batchDraw[calcTickTock].Clear(step, zoom);
  radar[calcTickTock].Clear();
  
  if(!player.GetSystem())
    return;
  
  // Now, all the ships must decide what they are doing next.
  ai.Step(player);
  
  // Perform actions for all the game objects. In general this is ordered from
  // bottom to top of the draw stack, but in some cases one object type must
  // "act" before another does.
  
  // The only action stellar objects perform is to launch defence fleets.
  const System *playerSystem = player.GetSystem();
  for(const StellarObject &object : playerSystem->Objects())
    if(object.GetPlanet())
      object.GetPlanet()->DeployDefence(newShips);
  
  // Keep track of the flagship to see if it jumps or enters a wormhole this turn.
  const Ship *flagship = player.Flagship();
  bool wasHyperspacing = (flagship && flagship->IsEnteringHyperspace());
  // Move all the ships.
  for(const shared_ptr<Ship> &it : ships)
    MoveShip(it);
  // If the flagship just began jumping, play the appropriate sound.
  if(!wasHyperspacing && flagship && flagship->IsEnteringHyperspace())
    Audio::Play(Audio::Get(flagship->IsUsingJumpDrive() ? "jump drive" : "hyperdrive"));
  // Check if the flagship just entered a new system.
  if(flagship && playerSystem != flagship->GetSystem())
  {
    // Wormhole travel: mark the wormhole "planet" as visited.
    if(!wasHyperspacing)
      for(const auto &it : playerSystem->Objects())
        if(it.GetPlanet() && it.GetPlanet()->IsWormhole() &&
            it.GetPlanet()->WormholeDestination(playerSystem) == flagship->GetSystem())
          player.Visit(it.GetPlanet());
    
    doFlash = Preferences::Has("Show hyperspace flash");
    playerSystem = flagship->GetSystem();
    player.SetSystem(playerSystem);
    EnterSystem();
  }
  Prune(ships);
  
  // Move the asteroids. This must be done before collision detection. Minables
  // may create visuals or flotsam.
  asteroids.Step(newVisuals, newFlotsam, step);
  
  // Move the flotsam. This must happen after the ships move, because flotsam
  // checks if any ship has picked it up.
  for(const shared_ptr<Flotsam> &it : flotsam)
    it->Move(newVisuals);
  Prune(flotsam);
  
  // Move the projectiles.
  for(Projectile &projectile : projectiles)
    projectile.Move(newVisuals, newProjectiles);
  Prune(projectiles);
  
  // Move the visuals.
  for(Visual &visual : visuals)
    visual.Move();
  Prune(visuals);
  
  // Perform various minor actions.
  SpawnFleets();
  SpawnPersons();
  SendHails();
  HandleMouseClicks();
  
  // Now, take the new objects that were generated this step and splice them
  // on to the ends of the respective lists of objects. These new objects will
  // be drawn this step (and the projectiles will participate in collision
  // detection) but they should not be moved, which is why we put off adding
  // them to the lists until now.
  ships.splice(ships.end(), newShips);
  Append(projectiles, newProjectiles);
  flotsam.splice(flotsam.end(), newFlotsam);
  Append(visuals, newVisuals);
  
  // Decrement the count of how long it's been since a ship last asked for help.
  if(grudgeTime)
    --grudgeTime;
  
  // Populate the collision detection lookup sets.
  FillCollisionSets();
  
  // Perform collision detection.
  for(Projectile &projectile : projectiles)
    DoCollisions(projectile);
  // Now that collision detection is done, clear the cache of ships with anti-
  // missile systems ready to fire.
  hasAntiMissile.clear();
  
  // Check for flotsam collection (collisions with ships).
  for(const shared_ptr<Flotsam> &it : flotsam)
    DoCollection(*it);
  
  // Check for ship scanning.
  for(const shared_ptr<Ship> &it : ships)
    DoScanning(it);
  
  // Draw the objects. Start by figuring out where the view should be centred:
  Point newCentre = centre;
  Point newCentreVelocity;
  if(flagship)
  {
    newCentre = flagship->Position();
    newCentreVelocity = flagship->Velocity();
  }
  draw[calcTickTock].SetCentre(newCentre, newCentreVelocity);
  batchDraw[calcTickTock].SetCentre(newCentre);
  radar[calcTickTock].SetCentre(newCentre);
  
  // Populate the radar.
  FillRadar();
  
  // Draw the planets.
  for(const StellarObject &object : playerSystem->Objects())
    if(object.HasSprite())
    {
      // Don't apply motion blur to very large planets and stars.
      if(object.Width() >= 280.)
        draw[calcTickTock].AddUnblurred(object);
      else
        draw[calcTickTock].Add(object);
    }
  // Draw the asteroids and minables.
  asteroids.Draw(draw[calcTickTock], newCentre, zoom);
  // Draw the flotsam.
  for(const shared_ptr<Flotsam> &it : flotsam)
    draw[calcTickTock].Add(*it);
  // Draw the ships. Skip the flagship, then draw it on top of all the others.
  bool showFlagship = false;
  for(const shared_ptr<Ship> &ship : ships)
    if(ship->GetSystem() == playerSystem && ship->HasSprite())
    {
      if(ship.get() != flagship)
      {
        AddSprites(*ship);
        if(ship->IsThrusting())
        {
          for(const auto &it : ship->Attributes().FlareSounds())
            if(it.second > 0)
              Audio::Play(it.first, ship->Position());
        }
      }
      else
        showFlagship = true;
    }
    
  if(flagship && showFlagship)
  {
    AddSprites(*flagship);
    if(flagship->IsThrusting())
    {
      for(const auto &it : flagship->Attributes().FlareSounds())
        if(it.second > 0)
          Audio::Play(it.first);
    }
  }
  // Draw the projectiles.
  for(const Projectile &projectile : projectiles)
    batchDraw[calcTickTock].Add(projectile, projectile.Clip());
  // Draw the visuals.
  for(const Visual &visual : visuals)
    batchDraw[calcTickTock].Add(visual);
  
  // Keep track of how much of the CPU time we are using.
  loadSum += loadTimer.Time();
  if(++loadCount == 60)
  {
    load = loadSum;
    loadSum = 0.;
    loadCount = 0;
  }
}



// Move a ship. Also determine if the ship should generate hyperspace sounds or
// boarding events, fire weapons, and launch fighters.
void Engine::MoveShip(const shared_ptr<Ship> &ship)
{
  const Ship *flagship = player.Flagship();
  
  bool isJump = ship->IsUsingJumpDrive();
  bool wasHere = (flagship && ship->GetSystem() == flagship->GetSystem());
  bool wasHyperspacing = ship->IsHyperspacing();
  // Give the ship the list of visuals so that it can draw explosions,
  // ion sparks, jump drive flashes, etc.
  ship->Move(newVisuals, newFlotsam);
  // Bail out if the ship just died.
  if(ship->ShouldBeRemoved())
  {
    // Make sure this ship's destruction was recorded, even if it died from
    // self-destruct.
    if(ship->IsDestroyed())
    {
      eventQueue.emplace_back(nullptr, ship, ShipEvent::DESTROY);
      // Any still-docked ships' destruction must be recorded as well.
      for(const auto &bay : ship->Bays())
        if(bay.ship)
          eventQueue.emplace_back(nullptr, bay.ship, ShipEvent::DESTROY);
    }
    return;
  }
  
  // Check if we need to play sounds for a ship jumping in or out of
  // the system. Make no sound if it entered via wormhole.
  if(ship.get() != flagship && ship->Zoom() == 1.)
  {
    // Did this ship just begin hyperspacing?
    if(wasHere && !wasHyperspacing && ship->IsHyperspacing())
      Audio::Play(
        Audio::Get(isJump ? "jump out" : "hyperdrive out"),
        ship->Position());
    
    // Did this ship just jump into the player's system?
    if(!wasHere && flagship && ship->GetSystem() == flagship->GetSystem())
      Audio::Play(
        Audio::Get(isJump ? "jump in" : "hyperdrive in"),
        ship->Position());
  }
  
  // Boarding:
  bool autoPlunder = !ship->IsYours();
  shared_ptr<Ship> victim = ship->Board(autoPlunder);
  if(victim)
    eventQueue.emplace_back(ship, victim,
      ship->GetGovernment()->IsEnemy(victim->GetGovernment()) ?
        ShipEvent::BOARD : ShipEvent::ASSIST);
  
  // The remaining actions can only be performed by ships in the current system.
  if(ship->GetSystem() != player.GetSystem())
    return;
  
  // Launch fighters.
  ship->Launch(newShips, newVisuals);
  
  // Fire weapons. If this returns true the ship has at least one anti-missile
  // system ready to fire.
  if(ship->Fire(newProjectiles, newVisuals))
    hasAntiMissile.push_back(ship.get());
}



// Fill in the collision detection sets, which are used for projectile collision
// and for flotsam collection. Cloaked ships are stored in a separate set because
// they can still be hit by some weapons (e.g. ones with a blast radius) but not
// by most others.
void Engine::FillCollisionSets()
{
  // Populate the collision detection set.
  shipCollisions.Clear(step);
  for(const shared_ptr<Ship> &it : ships)
    if(it->GetSystem() == player.GetSystem() && it->Zoom() == 1.)
      shipCollisions.Add(*it);
  
  // Get the ship collision set ready to query.
  shipCollisions.Finish();
}



// Spawn NPC (both mission and "regular") ships into the player's universe. Non-
// mission NPCs are only spawned in or adjacent to the player's system.
void Engine::SpawnFleets()
{
  // If the player has a pending boarding mission, spawn its NPCs.
  if(player.ActiveBoardingMission())
  {
    Place(player.ActiveBoardingMission()->NPCs(), player.FlagshipPtr());
    player.ClearActiveBoardingMission();
  }
  
  // Non-mission NPCs spawn at random intervals in neighbouring systems,
  // or coming from planets in the current one.
  for(const System::FleetProbability &fleet : player.GetSystem()->Fleets())
    if(!Random::Int(fleet.Period()))
    {
      const Government *gov = fleet.Get()->GetGovernment();
      if(!gov)
        continue;
      
      // Don't spawn a fleet if its allies in-system already far outnumber
      // its enemies. This is to avoid having a system get mobbed with
      // massive numbers of "reinforcements" during a battle.
      int64_t enemyStrength = ai.EnemyStrength(gov);
      if(enemyStrength && ai.AllyStrength(gov) > 2 * enemyStrength)
        continue;
      
      fleet.Get()->Enter(*player.GetSystem(), newShips);
    }
}



// At random intervals, create new special "persons" who enter the current system.
void Engine::SpawnPersons()
{
  if(Random::Int(36000) || player.GetSystem()->Links().empty())
    return;
  
  // Loop through all persons once to see if there are any who can enter
  // this system.
  int sum = 0;
  for(const auto &it : GameData::Persons())
    sum += it.second.Frequency(player.GetSystem());
  // Bail out if there are no eligible persons.
  if(!sum)
    return;
  
  // Adjustment factor: special persons will appear once every ten
  // minutes, but much less frequently if the game only specifies a
  // few of them. This way, they will become more common as I add
  // more, without needing to change the 10-minute constant above.
  sum = Random::Int(sum + 1000);
  for(const auto &it : GameData::Persons())
  {
    const Person &person = it.second;
    sum -= person.Frequency(player.GetSystem());
    if(sum < 0)
    {
      const System *source = nullptr;
      shared_ptr<Ship> parent;
      for(const shared_ptr<Ship> &ship : person.Ships())
      {
        ship->Recharge();
        if(ship->Name().empty())
          ship->SetName(it.first);
        ship->SetGovernment(person.GetGovernment());
        ship->SetPersonality(person.GetPersonality());
        ship->SetHail(person.GetHail());
        if(!parent)
          parent = ship;
        else
          ship->SetParent(parent);
        // Make sure all ships in a "person" definition enter from the
        // same source system.
        source = Fleet::Enter(*player.GetSystem(), *ship, source);
        newShips.push_back(ship);
      }
      
      break;
    }
  }
}



// At random intervals, have one of the ships in the game send you a hail.
void Engine::SendHails()
{
  if(Random::Int(600) || player.IsDead() || ships.empty())
    return;
  
  shared_ptr<Ship> source;
  unsigned i = Random::Int(ships.size());
  for(const shared_ptr<Ship> &it : ships)
    if(!i--)
    {
      source = it;
      break;
    }
  
  if(!CanSendHail(source, player.GetSystem()))
    return;
  
  // Generate a random hail message.
  SendMessage(source, source->GetHail(player));
}



// Handle any mouse clicks. This is done in the calculation thread rather than
// in the main UI thread to avoid race conditions.
void Engine::HandleMouseClicks()
{
  // Mouse clicks can't be issued if your flagship is dead.
  Ship *flagship = player.Flagship();
  if(!doClick || !flagship)
    return;
  
  // Check for clicks on stellar objects. Only left clicks apply, and the
  // flagship must not be in the process of landing or taking off.
  const System *playerSystem = player.GetSystem();
  if(!isRightClick && flagship->Zoom() == 1.)
    for(const StellarObject &object : playerSystem->Objects())
      if(object.HasSprite() && object.GetPlanet())
      {
        // If the player clicked to land on a planet,
        // do so unless already landing elsewhere.
        Point position = object.Position() - centre;
        const Planet *planet = object.GetPlanet();
        if(planet->IsAccessible(flagship) && (clickPoint - position).Length() < object.Radius())
        {
          if(&object == flagship->GetTargetStellar())
          {
            if(!planet->CanLand(*flagship))
              Messages::Add("The authorities on " + planet->Name()
                  + " refuse to let you land.");
            else
            {
              clickCommands |= Command::LAND;
              Messages::Add("Landing on " + planet->Name() + ".");
            }
          }
          else
            flagship->SetTargetStellar(&object);
        }
      }
  
  // Check for clicks on ships in this system.
  double clickRange = 50.;
  shared_ptr<Ship> clickTarget;
  for(shared_ptr<Ship> &ship : ships)
    if(ship->GetSystem() == playerSystem && &*ship != flagship && ship->IsTargetable())
    {
      Point position = ship->Position() - flagship->Position();
      const Mask &mask = ship->GetMask(step);
      double range = mask.Range(clickPoint - position, ship->Facing());
      if(range <= clickRange)
      {
        clickRange = range;
        clickTarget = ship;
        // If we've found an enemy within the click zone, favor
        // targeting it rather than any other ship. Otherwise, keep
        // checking for hits because another ship might be an enemy.
        if(!range && ship->GetGovernment()->IsEnemy())
          break;
      }
    }
  if(clickTarget)
  {
    if(isRightClick)
      ai.IssueShipTarget(player, clickTarget);
    else
    {
      // Left click: has your flagship select or board the target.
      if(clickTarget == flagship->GetTargetShip())
        clickCommands |= Command::BOARD;
      else
      {
        flagship->SetTargetShip(clickTarget);
        if(clickTarget->IsYours())
          player.SelectShip(clickTarget.get(), hasShift);
      }
    }
  }
  else if(isRightClick)
    ai.IssueMoveTarget(player, clickPoint + centre, playerSystem);
  else if(flagship->Attributes().Get("asteroid scan power"))
  {
    // If the click was not on any ship, check if it was on a minable.
    double scanRange = 100. * sqrt(flagship->Attributes().Get("asteroid scan power"));
    for(const shared_ptr<Minable> &minable : asteroids.Minables())
    {
      Point position = minable->Position() - flagship->Position();
      if(position.Length() > scanRange)
        continue;
      
      double range = clickPoint.Distance(position) - minable->Radius();
      if(range <= clickRange)
      {
        clickRange = range;
        flagship->SetTargetAsteroid(minable);
      }
    }
  }
}



// Perform collision detection. Note that unlike the preceding functions, this
// one adds any visuals that are created directly to the main visuals list. If
// this is multi-threaded in the future, that will need to change.
void Engine::DoCollisions(Projectile &projectile)
{
  // The asteroids can collide with projectiles, the same as any other
  // object. If the asteroid turns out to be closer than the ship, it
  // shields the ship (unless the projectile has a blast radius).
  Point hitVelocity;
  double closestHit = 1.;
  shared_ptr<Ship> hit;
  const Government *gov = projectile.GetGovernment();
  
  // If this "projectile" is a ship explosion, it always explodes.
  if(!gov)
    closestHit = 0.;
  else if(projectile.GetWeapon().IsPhasing() && projectile.Target())
  {
    // "Phasing" projectiles that have a target will never hit any other ship.
    shared_ptr<Ship> target = projectile.TargetPtr();
    if(target)
    {
      Point offset = projectile.Position() - target->Position();
      double range = target->GetMask(step).Collide(offset, projectile.Velocity(), target->Facing());
      if(range < 1.)
      {
        closestHit = range;
        hit = target;
      }
    }
  }
  else
  {
    // For weapons with a trigger radius, check if any detectable object will set it off.
    double triggerRadius = projectile.GetWeapon().TriggerRadius();
    if(triggerRadius)
      for(const Body *body : shipCollisions.Circle(projectile.Position(), triggerRadius))
        if(body == projectile.Target() || (gov->IsEnemy(body->GetGovernment())
            && reinterpret_cast<const Ship *>(body)->Cloaking() < 1.))
        {
          closestHit = 0.;
          break;
        }
    
    // If nothing triggered the projectile, check for collisions with ships.
    if(closestHit > 0.)
    {
      Ship *ship = reinterpret_cast<Ship *>(shipCollisions.Line(projectile, &closestHit));
      if(ship)
      {
        hit = ship->shared_from_this();
        hitVelocity = ship->Velocity();
      }
    }
    // "Phasing" projectiles can pass through asteroids. For all other
    // projectiles, check if they've hit an asteroid that is closer than any
    // ship that they have hit.
    if(!projectile.GetWeapon().IsPhasing())
    {
      Body *asteroid = asteroids.Collide(projectile, &closestHit);
      if(asteroid)
      {
        hitVelocity = asteroid->Velocity();
        hit.reset();
      }
    }
  }
  
  // Check if the projectile hit something.
  if(closestHit < 1.)
  {
    // Create the explosion the given distance along the projectile's
    // motion path for this step.
    projectile.Explode(visuals, closestHit, hitVelocity);
    
    // If this projectile has a blast radius, find all ships within its
    // radius. Otherwise, only one is damaged.
    double blastRadius = projectile.GetWeapon().BlastRadius();
    bool isSafe = projectile.GetWeapon().IsSafe();
    if(blastRadius)
    {
      // Even friendly ships can be hit by the blast, unless it is a
      // "safe" weapon.
      Point hitPos = projectile.Position() + closestHit * projectile.Velocity();
      for(Body *body : shipCollisions.Circle(hitPos, blastRadius))
      {
        Ship *ship = reinterpret_cast<Ship *>(body);
        if(isSafe && projectile.Target() != ship && !gov->IsEnemy(ship->GetGovernment()))
          continue;
        
        int eventType = ship->TakeDamage(projectile, ship != hit.get());
        if(eventType)
          eventQueue.emplace_back(gov, ship->shared_from_this(), eventType);
      }
    }
    else if(hit)
    {
      int eventType = hit->TakeDamage(projectile);
      if(eventType)
        eventQueue.emplace_back(gov, hit, eventType);
    }
    
    if(hit)
      DoGrudge(hit, gov);
  }
  else if(projectile.MissileStrength())
  {
    // If the projectile did not hit anything, give the anti-missile systems
    // a chance to shoot it down.
    for(Ship *ship : hasAntiMissile)
      if(ship == projectile.Target() || gov->IsEnemy(ship->GetGovernment()))
        if(ship->FireAntiMissile(projectile, visuals))
        {
          projectile.Kill();
          break;
        }
  }
}



// Check if any ship collected the given flotsam.
void Engine::DoCollection(Flotsam &flotsam)
{
  // Check if any ship can pick up this flotsam. Cloaked ships cannot act.
  Ship *collector = nullptr;
  for(Body *body : shipCollisions.Circle(flotsam.Position(), 5.))
  {
    Ship *ship = reinterpret_cast<Ship *>(body);
    if(!ship->CannotAct() && ship != flotsam.Source() && ship->Cargo().Free() >= flotsam.UnitSize())
    {
      collector = ship;
      break;
    }
  }
  if(!collector)
    return;
  
  // Transfer cargo from the flotsam to the collector ship.
  int amount = flotsam.TransferTo(collector);
  // If the collector is not one of the player's ships, we can bail out now.
  if(!collector->IsYours())
    return;
  
  // One of your ships picked up this flotsam. Describe who it was.
  string name = (!collector->GetParent() ? "You" :
      "Your ship \"" + collector->Name() + "\"") + " picked up ";
  // Describe what they collected from this flotsam.
  string commodity;
  string message;
  if(flotsam.OutfitType())
  {
    const Outfit *outfit = flotsam.OutfitType();
    if(outfit->Get("installable") < 0.)
    {
      commodity = outfit->Name();
      player.Harvest(outfit);
    }
    else
      message = name + to_string(amount) + " "
        + (amount == 1 ? outfit->Name() : outfit->PluralName()) + ".";
  }
  else
    commodity = flotsam.CommodityType();
  
  // If an ordinary commodity or harvestable was collected, describe it in
  // terms of tons, not in terms of units.
  if(!commodity.empty())
  {
    double amountInTons = amount * flotsam.UnitSize();
    message = name + (amountInTons == 1. ? "a ton" : Format::Number(amountInTons) + " tons")
      + " of " + Format::LowerCase(commodity) + ".";
  }
  
  // Unless something went wrong while forming the message, display it.
  if(!message.empty())
  {
    int free = collector->Cargo().Free();
    message += " (" + to_string(free) + (free == 1 ? " ton" : " tons");
    message += " of free space remaining.)";
    Messages::Add(message);
  }
}



// Scanning can't happen in the same loop as ship movement because it relies on
// all the ships already being in their final position for this step.
void Engine::DoScanning(const shared_ptr<Ship> &ship)
{
  int scan = ship->Scan();
  if(scan)
  {
    shared_ptr<Ship> target = ship->GetTargetShip();
    if(target && target->IsTargetable())
      eventQueue.emplace_back(ship, target, scan);
  }
}



// Fill in all the objects in the radar display.
void Engine::FillRadar()
{
  const Ship *flagship = player.Flagship();
  const System *playerSystem = player.GetSystem();
  
  // Add stellar objects.
  for(const StellarObject &object : playerSystem->Objects())
    if(object.HasSprite())
    {
      double r = max(2., object.Radius() * .03 + .5);
      radar[calcTickTock].Add(object.RadarType(flagship), object.Position(), r, r - 1.);
    }
  
  // Add pointers for neighbouring systems.
  if(flagship)
  {
    const System *targetSystem = flagship->GetTargetSystem();
    const set<const System *> &links = (flagship->Attributes().Get("jump drive")) ?
      playerSystem->Neighbours() : playerSystem->Links();
    for(const System *system : links)
      radar[calcTickTock].AddPointer(
        (system == targetSystem) ? Radar::SPECIAL : Radar::INACTIVE,
        system->Position() - playerSystem->Position());
  }
  
  // Add viewport brackets.
  if(!Preferences::Has("Disable viewport on radar"))
  {
    radar[calcTickTock].AddViewportBoundary(Screen::TopLeft() / zoom);
    radar[calcTickTock].AddViewportBoundary(Screen::TopRight() / zoom);
    radar[calcTickTock].AddViewportBoundary(Screen::BottomLeft() / zoom);
    radar[calcTickTock].AddViewportBoundary(Screen::BottomRight() / zoom);
  }
  
  // Add ships. Also check if hostile ships have newly appeared.
  bool hasHostiles = false;
  for(shared_ptr<Ship> &ship : ships)
    if(ship->GetSystem() == playerSystem)
    {
      // Do not show cloaked ships on the radar, except the player's ships.
      bool isYours = ship->IsYours();
      if(ship->Cloaking() >= 1. && !isYours)
        continue;
      
      // Figure out what radar colour should be used for this ship.
      bool isYourTarget = (flagship && ship == flagship->GetTargetShip());
      int type = isYourTarget ? Radar::SPECIAL : RadarType(*ship, step);
      // Calculate how big the radar dot should be.
      double size = sqrt(ship->Width() + ship->Height()) * .14 + .5;
      
      radar[calcTickTock].Add(type, ship->Position(), size);
      
      // Check if this is a hostile ship.
      hasHostiles |= (!ship->IsDisabled() && ship->GetGovernment()->IsEnemy()
        && ship->GetTargetShip() && ship->GetTargetShip()->IsYours());
    }
  // If hostile ships have appeared, play the siren.
  if(alarmTime)
    --alarmTime;
  else if(hasHostiles && !hadHostiles)
  {
    if(Preferences::Has("Warning siren"))
      Audio::Play(Audio::Get("alarm"));
    alarmTime = 180;
    hadHostiles = true;
  }
  else if(!hasHostiles)
    hadHostiles = false;
  
  // Add projectiles that have a missile strength or homing.
  for(Projectile &projectile : projectiles)
  {
    if(projectile.MissileStrength())
    {
      bool isEnemy = projectile.GetGovernment() && projectile.GetGovernment()->IsEnemy();
      radar[calcTickTock].Add(
        isEnemy ? Radar::SPECIAL : Radar::INACTIVE, projectile.Position(), 1.);
    }
    else if(projectile.GetWeapon().BlastRadius())
      radar[calcTickTock].Add(Radar::SPECIAL, projectile.Position(), 1.8);
  }
}



// Each ship is drawn as an entire stack of sprites, including hardpoint sprites
// and engine flares and any fighters it is carrying externally.
void Engine::AddSprites(const Ship &ship)
{
  bool hasFighters = ship.PositionFighters();
  double cloak = ship.Cloaking();
  bool drawCloaked = (cloak && ship.IsYours());
  
  if(hasFighters)
    for(const Ship::Bay &bay : ship.Bays())
      if(bay.side == Ship::Bay::UNDER && bay.ship)
      {
        if(drawCloaked)
          draw[calcTickTock].AddSwizzled(*bay.ship, 56);
        draw[calcTickTock].Add(*bay.ship, cloak);
      }
  
  if(ship.IsThrusting())
    for(const Ship::EnginePoint &point : ship.EnginePoints())
    {
      Point pos = ship.Facing().Rotate(point) * ship.Zoom() + ship.Position();
      // If multiple engines with the same flare are installed, draw up to
      // three copies of the flare sprite.
      for(const auto &it : ship.Attributes().FlareSprites())
        for(int i = 0; i < it.second && i < 3; ++i)
        {
          Body sprite(it.first, pos, ship.Velocity(), ship.Facing(), point.Zoom());
          draw[calcTickTock].Add(sprite, cloak);
        }
    }
  
  if(drawCloaked)
    draw[calcTickTock].AddSwizzled(ship, 56);
  draw[calcTickTock].Add(ship, cloak);
  for(const Hardpoint &hardpoint : ship.Weapons())
    if(hardpoint.GetOutfit() && hardpoint.GetOutfit()->HardpointSprite().HasSprite())
    {
      Body body(
        hardpoint.GetOutfit()->HardpointSprite(),
        ship.Position() + ship.Zoom() * ship.Facing().Rotate(hardpoint.GetPoint()),
        ship.Velocity(),
        ship.Facing() + hardpoint.GetAngle(),
        ship.Zoom());
      draw[calcTickTock].Add(body, cloak);
    }
  
  if(hasFighters)
    for(const Ship::Bay &bay : ship.Bays())
      if(bay.side == Ship::Bay::OVER && bay.ship)
      {
        if(drawCloaked)
          draw[calcTickTock].AddSwizzled(*bay.ship, 7);
        draw[calcTickTock].Add(*bay.ship, cloak);
      }
}



// If a ship just damaged another ship, update information on who has asked the
// player for assistance (and ask for assistance if appropriate).
void Engine::DoGrudge(const shared_ptr<Ship> &target, const Government *attacker)
{
  if(attacker->IsPlayer())
  {
    shared_ptr<const Ship> previous = grudge[target->GetGovernment()].lock();
    if(CanSendHail(previous, player.GetSystem()))
    {
      grudge[target->GetGovernment()].reset();
      SendMessage(previous, "Thank you for your assistance, Captain "
        + player.LastName() + "!");
    }
    return;
  }
  if(grudgeTime)
    return;
  
  // Check who currently has a grudge against this government. Also check if
  // someone has already said "thank you" today.
  if(grudge.count(attacker))
  {
    shared_ptr<const Ship> previous = grudge[attacker].lock();
    // If the previous ship is destroyed, or was able to send a
    // "thank you" already, skip sending a new thanks.
    if(!previous || CanSendHail(previous, player.GetSystem()))
      return;
  }
  
  // If an enemy of the player, or being attacked by those that are
  // not enemies of the player, do not request help.
  if(target->GetGovernment()->IsEnemy() || !attacker->IsEnemy())
    return;
  // Ensure that this attacked ship is able to send hails (e.g. not mute,
  // a player ship, automaton, etc.)
  if(!CanSendHail(target, player.GetSystem()))
    return;
  // If the hailer has a special language, the player must understand it.
  if(!target->GetGovernment()->Language().empty())
    if(!player.GetCondition("language: " + target->GetGovernment()->Language()))
      return;
  
  // No active ship has a grudge already against this government.
  // Check the relative strength of this ship and its attackers.
  double attackerStrength = 0.;
  int attackerCount = 0;
  for(const shared_ptr<Ship> &ship : ships)
    if(ship->GetGovernment() == attacker && ship->GetTargetShip() == target)
    {
      ++attackerCount;
      attackerStrength += (ship->Shields() + ship->Hull()) * ship->Cost();
    }
  
  // Only ask for help if outmatched.
  double targetStrength = (target->Shields() + target->Hull()) * target->Cost();
  if(attackerStrength <= targetStrength)
    return;
  
  // Ask for help more frequently if the battle is very lopsided.
  double ratio = attackerStrength / targetStrength - 1.;
  if(Random::Real() * 10. > ratio)
    return;
  
  grudge[attacker] = target;
  grudgeTime = 120;
  string message;
  if(target->GetPersonality().IsHeroic())
  {
    message = "Please assist us in destroying ";
    message += (attackerCount == 1 ? "this " : "these ");
    message += attacker->GetName();
    message += (attackerCount == 1 ? " ship." : " ships.");
  }
  else
  {
    message = "We are under attack by ";
    if(attackerCount == 1)
      message += "a ";
    message += attacker->GetName();
    message += (attackerCount == 1 ? " ship" : " ships");
    message += ". Please assist us!";
  }
  SendMessage(target, message);
}



// Constructor for the ship status display rings.
Engine::Status::Status(const Point &position, double outer, double inner, double radius, int type, double angle)
  : position(position), outer(outer), inner(inner), radius(radius), type(type), angle(angle)
{
}
