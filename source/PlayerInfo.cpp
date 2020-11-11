// PlayerInfo.cpp

#include "PlayerInfo.h"

#include "Audio.h"
#include "ConversationPanel.h"
#include "DataFile.h"
#include "DataWriter.h"
#include "Dialogue.h"
#include "Files.h"
#include "Format.h"
#include "GameData.h"
#include "Government.h"
#include "Hardpoint.h"
#include "Messages.h"
#include "Mission.h"
#include "Outfit.h"
#include "Person.h"
#include "Planet.h"
#include "Politics.h"
#include "Preferences.h"
#include "Random.h"
#include "SavedGame.h"
#include "Ship.h"
#include "ShipEvent.h"
#include "StartConditions.h"
#include "StellarObject.h"
#include "System.h"
#include "UI.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

using namespace std;



// Completely clear all loaded information, to prepare for loading a file or
// creating a new pilot.
void PlayerInfo::Clear()
{
  *this = PlayerInfo();

  Random::Seed(time(nullptr));
  GameData::Revert();
  Messages::Reset();
}



// Check if a player has been loaded.
bool PlayerInfo::IsLoaded() const
{
  return !firstName.empty();
}



// Make a new player.
void PlayerInfo::New()
{
  // Clear any previously loaded data.
  Clear();

  const StartConditions &start = GameData::Start();
  // Copy any ships in the start conditions.
  for(const Ship &ship : start.Ships())
  {
    ships.emplace_back(new Ship(ship));
    ships.back()->SetSystem(start.GetSystem());
    ships.back()->SetPlanet(start.GetPlanet());
    ships.back()->SetIsSpecial();
    ships.back()->SetIsYours();
    ships.back()->SetGovernment(GameData::PlayerGovernment());
  }
  // Load starting conditions from a "start" item in the data files. If no
  // such item exists, StartConditions defines default values.
  date = start.GetDate();
  GameData::SetDate(date);
  // Make sure the fleet depreciation object knows it is tracking the player's
  // fleet, not the planet's stock.
  depreciation.Init(ships, date.DaysSinceEpoch());

  SetSystem(start.GetSystem());
  SetPlanet(start.GetPlanet());
  accounts = start.GetAccounts();
  Ship::player = &accounts;
  start.GetConditions().Apply(conditions);
  UpdateAutoConditions();

  // Generate missions that will be available on the first day.
  CreateMissions();

  // Add to the list of events that should happen on certain days.
  for(const auto &it : GameData::Events())
    if(it.second.GetDate())
      AddEvent(it.second, it.second.GetDate());
}



// Load player information from a saved game file.
void PlayerInfo::Load(const string &path)
{
  // Make sure any previously loaded data is cleared.
  Clear();

  filePath = path;
  DataFile file(path);

  hasFullClearance = false;
  for(const DataNode &child : file)
  {
    // Basic player information and persistent UI settings:
    if(child.Token(0) == "pilot" && child.Size() >= 3)
    {
      firstName = child.Token(1);
      lastName = child.Token(2);
    }
    else if(child.Token(0) == "date" && child.Size() >= 4)
      date = Date(child.Value(1), child.Value(2), child.Value(3));
    else if(child.Token(0) == "system" && child.Size() >= 2)
      system = GameData::Systems().Get(child.Token(1));
    else if(child.Token(0) == "planet" && child.Size() >= 2)
      planet = GameData::Planets().Get(child.Token(1));
    else if(child.Token(0) == "clearance")
      hasFullClearance = true;
    else if(child.Token(0) == "launching")
      shouldLaunch = true;
    else if(child.Token(0) == "travel" && child.Size() >= 2)
    {
      const System *next = GameData::Systems().Find(child.Token(1));
      if(next)
        travelPlan.push_back(next);
    }
    else if(child.Token(0) == "travel destination" && child.Size() >= 2)
      travelDestination = GameData::Planets().Find(child.Token(1));
    else if(child.Token(0) == "map colouring" && child.Size() >= 2)
      mapColouring = child.Value(1);
    else if(child.Token(0) == "map zoom" && child.Size() >= 2)
      mapZoom = child.Value(1);
    else if(child.Token(0) == "collapsed" && child.Size() >= 2)
    {
      for(const DataNode &grand : child)
        collapsed[child.Token(1)].insert(grand.Token(0));
    }
    else if(child.Token(0) == "reputation with")
    {
      for(const DataNode &grand : child)
        if(grand.Size() >= 2)
          reputationChanges.emplace_back(
            GameData::Governments().Get(grand.Token(0)), grand.Value(1));
    }

    // Records of things you own:
    else if(child.Token(0) == "ship")
    {
      // Ships owned by the player have various special characteristics:
      ships.push_back(shared_ptr<Ship>(new Ship()));
      ships.back()->Load(child);
      ships.back()->SetIsSpecial();
      ships.back()->FinishLoading(false);
      ships.back()->SetIsYours();
    }
    else if(child.Token(0) == "groups" && child.Size() >= 2 && !ships.empty())
      groups[ships.back().get()] = child.Value(1);
    else if(child.Token(0) == "account")
      accounts.Load(child);
    else if(child.Token(0) == "cargo")
      cargo.Load(child);
    else if(child.Token(0) == "basis")
    {
      for(const DataNode &grand : child)
        if(grand.Size() >= 2)
          costBasis[grand.Token(0)] += grand.Value(1);
    }
    else if(child.Token(0) == "stock")
    {
      for(const DataNode &grand : child)
        if(grand.Size() >= 2)
          stock[GameData::Outfits().Get(grand.Token(0))] += grand.Value(1);
    }
    else if(child.Token(0) == "fleet depreciation")
      depreciation.Load(child);
    else if(child.Token(0) == "stock depreciation")
      stockDepreciation.Load(child);

    // Records of things you have done or are doing, or have happened to you:
    else if(child.Token(0) == "mission")
    {
      missions.emplace_back(child);
      cargo.AddMissionCargo(&missions.back());
    }
    else if(child.Token(0) == "available job")
      availableJobs.emplace_back(child);
    else if(child.Token(0) == "available mission")
      availableMissions.emplace_back(child);
    else if(child.Token(0) == "conditions")
    {
      for(const DataNode &grand : child)
        conditions[grand.Token(0)] = (grand.Size() >= 2) ? grand.Value(1) : 1;
    }
    else if(child.Token(0) == "event")
      gameEvents.emplace_back(child);
    else if(child.Token(0) == "changes")
    {
      for(const DataNode &grand : child)
        dataChanges.push_back(grand);
    }
    else if(child.Token(0) == "economy")
      economy = child;
    else if(child.Token(0) == "destroyed" && child.Size() >= 2)
      destroyedPersons.push_back(child.Token(1));

    // Records of things you have discovered:
    else if(child.Token(0) == "visited" && child.Size() >= 2)
      Visit(GameData::Systems().Find(child.Token(1)));
    else if(child.Token(0) == "visited planet" && child.Size() >= 2)
      Visit(GameData::Planets().Find(child.Token(1)));
    else if(child.Token(0) == "harvested")
    {
      for(const DataNode &grand : child)
        if(grand.Size() >= 2)
        {
          auto item = make_pair(
            GameData::Systems().Find(grand.Token(0)),
            GameData::Outfits().Get(grand.Token(1)));
          if(item.first)
            harvested.insert(item);
        }
    }
    else if(child.Token(0) == "logbook")
    {
      for(const DataNode &grand : child)
      {
        if(grand.Size() >= 3)
        {
          Date date(grand.Value(0), grand.Value(1), grand.Value(2));
          string text;
          for(const DataNode &great : grand)
          {
            if(!text.empty())
              text += "\n\t";
            text += great.Token(0);
          }
          logbook.emplace(date, text);
        }
        else if(grand.Size() >= 2)
        {
          string &text = specialLogs[grand.Token(0)][grand.Token(1)];
          for(const DataNode &great : grand)
          {
            if(!text.empty())
              text += "\n\t";
            text += great.Token(0);
          }
        }
      }
    }
  }
  // Based on the ships that were loaded, calculate the player's capacity for
  // cargo and passengers.
  UpdateCargoCapacities();

  // Strip anything after the "~" from snapshots, so that the file we save
  // will be the auto-save, not the snapshot.
  size_t pos = filePath.find('~');
  size_t namePos = filePath.length() - Files::Name(filePath).length();
  if(pos != string::npos && pos > namePos)
    filePath = filePath.substr(0, pos) + ".txt";

  // If a system was not specified in the player data, but the flagship is in
  // a particular system, set the system to that.
  if(!planet && !ships.empty())
  {
    for(shared_ptr<Ship> &ship : ships)
      if(ship->GetPlanet() && !ship->IsDisabled() && !ship->IsParked() && !ship->CanBeCarried())
      {
        planet = ship->GetPlanet();
        system = ship->GetSystem();
        break;
      }
  }

  // If no depreciation record was loaded, every item in the player's fleet
  // will count as non-depreciated.
  if(!depreciation.IsLoaded())
    depreciation.Init(ships, date.DaysSinceEpoch());

  // Modify the game data with any changes that were loaded from this file.
  ApplyChanges();

  Ship::player = &accounts;
}



// Load the most recently saved player (if any). Returns false when no save was loaded.
bool PlayerInfo::LoadRecent()
{
  string recentPath = Files::Read(Files::Config() + "recent.txt");
  // Trim trailing whitespace (including newlines) from the path.
  while(!recentPath.empty() && recentPath.back() <= ' ')
    recentPath.pop_back();

  if(recentPath.empty() || !Files::Exists(recentPath))
  {
    Clear();
    return false;
  }

  Load(recentPath);
  return true;
}



// Save this player. The file name is based on the player's name.
void PlayerInfo::Save() const
{
  // Don't save dead players or players that are not fully created.
  if(!CanBeSaved())
    return;

  // Remember that this was the most recently saved player.
  Files::Write(Files::Config() + "recent.txt", filePath + '\n');

  if(filePath.rfind(".txt") == filePath.length() - 4)
  {
    // Only update the backups if this save will have a newer date.
    SavedGame saved(filePath);
    if(saved.GetDate() != date.ToString())
    {
      string root = filePath.substr(0, filePath.length() - 4);
      string files[4] = {
        root + "~~previous-3.txt",
        root + "~~previous-2.txt",
        root + "~~previous-1.txt",
        filePath
      };
      for(int i = 0; i < 3; ++i)
        if(Files::Exists(files[i + 1]))
          Files::Move(files[i + 1], files[i]);
    }
  }

  Save(filePath);
}



// Get the base file name for the player, without the ".txt" extension. This
// will usually be "<first> <last>", but may be different if multiple players
// exist with the same name, in which case a number is appended.
string PlayerInfo::Identifier() const
{
  string name = Files::Name(filePath);
  return (name.length() < 4) ? "" : name.substr(0, name.length() - 4);
}



// Apply the given set of changes to the game data.
void PlayerInfo::AddChanges(list<DataNode> &changes)
{
  bool changedSystems = false;
  for(const DataNode &change : changes)
  {
    changedSystems |= (change.Token(0) == "system");
    changedSystems |= (change.Token(0) == "link");
    changedSystems |= (change.Token(0) == "unlink");
    GameData::Change(change);
  }
  if(changedSystems)
  {
    // Recalculate what systems have been seen.
    GameData::UpdateNeighbours();
    seen.clear();
    for(const System *system : visitedSystems)
    {
      seen.insert(system);
      for(const System *neighbour : system->Neighbours())
        seen.insert(neighbour);
    }
  }

  // Only move the changes into my list if they are not already there.
  if(&changes != &dataChanges)
    dataChanges.splice(dataChanges.end(), changes);
}



// Add an event that will happen at the given date.
void PlayerInfo::AddEvent(const GameEvent &event, const Date &date)
{
  gameEvents.push_back(event);
  gameEvents.back().SetDate(date);
}



// Mark this player as dead, and handle the changes to the player's fleet.
void PlayerInfo::Die(int response, const shared_ptr<Ship> &capturer)
{
  isDead = true;
  // The player loses access to all their ships if they die on a planet.
  if(GetPlanet() || !flagship)
    ships.clear();
  // If the flagship should explode due to choices made in a mission's
  // conversation, it should still appear in the player's ship list (but
  // will be red, because it is dead). The player's escorts will scatter
  // automatically, as they have a now-dead parent.
  else if(response == Conversation::EXPLODE)
    flagship->Destroy();
  // If it died in open combat, it is already marked destroyed.
  else if(!flagship->IsDestroyed())
  {
    // The player died due to the failed capture of an NPC or a
    // "mutiny". The flagship is either captured or changes government.
    if(!flagship->IsYours())
    {
      // The flagship was already captured, via BoardingPanel,
      // and its parent-escort relationships were updated in
      // Ship::WasCaptured().
    }
    // The referenced ship may not be boarded by the player, so before
    // letting it capture the flagship it must be near the flagship.
    else if(capturer && capturer->Position().Distance(flagship->Position()) <= 1.)
      flagship->WasCaptured(capturer);
    else
    {
      // A "mutiny" occurred.
      flagship->SetIsYours(false);
      // TODO: perhaps allow missions to set the new government.
      flagship->SetGovernment(GameData::Governments().Get("Independent"));
      // Your escorts do not follow it, nor does it wait for them.
      for(const shared_ptr<Ship> &ship : ships)
        ship->SetParent(nullptr);
    }
    // Remove the flagship from the player's ship list.
    auto it = find(ships.begin(), ships.end(), flagship);
    if(it != ships.end())
      ships.erase(it);
  }
}



// Query whether this player is dead.
bool PlayerInfo::IsDead() const
{
  return isDead;
}



// Get the player's first name.
const string &PlayerInfo::FirstName() const
{
  return firstName;
}



// Get the player's last name.
const string &PlayerInfo::LastName() const
{
  return lastName;
}



// Set the player's name. This will also set the saved game file name.
void PlayerInfo::SetName(const string &first, const string &last)
{
  firstName = first;
  lastName = last;

  string fileName = first + " " + last;

  // If there are multiple pilots with the same name, append a number to the
  // pilot name to generate a unique file name.
  filePath = Files::Saves() + fileName;
  int index = 0;
  while(true)
  {
    string path = filePath;
    if(index++)
      path += " " + to_string(index);
    path += ".txt";

    if(!Files::Exists(path))
    {
      filePath.swap(path);
      break;
    }
  }
}



// Get the current date (game world, not real world).
const Date &PlayerInfo::GetDate() const
{
  return date;
}



// Set the date to the next day, and perform all daily actions.
void PlayerInfo::IncrementDate()
{
  ++date;
  conditions["day"] = date.Day();
  conditions["month"] = date.Month();
  conditions["year"] = date.Year();

  // Check if any special events should happen today.
  auto it = gameEvents.begin();
  while(it != gameEvents.end())
  {
    if(date < it->GetDate())
      ++it;
    else
    {
      it->Apply(*this);
      it = gameEvents.erase(it);
    }
  }

  // Check if any missions have failed because of deadlines.
  for(Mission &mission : missions)
    if(mission.CheckDeadline(date) && mission.IsVisible())
      Messages::Add("You failed to meet the deadline for the mission \"" + mission.Name() + "\".");

  // Check what salaries and tribute the player receives.
  int64_t total[2] = {0, 0};
  static const string prefix[2] = {"salary: ", "tribute: "};
  for(int i = 0; i < 2; ++i)
  {
    auto it = conditions.lower_bound(prefix[i]);
    for( ; it != conditions.end() && !it->first.compare(0, prefix[i].length(), prefix[i]); ++it)
      total[i] += it->second;
  }
  if(total[0] || total[1])
  {
    string message = "You receive ";
    if(total[0])
      message += Format::Credits(total[0]) + " credits salary";
    if(total[0] && total[1])
      message += " and ";
    if(total[1])
      message += Format::Credits(total[1]) + " credits in tribute";
    message += ".";
    Messages::Add(message);
    accounts.AddCredits(total[0] + total[1]);
  }

  // For accounting, keep track of the player's net worth. This is for
  // calculation of yearly income to determine maximum mortgage amounts.
  int64_t assets = depreciation.Value(ships, date.DaysSinceEpoch());
  for(const shared_ptr<Ship> &ship : ships)
    assets += ship->Cargo().Value(system);

  // Have the player pay salaries, mortgages, etc. and print a message that
  // summarizes the payments that were made.
  string message = accounts.Step(assets, Salaries(), Maintenance());
  if(!message.empty())
    Messages::Add(message);

  // Reset the reload counters for all your ships.
  for(const shared_ptr<Ship> &ship : ships)
    ship->GetArmament().ReloadAll();

  // Re-calculate all automatic conditions.
  UpdateAutoConditions();
}



// Set the player's current start system, and mark that system as visited.
void PlayerInfo::SetSystem(const System *system)
{
  this->system = system;
  Visit(system);
}



// Get the player's current star system.
const System *PlayerInfo::GetSystem() const
{
  return system;
}



// Set the planet the player is landed on.
void PlayerInfo::SetPlanet(const Planet *planet)
{
  this->planet = planet;
}



// Get the planet the player is landed on.
const Planet *PlayerInfo::GetPlanet() const
{
  return planet;
}



// If the player is landed, return the stellar object they are on. Some planets
// (e.g. ringworlds) may include multiple stellar objects in the same system.
const StellarObject *PlayerInfo::GetStellarObject() const
{
  if(!system || !planet)
    return nullptr;

  double closestDistance = numeric_limits<double>::infinity();
  const StellarObject *closestObject = nullptr;
  for(const StellarObject &object : system->Objects())
    if(object.GetPlanet() == planet)
    {
      if(!Flagship())
        return &object;

      double distance = Flagship()->Position().Distance(object.Position());
      if(distance < closestDistance)
      {
        closestDistance = distance;
        closestObject = &object;
      }
    }
  return closestObject;
}



// Check if the player must take off immediately.
bool PlayerInfo::ShouldLaunch() const
{
  return shouldLaunch;
}



// Access the player's account information.
const Account &PlayerInfo::Accounts() const
{
  return accounts;
}



// Access the player's account information (and allow modifying it).
Account &PlayerInfo::Accounts()
{
  return accounts;
}



// Calculate how much the player pays in daily salaries.
int64_t PlayerInfo::Salaries() const
{
  // Don't count extra crew on anything but the flagship.
  int64_t crew = 0;
  const Ship *flagship = Flagship();
  if(flagship)
    crew = flagship->Crew() - flagship->RequiredCrew();

  // A ship that is "parked" remains on a planet and requires no salaries.
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && !ship->IsDestroyed())
      crew += ship->RequiredCrew();
  if(!crew)
    return 0;

  // Every crew member except the player receives 100 credits per day.
  return 100 * (crew - 1);
}



// Calculate the daily maintenance cost for all ships and in cargo outfits.
int64_t PlayerInfo::Maintenance() const
{
  int64_t maintenance = 0;
  // If the player is landed, then cargo will be in the player's
  // pooled cargo. Check there so that the bank panel can display the
  // correct total maintenance costs. When launched all cargo will be
  // in the player's ships instead of in the pooled cargo, so no outfit
  // will be counted twice.
  for(const auto &outfit : Cargo().Outfits())
    maintenance += max<int64_t>(0, outfit.first->Get("maintenance costs")) * outfit.second;
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsDestroyed())
    {
      maintenance += max<int64_t>(0, ship->Attributes().Get("maintenance costs"));
      for(const auto &outfit : ship->Cargo().Outfits())
        maintenance += max<int64_t>(0, outfit.first->Get("maintenance costs")) * outfit.second;
      if(!ship->IsParked())
        maintenance += max<int64_t>(0, ship->Attributes().Get("operating costs"));
    }
  return maintenance;
}



// Get a pointer to the ship that the player controls. This is always the first
// ship in the list.
const Ship *PlayerInfo::Flagship() const
{
  return const_cast<PlayerInfo *>(this)->FlagshipPtr().get();
}



// Get a pointer to the ship that the player controls. This is always the first
// ship in the list.
Ship *PlayerInfo::Flagship()
{
  return FlagshipPtr().get();
}



// Determine which ship is the flagship and return the shared pointer to it.
const shared_ptr<Ship> &PlayerInfo::FlagshipPtr()
{
  if(!flagship)
  {
    for(const shared_ptr<Ship> &it : ships)
      if(!it->IsParked() && it->GetSystem() == system && it->CanBeFlagship())
      {
        flagship = it;
        break;
      }
  }

  static const shared_ptr<Ship> empty;
  return (flagship && flagship->IsYours()) ? flagship : empty;
}



// Access the full list of ships that the player owns.
const vector<shared_ptr<Ship>> &PlayerInfo::Ships() const
{
  return ships;
}



// Inspect the flightworthiness of the player's active fleet, individually and
// as a whole, to determine which ships cannot travel with the group.
// Returns a mapping of ships to the reason their flight check failed.
 map<const shared_ptr<Ship>, vector<string>> PlayerInfo::FlightCheck() const
{
  // Count of all bay types in the active fleet.
  auto bayCount = map<string, size_t>{};
  // Classification of the present ships by category. Parked ships are ignored.
  auto categoryCount = map<string, vector<shared_ptr<Ship>>>{};

  auto flightChecks = map<const shared_ptr<Ship>, vector<string>>{};
  for(const auto &ship : ships)
    if(ship->GetSystem() == system && !ship->IsDisabled() && !ship->IsParked())
    {
      auto checks = ship->FlightCheck();
      if(!checks.empty())
        flightChecks.emplace(ship, checks);

      categoryCount[ship->Attributes().Category()].emplace_back(ship);
      if(ship->CanBeCarried() || ship->Bays().empty())
        continue;

      for(auto &bay : ship->Bays())
      {
        ++bayCount[bay.forCategory];
        // The bays should always be empty. But if not, count that ship too.
        if(bay.ship)
        {
          Files::LogError("Expected bay to be empty for " + ship->ModelName() + ": " + ship->Name());
          categoryCount[bay.ship->Attributes().Category()].emplace_back(bay.ship);
        }
      }
    }

  // Identify transportable ships that cannot jump and have no bay to be carried in.
  for(auto &bayType : bayCount)
  {
    const auto &shipsOfType = categoryCount[bayType.first];
    if(shipsOfType.empty())
      continue;
    for(const auto &carriable : shipsOfType)
    {
      if(carriable->JumpsRemaining() != 0)
      {
        // This ship can travel between systems and does not require a bay.
      }
      // This ship requires a bay to travel between systems.
      else if(bayType.second > 0)
        --bayType.second;
      else
      {
        // Include the lack of bay availability amongst any other
        // warnings for this carriable ship.
        auto it = flightChecks.find(carriable);
        string warning = "no bays?";
        if(it != flightChecks.end())
          it->second.emplace_back(warning);
        else
          flightChecks.emplace(carriable, vector<string>{warning});
      }
    }
  }
  return flightChecks;
}



// Add a captured ship to your fleet.
void PlayerInfo::AddShip(const shared_ptr<Ship> &ship)
{
  ships.push_back(ship);
  ship->SetIsSpecial();
  ship->SetIsYours();
}



// Buy a ship of the given model, and give it the given name.
void PlayerInfo::BuyShip(const Ship *model, const string &name)
{
  if(!model)
    return;

  int day = date.DaysSinceEpoch();
  int64_t cost = stockDepreciation.Value(*model, day);
  if(accounts.Credits() >= cost)
  {
    ships.push_back(shared_ptr<Ship>(new Ship(*model)));
    ships.back()->SetName(name);
    ships.back()->SetSystem(system);
    ships.back()->SetPlanet(planet);
    ships.back()->SetIsSpecial();
    ships.back()->SetIsYours();
    ships.back()->SetGovernment(GameData::PlayerGovernment());

    accounts.AddCredits(-cost);
    flagship.reset();

    // Record the transfer of this ship in the depreciation and stock info.
    depreciation.Buy(*model, day, &stockDepreciation);
    for(const auto &it : model->Outfits())
      stock[it.first] -= it.second;
  }
}



// Sell the given ship (if it belongs to the player).
void PlayerInfo::SellShip(const Ship *selected)
{
  for(auto it = ships.begin(); it != ships.end(); ++it)
    if(it->get() == selected)
    {
      int day = date.DaysSinceEpoch();
      int64_t cost = depreciation.Value(*selected, day);

      // Record the transfer of this ship in the depreciation and stock info.
      stockDepreciation.Buy(*selected, day, &depreciation);
      for(const auto &it : selected->Outfits())
        stock[it.first] += it.second;

      accounts.AddCredits(cost);
      ships.erase(it);
      flagship.reset();
      return;
    }
}



void PlayerInfo::DisownShip(const Ship *selected)
{
  for(auto it = ships.begin(); it != ships.end(); ++it)
    if(it->get() == selected)
    {
      ships.erase(it);
      flagship.reset();
      return;
    }
}



// Park or unpark the given ship. A parked ship remains on a planet instead of
// flying with the player, and requires no crew, nor daily salary payments.
void PlayerInfo::ParkShip(const Ship *selected, bool isParked)
{
  for(auto it = ships.begin(); it != ships.end(); ++it)
    if(it->get() == selected)
    {
      isParked &= !(*it)->IsDisabled();
      (*it)->SetIsParked(isParked);
      UpdateCargoCapacities();
      flagship.reset();
      return;
    }
}



// Rename the given ship.
void PlayerInfo::RenameShip(const Ship *selected, const string &name)
{
  for(auto it = ships.begin(); it != ships.end(); ++it)
    if(it->get() == selected)
    {
      (*it)->SetName(name);
      return;
    }
}



// Change the order of the given ship in the list.
void PlayerInfo::ReorderShip(int fromIndex, int toIndex)
{
  // Make sure the indices are valid.
  if(fromIndex == toIndex)
    return;
  if(static_cast<unsigned>(fromIndex) >= ships.size())
    return;
  if(static_cast<unsigned>(toIndex) >= ships.size())
    return;

  // Reorder the list.
  shared_ptr<Ship> ship = ships[fromIndex];
  ships.erase(ships.begin() + fromIndex);
  ships.insert(ships.begin() + toIndex, ship);
  flagship.reset();
}



int PlayerInfo::ReorderShips(const set<int> &fromIndices, int toIndex)
{
  if(fromIndices.empty() || static_cast<unsigned>(toIndex) >= ships.size())
    return -1;

  // When shifting ships up in the list, move to the desired index. If
  // moving down, move after the selected index.
  int direction = (*fromIndices.begin() < toIndex) ? 1 : 0;

  // Remove the ships from last to first, so that each removal leaves all the
  // remaining indices in the set still valid.
  vector<shared_ptr<Ship>> removed;
  for(set<int>::const_iterator it = fromIndices.end(); it != fromIndices.begin(); )
  {
    // The "it" pointer doesn't point to the beginning of the list, so it is
    // safe to decrement it here.
    --it;

    // Bail out if any invalid indices are encountered.
    if(static_cast<unsigned>(*it) >= ships.size())
      return -1;

    removed.insert(removed.begin(), ships[*it]);
    ships.erase(ships.begin() + *it);
    // If this index is before the insertion point, removing it causes the
    // insertion point to shift back one space.
    if(*it < toIndex)
      --toIndex;
  }
  // Make sure the insertion index is within the list.
  toIndex = min<int>(toIndex + direction, ships.size());
  ships.insert(ships.begin() + toIndex, removed.begin(), removed.end());
  flagship.reset();

  return toIndex;
}



// Find out how attractive the player's fleet is to pirates. Aside from a
// heavy freighter, no single ship should attract extra pirate attention.
pair<double, double> PlayerInfo::RaidFleetFactors() const
{
  double attraction = 0.;
  double deterrence = 0.;
  for(const shared_ptr<Ship> &ship : Ships())
  {
    if(ship->IsParked() || ship->IsDestroyed())
      continue;

    attraction += .4 * sqrt(ship->Attributes().Get("cargo space")) - 1.8;
    for(const Hardpoint &hardpoint : ship->Weapons())
      if(hardpoint.GetOutfit())
      {
        const Outfit *weapon = hardpoint.GetOutfit();
        if(weapon->Ammo() && !ship->OutfitCount(weapon->Ammo()))
          continue;
        double damage = weapon->ShieldDamage() + weapon->HullDamage();
        deterrence += .12 * damage / weapon->Reload();
      }
  }

  return make_pair(attraction, deterrence);
}



// Get cargo information.
CargoHold &PlayerInfo::Cargo()
{
  return cargo;
}



// Get cargo information.
const CargoHold &PlayerInfo::Cargo() const
{
  return cargo;
}



// Adjust the cost basis for the given commodity.
void PlayerInfo::AdjustBasis(const string &commodity, int64_t adjustment)
{
  costBasis[commodity] += adjustment;
}



// Get the cost basis for some number of tons of the given commodity. Each ton
// of the commodity that you own is assumed to have the same basis.
int64_t PlayerInfo::GetBasis(const string &commodity, int tons) const
{
  // Handle cost basis even when not landed on a planet.
  int total = cargo.Get(commodity);
  for(const auto &ship : ships)
    total += ship->Cargo().Get(commodity);
  if(!total)
    return 0;

  auto it = costBasis.find(commodity);
  int64_t basis = (it == costBasis.end()) ? 0 : it->second;
  return (basis * tons) / total;
}



// Switch cargo from being stored in ships to being stored here. Also recharge
// ships, check for mission completion, and apply fines for contraband.
void PlayerInfo::Land(UI *ui)
{
  // This can only be done while landed.
  if(!system || !planet)
    return;

  Audio::Play(Audio::Get("landing"));
  Audio::PlayMusic(planet->MusicName());

  // Mark this planet as visited.
  Visit(planet);
  if(planet == travelDestination)
    travelDestination = nullptr;

  // Remove any ships that have been destroyed or captured.
  map<string, int> lostCargo;
  vector<shared_ptr<Ship>>::iterator it = ships.begin();
  while(it != ships.end())
  {
    if((*it)->IsDestroyed() || !(*it)->IsYours())
    {
      // If any of your ships are destroyed, your cargo "cost basis" should
      // be adjusted based on what you lost.
      for(const auto &cargo : (*it)->Cargo().Commodities())
        if(cargo.second)
          lostCargo[cargo.first] += cargo.second;
      // Also, the ship and everything in it should be removed from your
      // depreciation records. Transfer it to a throw-away record:
      Depreciation().Buy(**it, date.DaysSinceEpoch(), &depreciation);

      it = ships.erase(it);
    }
    else
      ++it; 
  }

  // "Unload" all fighters, so they will get recharged, etc.
  for(const shared_ptr<Ship> &ship : ships)
    ship->UnloadBays();

  // Ships that are landed with you on the planet should fully recharge
  // and pool all their cargo together. Those in remote systems restore
  // what they can without landing. Fuel is also refilled.
  Refuel();
  bool hasSpaceport = planet->HasSpaceport() && planet->CanUseServices();
  UpdateCargoCapacities();
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && !ship->IsDisabled())
    {
      if(ship->GetSystem() == system)
      {
        ship->Recharge(hasSpaceport);
        ship->Cargo().TransferAll(cargo);
        if(!ship->GetPlanet())
          ship->SetPlanet(planet);
      }
      else
        ship->Recharge(false);
    }
  // Adjust cargo cost basis for any cargo lost due to a ship being destroyed.
  for(const auto &it : lostCargo)
    AdjustBasis(it.first, -(costBasis[it.first] * it.second) / (cargo.Get(it.first) + it.second));

  // Bring auto conditions up-to-date for missions to check your current status.
  UpdateAutoConditions();

  // Update missions that are completed, or should be failed.
  StepMissions(ui);
  UpdateCargoCapacities();

  // Create whatever missions this planet has to offer.
  if(!freshlyLoaded)
    CreateMissions();

  // Check if the player is doing anything illegal.
  if(!freshlyLoaded)
    Fine(ui);

  // Hire extra crew back if any were lost in-flight (i.e. boarding) or
  // some bunks were freed up upon landing (i.e. completed missions).
  if(Preferences::Has("Rehire extra crew when lost") && hasSpaceport && flagship)
  {
    int added = desiredCrew - flagship->Crew();
    if(added > 0)
    {
      flagship->AddCrew(added);
      Messages::Add("You hire " + to_string(added) + (added == 1
          ? " extra crew member to fill your now-empty bunk."
          : " extra crew members to fill your now-empty bunks."));
    }
  }

  freshlyLoaded = false;
  flagship.reset();
}



// Load the cargo back into your ships. This may require selling excess, in
// which case a message will be returned.
bool PlayerInfo::TakeOff(UI *ui)
{
  // This can only be done while landed.
  if(!system || !planet)
    return false;

  flagship.reset();
  flagship = FlagshipPtr();
  if(!flagship)
    return false;

  shouldLaunch = false;
  Audio::Play(Audio::Get("takeoff"));

  // Jobs are only available when you are landed.
  availableJobs.clear();
  availableMissions.clear();
  doneMissions.clear();
  stock.clear();

  // Special persons who appeared last time you left the planet, can appear again.
  GameData::ResetPersons();


  // Store the total cargo counts in case we need to adjust cost bases below.
  map<string, int> originalTotals = cargo.Commodities();

  // Move the flagship to the start of your list of ships. It does not make
  // sense that the flagship would change if you are reunited with a different
  // ship that was higher up the list.
  auto it = find(ships.begin(), ships.end(), flagship);
  if(it != ships.begin() && it != ships.end())
  {
    ships.erase(it);
    ships.insert(ships.begin(), flagship);
  }
  // Make sure your jump-capable ships all know who the flagship is.
  for(const shared_ptr<Ship> &ship : ships)
  {
    bool shouldHaveParent = (ship != flagship && !ship->IsParked() && (!ship->CanBeCarried() || ship->JumpFuel()));
    ship->SetParent(shouldHaveParent ? flagship : shared_ptr<Ship>());
  }
  // Make sure your flagship is not included in the escort selection.
  for(auto it = selectedShips.begin(); it != selectedShips.end(); )
  {
    shared_ptr<Ship> ship = it->lock();
    if(!ship || ship == flagship)
      it = selectedShips.erase(it);
    else
      ++it;
  }

  LoadCargo();

  // Recharge any ships that can be recharged, and determines the free bunk space.
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && !ship->IsDisabled())
    {
      ship->Recharge(false);
      if(ship->GetSystem() != system)
      {
        if(ship->GetPlanet())
          ship->Refuel(1);
        continue;
      }

      if(ship != flagship)
        ship->Cargo().SetBunks(ship->Attributes().Get("bunks") - ship->RequiredCrew());
      else
      {
        // Your flagship takes first priority for passengers.
        desiredCrew = ship->Crew();
        ship->Cargo().SetBunks(ship->Attributes().Get("bunks") - desiredCrew);
        for(const auto &it : cargo.PassengerList())
          cargo.TransferPassengers(it.first, it.second, ship->Cargo());
      }
    }

  if(cargo.Passengers())
  {
    int extra = min(cargo.Passengers(), flagship->Crew() - flagship->RequiredCrew());
    if(extra)
    {
      flagship->AddCrew(-extra);
      Messages::Add("You fired " + to_string(extra) + " crew members to free up bunks for passengers.");
      flagship->Cargo().SetBunks(flagship->Attributes().Get("bunks") - flagship->Crew());
      cargo.TransferAll(flagship->Cargo());
    }
  }

  int extra = flagship->Crew() + flagship->Cargo().Passengers() - flagship->Attributes().Get("bunks");
  if(extra > 0)
  {
    flagship->AddCrew(-extra);
    Messages::Add("You fired " + to_string(extra) + " crew members because you have no bunks for them.");
    flagship->Cargo().SetBunks(flagship->Attributes().Get("bunks") - flagship->Crew());
  }

  // For each active, carriable ship you own, try to find an active ship that has a bay for it.
  auto carriers = vector<Ship *>{};
  auto toLoad = vector<shared_ptr<Ship>>{};
  for(auto &ship : ships)
    if(!ship->IsParked() && !ship->IsDisabled())
    {
      if(ship->CanBeCarried())
        toLoad.emplace_back(ship);
      else if(ship->HasBays())
        carriers.emplace_back(ship.get());
    }
  if(!toLoad.empty())
  {
    size_t uncarried = toLoad.size();
    if(!carriers.empty())
    {
      // Order carried ships such that those requiring bays are loaded first. For
      // jump-capable carried ships, prefer loading those with a shorter range.
      stable_sort(toLoad.begin(), toLoad.end(),
        [](const shared_ptr<Ship> &a, const shared_ptr<Ship> &b)
        {
          return a->JumpsRemaining() < b->JumpsRemaining();
        });
      // We are guaranteed that each carried `ship` is not parked and not disabled, and that
      // all possible parents are also not parked, not disabled, and not `ship`.
      for(auto &ship : toLoad)
        for(auto &parent : carriers)
          if(parent->GetSystem() == ship->GetSystem() && parent->Carry(ship))
          {
            --uncarried;
            break;
          }
    }

    if(uncarried)
    {
      // The remaining uncarried ships are launched alongside the player.
      string message = (uncarried > 1) ? "Some escorts were" : "One escort was";
      Messages::Add(message + " unable to dock with a carrier.");
    }
  }

  // By now, all cargo should have been divvied up among your ships. So, any
  // mission cargo or passengers left behind cannot be carried, and those
  // missions have failed.
  vector<const Mission *> missionsToRemove;
  for(const auto &it : cargo.MissionCargo())
    if(it.second)
    {
      if(it.first->IsVisible())
        Messages::Add("Mission \"" + it.first->Name()
          + "\" failed because you do not have space for the cargo.");
      missionsToRemove.push_back(it.first);
    }
  for(const auto &it : cargo.PassengerList())
    if(it.second)
    {
      if(it.first->IsVisible())
        Messages::Add("Mission \"" + it.first->Name()
          + "\" failed because you do not have enough passenger bunks free.");
      missionsToRemove.push_back(it.first);

    }
  for(const Mission *mission : missionsToRemove)
    RemoveMission(Mission::FAIL, *mission, ui);

  // Any ordinary cargo left behind can be sold.
  int64_t income = 0;
  int day = date.DaysSinceEpoch();
  int64_t sold = cargo.Used();
  int64_t totalBasis = 0;
  if(sold)
  {
    for(const auto &commodity : cargo.Commodities())
    {
      if(!commodity.second)
        continue;

      // Figure out how much income you get for selling this cargo.
      int64_t value = commodity.second * system->Trade(commodity.first);
      income += value;

      int original = originalTotals[commodity.first];
      auto it = costBasis.find(commodity.first);
      if(!original || it == costBasis.end() || !it->second)
        continue;

      // Now, figure out how much of that income is profit by calculating
      // the cost basis for this cargo (which is just the total cost basis
      // multiplied by the percent of the cargo you are selling).
      int64_t basis = it->second * commodity.second / original;
      it->second -= basis;
      totalBasis += basis;
    }
    for(const auto &outfit : cargo.Outfits())
    {
      // Compute the total value for each type of excess outfit.
      if(!outfit.second)
        continue;
      int64_t cost = depreciation.Value(outfit.first, day, outfit.second);
      for(int i = 0; i < outfit.second; ++i)
        stockDepreciation.Buy(outfit.first, day, &depreciation);
      income += cost;
    }
  }
  accounts.AddCredits(income);
  cargo.Clear();
  stockDepreciation = Depreciation();
  if(sold)
  {
    // Report how much excess cargo was sold, and what profit you earned.
    ostringstream out;
    out << "You sold " << sold << " tons of excess cargo for " << Format::Credits(income) << " credits";
    if(totalBasis && totalBasis != income)
      out << " (for a profit of " << (income - totalBasis) << " credits).";
    else
      out << ".";
    Messages::Add(out.str());
  }

  return true;
}



void PlayerInfo::LoadCargo()
{
  // Load available cargo.
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && !ship->IsDisabled() && ship != flagship)
        cargo.TransferAll(ship->Cargo());
  // Load up your flagship last, so that it will have space free for any
  // plunder that you happen to acquire.
  cargo.TransferAll(flagship->Cargo());
}



// Unloads all cargo.
void PlayerInfo::UnloadCargo()
{
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsDisabled() && ship->GetSystem() == system)
      ship->Cargo().TransferAll(cargo);
}




void PlayerInfo::Refuel()
{
  double price = planet->GetFuelPrice();
  // Refuels only if the price is in the preferred range or 0 or the player is overriding
  // the preferences using the "Refuel All"-button.
  bool rechargesFuel = (price < Preferences::GetMaxPrice() || !price) && price >= 0;
  int rechargedFuel = 0;
  bool hasSpaceport = planet->HasSpaceport() && planet->CanUseServices();
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsDisabled())
    {
      if(ship->GetSystem() == system)
      {
        ship->Recharge(hasSpaceport);
        if(rechargesFuel)
          rechargedFuel += ship->Refuel(1);
        ship->Cargo().TransferAll(cargo);
        ship->SetPlanet(planet);
      }
      else
        ship->Recharge(false);
    }

  // Pays the fuel and messages the price that was paid.
  if(rechargedFuel && price > 0)
  {
    int fuelPrice = rechargedFuel * price;
    ostringstream out;
    out << "You paid " << Format::Credits(fuelPrice) << " credits to buy " << Format::Round(rechargedFuel) << " units of fuel.";
    Messages::Add(out.str());
    accounts.AddCredits(-fuelPrice);
  }
}



// Accessed with refuel button.
void PlayerInfo::RefuelRatio(double ratio)
{
  double price = planet->GetFuelPrice();
  int rechargedFuel = 0;
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsDisabled() && ship->GetSystem() == system)
      // This function is only accessed by planets with refuel services.
      rechargedFuel += ship->Refuel(ratio);

  // Pays the fuel and messages the price that was paid.
  if(rechargedFuel && price > 0)
  {
    int fuelPrice = rechargedFuel * price;
    ostringstream out;
    out << "You paid " << Format::Credits(fuelPrice) << " credits to buy " << Format::Round(rechargedFuel) << " units of fuel.";
    Messages::Add(out.str());
    accounts.AddCredits(-fuelPrice);
  }
}



double PlayerInfo::FuelNeeded(double ratio)
{
  double neededFuel = 0;
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsDisabled() && ship->GetSystem() == system)
      neededFuel += ship->FuelMissing(ratio);

  return neededFuel;
}



void PlayerInfo::LoadFighters()
{
  for(const shared_ptr<Ship> &ship : ships)
  {
    if(ship->IsParked() || ship->IsDisabled())
      continue;
    const string &category = ship->Attributes().Category();
    if(category == "Fighter" || category == "Drone")
      for(shared_ptr<Ship> &parent : ships)
        if(parent->GetSystem() == ship->GetSystem() && !parent->IsParked()
            && !parent->IsDisabled() && parent->Carry(ship))
          break;
  }
}



// Unloads all fighters.
void PlayerInfo::UnloadFighters()
{
  for(const shared_ptr<Ship> &ship : ships)
    ship->UnloadBays();
}



// Get the player's logbook.
const multimap<Date, string> &PlayerInfo::Logbook() const
{
  return logbook;
}



void PlayerInfo::AddLogEntry(const string &text)
{
  logbook.emplace(date, text);
}



const map<string, map<string, string>> &PlayerInfo::SpecialLogs() const
{
  return specialLogs;
}



void PlayerInfo::AddSpecialLog(const string &type, const string &name, const string &text)
{
  string &entry = specialLogs[type][name];
  if(!entry.empty())
    entry += "\n\t";
  entry += text;
}



bool PlayerInfo::HasLogs() const
{
  return !logbook.empty() || !specialLogs.empty();
}



// Call this after missions update, or if leaving the outfitter, shipyard, or
// hiring panel. Updates the information on how much space is available.
void PlayerInfo::UpdateCargoCapacities()
{
  int size = 0;
  int bunks = 0;
  flagship = FlagshipPtr();
  for(const shared_ptr<Ship> &ship : ships)
    if(ship->GetSystem() == system && !ship->IsParked() && !ship->IsDisabled())
    {
      size += ship->Attributes().Get("cargo space");
      int crew = (ship == flagship ? ship->Crew() : ship->RequiredCrew());
      bunks += ship->Attributes().Get("bunks") - crew;
    }
  cargo.SetSize(size);
  cargo.SetBunks(bunks);
}



// Get the list of active missions.
const list<Mission> &PlayerInfo::Missions() const
{
  return missions;
}



// Get the list of ordinary jobs that are available on the job board.
const list<Mission> &PlayerInfo::AvailableJobs() const
{
  return availableJobs;
}



// Return a pointer to the mission that was most recently accepted while in-flight.
const Mission *PlayerInfo::ActiveBoardingMission() const
{
  return activeBoardingMission;
}



// Accept the given job.
void PlayerInfo::AcceptJob(const Mission &mission, UI *ui)
{
  for(auto it = availableJobs.begin(); it != availableJobs.end(); ++it)
    if(&*it == &mission)
    {
      cargo.AddMissionCargo(&mission);
      it->Do(Mission::OFFER, *this);
      it->Do(Mission::ACCEPT, *this, ui);
      auto spliceIt = it->IsUnique() ? missions.begin() : missions.end();
      missions.splice(spliceIt, availableJobs, it);
      break;
    }
}



// Look at the list of available missions and see if any of them can be offered
// right now, in the given location (landing or spaceport). If there are no
// missions that can be accepted, return a null pointer.
Mission *PlayerInfo::MissionToOffer(Mission::Location location)
{
  if(ships.empty())
    return nullptr;

  // If a mission can be offered right now, move it to the start of the list
  // so we know what mission the callback is referring to, and return it.
  for(auto it = availableMissions.begin(); it != availableMissions.end(); ++it)
    if(it->IsAtLocation(location) && it->CanOffer(*this) && it->HasSpace(*this))
    {
      availableMissions.splice(availableMissions.begin(), availableMissions, it);
      return &availableMissions.front();
    }
  return nullptr;
}



// Check if any of the game's missions can be offered from this ship, given its
// relationship with the player. If none offer, return nullptr.
Mission *PlayerInfo::BoardingMission(const shared_ptr<Ship> &ship)
{
  // Do not create missions from existing mission NPC's, or the player's ships.
  if(ship->IsSpecial())
    return nullptr;
  // Ensure that boarding this NPC again does not create a mission.
  ship->SetIsSpecial();

   // Update auto conditions to reflect the player's flagship's free capacity. 
  UpdateAutoConditions(true);
   // "boardingMissions" is emptied by MissionCallback, but to be sure: 
  boardingMissions.clear();

  bool isEnemy = ship->GetGovernment()->IsEnemy();
  Mission::Location location = (isEnemy ? Mission::BOARDING : Mission::ASSISTING);

  // Check for available boarding or assisting missions.
  for(const auto &it : GameData::Missions())
    if(it.second.IsAtLocation(location) && it.second.CanOffer(*this, ship))
    {
      boardingMissions.push_back(it.second.Instantiate(*this, ship));
      if(boardingMissions.back().HasFailed(*this))
        boardingMissions.pop_back();
      else
        return &boardingMissions.back();
    }

  return nullptr;
}



// Engine calls this after placing the boarding mission's NPCs.
void PlayerInfo::ClearActiveBoardingMission()
{
  activeBoardingMission = nullptr;
}



// If one of your missions cannot be offered because you do not have enough
// space for it, and it specifies a message to be shown in that situation,
// show that message.
void PlayerInfo::HandleBlockedMissions(Mission::Location location, UI *ui)
{
  list<Mission> &missionList = availableMissions.empty() ? boardingMissions : availableMissions;
   if(ships.empty() || missionList.empty()) 
    return;

   for(auto it = missionList.begin(); it != missionList.end(); ++it)
    if(it->IsAtLocation(location) && it->CanOffer(*this) && !it->HasSpace(*this))
    {
      string message = it->BlockedMessage(*this);
      if(!message.empty())
      {
        ui->Push(new Dialogue(message));
        return;
      }
    }
}



// Callback for accepting or declining whatever mission has been offered.
// Responses which would kill the player are handled before the on offer
// conversation ended.
void PlayerInfo::MissionCallback(int response)
{
  list<Mission> &missionList = availableMissions.empty() ? boardingMissions : availableMissions;
  if(missionList.empty())
    return;

  Mission &mission = missionList.front();

  // If landed, this conversation may require the player to immediately depart.
  shouldLaunch |= (GetPlanet() && Conversation::RequiresLaunch(response));
  if(response == Conversation::ACCEPT || response == Conversation::LAUNCH)
  {
    bool shouldAutosave = mission.RecommendsAutosave();
    if(planet)
    {
      cargo.AddMissionCargo(&mission);
      UpdateCargoCapacities();
    }
    else if(Flagship())
      flagship->Cargo().AddMissionCargo(&mission);
    else
      return;

    // Move this mission from the offering list into the "accepted"
    // list, viewable on the MissionPanel. Unique missions are moved
    // to the front, so they appear at the top of the list if viewed.
    auto spliceIt = mission.IsUnique() ? missions.begin() : missions.end();
    missions.splice(spliceIt, missionList, missionList.begin());
    mission.Do(Mission::ACCEPT, *this);
    if(shouldAutosave)
      Autosave();
    // If this is a mission offered in-flight, expose a pointer to it
    // so Engine::SpawnFleets can add its ships without requiring the
    // player to land.
    if(mission.IsAtLocation(Mission::BOARDING) || mission.IsAtLocation(Mission::ASSISTING))
      activeBoardingMission = &*--spliceIt;
  }
  else if(response == Conversation::DECLINE || response == Conversation::FLEE)
  {
    mission.Do(Mission::DECLINE, *this);
    missionList.pop_front();
  }
  else if(response == Conversation::DEFER || response == Conversation::DEPART)
  {
    mission.Do(Mission::DEFER, *this);
    missionList.pop_front();
  }
}



// Basic callback, allowing conversations to force the player to depart from a
// planet without requiring a mission to offer.
void PlayerInfo::BasicCallback(int response)
{
  // If landed, this conversation may require the player to immediately depart.
  shouldLaunch |= (GetPlanet() && Conversation::RequiresLaunch(response));
}



// Mark a mission for removal, either because it was completed, or it failed,
// or because the player aborted it.
void PlayerInfo::RemoveMission(Mission::Trigger trigger, const Mission &mission, UI *ui)
{
  for(auto it = missions.begin(); it != missions.end(); ++it)
    if(&*it == &mission)
    {
      // Don't delete the mission yet, because the conversation or dialogue
      // panel may still be showing. Instead, just mark it as done. Doing
      // this first avoids the possibility of an infinite loop, e.g. if a
      // mission's "on fail" fails the mission itself.
      doneMissions.splice(doneMissions.end(), missions, it);

      it->Do(trigger, *this, ui);
      cargo.RemoveMissionCargo(&mission);
      for(shared_ptr<Ship> &ship : ships)
        ship->Cargo().RemoveMissionCargo(&mission);
      return;
    }
}



// Mark a mission as failed, but do not remove it from the mission list yet.
void PlayerInfo::FailMission(const Mission &mission)
{
  for(auto it = missions.begin(); it != missions.end(); ++it)
    if(&*it == &mission)
    {
      it->Fail();
      return;
    }
}



// Update mission status based on an event.
void PlayerInfo::HandleEvent(const ShipEvent &event, UI *ui)
{
  // Combat rating increases when you disable an enemy ship.
  if(event.ActorGovernment()->IsPlayer())
    if((event.Type() & ShipEvent::DISABLE) && event.Target())
    {
      auto &rating = conditions["combat rating"];
      static const int64_t maxRating = 2000'000'000;
      rating = min(maxRating, rating + (event.Target()->Cost() + 250'000) / 500'000);
    }

  for(Mission &mission : missions)
    mission.Do(event, *this, ui);

  // If the player's flagship was destroyed, the player is dead.
  if((event.Type() & ShipEvent::DESTROY) && !ships.empty() && event.Target().get() == Flagship())
    Die();
}



// Get the value of the given condition (default 0).
int64_t PlayerInfo::GetCondition(const string &name) const
{
  auto it = conditions.find(name);
  return (it == conditions.end()) ? 0 : it->second;
}



// Get mutable access to the player's list of conditions.
map<string, int64_t> &PlayerInfo::Conditions()
{
  return conditions;
}



// Access the player's list of conditions.
const map<string, int64_t> &PlayerInfo::Conditions() const
{
  return conditions;
}



// Set and check the reputation conditions, which missions and events can use to
// modify the player's reputation with other governments.
void PlayerInfo::SetReputationConditions()
{
  for(const auto &it : GameData::Governments())
  {
    int64_t rep = it.second.Reputation();
    conditions["reputation: " + it.first] = rep;
  }
}



void PlayerInfo::CheckReputationConditions()
{
  for(const auto &it : GameData::Governments())
  {
    int64_t rep = it.second.Reputation();
    int64_t newRep = conditions["reputation: " + it.first];
    if(newRep != rep)
      it.second.AddReputation(newRep - rep);
  }
}



// Check if the player knows the location of the given system (whether or not
// they have actually visited it).
bool PlayerInfo::HasSeen(const System *system) const
{
  for(const Mission &mission : availableJobs)
  {
    if(mission.Waypoints().count(system))
      return true;
    for(const Planet *planet : mission.Stopovers())
      if(planet->IsInSystem(system))
        return true;
  }

  for(const Mission &mission : missions)
  {
    if(!mission.IsVisible())
      continue;
    if(mission.Waypoints().count(system))
      return true;
    for(const Planet *planet : mission.Stopovers())
      if(planet->IsInSystem(system))
        return true;
  }

  return (seen.count(system) || KnowsName(system));
}



// Check if the player has visited the given system.
bool PlayerInfo::HasVisited(const System *system) const
{
  if(!system)
    return false;
  return visitedSystems.count(system);
}



// Check if the player has visited the given system.
bool PlayerInfo::HasVisited(const Planet *planet) const
{
  if(!planet)
    return false;
  return visitedPlanets.count(planet);
}



// Check if the player knows the name of a system, either from visiting there or
// because a job or active mission includes the name of that system.
bool PlayerInfo::KnowsName(const System *system) const
{
  if(HasVisited(system))
    return true;

  for(const Mission &mission : availableJobs)
    if(mission.Destination()->IsInSystem(system))
      return true;

  for(const Mission &mission : missions)
    if(mission.IsVisible() && mission.Destination()->IsInSystem(system))
      return true;

  return false;
}



// Mark the given system as visited, and mark all its neighbours as seen.
void PlayerInfo::Visit(const System *system)
{
  if(!system)
    return;

  visitedSystems.insert(system);
  seen.insert(system);
  for(const System *neighbour : system->Neighbours())
    seen.insert(neighbour);
}



// Mark the given planet as visited.
void PlayerInfo::Visit(const Planet *planet)
{
  if(planet && !planet->TrueName().empty())
    visitedPlanets.insert(planet);
}



// Mark a system as unvisited, even if visited previously.
void PlayerInfo::Unvisit(const System *system)
{
  if(!system)
    return;

  visitedSystems.erase(system);
  for(const StellarObject &object : system->Objects())
    if(object.GetPlanet())
      Unvisit(object.GetPlanet());
}



void PlayerInfo::Unvisit(const Planet *planet)
{
  if(!planet)
    return;

  visitedPlanets.erase(planet);
}



// Check if the player has a hyperspace route set.
bool PlayerInfo::HasTravelPlan() const
{
  return !travelPlan.empty();
}



// Access the player's travel plan.
const vector<const System *> &PlayerInfo::TravelPlan() const
{
  return travelPlan;
}



vector<const System *> &PlayerInfo::TravelPlan()
{
  return travelPlan;
}



// This is called when the player enters the system that is their current
// hyperspace target.
void PlayerInfo::PopTravel()
{
  if(!travelPlan.empty())
  {
    Visit(travelPlan.back());
    travelPlan.pop_back();
  }
}



// Get the planet to land on at the end of the travel path.
const Planet *PlayerInfo::TravelDestination() const
{
  return travelDestination;
}



// Set the planet to land on at the end of the travel path.
void PlayerInfo::SetTravelDestination(const Planet *planet)
{
  travelDestination = planet;
  if(planet->IsInSystem(system) && Flagship())
    Flagship()->SetTargetStellar(system->FindStellar(planet));
}



// Check which secondary weapon the player has selected.
const Outfit *PlayerInfo::SelectedWeapon() const
{
  return selectedWeapon;
}



// Cycle through all available secondary weapons.
void PlayerInfo::SelectNext()
{
  if(!flagship || flagship->Outfits().empty())
    return;

  // Start with the currently selected weapon, if any.
  auto it = flagship->Outfits().find(selectedWeapon);
  if(it == flagship->Outfits().end())
    it = flagship->Outfits().begin();
  else
    ++it;

  // Find the next secondary weapon.
  for( ; it != flagship->Outfits().end(); ++it)
    if(it->first->Icon())
    {
      selectedWeapon = it->first;
      return;
    }
  selectedWeapon = nullptr;
}



// Escorts currently selected for giving orders.
const vector<weak_ptr<Ship>> &PlayerInfo::SelectedShips() const
{
  return selectedShips;
}



// Select any player ships in the given box or list. Return true if any were
// selected, so we know not to search further for a match.
bool PlayerInfo::SelectShips(const Rectangle &box, bool hasShift)
{
  // If shift is not held down, replace the current selection.
  if(!hasShift)
    selectedShips.clear();
  // If shift is not held, the first ship in the box will also become the
  // player's flagship's target.
  bool first = !hasShift;

  bool matched = false;
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && ship->GetSystem() == system && ship.get() != Flagship()
        && box.Contains(ship->Position()))
    {
      matched = true;
      SelectShip(ship, &first);
    }
  return matched;
}



bool PlayerInfo::SelectShips(const vector<const Ship *> &stack, bool hasShift)
{
  // If shift is not held down, replace the current selection.
  if(!hasShift)
    selectedShips.clear();
  // If shift is not held, the first ship in the stack will also become the
  // player's flagship's target.
  bool first = !hasShift;

  // Loop through all the player's ships and check which of them are in the
  // given stack.
  bool matched = false;
  for(const shared_ptr<Ship> &ship : ships)
  {
    auto it = find(stack.begin(), stack.end(), ship.get());
    if(it != stack.end())
    {
      matched = true;
      SelectShip(ship, &first);
    }
  }
  return matched;
}



void PlayerInfo::SelectShip(const Ship *ship, bool hasShift)
{
  // If shift is not held down, replace the current selection.
  if(!hasShift)
    selectedShips.clear();

  bool first = !hasShift;
  for(const shared_ptr<Ship> &it : ships)
    if(it.get() == ship)
      SelectShip(it, &first);
}



void PlayerInfo::SelectGroup(int group, bool hasShift)
{
  int bit = (1 << group);
  // If the shift key is held down and all the ships in the given group are
  // already selected, deselect them all. Otherwise, select them all. The easy
  // way to do this is first to remove all the ships that match in one pass,
  // then add them in a subsequent pass if any were not selected.
  const Ship *oldTarget = nullptr;
  if(Flagship() && Flagship()->GetTargetShip())
  {
    oldTarget = Flagship()->GetTargetShip().get();
    Flagship()->SetTargetShip(shared_ptr<Ship>());
  }
  if(hasShift)
  {
    bool allWereSelected = true;
    for(const shared_ptr<Ship> &ship : ships)
      if(groups[ship.get()] & bit)
      {
        auto it = selectedShips.begin();
        for( ; it != selectedShips.end(); ++it)
          if(it->lock() == ship)
            break;
        if(it != selectedShips.end())
          selectedShips.erase(it);
        else
          allWereSelected = false;
      }
    if(allWereSelected)
      return;
  }
  else
    selectedShips.clear();

  // Now, go through and add any ships in the group to the selection. Even if
  // shift is held they won't be added twice, because we removed them above.
  for(const shared_ptr<Ship> &ship : ships)
    if(groups[ship.get()] & bit)
    {
      selectedShips.push_back(ship);
      if(ship.get() == oldTarget)
        Flagship()->SetTargetShip(ship);
    }
}



void PlayerInfo::SetGroup(int group, const set<Ship *> *newShips)
{
  int bit = (1 << group);
  int mask = ~bit;
  // First, remove any of your ships that are in the group.
  for(const shared_ptr<Ship> &ship : ships)
    groups[ship.get()] &= mask;
  // Then, add all the currently selected ships to the group.
  if(newShips)
  {
    for(const Ship *ship : *newShips)
      groups[ship] |= bit;
  }
  else
  {
    for(const weak_ptr<Ship> &ptr : selectedShips)
    {
      shared_ptr<Ship> ship = ptr.lock();
      if(ship)
        groups[ship.get()] |= bit;
    }
  }
}



set<Ship *> PlayerInfo::GetGroup(int group)
{
  int bit = (1 << group);
  set<Ship *> result;

  for(const shared_ptr<Ship> &ship : ships)
  {
    auto it = groups.find(ship.get());
    if(it != groups.end() && (it->second & bit))
      result.insert(ship.get());
  }
  return result;
}



// Keep track of any outfits that you have sold since landing. These will be
// available to buy back until you take off.
int PlayerInfo::Stock(const Outfit *outfit) const
{
  auto it = stock.find(outfit);
  return (it == stock.end() ? 0 : it->second);
}



// Transfer outfits from the player to the planet or vice versa.
void PlayerInfo::AddStock(const Outfit *outfit, int count)
{
  // If you sell an individual outfit that is not sold here and that you
  // acquired by buying a ship here, have it appear as "in stock" in case you
  // change your mind about selling it. (On the other hand, if you sell an
  // entire ship right after buying it, its outfits will not be "in stock.")
  if(count > 0 && stock[outfit] < 0)
    stock[outfit] = 0;
  stock[outfit] += count;

  int day = date.DaysSinceEpoch();
  if(count > 0)
  {
    // Remember how depreciated these items are.
    for(int i = 0; i < count; ++i)
      stockDepreciation.Buy(outfit, day, &depreciation);
  }
  else
  {
    // If the count is negative, outfits are being transferred from stock
    // into the player's possession.
    for(int i = 0; i < -count; ++i)
      depreciation.Buy(outfit, day, &stockDepreciation);
  }
}



// Get depreciation information.
const Depreciation &PlayerInfo::FleetDepreciation() const
{
  return depreciation;
}



const Depreciation &PlayerInfo::StockDepreciation() const
{
  return stockDepreciation;
}



void PlayerInfo::Harvest(const Outfit *type)
{
  if(type && system)
    harvested.insert(make_pair(system, type));
}



const set<pair<const System *, const Outfit *>> &PlayerInfo::Harvested() const
{
  return harvested;
}



// Get what colouring is currently selected in the map.
int PlayerInfo::MapColouring() const
{
  return mapColouring;
}



// Set what the map is being coloured by.
void PlayerInfo::SetMapColouring(int index)
{
  mapColouring = index;
}



// Get the map zoom level.
int PlayerInfo::MapZoom() const
{
  return mapZoom;
}



// Set the map zoom level.
void PlayerInfo::SetMapZoom(int level)
{
  mapZoom = level;
}



// Get the set of collapsed categories for the named panel.
set<string> &PlayerInfo::Collapsed(const string &name)
{
  return collapsed[name];
}



// Apply any "changes" saved in this player info to the global game state.
void PlayerInfo::ApplyChanges()
{
  for(const auto &it : reputationChanges)
    it.first->SetReputation(it.second);
  reputationChanges.clear();
  AddChanges(dataChanges);
  GameData::ReadEconomy(economy);
  economy = DataNode();

  // As a result of game data changes (e.g. unloading a mod) it's possible for
  // the player to end up in an undefined system or planet. In that case, move
  // them to the starting system to avoid crashing.
  if(planet && !system)
    system = planet->GetSystem();
  if(!planet || planet->Name().empty() || !system || system->Name().empty())
  {
    system = GameData::Start().GetSystem();
    planet = GameData::Start().GetPlanet();
  }

  // For any ship that did not store what system it is in or what planet it is
  // on, place it with the player. (In practice, every ship ought to have
  // specified its location, but this is to avoid null locations.)
  for(shared_ptr<Ship> &ship : ships)
  {
    if(!ship->GetSystem() || ship->GetSystem()->Name().empty())
      ship->SetSystem(system);
    if(ship->GetSystem() == system && (!ship->GetPlanet() || ship->GetPlanet()->Name().empty()))
      ship->SetPlanet(planet);
  }

  // Government changes may have changed the player's ship swizzles.
  for(shared_ptr<Ship> &ship : ships)
    ship->SetGovernment(GameData::PlayerGovernment());

  // Make sure all stellar objects are correctly positioned. This is needed
  // because EnterSystem() is not called the first time through.
  GameData::SetDate(GetDate());
  // SetDate() clears any bribes from yesterday, so restore any auto-clearance.
  for(const Mission &mission : Missions())
    if(mission.ClearanceMessage() == "auto")
    {
      mission.Destination()->Bribe(mission.HasFullClearance());
      for(const Planet *planet : mission.Stopovers())
        planet->Bribe(mission.HasFullClearance());
    }

  // It is sometimes possible for the player to be landed on a planet where
  // they do not have access to any services. So, this flag is used to specify
  // that in this case, the player has access to the planet's services.
  if(planet && hasFullClearance)
    planet->Bribe();
  hasFullClearance = false;

  // Check if any special persons have been destroyed.
  GameData::DestroyPersons(destroyedPersons);
  destroyedPersons.clear();

  // Check which planets you have dominated.
  static const string prefix = "tribute: ";
  for(auto it = conditions.lower_bound(prefix); it != conditions.end(); ++it)
  {
    if(it->first.compare(0, prefix.length(), prefix))
      break;

    const Planet *planet = GameData::Planets().Find(it->first.substr(prefix.length()));
    if(planet)
      GameData::GetPolitics().DominatePlanet(planet);
  }

  // Make sure all data defined in this saved game is valid.
  GameData::CheckReferences();
}



// Update the conditions that reflect the current status of the player.
void PlayerInfo::UpdateAutoConditions(bool isBoarding)
{
  // Bound financial conditions to +/- 4.6 x 10^18 credits, within the range of a 64-bit int.
  static constexpr int64_t limit = static_cast<int64_t>(1) << 62;
  conditions["net worth"] = min(limit, max(-limit, accounts.NetWorth()));
  conditions["credits"] = min(limit, accounts.Credits());
  conditions["unpaid mortgages"] = min(limit, accounts.TotalDebt("Mortgage"));
  conditions["unpaid fines"] = min(limit, accounts.TotalDebt("Fine"));
  conditions["unpaid salaries"] = min(limit, accounts.SalariesOwed());
  conditions["unpaid maintenance"] = min(limit, accounts.MaintenanceDue());
  conditions["credit score"] = accounts.CreditScore();
  // Serialize the current reputation with other governments.
  SetReputationConditions();
  // Helper lambda function to clear a range
  auto clearRange = [](map<string, int64_t> &conditionsMap, string firstStr, string lastStr)
  {
    auto first = conditionsMap.lower_bound(firstStr);
    auto last = conditionsMap.lower_bound(lastStr);
    if(first != last)
      conditionsMap.erase(first, last);
  };
  // Clear any existing ships: conditions. (Note: '!' = ' ' + 1.)
  clearRange(conditions, "ships: ", "ships:!");
  // Store special conditions for cargo and passenger space.
  conditions["cargo space"] = 0;
  conditions["passenger space"] = 0;
  for(const shared_ptr<Ship> &ship : ships)
    if(!ship->IsParked() && !ship->IsDisabled() && ship->GetSystem() == system)
    {
      conditions["cargo space"] += ship->Attributes().Get("cargo space");
      conditions["passenger space"] += ship->Attributes().Get("bunks") - ship->RequiredCrew();
      ++conditions["ships: " + ship->Attributes().Category()];
    }
  // If boarding a ship, missions should not consider the space available
  // in the player's entire fleet. The only fleet parameter offered to a
  // boarding mission is the fleet composition (e.g. 4 Frigates).
  if(isBoarding && flagship)
  {
    conditions["cargo space"] = flagship->Cargo().Free();
    conditions["passenger space"] = flagship->Cargo().BunksFree();
  }

  // Clear any existing flagship system: and planet: conditions. (Note: '!' = ' ' + 1.)
  clearRange(conditions, "flagship system: ", "flagship system:!");
  clearRange(conditions, "flagship planet: ", "flagship planet:!");

  // Store conditions for flagship current crew, required crew, and bunks.
  if(flagship)
  {
    conditions["flagship crew"] = flagship->Crew();
    conditions["flagship required crew"] = flagship->RequiredCrew();
    conditions["flagship bunks"] = flagship->Attributes().Get("bunks");
    if(flagship->GetSystem())
      conditions["flagship system: " + flagship->GetSystem()->Name()] = 1;
    if(flagship->GetPlanet())
      conditions["flagship planet: " + flagship->GetPlanet()->TrueName()] = 1;
  }
  else
  {
    conditions["flagship crew"] = 0;
    conditions["flagship required crew"] = 0;
    conditions["flagship bunks"] = 0;
  }

  // Conditions for your fleet's attractiveness to pirates:
  pair<double, double> factors = RaidFleetFactors();
  conditions["cargo attractiveness"] = factors.first;
  conditions["armament deterrence"] = factors.second;
  conditions["pirate attraction"] = factors.first - factors.second;
}



// New missions are generated each time you land on a planet.
void PlayerInfo::CreateMissions()
{
  boardingMissions.clear();

  // Check for available missions.
  bool skipJobs = planet && !planet->HasSpaceport();
  bool hasPriorityMissions = false;
  for(const auto &it : GameData::Missions())
  {
    if(it.second.IsAtLocation(Mission::BOARDING) || it.second.IsAtLocation(Mission::ASSISTING))
      continue;
    if(skipJobs && it.second.IsAtLocation(Mission::JOB))
      continue;

    if(it.second.CanOffer(*this))
    {
      list<Mission> &missions =
        it.second.IsAtLocation(Mission::JOB) ? availableJobs : availableMissions;

      missions.push_back(it.second.Instantiate(*this));
      if(missions.back().HasFailed(*this))
        missions.pop_back();
      else if(!it.second.IsAtLocation(Mission::JOB))
        hasPriorityMissions |= missions.back().HasPriority();
    }
  }

  // If any of the available missions are "priority" missions, no other
  // special missions will be offered in the spaceport.
  if(hasPriorityMissions)
  {
    auto it = availableMissions.begin();
    while(it != availableMissions.end())
    {
      if(it->IsAtLocation(Mission::SPACEPORT) && !it->HasPriority())
        it = availableMissions.erase(it);
      else
        ++it;
    }
  }
  else if(availableMissions.size() > 1)
  {
    // Minor missions only get offered if no other missions (including other
    // minor missions) are competing with them. This is to avoid having two
    // or three missions pop up as soon as you enter the spaceport.
    auto it = availableMissions.begin();
    while(it != availableMissions.end())
    {
      if(it->IsMinor())
      {
        it = availableMissions.erase(it);
        if(availableMissions.size() <= 1)
          break;
      }
      else
        ++it;
    }
  }
}



// Updates each mission upon landing, to perform landing actions (Stopover,
// Visit, Complete, Fail), and remove now-complete or now-failed missions.
void PlayerInfo::StepMissions(UI *ui)
{
  // Check for NPCs that have been destroyed without their destruction
  // being registered, e.g. by self-destruct:
  for(Mission &mission : missions)
    for(const NPC &npc : mission.NPCs())
      for(const shared_ptr<Ship> &ship : npc.Ships())
        if(ship->IsDestroyed())
          mission.Do(ShipEvent(nullptr, ship, ShipEvent::DESTROY), *this, ui);

  string visitText;
  int missionVisits = 0;
  auto substitutions = map<string, string>{
    {"<first>", firstName},
    {"<last>", lastName}
  };
  if(Flagship())
    substitutions["<ship>"] = Flagship()->Name();

  auto mit = missions.begin();
  while(mit != missions.end())
  {
    Mission &mission = *mit;
    ++mit;

    // If this is a stopover for the mission, perform the stopover action.
    mission.Do(Mission::STOPOVER, *this, ui);

    if(mission.HasFailed(*this))
      RemoveMission(Mission::FAIL, mission, ui);
    else if(mission.CanComplete(*this))
      RemoveMission(Mission::COMPLETE, mission, ui);
    else if(mission.Destination() == GetPlanet() && !freshlyLoaded)
    {
      mission.Do(Mission::VISIT, *this, ui);
      if(mission.IsUnique() || !mission.IsVisible())
        continue;

      // On visit dialogues are handled separately as to avoid a player
      // getting spammed by on visit dialogues if they are stacking jobs
      // from the same destination.
      if(visitText.empty())
      {
        const auto &text = mission.GetAction(Mission::VISIT).DialogueText();
        if(!text.empty())
          visitText = Format::Replace(text, substitutions);
      }
      ++missionVisits;
    }
  }
  if(!visitText.empty())
  {
    if(missionVisits > 1)
      visitText += "\n\t(You have " + Format::Number(missionVisits - 1) + " other unfinished " 
        + ((missionVisits > 2) ? "missions" : "mission") + " at this location.)";
    ui->Push(new Dialogue(visitText));
  }
  // One mission's actions may influence another mission, so loop through one
  // more time to see if any mission is now completed or failed due to a change
  // that happened in another mission the first time through.
  mit = missions.begin();
  while(mit != missions.end())
  {
    Mission &mission = *mit;
    ++mit;

    if(mission.HasFailed(*this))
      RemoveMission(Mission::FAIL, mission, ui);
    else if(mission.CanComplete(*this))
      RemoveMission(Mission::COMPLETE, mission, ui);
  }

  // Search for any missions that have failed but for which we are still
  // holding on to some cargo.
  set<const Mission *> active;
  for(const Mission &it : missions)
    active.insert(&it);

  vector<const Mission *> missionsToRemove;
  for(const auto &it : cargo.MissionCargo())
    if(!active.count(it.first))
      missionsToRemove.push_back(it.first);
  for(const auto &it : cargo.PassengerList())
    if(!active.count(it.first))
      missionsToRemove.push_back(it.first);
  for(const Mission *mission : missionsToRemove)
    cargo.RemoveMissionCargo(mission);
}



void PlayerInfo::Autosave() const
{
  if(!CanBeSaved() || filePath.length() < 4)
    return;

  string path = filePath.substr(0, filePath.length() - 4) + "~autosave.txt";
  Save(path);
}



void PlayerInfo::Save(const string &path) const
{
  DataWriter out(path);


  // Basic player information and persistent UI settings:

  // Pilot information:
  out.Write("pilot", firstName, lastName);
  out.Write("date", date.Day(), date.Month(), date.Year());
  if(system)
    out.Write("system", system->Name());
  if(planet)
    out.Write("planet", planet->Name());
  if(planet && planet->CanUseServices())
    out.Write("clearance");
  // This flag is set if the player must leave the planet immediately upon
  // loading the game (i.e. because a mission forced them to take off).
  if(shouldLaunch)
    out.Write("launching");
  for(const System *system : travelPlan)
    out.Write("travel", system->Name());
  if(travelDestination)
    out.Write("travel destination", travelDestination->TrueName());

  // Save the current setting for the map colouring;
  out.Write("map colouring", mapColouring);
  out.Write("map zoom", mapZoom);
  // Remember what categories are collapsed.
  for(const auto &it : collapsed)
  {
    // Skip panels where nothing was collapsed.
    if(it.second.empty())
      continue;

    out.Write("collapsed", it.first);
    out.BeginChild();
    {
      for(const auto &cit : it.second)
        out.Write(cit);
    }
    out.EndChild();
  }

  out.Write("reputation with");
  out.BeginChild();
  {
    for(const auto &it : GameData::Governments())
      if(!it.second.IsPlayer())
        out.Write(it.first, it.second.Reputation());
  }
  out.EndChild();


  // Records of things you own:
  out.Write();
  out.WriteComment("What you own:");

  // Save all the data for all the player's ships.
  for(const shared_ptr<Ship> &ship : ships)
  {
    ship->Save(out);
    auto it = groups.find(ship.get());
    if(it != groups.end() && it->second)
      out.Write("groups", it->second);
  }

  // Save accounting information, cargo, and cargo cost bases.
  accounts.Save(out);
  cargo.Save(out);
  if(!costBasis.empty())
  {
    out.Write("basis");
    out.BeginChild();
    {
      for(const auto &it : costBasis)
        if(it.second)
          out.Write(it.first, it.second);
    }
    out.EndChild();
  }

  if(!stock.empty())
  {
    out.Write("stock");
    out.BeginChild();
    {
      for(const auto &it : stock)
        if(it.second)
          out.Write(it.first->Name(), it.second);
    }
    out.EndChild();
  }
  depreciation.Save(out, date.DaysSinceEpoch());
  stockDepreciation.Save(out, date.DaysSinceEpoch());


  // Records of things you have done or are doing, or have happened to you:
  out.Write();
  out.WriteComment("What you've done:");

  // Save all missions (accepted or available).
  for(const Mission &mission : missions)
    mission.Save(out);
  for(const Mission &mission : availableJobs)
    mission.Save(out, "available job");
  for(const Mission &mission : availableMissions)
    mission.Save(out, "available mission");

  // Save any "condition" flags that are set.
  if(!conditions.empty())
  {
    out.Write("conditions");
    out.BeginChild();
    {
      for(const auto &it : conditions)
      {
        // If the condition's value is 1, don't bother writing the 1.
        if(it.second == 1)
          out.Write(it.first);
        else if(it.second)
          out.Write(it.first, it.second);
      }
    }
    out.EndChild();
  }

  // Save pending events, and changes that have happened due to past events.
  for(const GameEvent &event : gameEvents)
    event.Save(out);
  if(!dataChanges.empty())
  {
    out.Write("changes");
    out.BeginChild();
    {
      for(const DataNode &node : dataChanges)
        out.Write(node);
    }
    out.EndChild();
  }
  GameData::WriteEconomy(out);

  // Check which persons have been captured or destroyed.
  for(const auto &it : GameData::Persons())
    if(it.second.IsDestroyed())
      out.Write("destroyed", it.first);


  // Records of things you have discovered:
  out.Write();
  out.WriteComment("What you know:");

  // Save a list of systems the player has visited.
  for(const System *system : visitedSystems)
    if(!system->Name().empty())
      out.Write("visited", system->Name());

  // Save a list of planets the player has visited.
  for(const Planet *planet : visitedPlanets)
    if(!planet->TrueName().empty())
      out.Write("visited planet", planet->TrueName());

  if(!harvested.empty())
  {
    out.Write("harvested");
    out.BeginChild();
    {
      for(const auto &it : harvested)
        if(it.first && it.second)
          out.Write(it.first->Name(), it.second->Name());
    }
    out.EndChild();
  }

  out.Write("logbook");
  out.BeginChild();
  for(const auto &it : logbook)
  {
    out.Write(it.first.Day(), it.first.Month(), it.first.Year());
    out.BeginChild();
    {
      // Break the text up into paragraphs.
      for(const string &line : Format::Split(it.second, "\n\t"))
        out.Write(line);
    }
    out.EndChild();
  }
  for(const auto &it : specialLogs)
    for(const auto &eit : it.second)
    {
      out.Write(it.first, eit.first);
      out.BeginChild();
      {
        // Break the text up into paragraphs.
        for(const string &line : Format::Split(eit.second, "\n\t"))
          out.Write(line);
      }
      out.EndChild();
    }
  out.EndChild();
}



// Check (and perform) any fines incurred by planetary security. If the player
// has dominated the planet, or was given clearance to this planet by a mission,
// planetary security is avoided. Infiltrating implies evasion of security.
void PlayerInfo::Fine(UI *ui)
{
  const Planet *planet = GetPlanet();
  // Dominated planets should never fine you.
  // By default, uninhabited planets should not fine the player.
  if(GameData::GetPolitics().HasDominated(planet)
    || !(planet->IsInhabited() || planet->HasCustomSecurity()))
    return;

  // Planets should not fine you if you have mission clearance or are infiltrating.
  for(const Mission &mission : missions)
    if(mission.HasClearance(planet) || (!mission.HasFullClearance() &&
          (mission.Destination() == planet || mission.Stopovers().count(planet))))
      return;

  // The planet's government must have the authority to enforce laws.
  const Government *gov = planet->GetGovernment();
  if(!gov->CanEnforce(planet))
    return;

  string message = gov->Fine(*this, 0, nullptr, planet->Security());
  if(!message.empty())
  {
    if(message == "atrocity")
    {
      const Conversation *conversation = gov->DeathSentence();
      if(conversation)
        ui->Push(new ConversationPanel(*this, *conversation));
      else
      {
        message = "Before you can leave your ship, the " + gov->GetName()
          + " authorities show up and begin scanning it. They say, \"Captain "
          + LastName()
          + ", we detect highly illegal material on your ship.\""
          "\n\tYou are sentenced to lifetime imprisonment on a penal colony."
          " Your days of travelling the stars have come to an end.";
        ui->Push(new Dialogue(message));
      }
      // All ships belonging to the player should be removed.
      Die();
    }
    else
      ui->Push(new Dialogue(message));
  }
}



// Helper function to update the ship selection.
void PlayerInfo::SelectShip(const shared_ptr<Ship> &ship, bool *first)
{
  // Make sure this ship is not already selected.
  auto it = selectedShips.begin();
  for( ; it != selectedShips.end(); ++it)
    if(it->lock() == ship)
      break;
  if(it == selectedShips.end())
  {
    // This ship is not yet selected.
    selectedShips.push_back(ship);
    Ship *flagship = Flagship();
    if(*first && flagship && ship.get() != flagship)
    {
      flagship->SetTargetShip(ship);
      *first = false;
    }
  }
}



// Check that this player's current state can be saved.
bool PlayerInfo::CanBeSaved() const
{
  return (!isDead && planet && system && !firstName.empty() && !lastName.empty());
}