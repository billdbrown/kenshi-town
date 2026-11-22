// Town Ledger: kenshi-town's in-game panel.
//
// Press F8 in game to toggle a window listing the player faction and, below
// it, the Hub experiment: three questions about making the Hub the town.
//
//   1. Ore:   does hooking ZoneManager::getResource change what mines see?
//   2. Claim: does TownBase::setFaction hand the Hub's buildings to the player?
//   3. Build: can the player place buildings inside the Hub once it is theirs?
//
// Plus a development switch, "Dev: fast", that multiplies research and
// construction speed so testing doesn't wait on the game's pacing.
//
// Everything the experiment learns is written to RE_Kenshi_log.txt, prefixed
// "TownLedger:", so a play session can be read back afterwards.

#include <Debug.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/Town.h>
#include <kenshi/SharedKing.h>
#include <kenshi/ZoneManager.h>
#include <kenshi/Enums.h>
#include <kenshi/GlobalConstants.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/Item.h>
#include <kenshi/util/hand.h>
#include <kenshi/Building/Building.h>
// KenshiLib 0.4.0 bug: AI.h forward-declares `class CharacterMessage` while
// Character.h defines it as an enum. Rename AI.h's copy for the include. Only
// AI::sendMessage uses it, and we never call that.
#define CharacterMessage CharacterMessage_AIForwardDecl
#include <kenshi/AI/AI.h>
#undef CharacterMessage
#include <kenshi/AI/AITaskSystem.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Delegate.h>

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    const int TOGGLE_KEY = VK_F8;
    const int REFRESH_EVERY_N_FRAMES = 30;
    const int FIND_HUB_EVERY_N_FRAMES = 120;

    // Ore the Hub gets while the override is on. Scale assumed 0..1; the
    // first real readings in the log will confirm or correct that.
    const float HUB_STONE = 0.8f;
    const float HUB_IRON = 0.6f;
    const float HUB_COPPER = 0.5f;

    MyGUI::Window* g_window = NULL;
    MyGUI::EditBox* g_text = NULL;
    MyGUI::Button* g_oreButton = NULL;
    MyGUI::Button* g_claimButton = NULL;
    MyGUI::Button* g_devButton = NULL;
    bool g_keyWasDown = false;
    int g_frame = 0;

    // ---- The Hub, cached from the main loop so hooks never search for it.

    TownBase* g_hub = NULL;
    Faction* g_hubOriginalFaction = NULL;
    volatile bool g_hubKnown = false;
    volatile float g_hubX = 0, g_hubZ = 0, g_hubRadius = 0;

    bool insideHub(const Ogre::Vector3& p)
    {
        if (!g_hubKnown)
            return false;
        float dx = p.x - g_hubX, dz = p.z - g_hubZ;
        return dx * dx + dz * dz <= g_hubRadius * g_hubRadius;
    }

    TownBase* findHub()
    {
        if (!shou || !shou->townList)
            return NULL;
        lektor<RootObject*>& towns = shou->townList->getAllTowns();
        TownBase* fallback = NULL;
        for (uint32_t i = 0; i < towns.size(); ++i)
        {
            TownBase* t = static_cast<TownBase*>(towns[i]);
            if (!t)
                continue;
            std::string name = t->getName();
            if (name == "The Hub")
                return t;
            if (!fallback && name.find("Hub") != std::string::npos)
                fallback = t;
        }
        return fallback;
    }

    void updateHubCache()
    {
        TownBase* hub = findHub();
        if (hub != g_hub)
        {
            g_hub = hub;
            g_hubOriginalFaction = hub ? hub->getFaction() : NULL;
            if (hub)
                DebugLog("TownLedger: found town '" + hub->getName() + "'");
        }
        if (!hub)
        {
            g_hubKnown = false;
            return;
        }
        Ogre::Vector3 p = hub->getPosition();
        g_hubX = p.x;
        g_hubZ = p.z;
        g_hubRadius = hub->getRadius();
        g_hubKnown = true;
    }

    std::string factionName(Faction* f)
    {
        return f ? f->getName() : std::string("(none)");
    }

    // ---- Question 1: ore. Hook getResource, and count getResourceBase so we
    // learn whether anything reads the base map directly and would miss us.

    struct OreSample
    {
        volatile long calls;
        volatile long hubCalls;
        float lastOriginal;
        float lastReturned;
    };
    OreSample g_ore[8] = {};       // indexed by MiningResource
    volatile long g_baseCalls = 0;
    bool g_oreOverride = false;
    bool g_loggedScale = false;

    float hubOreFor(MiningResource r)
    {
        switch (r)
        {
        case STONE: return HUB_STONE;
        case IRON: return HUB_IRON;
        case COPPER: return HUB_COPPER;
        default: return -1.0f;
        }
    }

    float (*getResource_orig)(ZoneManager*, MiningResource, AreaBiomeGroup*, const Ogre::Vector3&) = NULL;

    float getResource_hook(ZoneManager* self, MiningResource r, AreaBiomeGroup* biome, const Ogre::Vector3& p)
    {
        float original = getResource_orig(self, r, biome, p);
        float returned = original;
        bool inside = insideHub(p);
        float target = hubOreFor(r);
        if (g_oreOverride && inside && target > original)
            returned = target;

        if (r >= 0 && r < 8)
        {
            OreSample& s = g_ore[r];
            InterlockedIncrement(&s.calls);
            if (inside)
            {
                InterlockedIncrement(&s.hubCalls);
                s.lastOriginal = original;
                s.lastReturned = returned;
            }
        }
        return returned;
    }

    float (*getResourceBase_orig)(ZoneManager*, MiningResource, AreaBiomeGroup*, const Ogre::Vector3&) = NULL;

    float getResourceBase_hook(ZoneManager* self, MiningResource r, AreaBiomeGroup* biome, const Ogre::Vector3& p)
    {
        InterlockedIncrement(&g_baseCalls);
        return getResourceBase_orig(self, r, biome, p);
    }

    // ---- Question 3: building. Watch the game's own placement verdicts.
    // Read-only: the first test showed normal placement rules apply in the Hub.

    long g_placeChecks = 0;
    long g_placeHubChecks = 0;
    long g_placeHubRefused = 0;
    long g_prospectChecks = 0;
    bool g_lastPlaceResult = false;
    bool g_lastPlaceInHub = false;
    bool g_lastProspect = false;

    bool (*placementVerification_orig)(PreviewBuilding*) = NULL;

    bool placementVerification_hook(PreviewBuilding* self)
    {
        bool ok = placementVerification_orig(self);
        bool inHub = insideHub(self->getPosition());
        ++g_placeChecks;
        if (inHub)
        {
            ++g_placeHubChecks;
            if (!ok)
                ++g_placeHubRefused;
        }
        g_lastPlaceResult = ok;
        g_lastPlaceInHub = inHub;
        return ok;
    }

    // ---- Dev speed. Multiplies two global constants. They aren't saved, and
    // the game may reload them (on load, for instance), so while the switch is
    // on the main loop re-asserts them, treating any foreign value as the new
    // base.

    const float DEV_RESEARCH_MULT = 100.0f;
    const float DEV_BUILD_MULT = 20.0f;

    bool g_devFast = false;
    float g_baseResearch = 0, g_baseBuild = 0;
    float g_setResearch = 0, g_setBuild = 0;

    void applyDevSpeed()
    {
        if (!con)
            return;
        if (con->RESEARCH_RATE != g_setResearch)
        {
            g_baseResearch = con->RESEARCH_RATE;
            g_setResearch = con->RESEARCH_RATE = g_baseResearch * DEV_RESEARCH_MULT;
        }
        if (con->BUILD_SPEED_MULTIPLIER != g_setBuild)
        {
            g_baseBuild = con->BUILD_SPEED_MULTIPLIER;
            g_setBuild = con->BUILD_SPEED_MULTIPLIER = g_baseBuild * DEV_BUILD_MULT;
        }
    }

    void restoreDevSpeed()
    {
        if (!con)
            return;
        if (con->RESEARCH_RATE == g_setResearch)
            con->RESEARCH_RATE = g_baseResearch;
        if (con->BUILD_SPEED_MULTIPLIER == g_setBuild)
            con->BUILD_SPEED_MULTIPLIER = g_baseBuild;
        g_setResearch = g_setBuild = 0;
    }

    bool (*checkProspecting_orig)(PreviewBuilding*) = NULL;

    bool checkProspecting_hook(PreviewBuilding* self)
    {
        bool ok = checkProspecting_orig(self);
        ++g_prospectChecks;
        g_lastProspect = ok;
        return ok;
    }

    // ---- Dev supplies: a General Storage Chest placed beside the selected
    // character, filled with building stock and a little research material.
    // What doesn't fit is discarded rather than dropped on the ground. Names
    // are the game's own, read out of gamedata.base / rebirth.mod.

    struct Supply
    {
        const char* name;
        int count;
    };
    const Supply DEV_SUPPLIES[] = {
        {"Building Materials", 40},
        {"Iron Plates", 20},
        {"Copper Alloy Plates", 15},
        {"Steel Bars", 10},
        {"Electrical Components", 10},
        {"Fabrics", 10},
        {"Book", 20},
        {"Engineering Research", 5},
        {"Ancient Science Book", 5},
        {"AI Core", 3},
    };
    const char* SUPPLY_CHEST_SID = "1459-gamedata.base"; // General Storage Chest

    // createBuilding takes Vector3 and Quaternion by value; declaring the exact
    // signature lets the compiler get the x64 ABI right (KenshiCoop's pattern).
    typedef Building* (__fastcall* CreateBuildingFn)(
        RootObjectFactory* self, GameData* data, Ogre::Vector3 position, TownBase* t,
        Faction* owner, Ogre::Quaternion rotation, FactoryCallbackInterface* cb,
        Layout* furnitureOf, Building* isDoorOf, GameSaveState* saveState,
        Building* isIndoorsOf, bool invisible, bool completed, bool isFoliage,
        int floorNumber, bool isOutsideFurniture);
    CreateBuildingFn g_createBuilding = NULL;

    // createItem is overloaded; resolve the six-argument form explicitly, the
    // way KenshiCoop does. `const hand&` is passed as a pointer.
    typedef Item* (__fastcall* CreateItemFn)(RootObjectFactory*, GameData*, const hand*, GameData*,
                                            GameData*, int, Faction*);
    CreateItemFn g_createItem = NULL;

    GameData* findItemData(const std::string& name)
    {
        const itemType kinds[] = {ITEM, ARTIFACTS, MAP_ITEM};
        for (int i = 0; i < 3; ++i)
            if (GameData* d = ou->gamedata.getDataByName(name, kinds[i]))
                return d;
        return NULL;
    }

    std::string giveDevSupplies()
    {
        if (!ou || !ou->player || !ou->theFactory || !g_createItem || !g_createBuilding)
            return "no game loaded";
        lektor<RootObject*> selected;
        ou->player->getAllSelectedObjects(selected, CHARACTER);
        if (selected.size() == 0 || !selected[0])
            return "select a character first";

        GameData* chestData = ou->gamedata.getData(SUPPLY_CHEST_SID);
        if (!chestData)
            return "General Storage Chest not found in game data";

        // Three metres to the side of the character. y = 0: createBuilding adds
        // the terrain height itself.
        Ogre::Vector3 at = selected[0]->getPosition();
        Ogre::Vector3 pos(at.x + 30.0f, 0.0f, at.z);
        TownBase* town = insideHub(pos) ? g_hub : NULL;
        Building* chest = g_createBuilding(
            ou->theFactory, chestData, pos, town, ou->player->getFaction(), Ogre::Quaternion::IDENTITY,
            NULL, NULL, NULL, NULL, NULL, false, true, false, 0, false);
        if (!chest)
            return "createBuilding returned no chest";
        RootObject* who = chest;

        std::ostringstream out;
        out << "chest at " << int(pos.x) << "," << int(pos.z) << ":";
        for (size_t s = 0; s < sizeof(DEV_SUPPLIES) / sizeof(DEV_SUPPLIES[0]); ++s)
        {
            GameData* d = findItemData(DEV_SUPPLIES[s].name);
            if (!d)
            {
                out << " [" << DEV_SUPPLIES[s].name << " not found]";
                continue;
            }
            int given = 0;
            for (int n = 0; n < DEV_SUPPLIES[s].count; ++n)
            {
                hand none;
                Item* it = g_createItem(ou->theFactory, d, &none, NULL, NULL, -1, NULL);
                if (!it)
                    break;
                // Full chest: destroy the item instead of dropping it.
                if (!who->giveItem(it, false, true))
                    break;
                ++given;
            }
            out << " " << DEV_SUPPLIES[s].name << " x" << given << ";";
        }
        return out.str();
    }

    // ---- The ledger text.

    bool kenshiHasFocus()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    std::string currentActivity(Character* c)
    {
        if (c->isDead())
            return "dead";
        if (!c->ai)
            return "-";
        AITaskSytem* tasks = c->ai->getTaskSystem();
        if (!tasks)
            return "-";
        std::string goal = tasks->getCurrentGoalString();
        return goal.empty() ? "idle" : goal;
    }

    void writeOreLine(std::ostringstream& out, const char* label, MiningResource r)
    {
        const OreSample& s = g_ore[r];
        out << "  " << label << ": " << s.calls << " calls, " << s.hubCalls << " in Hub";
        if (s.hubCalls)
            out << ", last " << s.lastOriginal << " -> " << s.lastReturned;
        out << "\n";
    }

    // ---- The Hub's buildings.

    void hubBuildings(std::vector<Building*>& out)
    {
        out.clear();
        if (!g_hub)
            return;
        TownBuildingsManager* mgr = g_hub->getBuildingsManager();
        if (!mgr)
            return;
        for (ogre_unordered_map<Building*, TownBuildingsManager::BuildingInfo>::type::iterator it =
                 mgr->buildingEntities.begin();
             it != mgr->buildingEntities.end(); ++it)
            if (it->first)
                out.push_back(it->first);
    }

    // Who owned each building before we claimed it, so Give back can undo it.
    std::map<Building*, Faction*> g_originalOwners;

    // Buildings that keep their owner when we claim the Hub: the bar's trader
    // stays by design; the Shinobi go, but through the data mod, not here.
    bool keepsOwnerOnClaim(Building* b)
    {
        std::string owner = factionName(b->getFaction());
        return owner == "Trade Ninjas" || owner == "Shinobi Thieves";
    }

    std::string buildingOwners()
    {
        std::vector<Building*> all;
        hubBuildings(all);
        std::map<std::string, int> byOwner;
        int destroyed = 0, broken = 0, dismantlable = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Building* b = all[i];
            ++byOwner[factionName(b->getFaction())];
            if (b->isDestroyed())
                ++destroyed;
            if (b->isBroken())
                ++broken;
            if (b->canDismantle())
                ++dismantlable;
        }
        std::ostringstream out;
        out << "  " << all.size() << " buildings:";
        for (std::map<std::string, int>::iterator it = byOwner.begin(); it != byOwner.end(); ++it)
            out << " " << it->first << " " << it->second << ";";
        out << "\n  destroyed " << destroyed << ", broken " << broken
            << ", dismantlable " << dismantlable << "\n";
        return out.str();
    }

    // One line per building, for the log only (too long for the window).
    std::string buildingCensus()
    {
        std::vector<Building*> all;
        hubBuildings(all);
        std::map<std::string, int> lines;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Building* b = all[i];
            std::ostringstream key;
            key << b->getName() << " | " << factionName(b->getFaction())
                << (b->isDestroyed() ? " | DESTROYED" : "")
                << (b->isBroken() ? " | broken" : "")
                << (b->canDismantle() ? " | can dismantle" : "");
            ++lines[key.str()];
        }
        std::ostringstream out;
        out << "Hub building census (" << all.size() << "):\n";
        for (std::map<std::string, int>::iterator it = lines.begin(); it != lines.end(); ++it)
            out << "  " << it->second << " x " << it->first << "\n";
        return out.str();
    }

    std::string buildHubReport()
    {
        std::ostringstream out;
        out << "--- Hub experiment ---\n";
        if (!g_hub)
        {
            out << "Hub not found yet.\n";
            return out.str();
        }
        out << "Town: " << g_hub->getName() << "  radius " << g_hubRadius << "\n";
        out << "Owner: " << factionName(g_hub->getFaction())
            << "  (was " << factionName(g_hubOriginalFaction) << ")\n";
        out << buildingOwners();

        out << "\n1. Ore override " << (g_oreOverride ? "ON" : "off") << "\n";
        writeOreLine(out, "stone", STONE);
        writeOreLine(out, "iron", IRON);
        writeOreLine(out, "copper", COPPER);
        out << "  base map reads: " << g_baseCalls << "\n";

        out << "\n3. Placement\n";
        out << "  checks " << g_placeChecks << ", in Hub " << g_placeHubChecks
            << ", refused in Hub " << g_placeHubRefused << "\n";
        if (g_placeChecks)
            out << "  last: " << (g_lastPlaceResult ? "valid" : "refused")
                << (g_lastPlaceInHub ? " (in Hub)" : " (outside)") << "\n";
        out << "  prospecting checks " << g_prospectChecks;
        if (g_prospectChecks)
            out << ", last " << (g_lastProspect ? "ok" : "zero");
        out << "\n";

        out << "\nDev: fast " << (g_devFast ? "ON" : "off");
        if (con)
            out << "  (research rate " << con->RESEARCH_RATE
                << ", build speed " << con->BUILD_SPEED_MULTIPLIER << ")";
        out << "\n";
        return out.str();
    }

    std::string buildLedger()
    {
        if (!ou || !ou->player)
            return "No game loaded.";

        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        std::ostringstream out;
        out << "Population: " << people.size() << "\n\n";
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            Character* c = people[i];
            if (!c)
                continue;
            out << c->displayName << "  -  " << currentActivity(c) << "\n";
        }
        out << "\n" << buildHubReport();
        return out.str();
    }

    void logReport(const std::string& why)
    {
        DebugLog("TownLedger: [" + why + "]\n" + buildHubReport());
    }

    bool hubIsOurs()
    {
        return g_hub && ou && ou->player && g_hub->getFaction() == ou->player->getFaction();
    }

    void refresh()
    {
        if (g_text)
            g_text->setCaption(buildLedger());
        if (g_oreButton)
            g_oreButton->setCaption(g_oreOverride ? "Ore: ON" : "Ore: off");
        if (g_devButton)
            g_devButton->setCaption(g_devFast ? "Dev: FAST" : "Dev: off");
        if (g_claimButton)
            g_claimButton->setCaption(hubIsOurs() ? "Give back" : "Claim Hub");
    }

    // ---- Buttons. Each one writes a report to the log as well.

    void onLogClick(MyGUI::WidgetPtr)
    {
        refresh();
        logReport("snapshot");
        if (g_hub)
            DebugLog("TownLedger: " + buildingCensus());
    }

    void onOreClick(MyGUI::WidgetPtr)
    {
        g_oreOverride = !g_oreOverride;
        refresh();
        logReport(g_oreOverride ? "ore override on" : "ore override off");
    }

    void onClaimClick(MyGUI::WidgetPtr)
    {
        if (!g_hub || !ou || !ou->player)
            return;
        bool ours = hubIsOurs();
        Faction* mine = ou->player->getFaction();
        Faction* to = ours ? g_hubOriginalFaction : mine;
        DebugLog("TownLedger: setFaction " + factionName(g_hub->getFaction()) + " -> " + factionName(to));
        g_hub->setFaction(to, NULL);

        // The town's own faction doesn't move its buildings; hand them over
        // one by one (and back again).
        std::vector<Building*> all;
        hubBuildings(all);
        int moved = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Building* b = all[i];
            if (!ours)
            {
                if (keepsOwnerOnClaim(b) || b->getFaction() == mine)
                    continue;
                g_originalOwners[b] = b->getFaction();
                b->setFaction(mine, NULL);
                ++moved;
            }
            else
            {
                std::map<Building*, Faction*>::iterator it = g_originalOwners.find(b);
                if (it == g_originalOwners.end())
                    continue;
                b->setFaction(it->second, NULL);
                ++moved;
            }
        }
        if (ours)
            g_originalOwners.clear();
        std::ostringstream note;
        note << (ours ? "gave Hub back, " : "claimed Hub, ") << moved << " buildings moved";
        refresh();
        logReport(note.str());
    }

    // Remove the selected buildings, if they're in the Hub, and append each to
    // mods/TownLedger/removals.txt (name, x, z, destroyed) so a curated list
    // can become the start's own clearance list. Not undoable.
    const char* REMOVALS_FILE = "mods\\TownLedger\\removals.txt";

    void onRemoveSelectedClick(MyGUI::WidgetPtr)
    {
        if (!ou || !ou->player || !g_hub)
            return;
        lektor<RootObject*> selected;
        ou->player->getAllSelectedObjects(selected, BUILDING);
        std::ostringstream note, file;
        note << "remove selected: " << selected.size() << " buildings selected";
        int removed = 0;
        for (uint32_t i = 0; i < selected.size(); ++i)
        {
            Building* b = static_cast<Building*>(selected[i]);
            if (!b)
                continue;
            Ogre::Vector3 p = b->getPosition();
            if (!insideHub(p))
            {
                note << "; skipped " << b->getName() << " (outside the Hub)";
                continue;
            }
            file << b->getName() << "\t" << p.x << "\t" << p.z << "\t"
                 << (b->isDestroyed() ? "destroyed" : "intact") << "\n";
            note << "; removed " << b->getName();
            ou->dynamicDestroyBuilding(hand(b));
            ++removed;
        }
        if (removed)
        {
            FILE* f = fopen(REMOVALS_FILE, "a");
            if (f)
            {
                fputs(file.str().c_str(), f);
                fclose(f);
            }
            else
                note << "; could not write " << REMOVALS_FILE;
        }
        DebugLog("TownLedger: [" + note.str() + "]");
        refresh();
    }

    // Remove every destroyed building in the Hub, through the same queue the
    // game uses for dynamic building removal. Not undoable: test on a spare save.
    void onClearRuinsClick(MyGUI::WidgetPtr)
    {
        if (!ou || !g_hub)
            return;
        std::vector<Building*> all;
        hubBuildings(all);
        int removed = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (!all[i]->isDestroyed())
                continue;
            ou->dynamicDestroyBuilding(hand(all[i]));
            ++removed;
        }
        std::ostringstream note;
        note << "cleared ruins, " << removed << " destroyed buildings queued for removal";
        refresh();
        logReport(note.str());
    }

    void onDevClick(MyGUI::WidgetPtr)
    {
        g_devFast = !g_devFast;
        if (g_devFast)
            applyDevSpeed();
        else
            restoreDevSpeed();
        refresh();
        logReport(g_devFast ? "dev fast on" : "dev fast off");
    }

    void onSuppliesClick(MyGUI::WidgetPtr)
    {
        std::string result = giveDevSupplies();
        DebugLog("TownLedger: [dev supplies] " + result);
        refresh();
    }

    // Buttons sit in two rows of three under the text.
    MyGUI::Button* addButton(MyGUI::Widget* client, int slot, const char* name, void (*click)(MyGUI::WidgetPtr))
    {
        const float width = 0.3f, gap = 0.02f;
        int col = slot % 3, row = slot / 3;
        MyGUI::Button* b = client->createWidgetReal<MyGUI::Button>(
            "Kenshi_Button1", 0.03f + col * (width + gap), 0.72f + row * 0.09f, width, 0.075f,
            MyGUI::Align::Left | MyGUI::Align::Bottom, name);
        b->eventMouseButtonClick += MyGUI::newDelegate(click);
        return b;
    }

    void createWindow()
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;

        g_window = gui->createWidgetReal<MyGUI::Window>(
            "Kenshi_WindowCX", 0.58f, 0.1f, 0.38f, 0.75f,
            MyGUI::Align::Default, "Window", "TownLedgerWindow");
        g_window->setCaption("Town Ledger");

        MyGUI::Widget* client = g_window->getClientWidget();
        g_text = client->createWidgetReal<MyGUI::EditBox>(
            "Kenshi_EditBox", 0.03f, 0.02f, 0.94f, 0.68f,
            MyGUI::Align::Stretch, "TownLedgerText");
        g_text->setEditMultiLine(true);
        g_text->setEditReadOnly(true);
        g_text->setEditWordWrap(true);
        g_text->setVisibleVScroll(true);

        addButton(client, 0, "TownLedgerLog", onLogClick)->setCaption("Log it");
        g_oreButton = addButton(client, 1, "TownLedgerOre", onOreClick);
        g_claimButton = addButton(client, 2, "TownLedgerClaim", onClaimClick);
        g_devButton = addButton(client, 3, "TownLedgerDev", onDevClick);
        addButton(client, 4, "TownLedgerSupplies", onSuppliesClick)->setCaption("Supplies");
        addButton(client, 5, "TownLedgerClearRuins", onClearRuinsClick)->setCaption("Clear ALL ruins");
        addButton(client, 6, "TownLedgerRemoveSelected", onRemoveSelectedClick)->setCaption("Remove selected");

        g_window->setVisible(false);
    }

    void toggle()
    {
        if (!g_window)
            createWindow();
        if (!g_window)
            return;
        bool show = !g_window->getVisible();
        g_window->setVisible(show);
        if (show)
            refresh();
    }

    void (*mainLoop_orig)(GameWorld*, float) = NULL;

    void mainLoop_hook(GameWorld* thisptr, float time)
    {
        mainLoop_orig(thisptr, time);

        ++g_frame;
        if (g_frame % FIND_HUB_EVERY_N_FRAMES == 1)
        {
            updateHubCache();
            if (g_devFast)
                applyDevSpeed();
        }

        // Once, after the first in-Hub ore reading, log the scale we got.
        if (!g_loggedScale && (g_ore[STONE].hubCalls || g_ore[IRON].hubCalls || g_ore[COPPER].hubCalls))
        {
            g_loggedScale = true;
            logReport("first ore reading in Hub");
        }

        bool down = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
        if (down && !g_keyWasDown && kenshiHasFocus())
            toggle();
        g_keyWasDown = down;

        if (g_window && g_window->getVisible() && g_frame % REFRESH_EVERY_N_FRAMES == 0)
            refresh();
    }

    // Experiment hooks are independent: one failing doesn't stop the rest.
    template <typename F>
    void tryHook(const char* what, intptr_t target, F detour, F* trampoline)
    {
        if (KenshiLib::SUCCESS != KenshiLib::AddHook(target, detour, trampoline))
            ErrorLog(std::string("TownLedger: could not hook ") + what);
        else
            DebugLog(std::string("TownLedger: hooked ") + what);
    }
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("TownLedger: loading");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            mainLoop_hook, &mainLoop_orig))
    {
        ErrorLog("TownLedger: could not hook the main loop");
        return;
    }

    tryHook("getResource", KenshiLib::GetRealAddress(&ZoneManager::getResource),
            getResource_hook, &getResource_orig);
    tryHook("getResourceBase", KenshiLib::GetRealAddress(&ZoneManager::getResourceBase),
            getResourceBase_hook, &getResourceBase_orig);
    tryHook("placementVerification", KenshiLib::GetRealAddress(&PreviewBuilding::_NV_placementVerification),
            placementVerification_hook, &placementVerification_orig);
    tryHook("checkProspectingIsNotZero", KenshiLib::GetRealAddress(&PreviewBuilding::_NV_checkProspectingIsNotZero),
            checkProspecting_hook, &checkProspecting_orig);

    g_createItem = (CreateItemFn)KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*, GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    g_createBuilding = (CreateBuildingFn)KenshiLib::GetRealAddress(&RootObjectFactory::createBuilding);

    DebugLog("TownLedger: ready, press F8 in game");
}
