// Town Ledger: kenshi-town's in-game panel.
//
// Two HUD buttons, Ledger and Roster, open the ledger (the Hub's state, the
// player faction, experiment and dev buttons) and the Town Roster (job
// profiles). Escape closes both. The original Hub experiment asked:
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
#include <kenshi/CharStats.h>
#include <kenshi/Inventory.h>
#include <kenshi/SaveManager.h>
#include <kenshi/util/hand.h>
// Tasker.h can't be included alongside AI/AITaskSystem.h: KenshiLib 0.4.0
// defines `enum taskPriority` and its enumerators in both. We need only two
// Tasker fields, so read them at the offsets Tasker.h documents.
namespace TaskerFields
{
    const size_t SUBJECT = 0x10;  // hand subject
    const size_t LOCATION = 0x58; // Ogre::Vector3 location
}
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
#include <mygui/MyGUI_ListBox.h>
#include <mygui/MyGUI_MultiListBox.h>
#include <mygui/MyGUI_TabControl.h>
#include <mygui/MyGUI_TabItem.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_ProgressBar.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>

#include "Squads.h"

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // The last thing a button did, shown at the top of the ledger as well as
    // logged: results were only in the log, so buttons looked dead.
    std::string g_lastAction;

    void actionLog(const std::string& line)
    {
        g_lastAction = line;
        DebugLog("TownLedger: " + line);
    }

    // No hotkeys: the HUD buttons open the windows (Kenshi's controls.cfg
    // already binds most F-keys, F8 screenshot and F9 quick load among them).
    bool g_escWasDown = false;
    // Armed when the Hub is (re)found, i.e. after a load; see autoReapplyTick.
    bool g_reapplyPending = false;
    size_t g_reapplyLastCount = 0;
    void onRosterClick(MyGUI::WidgetPtr);     // the Town Roster, further down
    void onArrivalNowClick(MyGUI::WidgetPtr); // arrivals, further down
    void onGarrisonDueNow(MyGUI::WidgetPtr);  // the garrison contract, further down
    void onCleanupHubNow(MyGUI::WidgetPtr);   // the Hub clean-up, further down
    void startContractOnClaim();               // the per-game record, further down
    void toast(const std::string& text);       // HUD toasts, further down
    std::string plural(int n, const char* one, const char* many); // the tray, further down
    const int REFRESH_EVERY_N_FRAMES = 30;
    const int FIND_HUB_EVERY_N_FRAMES = 120;

    // Ore the Hub gets while the override is on. Scale assumed 0..1; the
    // first real readings in the log will confirm or correct that.
    const float HUB_STONE = 0.8f;
    const float HUB_IRON = 0.6f;
    const float HUB_COPPER = 0.5f;

    MyGUI::Window* g_window = NULL;
    MyGUI::EditBox* g_text = NULL;
    MyGUI::Button* g_claimButton = NULL;
    MyGUI::Button* g_devButton = NULL;
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

    std::string factionName(Faction* f);

    size_t buildingCount(TownBase* t)
    {
        TownBuildingsManager* mgr = t->getBuildingsManager();
        return mgr ? mgr->buildingEntities.size() : 0;
    }

    // More than one town object can be named "The Hub" (one with 0 buildings
    // was picked on 2026-11-22, so the claim went to the wrong one). Prefer
    // the active one with the most buildings; log every candidate when the
    // set changes.
    std::string g_hubCandidatesSeen;
    std::vector<TownBase*> g_hubTwins;
    bool g_worldChanged = false; // set by findHub, consumed by currentContract

    TownBase* findHub()
    {
        if (!shou || !shou->townList)
            return NULL;
        lektor<RootObject*>& towns = shou->townList->getAllTowns();
        TownBase* best = NULL;
        int bestScore = -1;
        std::ostringstream seen;
        for (uint32_t i = 0; i < towns.size(); ++i)
        {
            TownBase* t = static_cast<TownBase*>(towns[i]);
            if (!t || t->getName().find("Hub") == std::string::npos)
                continue;
            size_t n = buildingCount(t);
            bool active = t->isActive();
            bool exact = t->getName() == "The Hub";
            // By building count, not "active": which part is active flips with ownership.
            int score = (exact ? 1000000 : 0) + int(n);
            Ogre::Vector3 p = t->getPosition();
            seen << "\n  " << t->getName() << " | " << factionName(t->getFaction()) << " | "
                 << n << " buildings | " << (active ? "active" : "inactive") << " | at "
                 << int(p.x) << "," << int(p.z) << " | " << (t->isTown() ? "Town" : "not a Town");
            if (score > bestScore)
            {
                best = t;
                bestScore = score;
            }
        }
        // Every town near the chosen Hub, whatever it's called: claimed
        // buildings may have been regrouped into a player town on reload.
        if (best)
        {
            Ogre::Vector3 c = best->getPosition();
            for (uint32_t i = 0; i < towns.size(); ++i)
            {
                TownBase* t = static_cast<TownBase*>(towns[i]);
                if (!t || t == best || t->getName().find("Hub") != std::string::npos)
                    continue;
                Ogre::Vector3 p = t->getPosition();
                float dx = p.x - c.x, dz = p.z - c.z;
                if (dx * dx + dz * dz > 5000.0f * 5000.0f)
                    continue;
                seen << "\n  nearby: " << t->getName() << " | " << factionName(t->getFaction()) << " | "
                     << buildingCount(t) << " buildings | " << (t->isActive() ? "active" : "inactive")
                     << " | at " << int(p.x) << "," << int(p.z) << " | radius " << t->getRadius();
            }
        }
        if (seen.str() != g_hubCandidatesSeen)
        {
            g_hubCandidatesSeen = seen.str();
            DebugLog("TownLedger: towns at the Hub:" + g_hubCandidatesSeen);
        }

        // Twins: other town objects with the same name at the same spot. The
        // Hub loads as the real town plus an empty one, and new buildings get
        // assigned to the empty one.
        // A different world has loaded if none of the Hub objects we last saw
        // still exist; an unsaved game's per-save record resets then.
        static std::vector<TownBase*> s_lastParts;
        if (best)
        {
            bool known = s_lastParts.empty();
            for (uint32_t i = 0; i < towns.size() && !known; ++i)
                known = std::find(s_lastParts.begin(), s_lastParts.end(), static_cast<TownBase*>(towns[i])) !=
                        s_lastParts.end();
            if (!known)
                g_worldChanged = true;
        }

        g_hubTwins.clear();
        if (best)
        {
            Ogre::Vector3 c = best->getPosition();
            for (uint32_t i = 0; i < towns.size(); ++i)
            {
                TownBase* t = static_cast<TownBase*>(towns[i]);
                if (!t || t == best || t->getName() != best->getName())
                    continue;
                Ogre::Vector3 p = t->getPosition();
                float dx = p.x - c.x, dz = p.z - c.z;
                if (dx * dx + dz * dz < 100.0f * 100.0f)
                    g_hubTwins.push_back(t);
            }
            s_lastParts.assign(1, best);
            s_lastParts.insert(s_lastParts.end(), g_hubTwins.begin(), g_hubTwins.end());
        }
        return best;
    }

    // The Hub is two town objects at one spot, and they are one town to us:
    // the original town's buildings in one, the buildings the player put up in
    // the other, and which of them Kenshi calls "active" flips with ownership.
    bool hubPartIsOurs(TownBase* t)
    {
        return t && ou && ou->player && t->getFaction() == ou->player->getFaction();
    }

    bool hubIsOurs();

    std::vector<TownBase*> hubParts()
    {
        std::vector<TownBase*> parts;
        if (g_hub)
            parts.push_back(g_hub);
        parts.insert(parts.end(), g_hubTwins.begin(), g_hubTwins.end());
        return parts;
    }

    // The player's base: the Hub part holding the buildings the player put up,
    // where Kenshi's builders look for work. While the Hub is ours exactly one
    // part is "active" (the log, 2026-11-22), and that's the base. It's picked
    // once and remembered until the parts change (a load), because building
    // counts can't identify it: consolidating makes the base the bigger part.
    TownBase* g_basePart = NULL;
    bool g_consolidateFailed = false;

    TownBase* hubBase()
    {
        std::vector<TownBase*> parts = hubParts();
        if (parts.size() < 2)
            return g_hub;
        if (g_basePart && std::find(parts.begin(), parts.end(), g_basePart) != parts.end())
            return g_basePart;
        g_basePart = NULL;
        if (!hubIsOurs())
            return NULL;
        TownBase* active = NULL;
        int nActive = 0;
        for (size_t i = 0; i < parts.size(); ++i)
            if (parts[i]->isActive())
            {
                active = parts[i];
                ++nActive;
            }
        if (nActive == 1)
            g_basePart = active;
        return g_basePart;
    }

    // Make the Hub one town where it matters: move every building we own out
    // of the original part into the base, so builders (and whatever else
    // looks only at the player's base) see all of it. Runs on Claim and after
    // each load. Moves one building first and checks it landed, and stops if
    // Building::_changeTown doesn't take. Buildings we don't own (the bar's
    // Trade Ninjas, the Shinobi) stay put. Returns "" when there was nothing to do.
    std::string consolidateHub()
    {
        TownBase* base = hubBase();
        std::vector<TownBase*> parts = hubParts();
        if (!base || parts.size() < 2 || !base->isTown() || !ou || !ou->player)
            return "";
        Faction* mine = ou->player->getFaction();
        std::vector<Building*> toMove;
        for (size_t p = 0; p < parts.size(); ++p)
        {
            if (parts[p] == base)
                continue;
            TownBuildingsManager* mgr = parts[p]->getBuildingsManager();
            if (!mgr)
                continue;
            for (ogre_unordered_map<Building*, TownBuildingsManager::BuildingInfo>::type::iterator it =
                     mgr->buildingEntities.begin();
                 it != mgr->buildingEntities.end(); ++it)
                if (it->first && it->first->getFaction() == mine)
                    toMove.push_back(it->first);
        }
        if (toMove.empty() || g_consolidateFailed)
            return "";
        toMove[0]->_changeTown(base->isTown());
        if (toMove[0]->getTown() != base)
        {
            g_consolidateFailed = true; // don't retry (and re-log) every tick
            return "consolidating the Hub: the first move did NOT take, so nothing else was moved";
        }
        for (size_t i = 1; i < toMove.size(); ++i)
            toMove[i]->_changeTown(base->isTown());
        std::ostringstream out;
        out << "moved " << toMove.size() << " buildings into the base (now " << buildingCount(base) << ")";
        return out.str();
    }

    // How finished a building is: 1 when standing, below 1 while unfinished.
    float buildProgress(Building* b)
    {
        Building::ConstructionState* st = b->getBuildState();
        return st ? st->constructionProgress : 1.0f;
    }

    void setHubOwner(Faction* owner)
    {
        if (!g_hub || !owner)
            return;
        if (g_hub->getFaction() != owner)
            g_hub->setFaction(owner, NULL);
        for (size_t i = 0; i < g_hubTwins.size(); ++i)
            if (g_hubTwins[i]->getFaction() != owner)
            {
                DebugLog("TownLedger: twin Hub " + factionName(g_hubTwins[i]->getFaction()) + " -> " +
                         factionName(owner));
                g_hubTwins[i]->setFaction(owner, NULL);
            }
    }

    // Heal a half-claimed Hub, only ever towards the player: if any part is
    // ours, all of it is. (Syncing from whichever part loaded first once
    // handed a saved claim back to the Outlaws on load.) Give back sets the
    // owner explicitly with setHubOwner.
    void syncHubTwins()
    {
        if (!g_hub || !ou || !ou->player)
            return;
        bool anyOurs = hubPartIsOurs(g_hub);
        for (size_t i = 0; i < g_hubTwins.size() && !anyOurs; ++i)
            anyOurs = hubPartIsOurs(g_hubTwins[i]);
        if (anyOurs)
            setHubOwner(ou->player->getFaction());
    }

    // The Hub's vanilla owner. Looked up by stringId so Give back works even
    // after a reload, when the Hub already loads as ours.
    const char* HUB_VANILLA_OWNER_SID = "42022-rebirth.mod"; // Holy Nation Outlaws

    Faction* hubVanillaOwner()
    {
        return (ou && ou->factionMgr) ? ou->factionMgr->getFactionByStringID(HUB_VANILLA_OWNER_SID) : NULL;
    }

    // A claim made in play lets the player build in the Hub, but after a
    // reload the town loads as Nameless and building is refused again: the
    // save keeps the owner but not whatever setFaction registered. So when the
    // Hub loads already ours, hand it over again (old owner, then us) once.
    void reassertClaim(TownBase* hub)
    {
        if (!ou || !ou->player)
            return;
        Faction* mine = ou->player->getFaction();
        Faction* vanilla = hubVanillaOwner();
        if (hub->getFaction() != mine || !vanilla || vanilla == mine)
            return;
        hub->setFaction(vanilla, NULL);
        hub->setFaction(mine, NULL);
        DebugLog("TownLedger: Hub loaded as ours; claim re-applied (via " + factionName(vanilla) + ")");
    }

    void updateHubCache()
    {
        TownBase* hub = findHub();
        if (hub != g_hub)
        {
            g_hub = hub;
            g_hubOriginalFaction = hub ? hub->getFaction() : NULL;
            g_reapplyPending = true; // a new Hub object means a (re)load
            g_reapplyLastCount = 0;
            if (hub)
            {
                DebugLog("TownLedger: found town '" + hub->getName() + "' owned by " +
                         factionName(hub->getFaction()));
                if (ou && ou->player && hub->getFaction() == ou->player->getFaction())
                {
                    g_hubOriginalFaction = hubVanillaOwner();
                    reassertClaim(hub);
                }
            }
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
        syncHubTwins();
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
    bool g_oreOverride = true; // always on (Bill, 2026-11-22): no toggle
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

    // ---- Building inside the Hub. PreviewBuilding::inTown is the town a
    // placement would INTRUDE on: the click handler (RVA 0x4E27E0 in the disk
    // exe) refuses with "Can't build too close to another town." whenever it's
    // set, and a null inTown means the spot is free (it shows blue vs green).
    // The game flags a town there unless the town counts as the player's.
    //
    // The Hub loads as two town objects at one spot, the real one and an empty
    // twin, and the twin stays Holy Nation Outlaws after a claim, so it got
    // flagged. Fixes: syncHubTwins() gives the twin the Hub's owner, and the
    // hook below clears inTown if it is ever our own Hub or its twin.
    // (An earlier attempt did the opposite, filling inTown in, and caused the
    // refusal it meant to fix.)
    //
    // The unnamed function at RVA 0x4D6D90 on disk (found by signature at
    // 0x4D6300 in memory) also writes inTown but didn't run during placement.

    const uintptr_t WHICH_TOWN_RVA = 0x4D6D90;
    // The function's first 12 bytes in Kenshi 1.0.68, checked before hooking
    // so a different game build is left alone.
    const unsigned char WHICH_TOWN_PROLOGUE[] = {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54,
                                                 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

    long g_whichTownCalls = 0, g_whichTownNullInHub = 0, g_whichTownFixed = 0;
    std::string g_whichTownDiag;

    void (*whichTown_orig)(PreviewBuilding*) = NULL;
    bool hubIsOurs();

    std::string describeTown(TownBase* t)
    {
        if (!t)
            return "none";
        Faction* f = t->getFaction();
        std::ostringstream out;
        out << t->getName() << " (" << factionName(f) << (f && f->isPlayer ? ", isPlayer" : ", not player")
            << (ou && ou->player && f == ou->player->getFaction() ? ", is our faction" : "") << ")";
        return out.str();
    }

    // After the game decides which town a preview inside the Hub intrudes on,
    // record it (once per change) and clear it if it's our own Hub.
    std::string g_lastAssigned;

    void giveHubIfTownless(PreviewBuilding* self)
    {
        ++g_whichTownCalls;
        if (!g_hub || !insideHub(self->getPosition()))
            return;

        // Record which town the game assigned, each time it changes.
        {
            std::ostringstream a;
            a << (self->inTown ? describeTown(self->inTown) : std::string("none"))
              << (self->inTown && self->inTown == g_hub->isTown() ? " = our Hub" : "");
            if (self->inTown)
                a << ", " << buildingCount(self->inTown) << " buildings";
            if (a.str() != g_lastAssigned)
            {
                g_lastAssigned = a.str();
                DebugLog("TownLedger: [building town assigned] " + g_lastAssigned);
            }
        }

        if (!self->inTown)
        {
            ++g_whichTownNullInHub;
            return;
        }

        // inTown is the town being intruded on, and the click handler refuses
        // ("Can't build too close to another town.") whenever it's set; null
        // means the spot is free. If the game flags our own Hub, or the Hub's
        // empty twin, while the Hub is ours, clear it.
        if (!hubIsOurs())
            return;
        TownBase* flagged = self->inTown;
        bool ourHub = flagged == g_hub;
        for (size_t i = 0; i < g_hubTwins.size() && !ourHub; ++i)
            ourHub = flagged == g_hubTwins[i];
        if (ourHub)
        {
            self->inTown = NULL;
            ++g_whichTownFixed;
        }
    }

    void whichTown_hook(PreviewBuilding* self)
    {
        whichTown_orig(self);
        giveHubIfTownless(self);
    }

    // The click handler that refuses placement calls the preview's
    // buildingPlacementUpdate (vtable +0x50) and then reads inTown, so fixing
    // inTown on the way out of it covers whichever code set it. The building-
    // town function above turned out not to run during ordinary placement.
    void (*placementUpdate_orig)(PreviewBuilding*, physHit&, bool&, bool&) = NULL;

    void placementUpdate_hook(PreviewBuilding* self, physHit& hit, bool& rightUp, bool& leftUp)
    {
        placementUpdate_orig(self, hit, rightUp, leftUp);
        giveHubIfTownless(self);
    }

    // At RVA 0x4D6D90 in memory the prologue check failed, so the function is
    // found by a signature from its body instead: `mov rsi, rcx` through
    // `mov r15d, 10`, unique in kenshi_x64.exe's .text, 0x43 bytes in.
    const unsigned char WHICH_TOWN_SIG[] = {
        0x48, 0x8b, 0xf1, 0x33, 0xff, 0x44, 0x8b, 0xef, 0x89, 0x7c, 0x24, 0x4c, 0x48, 0x8b, 0x89,
        0xd8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x75, 0x07, 0x48, 0x8b, 0x8e, 0xd0, 0x00, 0x00,
        0x00, 0x41, 0xbf, 0x0a, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x07, 0xe8};
    const size_t WHICH_TOWN_SIG_OFFSET = 0x43;

    std::string hexBytes(const unsigned char* p, size_t n)
    {
        std::ostringstream out;
        for (size_t i = 0; i < n; ++i)
        {
            char b[4];
            sprintf(b, "%02x ", p[i]);
            out << b;
        }
        return out.str();
    }

    // Every match of `sig` in the game module's .text.
    void findInGameText(const unsigned char* sig, size_t n, std::vector<unsigned char*>& out)
    {
        out.clear();
        unsigned char* base = reinterpret_cast<unsigned char*>(GetModuleHandle(NULL));
        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
        {
            if (memcmp(sec->Name, ".text", 5) != 0)
                continue;
            unsigned char* p = base + sec->VirtualAddress;
            unsigned char* end = p + sec->Misc.VirtualSize - n;
            for (; p < end; ++p)
                if (*p == sig[0] && memcmp(p, sig, n) == 0)
                    out.push_back(p);
        }
    }

    void hookWhichTown()
    {
        unsigned char* base = reinterpret_cast<unsigned char*>(GetModuleHandle(NULL));
        DebugLog("TownLedger: bytes at RVA 0x4D6D90 in memory: " + hexBytes(base + WHICH_TOWN_RVA, 16));

        std::vector<unsigned char*> hits;
        findInGameText(WHICH_TOWN_SIG, sizeof(WHICH_TOWN_SIG), hits);
        if (hits.size() != 1)
        {
            std::ostringstream msg;
            msg << "TownLedger: building-town signature matched " << hits.size() << " times; not hooking";
            ErrorLog(msg.str());
            return;
        }
        unsigned char* fn = hits[0] - WHICH_TOWN_SIG_OFFSET;
        std::ostringstream where;
        where << "TownLedger: building-town function found at RVA 0x" << std::hex << (fn - base)
              << ", starts " << hexBytes(fn, 12);
        DebugLog(where.str());
        if (memcmp(fn, WHICH_TOWN_PROLOGUE, sizeof(WHICH_TOWN_PROLOGUE)) != 0)
        {
            ErrorLog("TownLedger: building-town function's prologue isn't as expected; not hooking it");
            return;
        }
        if (KenshiLib::SUCCESS != KenshiLib::AddHook(reinterpret_cast<intptr_t>(fn), whichTown_hook, &whichTown_orig))
            ErrorLog("TownLedger: could not hook the building-town function");
        else
            DebugLog("TownLedger: hooked the building-town function");
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
        {"Steel bars", 10}, // "Steel Bars" is the storage building, not the item
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

    // ---- Labour test: citizens and roles.
    //
    // A citizen is a player-faction character the player can no longer select;
    // the plugin gives them their jobs. A role is a list of permajobs captured
    // from a character the player set up by hand, so we never guess task types:
    // Kenshi's own UI decides what "operate this mine" means, we copy it.
    //
    // Test-only state, in memory: a reload forgets who is a citizen.

    std::set<Character*> g_citizens;
    std::string currentActivity(Character* c);

    struct RoleJob
    {
        TaskType type;
        RootObject* subject;
        Ogre::Vector3 location;
        std::string name;
    };
    std::vector<RoleJob> g_role;
    std::string g_roleFrom;

    bool isCitizen(RootObject* obj)
    {
        return obj && g_citizens.count(static_cast<Character*>(obj)) != 0;
    }

    void selectedCharacters(std::vector<Character*>& out)
    {
        out.clear();
        if (!ou || !ou->player)
            return;
        lektor<RootObject*> selected;
        ou->player->getAllSelectedObjects(selected, CHARACTER);
        for (uint32_t i = 0; i < selected.size(); ++i)
            if (selected[i])
                out.push_back(static_cast<Character*>(selected[i]));
    }

    RootObject* taskSubject(const Tasker* t)
    {
        if (!t)
            return NULL;
        const hand* h = reinterpret_cast<const hand*>(reinterpret_cast<const char*>(t) + TaskerFields::SUBJECT);
        return h->getRootObject();
    }

    Ogre::Vector3 taskLocation(const Tasker* t)
    {
        if (!t)
            return Ogre::Vector3::ZERO;
        return *reinterpret_cast<const Ogre::Vector3*>(reinterpret_cast<const char*>(t) + TaskerFields::LOCATION);
    }

    // ---- Dev: money and a recruit, so the labour test doesn't wait on the
    // early game. The player's cats live in the faction's Ownerships record;
    // its class is in Platoon.h, which clashes with Building.h in KenshiLib
    // 0.4.0, so the money field is read at its documented offset.

    const size_t OWNERSHIPS_MONEY = 0x88; // Ownerships::money

    int* factionMoney(Faction* f)
    {
        if (!f || !f->factionOwnerships)
            return NULL;
        return reinterpret_cast<int*>(reinterpret_cast<char*>(f->factionOwnerships) + OWNERSHIPS_MONEY);
    }

    const int DEV_MONEY = 5000;
    const char* DEV_RECRUIT_SID = "2205-gamedata.base"; // Drifter

    // createRandomCharacter takes Vector3 by value; exact signature for the x64
    // ABI, as KenshiCoop declares it.
    typedef RootObject* (__fastcall* CreateCharFn)(
        RootObjectFactory* self, Faction* faction, Ogre::Vector3 position,
        RootObjectContainer* owner, GameData* characterTemplate, Building* home, float age);
    CreateCharFn g_createChar = NULL;

    void onMoneyClick(MyGUI::WidgetPtr)
    {
        int* money = (ou && ou->player) ? factionMoney(ou->player->getFaction()) : NULL;
        if (!money)
        {
            actionLog("[dev money] no player faction money record");
            return;
        }
        int before = *money;
        *money += DEV_MONEY;
        std::ostringstream note;
        note << "dev money: " << before << " -> " << *money;
        actionLog("[" + note.str() + "]");
    }

    void onSpawnRecruitClick(MyGUI::WidgetPtr)
    {
        std::vector<Character*> chars;
        selectedCharacters(chars);
        if (chars.empty() || !g_createChar || !ou->theFactory)
        {
            actionLog("[spawn recruit] select one of your characters first");
            return;
        }
        GameData* tmpl = ou->gamedata.getData(DEV_RECRUIT_SID);
        if (!tmpl)
        {
            actionLog("[spawn recruit] Drifter template not found");
            return;
        }
        Ogre::Vector3 at = chars[0]->getPosition();
        Ogre::Vector3 pos(at.x - 20.0f, at.y, at.z);
        RootObject* r = g_createChar(ou->theFactory, ou->player->getFaction(), pos, NULL, tmpl, NULL, 25.0f);
        if (!r)
        {
            actionLog("[spawn recruit] createRandomCharacter returned nothing");
            return;
        }
        Character* c = static_cast<Character*>(r);
        bool joined = ou->player->recruit(c, false);
        actionLog("[spawn recruit] " + c->getName() + (joined ? " joined" : " spawned, recruit refused"));
    }

    std::string describeJobs(Character* c)
    {
        std::ostringstream out;
        int n = c->getPermajobCount();
        if (n == 0)
            return "no jobs";
        for (int i = 0; i < n; ++i)
        {
            const Tasker* t = c->getPermajobData(i);
            RootObject* subject = taskSubject(t);
            out << (i ? "; " : "") << c->getPermajobName(i) << " [" << int(c->getPermajob(i)) << "]";
            if (subject)
                out << " -> " << subject->getName();
        }
        return out.str();
    }

    // Selection lock. Every way of selecting a character (click, box, squad
    // portrait) is expected to reach one of these two; the test checks that.
    long g_selectionBlocked = 0;

    void (*selectObject_orig)(PlayerInterface*, RootObject*, bool) = NULL;

    void selectObject_hook(PlayerInterface* self, RootObject* obj, bool modifier)
    {
        if (isCitizen(obj))
        {
            ++g_selectionBlocked;
            return;
        }
        selectObject_orig(self, obj, modifier);
    }

    void (*objectSelected_orig)(PlayerInterface*, RootObject*, bool) = NULL;

    void objectSelected_hook(PlayerInterface* self, RootObject* obj, bool select)
    {
        if (select && isCitizen(obj))
        {
            ++g_selectionBlocked;
            return;
        }
        objectSelected_orig(self, obj, select);
    }

    // Citizens carry a tag in front of their name: "[Citizen] Double" with no
    // profile yet, "[Miner] Double" once given one (see the Town Roster).
    // Untagged characters are the retinue. The tag shows in game everywhere a
    // name does, and since names are saved it is how citizenship and profiles
    // survive a reload (rebuildCitizensFromNames).
    const std::string CITIZEN_TAG = "Citizen";

    // "[Miner] Double" -> "Miner"; "" when untagged.
    std::string nameTag(Character* c)
    {
        std::string n = c->getName();
        size_t close = n.find("] ");
        if (n.empty() || n[0] != '[' || close == std::string::npos)
            return "";
        return n.substr(1, close - 1);
    }

    std::string bareName(Character* c)
    {
        std::string n = c->getName();
        return nameTag(c).empty() ? n : n.substr(n.find("] ") + 2);
    }

    void setNameTag(Character* c, const std::string& tag)
    {
        std::string want = tag.empty() ? bareName(c) : "[" + tag + "] " + bareName(c);
        if (c->getName() != want)
            c->setName(want);
    }

    bool hasCitizenPrefix(Character* c)
    {
        return !nameTag(c).empty();
    }

    void rebuildCitizensFromNames()
    {
        if (!ou || !ou->player)
            return;
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        std::set<Character*> found;
        for (uint32_t i = 0; i < people.size(); ++i)
            if (people[i] && hasCitizenPrefix(people[i]))
                found.insert(people[i]);
        if (found != g_citizens)
        {
            g_citizens = found;
            std::ostringstream note;
            note << "TownLedger: citizens from names: " << g_citizens.size();
            DebugLog(note.str());
        }
    }

    void onMakeCitizenClick(MyGUI::WidgetPtr)
    {
        std::vector<Character*> chars;
        selectedCharacters(chars);
        std::ostringstream note;
        note << "make citizen:";
        for (size_t i = 0; i < chars.size(); ++i)
        {
            Character* c = chars[i];
            if (!hasCitizenPrefix(c))
                setNameTag(c, CITIZEN_TAG);
            g_citizens.insert(c);
            ou->player->objectSelected(c, false);
            note << " " << c->getName();
        }
        if (chars.empty())
            note << " nobody selected";
        actionLog("[" + note.str() + "]");
    }

    void onFreeCitizensClick(MyGUI::WidgetPtr)
    {
        std::ostringstream note;
        note << "freed " << g_citizens.size() << " citizens";
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
            setNameTag(*it, "");
        g_citizens.clear();
        actionLog("[" + note.str() + "]");
    }

    void onCaptureRoleClick(MyGUI::WidgetPtr)
    {
        std::vector<Character*> chars;
        selectedCharacters(chars);
        if (chars.empty())
        {
            actionLog("[capture role] select the character whose jobs to copy");
            return;
        }
        Character* c = chars[0];
        g_role.clear();
        g_roleFrom = c->getName();
        for (int i = 0; i < c->getPermajobCount(); ++i)
        {
            const Tasker* t = c->getPermajobData(i);
            RoleJob job;
            job.type = c->getPermajob(i);
            job.subject = taskSubject(t);
            job.location = taskLocation(t);
            job.name = c->getPermajobName(i);
            g_role.push_back(job);
        }
        actionLog("[capture role] from " + g_roleFrom + ": " + describeJobs(c));
    }

    // Add one standing job to a citizen with Character::addJob(shift=false,
    // add=true). On 2026-11-22 this made Double mine the stone mine, but his
    // job list read empty straight afterwards, so the list is checked again a
    // few seconds later (checkAssignedJobs) instead of retrying other ways,
    // which would stack duplicate jobs.

    int g_checkJobsAtFrame = -1;

    std::string addStandingJob(Character* c, const RoleJob& job)
    {
        c->addJob(job.type, job.subject, false, true, job.location);
        return "added";
    }

    void checkAssignedJobs()
    {
        std::ostringstream note;
        note << "job lists after assigning:";
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
            note << "\n  " << (*it)->getName() << ": " << describeJobs(*it) << " (doing: "
                 << currentActivity(*it) << ")";
        actionLog("[" + note.str() + "]");
    }

    void onAssignRoleClick(MyGUI::WidgetPtr)
    {
        if (g_role.empty())
        {
            actionLog("[assign role] no role yet: select a character with jobs set up, click Capture role");
            return;
        }
        if (g_citizens.empty())
        {
            actionLog("[assign role] no citizens yet: select a character, click Make citizen");
            return;
        }
        std::ostringstream note;
        note << "assign role (" << g_role.size() << " jobs from " << g_roleFrom << ") to "
             << g_citizens.size() << " citizens";
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
        {
            Character* c = *it;
            c->clearPermajobs();
            for (size_t j = 0; j < g_role.size(); ++j)
                note << "\n  " << g_role[j].name << ": " << addStandingJob(c, g_role[j]);
            note << "\n  " << c->getName() << " now: " << describeJobs(c);
        }
        actionLog("[" + note.str() + "]");
        g_checkJobsAtFrame = g_frame + 180; // about three seconds
    }

    std::string buildCitizenReport()
    {
        std::ostringstream out;
        out << "\n--- Labour test ---\n";
        out << "Role: ";
        if (g_role.empty())
            out << "none captured";
        else
        {
            out << g_role.size() << " jobs from " << g_roleFrom << ":";
            for (size_t j = 0; j < g_role.size(); ++j)
                out << (j ? "," : "") << " " << g_role[j].name;
        }
        out << "\nCitizens: " << g_citizens.size() << ", selections blocked " << g_selectionBlocked << "\n";
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
            out << "  " << (*it)->getName() << " - " << currentActivity(*it) << "\n    " << describeJobs(*it) << "\n";
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

    // Every building in the Hub: both town objects (see setHubOwner).
    void hubBuildings(std::vector<Building*>& out)
    {
        out.clear();
        if (!g_hub)
            return;
        std::vector<TownBase*> parts(1, g_hub);
        parts.insert(parts.end(), g_hubTwins.begin(), g_hubTwins.end());
        for (size_t p = 0; p < parts.size(); ++p)
        {
            TownBuildingsManager* mgr = parts[p]->getBuildingsManager();
            if (!mgr)
                continue;
            for (ogre_unordered_map<Building*, TownBuildingsManager::BuildingInfo>::type::iterator it =
                     mgr->buildingEntities.begin();
                 it != mgr->buildingEntities.end(); ++it)
                if (it->first)
                    out.push_back(it->first);
        }
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
        out << "  building town: " << g_whichTownCalls << " previews, " << g_whichTownNullInHub
            << " clear in the Hub, " << g_whichTownFixed << " cleared of our own Hub\n";
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
        // The box is small: the Hub's state first, details below the fold.
        if (g_hub)
            out << "Hub: " << factionName(g_hub->getFaction()) << ", " << buildingCount(g_hub)
                << " buildings" << (hubIsOurs() ? " (ours)" : "") << "\nBuild fixes: " << g_whichTownFixed
                << " of " << g_whichTownCalls << " previews cleared (our Hub flagged as foreign)\n";
        else
            out << "Hub not found\n";
        if (!g_lastAction.empty())
            out << "Last: " << g_lastAction << "\n";
        out << "Population: " << people.size() << ", citizens " << g_citizens.size()
            << ", role " << (g_role.empty() ? std::string("none") : g_roleFrom) << "\n\n";
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            Character* c = people[i];
            if (!c)
                continue;
            out << c->displayName << "  -  " << currentActivity(c) << "\n";
        }
        out << buildCitizenReport();
        out << "\n" << buildHubReport();
        return out.str();
    }

    void logReport(const std::string& why)
    {
        g_lastAction = "[" + why + "]";
        DebugLog("TownLedger: [" + why + "]\n" + buildHubReport());
    }

    bool hubIsOurs()
    {
        if (hubPartIsOurs(g_hub))
            return true;
        for (size_t i = 0; i < g_hubTwins.size(); ++i)
            if (hubPartIsOurs(g_hubTwins[i]))
                return true;
        return false;
    }

    void refresh()
    {
        if (g_text)
            g_text->setCaption(buildLedger());
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

    void onClaimClick(MyGUI::WidgetPtr)
    {
        if (!g_hub || !ou || !ou->player)
            return;
        bool ours = hubIsOurs();
        Faction* mine = ou->player->getFaction();
        Faction* to = ours ? g_hubOriginalFaction : mine;
        if (ours && !to)
            to = hubVanillaOwner();
        DebugLog("TownLedger: setFaction " + factionName(g_hub->getFaction()) + " -> " + factionName(to));
        setHubOwner(to);

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
        else
        {
            std::string merged = consolidateHub();
            if (!merged.empty())
                DebugLog("TownLedger: " + merged);
            startContractOnClaim(); // the clean-up follows on the next tick
        }
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
        actionLog("[" + note.str() + "]");
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
        actionLog("[dev supplies] " + result);
        refresh();
    }

    // Buttons sit in rows of three under the text.
    MyGUI::Button* addButton(MyGUI::Widget* client, int slot, const char* name, void (*click)(MyGUI::WidgetPtr))
    {
        const float width = 0.3f, gap = 0.02f;
        int col = slot % 3, row = slot / 3;
        MyGUI::Button* b = client->createWidgetReal<MyGUI::Button>(
            "Kenshi_Button1", 0.03f + col * (width + gap), 0.38f + row * 0.098f, width, 0.085f,
            MyGUI::Align::Left | MyGUI::Align::Bottom, name);
        b->eventMouseButtonClick += MyGUI::newDelegate(click);
        return b;
    }

    // The skin's X raises a "close" window button; nothing handled it.
    void onWindowButton(MyGUI::Window* sender, const std::string& name)
    {
        if (name == "close")
            sender->setVisible(false);
    }

    void createLedgerContent(MyGUI::Widget* client)
    {
        g_text = client->createWidgetReal<MyGUI::EditBox>(
            "Kenshi_EditBox", 0.03f, 0.02f, 0.94f, 0.34f,
            MyGUI::Align::Stretch, "TownLedgerText");
        g_text->setEditMultiLine(true);
        g_text->setEditReadOnly(true);
        g_text->setEditWordWrap(true);
        g_text->setVisibleVScroll(true);

        // Row 1: the labour test. Row 2: the town. Rows 3-4: development tools.
        addButton(client, 0, "TownLedgerCaptureRole", onCaptureRoleClick)->setCaption("Capture role");
        addButton(client, 1, "TownLedgerMakeCitizen", onMakeCitizenClick)->setCaption("Make citizen");
        addButton(client, 2, "TownLedgerAssignRole", onAssignRoleClick)->setCaption("Assign role");
        g_claimButton = addButton(client, 3, "TownLedgerClaim", onClaimClick);
        addButton(client, 4, "TownLedgerFreeCitizens", onFreeCitizensClick)->setCaption("Free citizens");
        addButton(client, 5, "TownLedgerLog", onLogClick)->setCaption("Log it");
        addButton(client, 6, "TownLedgerSupplies", onSuppliesClick)->setCaption("Supplies");
        addButton(client, 7, "TownLedgerMoney", onMoneyClick)->setCaption("+5000 cats");
        addButton(client, 8, "TownLedgerSpawnRecruit", onSpawnRecruitClick)->setCaption("Spawn recruit");
        g_devButton = addButton(client, 9, "TownLedgerDev", onDevClick);
        addButton(client, 10, "TownLedgerRemoveSelected", onRemoveSelectedClick)->setCaption("Remove selected");
        addButton(client, 11, "TownLedgerClearRuins", onClearRuinsClick)->setCaption("Clear ALL ruins");
        addButton(client, 12, "TownLedgerRoster", onRosterClick)->setCaption("Roster tab");
        addButton(client, 13, "TownLedgerArrivalNow", onArrivalNowClick)->setCaption("Arrival now");
        addButton(client, 14, "TownLedgerGarrisonDue", onGarrisonDueNow)->setCaption("Garrison due now");
        addButton(client, 15, "TownLedgerCleanup", onCleanupHubNow)->setCaption("Clean up Hub now");
    }

    void showTab(size_t tab); // the main window, further down

    void toggle()
    {
        showTab(3); // Ledger
    }

    // ==== Town Roster: job profiles, and who works which ================
    //
    // A profile is a named set of standing jobs, captured from a character the
    // player set up by hand. Each job remembers its building by name and
    // position, so it can be found again after a reload or a rebuild: the
    // nearest building of that name in the Hub. Profiles are saved to
    // mods/TownLedger/profiles.txt. Who has which profile lives in the name
    // tag ("[Miner] Double"), so assignments are saved with the game.

    struct ProfileJob
    {
        TaskType type;
        std::string label;    // the game's own job name, e.g. "Operating machine"
        std::string building; // target building's name, "" for none
        Ogre::Vector3 at;     // where that building was
        Ogre::Vector3 location;
    };

    struct Profile
    {
        std::string name;
        std::vector<ProfileJob> jobs;
    };

    std::vector<Profile> g_profiles;
    const char* PROFILES_FILE = "mods\\TownLedger\\profiles.txt";

    Profile* findProfile(const std::string& name)
    {
        for (size_t i = 0; i < g_profiles.size(); ++i)
            if (g_profiles[i].name == name)
                return &g_profiles[i];
        return NULL;
    }

    // Tabs, newlines and brackets would break the file or the name tag.
    std::string fileSafe(const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i] != '\t' && s[i] != '\n' && s[i] != '\r' && s[i] != '[' && s[i] != ']')
                out += s[i];
        return out;
    }

    void saveProfiles()
    {
        FILE* f = fopen(PROFILES_FILE, "w");
        if (!f)
        {
            actionLog(std::string("[roster] could not write ") + PROFILES_FILE);
            return;
        }
        fprintf(f, "# Kenshi Town job profiles. profile<TAB>name, then job<TAB>type<TAB>label<TAB>building<TAB>x y z<TAB>lx ly lz\n");
        for (size_t i = 0; i < g_profiles.size(); ++i)
        {
            fprintf(f, "profile\t%s\n", g_profiles[i].name.c_str());
            for (size_t j = 0; j < g_profiles[i].jobs.size(); ++j)
            {
                const ProfileJob& job = g_profiles[i].jobs[j];
                fprintf(f, "job\t%d\t%s\t%s\t%f %f %f\t%f %f %f\n", int(job.type), job.label.c_str(),
                        job.building.c_str(), job.at.x, job.at.y, job.at.z, job.location.x, job.location.y,
                        job.location.z);
            }
        }
        fclose(f);
    }

    void loadProfiles()
    {
        g_profiles.clear();
        FILE* f = fopen(PROFILES_FILE, "r");
        if (!f)
            return;
        char line[1024];
        while (fgets(line, sizeof(line), f))
        {
            std::string l(line);
            while (!l.empty() && (l[l.size() - 1] == '\n' || l[l.size() - 1] == '\r'))
                l.erase(l.size() - 1);
            std::vector<std::string> parts;
            size_t start = 0, tab;
            while ((tab = l.find('\t', start)) != std::string::npos)
            {
                parts.push_back(l.substr(start, tab - start));
                start = tab + 1;
            }
            parts.push_back(l.substr(start));
            if (parts.size() == 2 && parts[0] == "profile")
            {
                Profile p;
                p.name = parts[1];
                g_profiles.push_back(p);
            }
            else if (parts.size() == 6 && parts[0] == "job" && !g_profiles.empty())
            {
                ProfileJob job;
                job.type = TaskType(atoi(parts[1].c_str()));
                job.label = parts[2];
                job.building = parts[3];
                sscanf(parts[4].c_str(), "%f %f %f", &job.at.x, &job.at.y, &job.at.z);
                sscanf(parts[5].c_str(), "%f %f %f", &job.location.x, &job.location.y, &job.location.z);
                g_profiles.back().jobs.push_back(job);
            }
        }
        fclose(f);
        std::ostringstream note;
        note << "TownLedger: loaded " << g_profiles.size() << " job profiles";
        DebugLog(note.str());
    }

    // The building a job means now: same name, nearest to where it was, in
    // the Hub. NULL if there is no such building any more.
    Building* resolveBuilding(const ProfileJob& job)
    {
        if (job.building.empty())
            return NULL;
        std::vector<Building*> all;
        hubBuildings(all);
        Building* best = NULL;
        float bestD = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (all[i]->getName() != job.building)
                continue;
            Ogre::Vector3 p = all[i]->getPosition();
            float d = (p - job.at).squaredLength();
            if (!best || d < bestD)
            {
                best = all[i];
                bestD = d;
            }
        }
        return best;
    }

    // Squad names that mirror the roster: the retinue, plain citizens, and one
    // squad per profile, named after it.
    const std::string RETINUE_SQUAD = "Retinue";
    const std::string CITIZEN_SQUAD = "Citizens";

    std::string squadFor(Character* c)
    {
        std::string tag = nameTag(c);
        if (tag.empty())
            return RETINUE_SQUAD;
        return tag == CITIZEN_TAG ? CITIZEN_SQUAD : tag;
    }

    void syncSquad(Character* c)
    {
        std::string moved = putInSquad(c, squadFor(c));
        if (!moved.empty())
            DebugLog("TownLedger: " + bareName(c) + ": " + moved);
    }

    // Give a character a profile: tag, citizenship, squad, and the profile's
    // jobs. Returns a short account for the status line.
    std::string applyProfile(Character* c, const Profile& p)
    {
        setNameTag(c, p.name);
        g_citizens.insert(c);
        syncSquad(c);
        c->clearPermajobs();
        int added = 0, missing = 0;
        for (size_t j = 0; j < p.jobs.size(); ++j)
        {
            const ProfileJob& job = p.jobs[j];
            Building* b = resolveBuilding(job);
            if (!job.building.empty() && !b)
            {
                ++missing;
                continue;
            }
            c->addJob(job.type, b, false, true, b ? b->getPosition() : job.location);
            ++added;
        }
        std::ostringstream out;
        out << bareName(c) << " -> " << p.name << ": " << added << " jobs";
        if (missing)
            out << ", " << missing << " buildings not found";
        return out.str();
    }

    // Every citizen with a profile gets its jobs again, and everyone goes to
    // their squad. Returns the account, or "" if there was nobody to do.
    std::string reapplyAll()
    {
        std::ostringstream note;
        int n = 0;
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
        {
            Profile* p = findProfile(nameTag(*it));
            if (!p)
                continue;
            note << "\n  " << applyProfile(*it, *p);
            ++n;
        }
        if (ou && ou->player)
        {
            const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
            for (uint32_t i = 0; i < people.size(); ++i)
                if (people[i])
                    syncSquad(people[i]);
        }
        return n ? note.str() : "";
    }

    // After a load, reapply once the Hub's buildings have finished loading:
    // jobs point at buildings, which can be rebuilt or moved between sessions.
    // Armed when the Hub is (re)found; fires when its building count has held
    // steady for one check.

    void autoReapplyTick()
    {
        if (!g_reapplyPending || !g_hub)
            return;
        // Wait for both the Hub's buildings and the player's characters to
        // finish loading: on 2026-11-22 the buildings settled first and the
        // reapply found no citizens, because their names hadn't loaded yet.
        size_t people = (ou && ou->player) ? ou->player->getAllPlayerCharacters().size() : 0;
        size_t n = buildingCount(g_hub) * 1000 + people;
        if (buildingCount(g_hub) == 0 || people == 0 || n != g_reapplyLastCount)
        {
            g_reapplyLastCount = n;
            return;
        }
        g_reapplyPending = false;
        // Fold anything ours left in the original Hub part into the base,
        // before jobs are given out against those buildings.
        std::string merged = consolidateHub();
        if (!merged.empty())
            actionLog("[hub] after load: " + merged);
        rebuildCitizensFromNames();
        std::string done = reapplyAll();
        actionLog("[roster] after load:" + (done.empty() ? std::string(" nobody has a profile") : done));
    }

    // ==== Arrivals ============================================================
    //
    // Every ARRIVAL_EVERY_HOURS of game time, one to three drifters turn up
    // outside the Hub's gate as neutrals and wait, up to GATE_MAX at once. The
    // Town Roster lists them; Accept recruits them as citizens with no job,
    // Turn away leaves them be. Game hours, so a paused game brings no one.

    const double ARRIVAL_EVERY_HOURS = 12.0;
    const size_t GATE_MAX = 3;

    // Skills that matter to the town, in the order "best at" breaks ties.
    const StatsEnumerated WORK_STATS[] = {
        STAT_LABOURING, STAT_FARMING,        STAT_COOKING,         STAT_ENGINEERING,
        STAT_SCIENCE,   STAT_SMITHING_WEAPON, STAT_SMITHING_ARMOUR, STAT_MEDIC,
        STAT_MELEE_ATTACK, STAT_MELEE_DEFENCE, STAT_TOUGHNESS,      STAT_STRENGTH};
    const size_t WORK_STAT_COUNT = sizeof(WORK_STATS) / sizeof(WORK_STATS[0]);

    // "Farming 34, Cooking 18": a character's best `n` town skills.
    std::string topSkills(Character* c, size_t n)
    {
        CharStats* st = c ? c->getStats() : NULL;
        if (!st)
            return "";
        std::vector<std::pair<float, size_t> > ranked;
        for (size_t i = 0; i < WORK_STAT_COUNT; ++i)
            ranked.push_back(std::make_pair(-st->getStat(WORK_STATS[i], false), i));
        std::sort(ranked.begin(), ranked.end());
        std::ostringstream out;
        for (size_t i = 0; i < n && i < ranked.size(); ++i)
            out << (i ? ", " : "") << CharStats::getStatName(WORK_STATS[ranked[i].second]) << " "
                << int(-ranked[i].first);
        return out.str();
    }

    // Backgrounds: an arrival's head start in a trade. Skills are raised to a
    // random value in the range (never lowered); everything else stays at
    // Drifter level, and training afterwards is Kenshi's own.
    struct StatRange
    {
        StatsEnumerated stat;
        int lo, hi;
    };
    struct Background
    {
        const char* name;
        StatRange skills[3];
    };
    const Background BACKGROUNDS[] = {
        // Main skill 14-24, second 8-14, third 5-10; Bill found 25-40 too strong.
        {"Labourer", {{STAT_LABOURING, 14, 24}, {STAT_STRENGTH, 8, 14}, {STAT_TOUGHNESS, 5, 10}}},
        {"Farmhand", {{STAT_FARMING, 14, 24}, {STAT_COOKING, 8, 14}, {STAT_LABOURING, 5, 10}}},
        {"Ex-guard", {{STAT_MELEE_ATTACK, 14, 24}, {STAT_MELEE_DEFENCE, 8, 14}, {STAT_TOUGHNESS, 5, 10}}},
        {"Tinkerer", {{STAT_ENGINEERING, 14, 24}, {STAT_SCIENCE, 8, 14}, {STAT_DEXTERITY, 5, 10}}},
        {"Smith", {{STAT_SMITHING_WEAPON, 14, 24}, {STAT_SMITHING_ARMOUR, 8, 14}, {STAT_STRENGTH, 5, 10}}},
        {"Medic", {{STAT_MEDIC, 14, 24}, {STAT_DEXTERITY, 8, 14}, {STAT_SCIENCE, 5, 10}}},
    };
    const size_t BACKGROUND_COUNT = sizeof(BACKGROUNDS) / sizeof(BACKGROUNDS[0]);

    // Drifters come out of the template with good all-round stats. Knock every
    // skill down to a random 1-8 first, so a background is what stands out.
    void humbleStats(Character* c)
    {
        CharStats* st = c->getStats();
        if (!st)
            return;
        for (int i = STAT_STRENGTH; i < STAT_END; ++i)
        {
            float& v = st->getStatRef(StatsEnumerated(i));
            float cap = float(1 + rand() % 8);
            if (v > cap)
                v = cap;
        }
    }

    void applyBackground(Character* c, const Background& b)
    {
        CharStats* st = c->getStats();
        if (!st)
            return;
        for (int i = 0; i < 3; ++i)
        {
            const StatRange& r = b.skills[i];
            float& v = st->getStatRef(r.stat);
            float want = float(r.lo + rand() % (r.hi - r.lo + 1));
            if (want > v)
                v = want;
        }
    }

    struct Arrival
    {
        Character* who;
        double hour; // in-game hour they arrived
        std::string background;
    };
    std::vector<Arrival> g_gate;
    double g_nextArrivalHour = -1; // < 0: not scheduled yet (set after a load)

    double gameHours()
    {
        return (ou && ou->player) ? ou->getTimeStamp_inGameHours().getTotalHours() : 0.0;
    }

    // Who waiting at the gate is still there and still not ours.
    void pruneGate()
    {
        std::vector<Arrival> keep;
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        for (size_t i = 0; i < g_gate.size(); ++i)
        {
            bool joined = false;
            for (uint32_t j = 0; j < people.size() && !joined; ++j)
                joined = people[j] == g_gate[i].who;
            if (!joined && !g_gate[i].who->isDead())
                keep.push_back(g_gate[i]);
        }
        g_gate.swap(keep);
    }

    Faction* arrivalFaction()
    {
        if (!ou || !ou->factionMgr)
            return NULL;
        Faction* f = ou->factionMgr->getFactionByName("Drifters");
        return f ? f : ou->factionMgr->getEmptyFaction();
    }

    // Spawn a group at the gate. Returns how many came, for the log.
    int spawnArrivals()
    {
        if (!g_hub || !g_createChar || !ou || !ou->theFactory)
            return 0;
        GameData* tmpl = ou->gamedata.getData(DEV_RECRUIT_SID);
        Faction* faction = arrivalFaction();
        if (!tmpl || !faction)
            return 0;
        Ogre::Vector3 gate = g_hub->getPositionOutsideTownGates(200.0f);
        if (gate.squaredLength() < 1.0f) // no gate: the edge of town, eastwards
        {
            gate = g_hub->getPosition();
            gate.x += g_hubRadius * 0.9f;
        }
        int want = 1 + rand() % 3;
        int came = 0;
        for (int i = 0; i < want && g_gate.size() < GATE_MAX; ++i)
        {
            Ogre::Vector3 at(gate.x + 25.0f * i, gate.y, gate.z + 15.0f * (i % 2));
            RootObject* r = g_createChar(ou->theFactory, faction, at, NULL, tmpl, NULL, 25.0f);
            if (!r)
                break;
            Arrival a;
            a.who = static_cast<Character*>(r);
            a.hour = gameHours();
            const Background& b = BACKGROUNDS[rand() % BACKGROUND_COUNT];
            humbleStats(a.who);
            applyBackground(a.who, b);
            a.background = b.name;
            g_gate.push_back(a);
            ++came;
        }
        if (came)
        {
            std::ostringstream note;
            note << "[arrivals] " << came << " drifter" << (came > 1 ? "s" : "") << " at the gate ("
                 << factionName(faction) << "): open the Roster";
            actionLog(note.str());
            std::ostringstream t;
            t << came << (came > 1 ? " drifters are" : " drifter is") << " at the gate";
            toast(t.str());
        }
        return came;
    }

    // Main-loop tick: schedule after a load, spawn when due.
    void arrivalsTick()
    {
        if (!ou || !ou->player || !g_hub || !hubIsOurs())
            return;
        pruneGate();
        double now = gameHours();
        if (g_nextArrivalHour < 0 || g_nextArrivalHour > now + ARRIVAL_EVERY_HOURS * 2)
        {
            g_nextArrivalHour = now + ARRIVAL_EVERY_HOURS;
            return;
        }
        if (now < g_nextArrivalHour)
            return;
        g_nextArrivalHour = now + ARRIVAL_EVERY_HOURS;
        if (g_gate.size() < GATE_MAX)
            spawnArrivals();
    }

    void onArrivalNowClick(MyGUI::WidgetPtr)
    {
        if (!g_hub)
        {
            actionLog("[arrivals] no Hub found");
            return;
        }
        if (g_gate.size() >= GATE_MAX)
        {
            actionLog("[arrivals] the gate is full: accept or turn someone away first");
            return;
        }
        if (!spawnArrivals())
            actionLog("[arrivals] nobody came (spawn failed; see the log)");
    }

    // ---- The window.
    //
    //   Profiles            | People: Name | Profile | Doing
    //   [selected's jobs]   | "Assign: Double -> Miner"
    //   [name] Capture Del  | Assign | Reapply jobs
    //   status line

    MyGUI::Window* g_roster = NULL;
    MyGUI::ListBox* g_rosterProfiles = NULL;
    MyGUI::MultiListBox* g_rosterPeople = NULL;
    MyGUI::EditBox* g_rosterJobs = NULL;
    MyGUI::EditBox* g_rosterName = NULL;
    MyGUI::EditBox* g_rosterPick = NULL;
    MyGUI::EditBox* g_rosterStatus = NULL;
    MyGUI::ListBox* g_rosterGate = NULL;
    std::vector<std::string> g_rosterGateRows;
    void refreshGate(); // with the gate handlers, below
    std::vector<Character*> g_rosterPeopleRows;
    std::vector<std::string> g_rosterProfileRows, g_rosterPeopleText;

    // Rows 0 and 1 of the profile list are fixed choices, not profiles.
    const size_t ROW_RETINUE = 0, ROW_CITIZEN = 1, FIRST_PROFILE_ROW = 2;

    // Refill a list only when its rows changed, keeping the selection, so a
    // twice-a-second refresh doesn't fight the player's clicks.
    void fillList(MyGUI::ListBox* list, std::vector<std::string>& shown, const std::vector<std::string>& rows)
    {
        if (rows == shown)
            return;
        size_t sel = list->getIndexSelected();
        list->removeAllItems();
        for (size_t i = 0; i < rows.size(); ++i)
            list->addItem(rows[i]);
        if (sel != MyGUI::ITEM_NONE && sel < rows.size())
            list->setIndexSelected(sel);
        shown = rows;
    }

    // Same for the people table; each row is three cells joined by tabs.
    void fillTable(MyGUI::MultiListBox* list, std::vector<std::string>& shown, const std::vector<std::string>& rows)
    {
        if (rows == shown)
            return;
        size_t sel = list->getIndexSelected();
        list->removeAllItems();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            std::vector<std::string> cells;
            size_t start = 0, tab;
            while ((tab = rows[i].find('\t', start)) != std::string::npos)
            {
                cells.push_back(rows[i].substr(start, tab - start));
                start = tab + 1;
            }
            cells.push_back(rows[i].substr(start));
            list->addItem(cells[0]);
            for (size_t c = 1; c < cells.size() && c < 4; ++c)
                list->setSubItemNameAt(c, i, cells[c]);
        }
        if (sel != MyGUI::ITEM_NONE && sel < rows.size())
            list->setIndexSelected(sel);
        shown = rows;
    }

    std::string profileRowName(size_t row)
    {
        if (row == ROW_RETINUE)
            return "Retinue";
        if (row == ROW_CITIZEN)
            return "Citizen, no job";
        if (row != MyGUI::ITEM_NONE && row - FIRST_PROFILE_ROW < g_profiles.size())
            return g_profiles[row - FIRST_PROFILE_ROW].name;
        return "";
    }

    Character* rosterSelectedPerson()
    {
        size_t i = g_rosterPeople->getIndexSelected();
        return (i != MyGUI::ITEM_NONE && i < g_rosterPeopleRows.size()) ? g_rosterPeopleRows[i] : NULL;
    }

    // The two lines that follow the selections: the selected profile's jobs,
    // and what Assign would do.
    void refreshRosterPicks()
    {
        size_t row = g_rosterProfiles->getIndexSelected();
        std::ostringstream jobs;
        if (row == ROW_RETINUE)
            jobs << "Yours to control directly. Jobs are left as they are.";
        else if (row == ROW_CITIZEN)
            jobs << "A citizen with no standing jobs.";
        else if (row != MyGUI::ITEM_NONE && row - FIRST_PROFILE_ROW < g_profiles.size())
        {
            const Profile& p = g_profiles[row - FIRST_PROFILE_ROW];
            for (size_t j = 0; j < p.jobs.size(); ++j)
                jobs << (j ? "\n" : "") << p.jobs[j].label
                     << (p.jobs[j].building.empty() ? "" : " -> " + p.jobs[j].building);
            if (p.jobs.empty())
                jobs << "No jobs.";
        }
        else
            jobs << "Pick a profile.";
        g_rosterJobs->setCaption(jobs.str());

        Character* c = rosterSelectedPerson();
        std::string target = profileRowName(row);
        std::string pick = "Assign: " + (c ? bareName(c) : std::string("(pick a person)")) + "  ->  " +
                           (target.empty() ? std::string("(pick a profile)") : target);
        if (c)
            pick += "     (" + topSkills(c, 3) + ")";
        g_rosterPick->setCaption(pick);
    }

    void refreshRoster()
    {
        if (!g_roster || !g_roster->getVisible() || !ou || !ou->player)
            return;

        std::vector<std::string> profileRows;
        profileRows.push_back("Retinue (you control them)");
        profileRows.push_back("Citizen, no job");
        for (size_t i = 0; i < g_profiles.size(); ++i)
        {
            std::ostringstream row;
            row << g_profiles[i].name << "  (" << g_profiles[i].jobs.size() << " jobs)";
            profileRows.push_back(row.str());
        }
        fillList(g_rosterProfiles, g_rosterProfileRows, profileRows);

        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        std::vector<std::string> peopleRows;
        g_rosterPeopleRows.clear();
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            Character* c = people[i];
            if (!c)
                continue;
            std::string tag = nameTag(c);
            peopleRows.push_back(bareName(c) + "\t" + (tag.empty() ? "Retinue" : tag) + "\t" + topSkills(c, 1) +
                                 "\t" + currentActivity(c));
            g_rosterPeopleRows.push_back(c);
        }
        fillTable(g_rosterPeople, g_rosterPeopleText, peopleRows);
        refreshGate();

        refreshRosterPicks();
        g_rosterStatus->setCaption(g_lastAction);
    }

    void onRosterProfilePicked(MyGUI::ListBox*, size_t)
    {
        refreshRosterPicks();
    }

    void lookAt(Character* c)
    {
        if (c && ou && ou->player)
            ou->player->focusCamera(c->getPosition());
    }

    void onRosterPersonPicked(MyGUI::MultiListBox*, size_t)
    {
        lookAt(rosterSelectedPerson());
        refreshRosterPicks();
    }

    void refreshGate()
    {
        std::vector<std::string> rows;
        double now = gameHours();
        for (size_t i = 0; i < g_gate.size(); ++i)
        {
            std::ostringstream row;
            row << g_gate[i].who->getName() << "  -  " << g_gate[i].background << ": "
                << topSkills(g_gate[i].who, 2) << "  -  waiting " << int(now - g_gate[i].hour) << "h";
            rows.push_back(row.str());
        }
        if (rows.empty())
            rows.push_back("Nobody at the gate.");
        fillList(g_rosterGate, g_rosterGateRows, rows);
    }

    Character* gateSelected(size_t* index)
    {
        size_t i = g_rosterGate->getIndexSelected();
        if (i == MyGUI::ITEM_NONE || i >= g_gate.size())
            return NULL;
        *index = i;
        return g_gate[i].who;
    }

    void onRosterGatePicked(MyGUI::ListBox*, size_t)
    {
        size_t i = 0;
        lookAt(gateSelected(&i));
    }

    void onGateAccept(MyGUI::WidgetPtr)
    {
        size_t i = 0;
        Character* c = gateSelected(&i);
        if (!c)
        {
            actionLog("[arrivals] pick someone at the gate first");
            refreshRoster();
            return;
        }
        bool joined = ou->player->recruit(c, false);
        if (!joined)
        {
            actionLog("[arrivals] " + c->getName() + " would not join (recruit refused)");
            refreshRoster();
            return;
        }
        g_gate.erase(g_gate.begin() + i);
        setNameTag(c, CITIZEN_TAG);
        g_citizens.insert(c);
        syncSquad(c);
        ou->player->objectSelected(c, false);
        actionLog("[arrivals] " + bareName(c) + " joined the Hub as a citizen: give them a profile");
        refreshRoster();
    }

    void onGateTurnAway(MyGUI::WidgetPtr)
    {
        size_t i = 0;
        Character* c = gateSelected(&i);
        if (!c)
        {
            actionLog("[arrivals] pick someone at the gate first");
            refreshRoster();
            return;
        }
        g_gate.erase(g_gate.begin() + i);
        actionLog("[arrivals] turned " + c->getName() + " away");
        refreshRoster();
    }

    void onRosterNewProfile(MyGUI::WidgetPtr)
    {
        std::vector<Character*> chars;
        selectedCharacters(chars);
        if (chars.empty() || chars[0]->getPermajobCount() == 0)
        {
            actionLog("[roster] select a character whose jobs you've set up, type a name, then Capture");
            refreshRoster();
            return;
        }
        Character* c = chars[0];
        std::string name = fileSafe(g_rosterName->getOnlyText().asUTF8());
        if (name.empty() || name == CITIZEN_TAG || name == RETINUE_SQUAD || name == CITIZEN_SQUAD)
        {
            std::ostringstream n;
            n << "Profile " << g_profiles.size() + 1;
            name = n.str();
        }
        Profile p;
        p.name = name;
        for (int i = 0; i < c->getPermajobCount(); ++i)
        {
            const Tasker* t = c->getPermajobData(i);
            RootObject* subject = taskSubject(t);
            ProfileJob job;
            job.type = c->getPermajob(i);
            job.label = fileSafe(c->getPermajobName(i));
            job.building = subject ? fileSafe(subject->getName()) : "";
            job.at = subject ? subject->getPosition() : Ogre::Vector3::ZERO;
            job.location = taskLocation(t);
            p.jobs.push_back(job);
        }
        Profile* existing = findProfile(name);
        if (existing)
            *existing = p;
        else
            g_profiles.push_back(p);
        saveProfiles();
        actionLog("[roster] " + std::string(existing ? "updated" : "new") + " profile " + name + " from " +
                  bareName(c) + ": " + describeJobs(c));
        refreshRoster();
    }

    void onRosterDeleteProfile(MyGUI::WidgetPtr)
    {
        size_t row = g_rosterProfiles->getIndexSelected();
        if (row == MyGUI::ITEM_NONE || row < FIRST_PROFILE_ROW || row - FIRST_PROFILE_ROW >= g_profiles.size())
        {
            actionLog("[roster] pick a profile (not Retinue or Citizen) to delete");
            refreshRoster();
            return;
        }
        std::string name = g_profiles[row - FIRST_PROFILE_ROW].name;
        g_profiles.erase(g_profiles.begin() + (row - FIRST_PROFILE_ROW));
        // Its people keep their jobs but go back to plain citizens.
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
            if (nameTag(*it) == name)
            {
                setNameTag(*it, CITIZEN_TAG);
                syncSquad(*it);
            }
        saveProfiles();
        actionLog("[roster] deleted profile " + name);
        refreshRoster();
    }

    void onRosterAssign(MyGUI::WidgetPtr)
    {
        Character* c = rosterSelectedPerson();
        size_t row = g_rosterProfiles->getIndexSelected();
        if (!c || row == MyGUI::ITEM_NONE)
        {
            actionLog("[roster] pick a person on the right and a profile on the left, then Assign");
            refreshRoster();
            return;
        }
        if (row == ROW_RETINUE)
        {
            setNameTag(c, "");
            g_citizens.erase(c);
            syncSquad(c);
            actionLog("[roster] " + bareName(c) + " -> retinue (yours to control)");
        }
        else if (row == ROW_CITIZEN)
        {
            setNameTag(c, CITIZEN_TAG);
            g_citizens.insert(c);
            syncSquad(c);
            c->clearPermajobs();
            ou->player->objectSelected(c, false);
            actionLog("[roster] " + bareName(c) + " -> citizen, no job");
        }
        else if (row - FIRST_PROFILE_ROW < g_profiles.size())
        {
            actionLog("[roster] " + applyProfile(c, g_profiles[row - FIRST_PROFILE_ROW]));
            ou->player->objectSelected(c, false);
        }
        refreshRoster();
    }

    void onRosterReapply(MyGUI::WidgetPtr)
    {
        std::string done = reapplyAll();
        actionLog("[roster] reapplied:" + (done.empty() ? std::string(" nobody has a profile") : done));
        refreshRoster();
    }

    void onRosterWindowButton(MyGUI::Window* sender, const std::string& name)
    {
        if (name == "close")
            sender->setVisible(false);
    }

    MyGUI::Button* rosterButton(MyGUI::Widget* client, float x, float y, float w, const char* name,
                                const char* caption, void (*click)(MyGUI::WidgetPtr))
    {
        MyGUI::Button* b = client->createWidgetReal<MyGUI::Button>("Kenshi_Button1", x, y, w, 0.075f,
                                                                   MyGUI::Align::Default, name);
        b->setCaption(caption);
        b->eventMouseButtonClick += MyGUI::newDelegate(click);
        return b;
    }

    MyGUI::EditBox* rosterText(MyGUI::Widget* client, float x, float y, float w, float h, const char* name)
    {
        MyGUI::EditBox* e = client->createWidgetReal<MyGUI::EditBox>("Kenshi_EditBox", x, y, w, h,
                                                                     MyGUI::Align::Default, name);
        e->setEditMultiLine(true);
        e->setEditReadOnly(true);
        e->setEditWordWrap(true);
        return e;
    }

    void createRosterContent(MyGUI::Widget* client)
    {

        // Left column: profiles, the selected one's jobs, and making profiles.
        g_rosterProfiles = client->createWidgetReal<MyGUI::ListBox>("Kenshi_ListBox", 0.02f, 0.02f, 0.36f, 0.40f,
                                                                    MyGUI::Align::Default, "TownRosterProfiles");
        g_rosterProfiles->eventListChangePosition += MyGUI::newDelegate(onRosterProfilePicked);
        g_rosterJobs = rosterText(client, 0.02f, 0.44f, 0.36f, 0.24f, "TownRosterJobs");
        g_rosterName = client->createWidgetReal<MyGUI::EditBox>("Kenshi_EditBox", 0.02f, 0.70f, 0.36f, 0.075f,
                                                                MyGUI::Align::Default, "TownRosterName");
        g_rosterName->setCaption("Miner");
        rosterButton(client, 0.02f, 0.785f, 0.175f, "TownRosterNew", "Capture", onRosterNewProfile);
        rosterButton(client, 0.205f, 0.785f, 0.175f, "TownRosterDelete", "Delete", onRosterDeleteProfile);

        // Right column: people as a table, what Assign will do, and actions.
        g_rosterPeople = client->createWidgetReal<MyGUI::MultiListBox>(
            "Kenshi_MultiListBox", 0.40f, 0.02f, 0.58f, 0.40f, MyGUI::Align::Default, "TownRosterPeople");
        int w = g_rosterPeople->getWidth();
        g_rosterPeople->addColumn("Name", w * 24 / 100);
        g_rosterPeople->addColumn("Profile", w * 18 / 100);
        g_rosterPeople->addColumn("Best at", w * 26 / 100);
        g_rosterPeople->addColumn("Doing", w * 32 / 100 - 4);
        g_rosterPeople->eventListChangePosition += MyGUI::newDelegate(onRosterPersonPicked);
        g_rosterGate = client->createWidgetReal<MyGUI::ListBox>("Kenshi_ListBox", 0.40f, 0.44f, 0.58f, 0.16f,
                                                                MyGUI::Align::Default, "TownRosterGate");
        g_rosterGate->eventListChangePosition += MyGUI::newDelegate(onRosterGatePicked);
        rosterButton(client, 0.40f, 0.61f, 0.285f, "TownRosterAccept", "Accept", onGateAccept);
        rosterButton(client, 0.695f, 0.61f, 0.285f, "TownRosterTurnAway", "Turn away", onGateTurnAway);
        g_rosterPick = rosterText(client, 0.40f, 0.70f, 0.58f, 0.075f, "TownRosterPick");
        rosterButton(client, 0.40f, 0.785f, 0.285f, "TownRosterAssign", "Assign", onRosterAssign);
        rosterButton(client, 0.695f, 0.785f, 0.285f, "TownRosterReapply", "Reapply jobs", onRosterReapply);

        g_rosterStatus = rosterText(client, 0.02f, 0.88f, 0.96f, 0.10f, "TownRosterStatus");
    }

    void toggleRoster()
    {
        showTab(1); // Roster
    }

    void onRosterClick(MyGUI::WidgetPtr)
    {
        toggleRoster();
    }

    // ==== The garrison contract ===============================================
    //
    // The Holy Nation Outlaws who guard the Hub stay on for GRACE_DAYS after it
    // becomes ours. After that they want paying, a week at a time; a day
    // overdue and they leave, and the Hub is guarded by whoever we've trained.
    // Kept per save (by save name) in mods/TownLedger/contracts.txt.

    const double GRACE_DAYS = 10.0;
    const int FEE_PER_GUARD_PER_DAY = 25;
    const double OVERDUE_HOURS = 24.0;
    const int NO_WARNING = 999;
    const char* CONTRACTS_FILE = "mods\\TownLedger\\contracts.txt";

    struct Contract
    {
        double start;     // game hour the Hub became ours
        double paidUntil; // game hour the current term ends
        bool gone;        // the garrison has left
        bool cleaned;     // the first-claim Hub clean-up has run in this save
        int warned;       // smallest warning threshold given (48, 24, 0), NO_WARNING if none
        int quest;        // the guided-start task in progress (index into QUESTS)
    };
    std::map<std::string, Contract> g_contracts;
    bool g_contractsLoaded = false;

    std::string saveName()
    {
        SaveManager* sm = SaveManager::getSingleton();
        std::string n = sm ? sm->getCurrentGame() : std::string();
        return n.empty() ? std::string("(unsaved)") : fileSafe(n);
    }

    void saveContracts()
    {
        FILE* f = fopen(CONTRACTS_FILE, "w");
        if (!f)
            return;
        fprintf(f, "# Kenshi Town per-save state: save<TAB>start hour<TAB>paid until hour<TAB>gone<TAB>cleaned<TAB>quest\n");
        for (std::map<std::string, Contract>::iterator it = g_contracts.begin(); it != g_contracts.end(); ++it)
            if (it->first != "(unsaved)")
                fprintf(f, "%s\t%f\t%f\t%d\t%d\t%d\n", it->first.c_str(), it->second.start, it->second.paidUntil,
                        it->second.gone ? 1 : 0, it->second.cleaned ? 1 : 0, it->second.quest);
        fclose(f);
    }

    void loadContracts()
    {
        g_contractsLoaded = true;
        FILE* f = fopen(CONTRACTS_FILE, "r");
        if (!f)
            return;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == '#')
                continue;
            char name[256];
            double start = 0, paid = 0;
            int gone = 0, cleaned = 1, quest = 0; // older lines: clean-up counts as done
            if (sscanf(line, "%255[^\t]\t%lf\t%lf\t%d\t%d\t%d", name, &start, &paid, &gone, &cleaned, &quest) >= 4)
            {
                Contract c = {start, paid, gone != 0, cleaned != 0, NO_WARNING};
                c.quest = quest;
                if (std::string(name) != "(unsaved)")
                    g_contracts[name] = c;
            }
        }
        fclose(f);
    }

    // The Outlaws inside the Hub right now.
    void garrison(std::vector<Character*>& out)
    {
        out.clear();
        Faction* outlaws = hubVanillaOwner();
        if (!outlaws || !g_hub)
            return;
        lektor<RootObject*> found;
        outlaws->getCharactersInArea(found, g_hub->getPosition(), g_hubRadius, false);
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            Character* c = static_cast<Character*>(found[i]);
            if (c && !c->isDead())
                out.push_back(c);
        }
    }

    int weeklyFee(size_t guards)
    {
        return int(guards) * FEE_PER_GUARD_PER_DAY * 7;
    }

    // The record in play this session. Save names can't identify a game: in a
    // new game, SaveManager::getCurrentGame() still returns the last save
    // loaded (2026-11-22: a new game picked up "autosave1"'s record and so
    // skipped the deed claim and the clean-up). So the record lives in memory:
    // started fresh by claiming the Hub (once per game), looked up by save name
    // only when a different world has loaded, and written under whatever name
    // the game is saved with.
    Contract g_current;
    bool g_currentValid = false;

    Contract* currentContract()
    {
        if (!g_contractsLoaded)
            loadContracts();
        if (g_worldChanged)
        {
            g_currentValid = false;
            g_worldChanged = false;
        }
        if (!g_currentValid)
        {
            std::map<std::string, Contract>::iterator it = g_contracts.find(saveName());
            if (it != g_contracts.end())
            {
                g_current = it->second;
                g_currentValid = true;
            }
        }
        return g_currentValid ? &g_current : NULL;
    }

    // Write the record in play under the current save name.
    void persistContract()
    {
        if (!g_currentValid)
            return;
        g_contracts[saveName()] = g_current;
        saveContracts();
    }

    void storeContract(const Contract& c)
    {
        g_current = c;
        g_currentValid = true;
        persistContract();
    }

    // Claiming the Hub starts a fresh contract with the clean-up still to do.
    // Called from Claim; a reload doesn't claim (the Hub loads as ours).
    void startContractOnClaim()
    {
        double now = gameHours();
        Contract fresh = {now, now + GRACE_DAYS * 24.0, false, false, NO_WARNING};
        storeContract(fresh);
        std::ostringstream note;
        note << "[garrison] the Outlaws will guard the Hub for " << int(GRACE_DAYS)
             << " days. After that they want paying (see Town).";
        actionLog(note.str());
    }

    void garrisonLeaves(Contract& c, const std::string& why)
    {
        std::vector<Character*> guards;
        garrison(guards);
        for (size_t i = 0; i < guards.size(); ++i)
            ou->destroy(guards[i], false, "Kenshi Town: garrison left");
        c.gone = true;
        persistContract();
        std::ostringstream note;
        note << "[garrison] " << guards.size() << " Outlaws left the Hub (" << why
             << "). The Hub is guarded by whoever you've trained.";
        actionLog(note.str());
    }

    // ---- First-claim clean-up: the "stock" Hub a new game starts with.
    //
    // Building names to remove, one per line, in mods/TownLedger/hub_cleanup.txt
    // (written with Bill's starting list if missing). Runs once per save, the
    // first time the Hub is ours, so a fresh game gets a tidier Hub; "Clean up
    // Hub now" on the Ledger tab runs it on demand. The Hub's buildings come
    // from the world map, not the town record, so a data mod can't do this.
    const char* CLEANUP_FILE = "mods\\TownLedger\\hub_cleanup.txt";

    // Bill's choice (2026-11-22): keep the Longhouses, the Stationhouse and the
    // Snailhouse; lose every other house and the broken street props.
    const char* CLEANUP_DEFAULT[] = {
        "Old Storm House", "Small Shack", "Bughouse", "L-House", "Residential Small",
        "Holy Sinner Torch Broken", "Holy Street Torch Broken", "Holy Street Torch Snapped",
        "Emperor Statue Broken", "Wind Generator Broken"};
    const size_t CLEANUP_DEFAULT_COUNT = sizeof(CLEANUP_DEFAULT) / sizeof(CLEANUP_DEFAULT[0]);

    void cleanupList(std::set<std::string>& names)
    {
        names.clear();
        FILE* f = fopen(CLEANUP_FILE, "r");
        if (!f)
        {
            f = fopen(CLEANUP_FILE, "w");
            if (f)
            {
                fprintf(f, "# Buildings removed from the Hub the first time it's claimed in a save.\n"
                           "# One building name per line, exactly as the Buildings tab shows it.\n");
                for (size_t i = 0; i < CLEANUP_DEFAULT_COUNT; ++i)
                    fprintf(f, "%s\n", CLEANUP_DEFAULT[i]);
                fclose(f);
            }
            for (size_t i = 0; i < CLEANUP_DEFAULT_COUNT; ++i)
                names.insert(CLEANUP_DEFAULT[i]);
            return;
        }
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            std::string n(line);
            while (!n.empty() && (n[n.size() - 1] == '\n' || n[n.size() - 1] == '\r' || n[n.size() - 1] == ' '))
                n.erase(n.size() - 1);
            if (!n.empty() && n[0] != '#')
                names.insert(n);
        }
        fclose(f);
    }

    std::string cleanupHub()
    {
        std::set<std::string> names;
        cleanupList(names);
        std::vector<Building*> all;
        hubBuildings(all);
        std::map<std::string, int> removed;
        for (size_t i = 0; i < all.size(); ++i)
            if (names.count(all[i]->getName()))
            {
                ++removed[all[i]->getName()];
                ou->dynamicDestroyBuilding(hand(all[i]));
            }
        std::ostringstream out;
        out << "Hub clean-up:";
        if (removed.empty())
            out << " nothing on the list was there";
        for (std::map<std::string, int>::iterator it = removed.begin(); it != removed.end(); ++it)
            out << (it == removed.begin() ? " removed " : ", ") << it->second << " x " << it->first;
        return out.str();
    }

    // ---- The deed. The Kenshi Town start gives the founder a "Deed to the
    // Hub" (TownLedger.mod); in a game where the Hub was never claimed, the
    // plugin claims it for whoever holds the deed.
    const char* DEED_NAME = "Deed to the Hub";        // match scripts/build_mods.py
    const char* FOUNDER_NAME = "Kenshi Town founder"; // the start's leader template

    bool playerHoldsDeed()
    {
        if (!ou || !ou->player)
            return false;
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            // The Kenshi Town start's founder counts as holding the deed, in
            // case the item itself didn't make it into their pockets.
            GameData* tmpl = people[i] ? people[i]->getGameData() : NULL;
            if (tmpl && tmpl->name == FOUNDER_NAME)
                return true;
            Inventory* inv = people[i] ? people[i]->getInventory() : NULL;
            if (!inv)
                continue;
            lektor<InventorySection*>& sections = inv->getAllSections();
            for (uint32_t s = 0; s < sections.size(); ++s)
            {
                if (!sections[s])
                    continue;
                const Ogre::vector<InventorySection::SectionItem>::type& items = sections[s]->getItems();
                for (size_t k = 0; k < items.size(); ++k)
                    if (items[k].item && items[k].item->getName() == DEED_NAME)
                        return true;
            }
        }
        return false;
    }

    void onClaimClick(MyGUI::WidgetPtr);

    void deedTick()
    {
        if (!g_hub || hubIsOurs() || buildingCount(g_hub) == 0)
            return; // no Hub yet, already ours, or still loading
        if (!playerHoldsDeed())
            return;
        onClaimClick(NULL);
        actionLog("[hub] you hold the deed: the Hub is yours");
    }

    void onCleanupHubNow(MyGUI::WidgetPtr)
    {
        if (!g_hub || !hubIsOurs())
        {
            actionLog("[hub] claim the Hub first");
            return;
        }
        actionLog("[hub] " + cleanupHub());
    }

    // Main-loop tick: start the contract when the Hub is first ours in this
    // save, warn as it runs down, and let the garrison go when overdue.
    void contractTick()
    {
        if (!ou || !ou->player || !g_hub || !hubIsOurs())
            return;
        double now = gameHours();
        Contract* c = currentContract();
        static std::string s_persistedAs;
        if (c && saveName() != s_persistedAs) // saved under a new name: write it there too
        {
            persistContract();
            s_persistedAs = saveName();
        }
        if (!c)
        {
            // Ours with no record: a save from before records, or one saved
            // under a name we never saw. Start the contract, but don't clean up
            // a Hub the player has already been living in.
            Contract fresh = {now, now + GRACE_DAYS * 24.0, false, true, NO_WARNING};
            storeContract(fresh);
            return;
        }
        if (!c->cleaned && buildingCount(g_hub) > 0)
        {
            c->cleaned = true;
            persistContract();
            actionLog("[hub] " + cleanupHub());
        }
        if (c->gone)
            return;
        double left = c->paidUntil - now;
        if (left <= -OVERDUE_HOURS)
        {
            garrisonLeaves(*c, "unpaid");
            return;
        }
        // Warn once per threshold crossed: 2 days left, 1 day left, due.
        int bucket = left <= 0 ? 0 : left <= 24 ? 24 : left <= 48 ? 48 : NO_WARNING;
        if (bucket < c->warned)
        {
            c->warned = bucket;
            std::vector<Character*> guards;
            garrison(guards);
            std::ostringstream note;
            if (bucket == 0)
                note << "[garrison] payment is due: " << weeklyFee(guards.size())
                     << " cats for a week, or they leave in a day (Town window)";
            else
                note << "[garrison] " << bucket / 24 << " day" << (bucket > 24 ? "s" : "")
                     << " until the Outlaws want paying: " << weeklyFee(guards.size()) << " cats a week";
            actionLog(note.str());
        }
    }

    std::string contractLine()
    {
        if (!g_hub || !hubIsOurs())
            return "Garrison: the Hub isn't ours.";
        Contract* c = currentContract();
        std::vector<Character*> guards;
        garrison(guards);
        std::ostringstream out;
        if (!c)
            out << "Garrison: " << guards.size() << " Outlaws (contract starts shortly)";
        else if (c->gone)
            out << "Garrison: gone. The Hub is guarded by your own people.";
        else
        {
            double left = c->paidUntil - gameHours();
            bool grace = c->paidUntil <= c->start + GRACE_DAYS * 24.0 + 0.01;
            out << "Garrison: " << guards.size() << " Outlaws, ";
            if (left > 0)
                out << int(left / 2.4) / 10.0 << " days " << (grace ? "of grace" : "paid") << " left";
            else
                out << "PAYMENT DUE, they leave in " << int(left + OVERDUE_HOURS) << "h";
            out << ". A week costs " << weeklyFee(guards.size()) << " cats.";
        }
        return out.str();
    }

    void onGarrisonPay(MyGUI::WidgetPtr)
    {
        Contract* c = currentContract();
        int* money = (ou && ou->player) ? factionMoney(ou->player->getFaction()) : NULL;
        if (!c || c->gone || !money)
        {
            actionLog("[garrison] there's no contract to pay");
            return;
        }
        std::vector<Character*> guards;
        garrison(guards);
        int fee = weeklyFee(guards.size());
        if (*money < fee)
        {
            std::ostringstream note;
            note << "[garrison] a week costs " << fee << " cats; you have " << *money;
            actionLog(note.str());
            return;
        }
        *money -= fee;
        double now = gameHours();
        c->paidUntil = (c->paidUntil > now ? c->paidUntil : now) + 7 * 24.0;
        c->warned = NO_WARNING;
        persistContract();
        std::ostringstream note;
        note << "[garrison] paid " << fee << " cats: the Outlaws stay another week";
        actionLog(note.str());
    }

    // Dev: make the contract fall due now, to test the end of the grace period.
    void onGarrisonDueNow(MyGUI::WidgetPtr)
    {
        Contract* c = currentContract();
        if (!c || c->gone)
        {
            actionLog("[garrison] no running contract (claim the Hub first, or it already ended)");
            return;
        }
        c->paidUntil = gameHours();
        c->warned = NO_WARNING;
        persistContract();
        actionLog("[garrison] dev: contract made due now; they leave in a day unless paid");
    }

    void onGarrisonDismiss(MyGUI::WidgetPtr)
    {
        Contract* c = currentContract();
        if (!c || c->gone)
        {
            actionLog("[garrison] there's no garrison to dismiss");
            return;
        }
        garrisonLeaves(*c, "dismissed");
    }

    // ==== Town window: the Hub at a glance ====================================
    //
    // Population by profile, storage, who's at the gate, and a roll-up of
    // every item in the Hub's player-owned storage. The base that food,
    // contracts and the Engineer's "storage is full" rule will read from.

    MyGUI::Window* g_town = NULL;
    MyGUI::EditBox* g_townSummary[4] = {NULL, NULL, NULL, NULL};
    std::string questLine(); // the guided start, further down
    MyGUI::MultiListBox* g_townStock = NULL;
    std::vector<std::string> g_townStockRows;

    struct StockLine
    {
        int count;
        std::set<Building*> places;
    };

    // Every item in the Hub's player-owned buildings, by name. Returns how
    // many of those buildings have an inventory at all.
    int collectStock(std::map<std::string, StockLine>& out)
    {
        out.clear();
        if (!ou || !ou->player || !g_hub)
            return 0;
        Faction* mine = ou->player->getFaction();
        std::vector<Building*> all;
        hubBuildings(all);
        int containers = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Building* b = all[i];
            if (b->getFaction() != mine)
                continue;
            Inventory* inv = b->getInventory();
            if (!inv)
                continue;
            ++containers;
            lektor<InventorySection*>& sections = inv->getAllSections();
            for (uint32_t s = 0; s < sections.size(); ++s)
            {
                if (!sections[s])
                    continue;
                const Ogre::vector<InventorySection::SectionItem>::type& items = sections[s]->getItems();
                for (size_t k = 0; k < items.size(); ++k)
                {
                    Item* item = items[k].item;
                    if (!item)
                        continue;
                    StockLine& line = out[item->getName()];
                    line.count += item->quantity > 0 ? item->quantity : 1;
                    line.places.insert(b);
                }
            }
        }
        return containers;
    }

    void refreshTown()
    {
        if (!g_town || !g_town->getVisible() || !ou || !ou->player)
            return;

        // Summary: people by profile, storage, the gate.
        std::map<std::string, int> byProfile;
        int retinue = 0, citizens = 0;
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            if (!people[i])
                continue;
            std::string tag = nameTag(people[i]);
            if (tag.empty())
                ++retinue;
            else
            {
                ++citizens;
                ++byProfile[tag == CITIZEN_TAG ? std::string("no job") : tag];
            }
        }
        std::map<std::string, StockLine> stock;
        int containers = collectStock(stock);

        std::ostringstream sum;
        sum << "The Hub: " << (g_hub ? factionName(g_hub->getFaction()) : std::string("not found"))
            << "    Population " << retinue + citizens << ": retinue " << retinue << ", citizens " << citizens;
        if (!byProfile.empty())
        {
            sum << " (";
            for (std::map<std::string, int>::iterator it = byProfile.begin(); it != byProfile.end(); ++it)
                sum << (it == byProfile.begin() ? "" : ", ") << it->first << " " << it->second;
            sum << ")";
        }
        sum << "\nStorage: " << containers << " containers, " << stock.size() << " kinds of item"
            << "    At the gate: " << g_gate.size() << "\n" << contractLine() << "\n" << questLine();
        // One box per line: a multi-line caption showed only its first line.
        std::string all = sum.str();
        size_t start = 0;
        for (int line = 0; line < 4; ++line)
        {
            size_t nl = all.find('\n', start);
            g_townSummary[line]->setCaption(start == std::string::npos ? std::string()
                                            : all.substr(start, nl == std::string::npos ? std::string::npos
                                                                                       : nl - start));
            start = nl == std::string::npos ? std::string::npos : nl + 1;
        }

        // Stock table, biggest first.
        std::vector<std::pair<int, std::string> > order;
        for (std::map<std::string, StockLine>::iterator it = stock.begin(); it != stock.end(); ++it)
            order.push_back(std::make_pair(-it->second.count, it->first));
        std::sort(order.begin(), order.end());
        std::vector<std::string> rows;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const StockLine& line = stock[order[i].second];
            std::ostringstream row;
            row << order[i].second << "\t" << line.count << "\t" << line.places.size()
                << (line.places.size() == 1 ? " container" : " containers");
            rows.push_back(row.str());
        }
        if (rows.empty())
            rows.push_back("Nothing in storage yet.\t\t");
        fillTable(g_townStock, g_townStockRows, rows);
    }

    void createTownContent(MyGUI::Widget* client)
    {
        for (int line = 0; line < 4; ++line)
        {
            std::ostringstream name;
            name << "TownAdminSummary" << line;
            g_townSummary[line] = rosterText(client, 0.02f, 0.02f + line * 0.065f, 0.96f, 0.06f, name.str().c_str());
        }
        rosterButton(client, 0.02f, 0.285f, 0.30f, "TownAdminPay", "Pay garrison a week", onGarrisonPay);
        rosterButton(client, 0.33f, 0.285f, 0.20f, "TownAdminDismiss", "Dismiss", onGarrisonDismiss);
        g_townStock = client->createWidgetReal<MyGUI::MultiListBox>(
            "Kenshi_MultiListBox", 0.02f, 0.375f, 0.96f, 0.605f, MyGUI::Align::Default, "TownAdminStock");
        int w = g_townStock->getWidth();
        g_townStock->addColumn("Item", w * 55 / 100);
        g_townStock->addColumn("Count", w * 18 / 100);
        g_townStock->addColumn("In", w * 27 / 100 - 4);
    }

    // ==== Buildings tab: find things in the Hub, repair ruins, clear debris ===
    //
    // Every building in the Hub, nearest to where the camera is looking first,
    // so "the thing I'm looking at" is at the top. Clicking a row moves the
    // camera to it. Repair turns a ruin back into an unfinished building for
    // builders to put up with materials (experimental: how Kenshi takes a
    // ruin being un-destroyed is unknown); Repair now finishes it on the spot
    // (dev). Remove takes it away, as Remove selected did.

    MyGUI::MultiListBox* g_bList = NULL;
    MyGUI::Button* g_bRuinsButton = NULL;
    std::vector<Building*> g_bRows;
    std::vector<std::string> g_bShown;
    bool g_bRuinsOnly = true;

    void refreshBuildings()
    {
        if (!g_bList || !ou || !ou->player || !g_hub)
            return;
        Ogre::Vector3 eye = ou->player->getCameraCenter();
        std::vector<Building*> all;
        hubBuildings(all);
        std::vector<std::pair<float, Building*> > order;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (g_bRuinsOnly && !all[i]->isDestroyed() && buildProgress(all[i]) >= 1.0f)
                continue;
            Ogre::Vector3 p = all[i]->getPosition();
            float dx = p.x - eye.x, dz = p.z - eye.z;
            order.push_back(std::make_pair(dx * dx + dz * dz, all[i]));
        }
        std::sort(order.begin(), order.end());
        g_bRows.clear();
        std::vector<std::string> rows;
        for (size_t i = 0; i < order.size() && i < 200; ++i)
        {
            Building* b = order[i].second;
            std::ostringstream row;
            float built = buildProgress(b);
            row << b->getName() << "\t";
            if (b->isDestroyed())
                row << "ruin";
            else if (built < 1.0f)
                row << "unfinished " << int(built * 100.0f) << "%";
            else
                row << "standing";
            row << "\t" << (b->getTown() == hubBase() ? "base" : "old Hub") << "\t" << factionName(b->getFaction())
                << "\t" << int(sqrt(order[i].first) / 10.0f) << " m";
            rows.push_back(row.str());
            g_bRows.push_back(b);
        }
        if (rows.empty())
            rows.push_back(g_bRuinsOnly ? "No ruins or unfinished buildings.\t\t\t\t" : "No buildings.\t\t\t\t");
        fillTable(g_bList, g_bShown, rows);
        if (g_bRuinsButton)
            g_bRuinsButton->setCaption(g_bRuinsOnly ? "Showing: ruins, unfinished" : "Showing: everything");
    }

    Building* buildingPicked()
    {
        size_t i = g_bList ? g_bList->getIndexSelected() : MyGUI::ITEM_NONE;
        return (i != MyGUI::ITEM_NONE && i < g_bRows.size()) ? g_bRows[i] : NULL;
    }

    void onBuildingPicked(MyGUI::MultiListBox*, size_t)
    {
        Building* b = buildingPicked();
        if (b && ou && ou->player)
            ou->player->focusCamera(b->getPosition());
    }

    void onBuildingsNearCamera(MyGUI::WidgetPtr)
    {
        g_bShown.clear();
        refreshBuildings();
    }

    void onBuildingsRuinsToggle(MyGUI::WidgetPtr)
    {
        g_bRuinsOnly = !g_bRuinsOnly;
        g_bShown.clear();
        refreshBuildings();
    }

    std::string describeRepairState(Building* b)
    {
        std::ostringstream out;
        Building::ConstructionState* st = b->getBuildState_ActualNonShared();
        out << (b->isDestroyed() ? "still a ruin" : "no longer a ruin");
        if (st)
            out << ", construction " << int(st->constructionProgress * 100.0f) << "%"
                << (st->isComplete ? ", marked complete" : ", not complete") << ", materials needed "
                << int(st->getTotalMats() - st->getTotalMatsPresent());
        return out.str();
    }

    // Kenshi's builders only take construction work in the player's base,
    // which is the Hub part that holds the buildings the player put up; the
    // original Hub's buildings sit in the other part, so a repaired gate there
    // was never built (Bill, 2026-11-22: they build anything but the gate).
    // Building::_changeTown moves one building across.
    std::string moveIntoBase(Building* b)
    {
        TownBase* base = hubBase();
        if (!base || !base->isTown())
            return "no base part to move into";
        if (b->getTown() == base)
            return "already in the base";
        size_t before = buildingCount(base);
        b->_changeTown(base->isTown());
        std::ostringstream out;
        out << (b->getTown() == base ? "moved into the base" : "move did NOT take") << " (base had " << before
            << " buildings, now " << buildingCount(base) << ")";
        return out.str();
    }

    // A ruin becomes an unfinished building in the base, for builders to put
    // up with materials (or finished on the spot: dev).
    void repairBuilding(Building* b, bool instantly)
    {
        if (!b->isDestroyed())
        {
            actionLog("[buildings] " + b->getName() + " isn't a ruin");
            return;
        }
        b->setDestroyed(false);
        if (!instantly)
        {
            // Un-ruining alone left the building marked complete: repairs
            // finished the moment they were ordered (2026-11-22). Reset its
            // own construction state so builders have to bring materials.
            Building::ConstructionState* st = b->getBuildState_ActualNonShared();
            if (st)
            {
                st->setup(b->data);
                st->isComplete = false;
                st->isDismantled = false;
                st->isPaused = false;
                st->constructionProgress = 0.0f;
            }
        }
        b->setConstructionProgress(instantly ? 1.0f : 0.0f);
        std::string moved = moveIntoBase(b);
        actionLog("[buildings] repair " + std::string(instantly ? "now" : "ordered") + ": " + b->getName() + ": " +
                  describeRepairState(b) + "; " + moved + (instantly ? "" : " (needs builders and materials)"));
    }

    void repairPicked(bool instantly)
    {
        Building* b = buildingPicked();
        if (!b)
        {
            actionLog("[buildings] pick a building in the list first");
            return;
        }
        repairBuilding(b, instantly);
        g_bShown.clear();
        refreshBuildings();
    }

    // ---- Repair on Kenshi's own building panel, under Dismantle.
    //
    // Building::getGUIDestroyButton adds the Dismantle button to the panel a
    // selected building opens (found 2026-11-22: the function using the
    // "Dismantle" string, disk RVA 0x2FEA10, asks canDismantle through vtable
    // +0x278; KenshiLib lists it at vtable +0x3A8). It's protected, so its
    // address is read from live buildings' vtables and hooked there, once per
    // distinct implementation. After it runs, a ruin in our Hub gets a Repair
    // line; the line's callback repairs whatever building the panel was last
    // filled for.
    typedef void (*GuiButtonsFn)(Building*, DatapanelGUI*, int);
    const size_t VT_GET_GUI_DESTROY_BUTTON = 0x3A8 / sizeof(void*);
    const int PANEL_HOOK_MAX = 4;
    GuiButtonsFn g_panelOrig[PANEL_HOOK_MAX] = {NULL, NULL, NULL, NULL};
    void* g_panelTarget[PANEL_HOOK_MAX] = {NULL, NULL, NULL, NULL};
    int g_panelHooks = 0;
    Building* g_panelBuilding = NULL;

    bool inHub(Building* b)
    {
        std::vector<Building*> all;
        hubBuildings(all);
        return std::find(all.begin(), all.end(), b) != all.end();
    }

    void onPanelRepair(DataPanelLine*)
    {
        Building* b = g_panelBuilding;
        if (!b || !g_hub || !hubIsOurs() || !inHub(b))
            return;
        repairBuilding(b, false);
        toast("Repair ordered: " + b->getName() + ". Builders will need materials.");
    }

    void addRepairLine(Building* b, DatapanelGUI* dp, int category)
    {
        if (!b || !dp || !g_hub || !b->isDestroyed() || !hubIsOurs() || !inHub(b))
            return;
        g_panelBuilding = b;
        DataPanelLine_Button* line = dp->setLineButton("KenshiTownRepair", "Repair", category);
        if (line && line->callback.empty())
            line->callback = MyGUI::newDelegate(onPanelRepair);
    }

    template <int N>
    void panelButtonsHook(Building* b, DatapanelGUI* dp, int category)
    {
        g_panelOrig[N](b, dp, category);
        addRepairLine(b, dp, category);
    }

    GuiButtonsFn panelDetour(int n)
    {
        switch (n)
        {
        case 0: return panelButtonsHook<0>;
        case 1: return panelButtonsHook<1>;
        case 2: return panelButtonsHook<2>;
        default: return panelButtonsHook<3>;
        }
    }

    void hookBuildingPanels()
    {
        if (g_panelHooks >= PANEL_HOOK_MAX || !g_hub)
            return;
        std::vector<Building*> all;
        hubBuildings(all);
        for (size_t i = 0; i < all.size() && g_panelHooks < PANEL_HOOK_MAX; ++i)
        {
            void** vtable = *reinterpret_cast<void***>(all[i]);
            void* fn = vtable[VT_GET_GUI_DESTROY_BUTTON];
            bool known = false;
            for (int k = 0; k < g_panelHooks; ++k)
                known = known || g_panelTarget[k] == fn;
            if (known)
                continue;
            int n = g_panelHooks++;
            g_panelTarget[n] = fn;
            std::ostringstream what;
            what << "building panel buttons #" << n << " (" << all[i]->getName() << ")";
            if (KenshiLib::SUCCESS != KenshiLib::AddHook(reinterpret_cast<intptr_t>(fn), panelDetour(n), &g_panelOrig[n]))
                ErrorLog("TownLedger: could not hook " + what.str());
            else
                DebugLog("TownLedger: hooked " + what.str());
        }
    }

    void onBuildingMoveToBase(MyGUI::WidgetPtr)
    {
        Building* b = buildingPicked();
        if (!b)
        {
            actionLog("[buildings] pick a building in the list first");
            return;
        }
        actionLog("[buildings] " + b->getName() + ": " + moveIntoBase(b));
        g_bShown.clear();
        refreshBuildings();
    }

    void onBuildingRepair(MyGUI::WidgetPtr)
    {
        repairPicked(false);
    }

    void onBuildingRepairNow(MyGUI::WidgetPtr)
    {
        repairPicked(true);
    }

    void onBuildingRemove(MyGUI::WidgetPtr)
    {
        Building* b = buildingPicked();
        if (!b)
        {
            actionLog("[buildings] pick a building in the list first");
            return;
        }
        Ogre::Vector3 p = b->getPosition();
        FILE* f = fopen(REMOVALS_FILE, "a");
        if (f)
        {
            fprintf(f, "%s\t%f\t%f\t%s\n", b->getName().c_str(), p.x, p.z, b->isDestroyed() ? "destroyed" : "intact");
            fclose(f);
        }
        actionLog("[buildings] removed " + b->getName());
        ou->dynamicDestroyBuilding(hand(b));
        g_bShown.clear();
        g_bRows.clear();
        g_bList->removeAllItems();
    }

    void createBuildingsContent(MyGUI::Widget* client)
    {
        g_bList = client->createWidgetReal<MyGUI::MultiListBox>("Kenshi_MultiListBox", 0.02f, 0.02f, 0.96f, 0.69f,
                                                                MyGUI::Align::Default, "TownBuildingsList");
        int w = g_bList->getWidth();
        g_bList->addColumn("Building", w * 38 / 100);
        g_bList->addColumn("State", w * 20 / 100);
        g_bList->addColumn("Part", w * 14 / 100);
        g_bList->addColumn("Owner", w * 16 / 100);
        g_bList->addColumn("Away", w * 12 / 100 - 4);
        g_bList->eventListChangePosition += MyGUI::newDelegate(onBuildingPicked);
        // Row 1: finding things. Row 2: doing things.
        rosterButton(client, 0.02f, 0.73f, 0.30f, "TownBuildingsNear", "Near camera", onBuildingsNearCamera);
        g_bRuinsButton = rosterButton(client, 0.34f, 0.73f, 0.30f, "TownBuildingsRuins", "Showing: ruins",
                                      onBuildingsRuinsToggle);
        rosterButton(client, 0.66f, 0.73f, 0.32f, "TownBuildingsMove", "Move to base", onBuildingMoveToBase);
        rosterButton(client, 0.02f, 0.82f, 0.30f, "TownBuildingsRepair", "Repair", onBuildingRepair);
        rosterButton(client, 0.34f, 0.82f, 0.30f, "TownBuildingsRepairNow", "Repair now", onBuildingRepairNow);
        rosterButton(client, 0.66f, 0.82f, 0.32f, "TownBuildingsRemove", "Remove", onBuildingRemove);
        rosterText(client, 0.02f, 0.91f, 0.96f, 0.07f, "TownBuildingsHelp")
            ->setCaption("Look at the spot, press Near camera: what you're looking at is at the top.");
    }

    // ==== The main window: one window, four tabs ===============================

    MyGUI::Window* g_main = NULL;
    MyGUI::TabControl* g_tabs = NULL;

    void createMain()
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;
        g_main = gui->createWidgetReal<MyGUI::Window>("Kenshi_WindowCX", 0.17f, 0.10f, 0.58f, 0.72f,
                                                      MyGUI::Align::Default, "Window", "KenshiTownWindow");
        g_main->setCaption("Kenshi Town");
        g_main->eventWindowButtonPressed += MyGUI::newDelegate(onWindowButton);
        g_tabs = g_main->getClientWidget()->createWidgetReal<MyGUI::TabControl>(
            "Kenshi_TabControl", 0.0f, 0.0f, 1.0f, 1.0f, MyGUI::Align::Stretch, "KenshiTownTabs");
        g_tabs->setButtonAutoWidth(false);
        g_tabs->setButtonDefaultWidth(140);
        createTownContent(g_tabs->addItem("Town"));
        createRosterContent(g_tabs->addItem("Roster"));
        createBuildingsContent(g_tabs->addItem("Buildings"));
        createLedgerContent(g_tabs->addItem("Ledger"));
        // The old per-window pointers all mean "the main window is open" now.
        g_window = g_roster = g_town = g_main;
        g_main->setVisible(false);
    }

    // Open the window on a tab; the same tab again closes it.
    void showTab(size_t tab)
    {
        if (!g_main)
            createMain();
        if (!g_main)
            return;
        bool sameTab = g_tabs->getIndexSelected() == tab;
        if (g_main->getVisible() && sameTab)
        {
            g_main->setVisible(false);
            return;
        }
        g_tabs->setIndexSelected(tab);
        g_main->setVisible(true);
        g_rosterProfileRows.clear(); // force refills
        g_rosterPeopleText.clear();
        g_rosterGateRows.clear();
        g_townStockRows.clear();
        g_bShown.clear();
        refresh();
        refreshRoster();
        refreshTown();
        refreshBuildings();
    }

    void toggleTown()
    {
        showTab(0); // Town
    }

    void onTownClick(MyGUI::WidgetPtr)
    {
        if (g_main && g_main->getVisible())
            g_main->setVisible(false);
        else
            showTab(g_tabs ? g_tabs->getIndexSelected() : 0);
    }

    // ==== Toasts ===============================================================
    //
    // Short-lived messages on the right of the HUD, right-justified against the
    // screen edge, stacked above the Town button and the tray. Newest at the
    // bottom; each fades after TOAST_SECONDS. Clicking one opens the Town tab.

    struct Toast
    {
        std::string text;
        DWORD until; // GetTickCount() when it goes
    };
    std::vector<Toast> g_toasts;
    const DWORD TOAST_SECONDS = 10;
    const size_t TOAST_MAX = 4;

    void toast(const std::string& text)
    {
        Toast t;
        t.text = text;
        t.until = GetTickCount() + TOAST_SECONDS * 1000;
        g_toasts.push_back(t);
        if (g_toasts.size() > TOAST_MAX)
            g_toasts.erase(g_toasts.begin());
        DebugLog("TownLedger: [toast] " + text);
    }

    // ==== The guided start ======================================================
    //
    // A short chain of tasks that walks a new town through its first days:
    // Kenshi itself has no quests. One task at a time, checked every couple of
    // seconds; finishing one pays a little and starts the next. The step in
    // progress is kept in the per-game record. Shown in the Town tab and
    // announced with toasts.

    bool hubHasStanding(const char* nameContains)
    {
        if (!ou || !ou->player)
            return false;
        Faction* mine = ou->player->getFaction();
        std::vector<Building*> all;
        hubBuildings(all);
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i]->getFaction() == mine && !all[i]->isDestroyed() && buildProgress(all[i]) >= 1.0f &&
                all[i]->getName().find(nameContains) != std::string::npos)
                return true;
        return false;
    }

    int stockOf(const char* item)
    {
        std::map<std::string, StockLine> stock;
        collectStock(stock);
        std::map<std::string, StockLine>::iterator it = stock.find(item);
        return it == stock.end() ? 0 : it->second.count;
    }

    // How far along a task is, for the tracker: have/need fills the bar, text
    // says it in words.
    struct QuestProgress
    {
        int have, need;
        std::string text;
    };

    std::string lowerCase(std::string s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = char(tolower((unsigned char)s[i]));
        return s;
    }

    std::string outOf(int have, int need)
    {
        std::ostringstream out;
        out << have << " / " << need;
        return out.str();
    }

    // The storm houses left in the Hub. The first-claim clean-up removes the
    // "Old Storm House"s, and the two that survive it are named otherwise, so
    // any name with "storm" in it counts, ruined or not (2026-11-22: matching
    // the exact name made this task finish the moment a new game began). The
    // most seen since the world loaded is the bar's whole.
    int g_stormMax = 0;
    bool g_stormLogged = false;

    bool qStormHousesCleared(QuestProgress& p)
    {
        std::vector<Building*> all;
        hubBuildings(all);
        int left = 0;
        std::map<std::string, int> names;
        for (size_t i = 0; i < all.size(); ++i)
            if (lowerCase(all[i]->getName()).find("storm") != std::string::npos)
            {
                ++left;
                ++names[all[i]->getName()];
            }
        if (!g_stormLogged)
        {
            g_stormLogged = true;
            std::ostringstream out;
            out << "[quest] storm houses in the Hub:";
            for (std::map<std::string, int>::iterator it = names.begin(); it != names.end(); ++it)
                out << " " << it->second << " x " << it->first << ";";
            actionLog(out.str());
        }
        if (left > g_stormMax)
            g_stormMax = left;
        p.need = g_stormMax > 0 ? g_stormMax : 1;
        p.have = p.need - left;
        std::ostringstream t;
        t << left << " left to dismantle";
        p.text = left == 0 ? "All cleared" : t.str();
        return left == 0;
    }

    // A building of ours in the Hub whose name contains `nameContains`:
    // finished, under construction (the bar follows it) or not started.
    bool qBuilt(const char* nameContains, QuestProgress& p)
    {
        p.have = 0;
        p.need = 1;
        p.text = "Not started";
        if (!ou || !ou->player)
            return false;
        Faction* mine = ou->player->getFaction();
        std::vector<Building*> all;
        hubBuildings(all);
        float best = -1.0f;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i]->getFaction() == mine && !all[i]->isDestroyed() &&
                all[i]->getName().find(nameContains) != std::string::npos)
            {
                float done = buildProgress(all[i]);
                if (done > best)
                    best = done;
            }
        if (best >= 1.0f)
        {
            p.have = 1;
            p.text = "Built";
            return true;
        }
        if (best >= 0.0f)
        {
            p.need = 100;
            p.have = int(best * 100.0f);
            std::ostringstream t;
            t << "Building " << p.have << "%";
            p.text = t.str();
        }
        return false;
    }

    // The Town Hall: any Longhouse of the Hub's, repaired and standing. They
    // start as ruins; Repair is on the building's own panel, under Dismantle.
    bool qTownHall(QuestProgress& p)
    {
        p.have = 0;
        p.need = 1;
        p.text = "Ruined: select one, press Repair";
        std::vector<Building*> all;
        hubBuildings(all);
        float best = -1.0f;
        bool any = false;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (lowerCase(all[i]->getName()).find("longhouse") == std::string::npos)
                continue;
            any = true;
            if (all[i]->isDestroyed())
                continue;
            float done = buildProgress(all[i]);
            if (done > best)
                best = done;
        }
        if (!any)
            p.text = "No Longhouse left in the Hub";
        if (best >= 1.0f)
        {
            p.have = 1;
            p.text = "Repaired";
            return true;
        }
        if (best >= 0.0f)
        {
            p.need = 100;
            p.have = int(best * 100.0f);
            std::ostringstream t;
            t << "Repairing " << p.have << "%";
            p.text = t.str();
        }
        return false;
    }

    bool qResearchBench(QuestProgress& p)
    {
        return qBuilt("Research Bench", p);
    }

    bool qMaterialStorage(QuestProgress& p)
    {
        return qBuilt("Storage: Building Material", p);
    }

    bool qStoneMine(QuestProgress& p)
    {
        return qBuilt("Stone Mine", p);
    }

    bool qStoneProcessor(QuestProgress& p)
    {
        return qBuilt("Stone Processor", p);
    }

    bool qFirstCitizen(QuestProgress& p)
    {
        p.need = 1;
        p.have = g_citizens.empty() ? 0 : 1;
        p.text = outOf(p.have, p.need);
        return p.have >= p.need;
    }

    bool qCitizenWithJob(QuestProgress& p)
    {
        int working = 0;
        for (std::set<Character*>::iterator it = g_citizens.begin(); it != g_citizens.end(); ++it)
            if (nameTag(*it) != CITIZEN_TAG && !nameTag(*it).empty())
                ++working;
        p.need = 1;
        p.have = working > 0 ? 1 : 0;
        p.text = outOf(p.have, p.need);
        return working > 0;
    }

    bool qBedsForAll(QuestProgress& p)
    {
        p.have = 0;
        p.need = 1;
        if (!ou || !ou->player)
            return false;
        Faction* mine = ou->player->getFaction();
        std::vector<Building*> all;
        hubBuildings(all);
        int beds = 0;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i]->getFaction() == mine && all[i]->getSpecialFunction() == BF_BED && buildProgress(all[i]) >= 1.0f)
                ++beds;
        int people = int(ou->player->getAllPlayerCharacters().size());
        p.need = people > 0 ? people : 1;
        p.have = beds < p.need ? beds : p.need;
        std::ostringstream t;
        t << beds << (beds == 1 ? " bed for " : " beds for ") << plural(people, "person", "people");
        p.text = t.str();
        return beds > 0 && beds >= people;
    }

    bool qStockpile(QuestProgress& p)
    {
        int have = stockOf("Building Material");
        p.need = 100;
        p.have = have < 100 ? have : 100;
        p.text = outOf(have, 100);
        return have >= 100;
    }

    bool qOwnGuard(QuestProgress& p)
    {
        p.have = 0;
        p.need = 15;
        if (!ou || !ou->player)
            return false;
        float best = 0.0f;
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            CharStats* st = people[i] ? people[i]->getStats() : NULL;
            if (st && st->getStat(STAT_MELEE_ATTACK, false) > best)
                best = st->getStat(STAT_MELEE_ATTACK, false);
        }
        p.have = int(best) < 15 ? int(best) : 15;
        std::ostringstream t;
        t << "Best Melee Attack " << int(best) << " / 15";
        p.text = t.str();
        return best >= 15.0f;
    }

    struct Quest
    {
        const char* title;
        const char* hint; // one short line: the tracker doesn't wrap
        int reward;
        bool (*done)(QuestProgress&);
    };
    const Quest QUESTS[] = {
        {"Clear the old storm houses", "Dismantle them: their materials start your stockpile.", 200,
         qStormHousesCleared},
        {"Repair a Longhouse: your Town Hall", "Select a ruined Longhouse, press Repair. Builders do the rest.", 300,
         qTownHall},
        {"Set up a research bench", "Build a Small Research Bench inside the Town Hall.", 300, qResearchBench},
        // KenshiLib exposes no research state, so the research is folded into
        // the store it unlocks: the task finishes when the store is built.
        {"Research storage, build a materials store", "Research Storage Boxes at the bench, then Build, Storage.",
         300, qMaterialStorage},
        {"Open a stone mine", "Build a Stone Mine and set someone to work it.", 300, qStoneMine},
        {"Process stone into materials", "Build a Stone Processor next to the mine.", 300, qStoneProcessor},
        {"Take in your first citizen", "Drifters come to the gate. Accept one in the Roster.", 200, qFirstCitizen},
        {"Give a citizen a job", "Roster: capture a profile, then Assign it.", 300, qCitizenWithJob},
        {"A bed for everyone", "Build beds until everyone has one.", 300, qBedsForAll},
        {"Stockpile 100 building materials", "Keep the mine and processor running.", 500, qStockpile},
        {"Ready a guard of your own", "Train Melee Attack to 15, or take in an Ex-guard.", 500, qOwnGuard},
    };
    const int QUEST_COUNT = int(sizeof(QUESTS) / sizeof(QUESTS[0]));

    std::string questLine()
    {
        Contract* c = currentContract();
        if (!c || !hubIsOurs())
            return "Task: claim the Hub.";
        if (c->quest >= QUEST_COUNT)
            return "The Hub stands on its own feet. No more tasks for now.";
        std::ostringstream out;
        out << "Task " << c->quest + 1 << "/" << QUEST_COUNT << ": " << QUESTS[c->quest].title << ". "
            << QUESTS[c->quest].hint;
        return out.str();
    }

    int g_questAnnounced = -1;     // the step last toasted since the world loaded
    int g_questWarm = 0;           // main-loop ticks the Hub has been ours since then
    TownBase* g_questHub = NULL;   // the Hub those counts belong to
    QuestProgress g_questShown = {0, 1, ""}; // the current task's progress, for the tracker
    int g_questJustDone = -1;      // a task just finished, shown as complete until...
    DWORD g_questDoneUntil = 0;    // ...this GetTickCount()
    const DWORD QUEST_DONE_SHOW_MS = 4000;

    void questTick()
    {
        if (!ou || !ou->player || !g_hub || !hubIsOurs())
        {
            g_questWarm = 0;
            return;
        }
        if (g_hub != g_questHub) // a different world loaded
        {
            g_questHub = g_hub;
            g_questWarm = 0;
            g_stormMax = 0;
            g_stormLogged = false;
            g_questAnnounced = -1;
            g_questJustDone = -1;
        }
        Contract* c = currentContract();
        if (!c || c->quest >= QUEST_COUNT || buildingCount(g_hub) == 0)
            return;
        // Let a load settle, and its loading screen go, before judging or
        // announcing anything: a new game's clean-up runs in these first ticks.
        if (++g_questWarm < 3)
            return;
        if (int(g_questDoneUntil - GetTickCount()) > 0)
            return; // the tracker is still showing the last task as complete
        const Quest& q = QUESTS[c->quest];
        QuestProgress p;
        bool done = q.done(p);
        g_questShown = p;
        if (done)
        {
            int* money = factionMoney(ou->player->getFaction());
            if (money)
                *money += q.reward;
            std::ostringstream t;
            t << "Done: " << q.title << "  (+" << q.reward << " cats)";
            toast(t.str());
            // Played by us, not Kenshi (whose sounds are hashed Wwise events):
            // made by scripts/assets/chime.py, installed with the mod.
            PlaySoundA("mods\TownLedger\task_complete.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            g_questJustDone = c->quest;
            g_questDoneUntil = GetTickCount() + QUEST_DONE_SHOW_MS;
            ++c->quest;
            persistContract();
            if (c->quest >= QUEST_COUNT)
                toast("The Hub stands on its own feet.");
            return; // the next task is announced once the tracker has moved on
        }
        if (g_questAnnounced != c->quest)
        {
            g_questAnnounced = c->quest;
            toast(std::string("Task: ") + q.title);
        }
    }
    // The Town button sits centred above the squad panel (Kenshi's own layout,
    // data/gui/layout/Kenshi_MainPanel.layout: SquadPanel at x 0.309, width
    // 0.397, top 0.788). Above the Money / Day box Kenshi opens its building
    // inspector, and left of that are the speed buttons; both got covered.
    const float HUD_X = 0.457f, HUD_Y = 0.747f, HUD_W = 0.10f, HUD_H = 0.035f;

    // Everything else lives in one column on the right edge, top down: the
    // task tracker, then the tray's alerts, then toasts. Each part starts
    // where the one above it ended (g_columnY).
    const float COL_RIGHT = 0.995f, COL_W = 0.215f, COL_TOP = 0.09f, COL_GAP = 0.006f;
    float g_columnY = COL_TOP;

    // Width of a right-justified line for `text`, capped at the column's.
    float lineWidth(const std::string& text)
    {
        float w = 0.02f + 0.0058f * float(text.size());
        return w > COL_W ? COL_W : w;
    }
    // ==== Notification tray ===================================================
    //
    // A short stack of clickable lines above the Town button: what needs
    // attention right now, most urgent first. Recomputed every couple of
    // seconds; an issue disappears when it's dealt with. Clicking a line moves
    // the camera to what it's about, or opens the tab that deals with it.

    struct Notice
    {
        int priority;      // lower first
        std::string text;
        int tab;           // tab to open, or -1
        bool hasPlace;
        Ogre::Vector3 place; // where to point the camera
    };
    std::vector<Notice> g_notices;
    const size_t TRAY_LINES = 5;
    MyGUI::Button* g_trayLines[TRAY_LINES + 1] = {NULL, NULL, NULL, NULL, NULL, NULL};

    bool noticeOrder(const Notice& a, const Notice& b)
    {
        return a.priority < b.priority;
    }

    Notice notice(int priority, const std::string& text, int tab)
    {
        Notice n;
        n.priority = priority;
        n.text = text;
        n.tab = tab;
        n.hasPlace = false;
        return n;
    }

    // 90% or more of a building's inventory slots taken. Equipment sections
    // (a character's, not a building's) are skipped.
    bool buildingIsFull(Building* b)
    {
        Inventory* inv = b->getInventory();
        if (!inv)
            return false;
        lektor<InventorySection*>& sections = inv->getAllSections();
        int cells = 0, used = 0;
        for (uint32_t s = 0; s < sections.size(); ++s)
        {
            InventorySection* sec = sections[s];
            if (!sec || sec->isAnEquippedItemSection || sec->width <= 0 || sec->height <= 0)
                continue;
            cells += sec->width * sec->height;
            const Ogre::vector<InventorySection::SectionItem>::type& items = sec->getItems();
            for (size_t k = 0; k < items.size(); ++k)
                used += int(items[k].w) * int(items[k].h);
        }
        return cells > 0 && used * 10 >= cells * 9;
    }

    std::string plural(int n, const char* one, const char* many)
    {
        std::ostringstream out;
        out << n << " " << (n == 1 ? one : many);
        return out.str();
    }

    void computeNotices()
    {
        g_notices.clear();
        if (!ou || !ou->player || !g_hub || !hubIsOurs())
            return;
        Faction* mine = ou->player->getFaction();

        // Beds and full buildings, from the Hub's buildings.
        std::vector<Building*> all;
        hubBuildings(all);
        int beds = 0;
        std::map<std::string, std::vector<Building*> > full;
        for (size_t i = 0; i < all.size(); ++i)
        {
            Building* b = all[i];
            if (b->getFaction() != mine || b->isDestroyed())
                continue;
            BuildingFunction f = b->getSpecialFunction();
            if (f == BF_BED)
                ++beds;
            if (f != BF_BED && buildingIsFull(b))
                full[b->getName()].push_back(b);
        }

        // People: idle, no job.
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        int population = 0, idle = 0, noJob = 0;
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            Character* c = people[i];
            if (!c || c->isDead())
                continue;
            ++population;
            std::string tag = nameTag(c);
            if (tag.empty())
                continue; // the retinue is the player's business
            if (tag == CITIZEN_TAG)
                ++noJob;
            std::string doing = currentActivity(c);
            if (doing == "idle" || doing.find("Sitting") != std::string::npos ||
                doing.find("Idle") != std::string::npos)
                ++idle;
        }

        if (population > beds)
            g_notices.push_back(notice(2, plural(population - beds, "person has", "people have") + " no bed", 2));
        for (std::map<std::string, std::vector<Building*> >::iterator it = full.begin(); it != full.end(); ++it)
        {
            std::ostringstream t;
            if (it->second.size() == 1)
                t << it->first << " is full";
            else
                t << it->second.size() << " x " << it->first << " are full";
            Notice n = notice(3, t.str(), -1);
            n.hasPlace = true;
            n.place = it->second[0]->getPosition();
            g_notices.push_back(n);
        }
        if (noJob)
            g_notices.push_back(notice(4, plural(noJob, "citizen has", "citizens have") + " no job", 1));
        if (idle)
            g_notices.push_back(notice(5, plural(idle, "citizen", "citizens") + " idle", 1));
        if (!g_gate.empty())
            g_notices.push_back(notice(1, plural(int(g_gate.size()), "drifter", "drifters") + " at the gate", 1));

        // The garrison contract.
        Contract* c = currentContract();
        if (c && !c->gone)
        {
            double left = c->paidUntil - gameHours();
            if (left <= 0)
                g_notices.push_back(notice(0, "Garrison payment due: they leave in a day", 0));
            else if (left <= 48)
            {
                std::ostringstream t;
                t << "Garrison wants paying in " << int(left / 24.0 + 0.5) << (left > 36 ? " days" : " day");
                g_notices.push_back(notice(2, t.str(), 0));
            }
        }

        std::stable_sort(g_notices.begin(), g_notices.end(), noticeOrder);
    }

    void onTrayClick(MyGUI::WidgetPtr w)
    {
        for (size_t i = 0; i < TRAY_LINES && i < g_notices.size(); ++i)
        {
            if (g_trayLines[i] != w)
                continue;
            const Notice& n = g_notices[i];
            if (n.hasPlace && ou && ou->player)
                ou->player->focusCamera(n.place);
            if (n.tab >= 0 && !(g_main && g_main->getVisible() && g_tabs->getIndexSelected() == size_t(n.tab)))
                showTab(size_t(n.tab));
            return;
        }
        if (w == g_trayLines[TRAY_LINES])
            showTab(0);
    }

    MyGUI::Button* g_toastLines[4] = {NULL, NULL, NULL, NULL};

    void onToastClick(MyGUI::WidgetPtr)
    {
        showTab(0);
    }

    // Right-justified against the screen edge, below the tray. Newest at the
    // top.
    void updateToasts(bool inGame)
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;
        DWORD now = GetTickCount();
        for (size_t i = 0; i < g_toasts.size();)
            if (int(g_toasts[i].until - now) <= 0)
                g_toasts.erase(g_toasts.begin() + i);
            else
                ++i;
        for (size_t i = 0; i < TOAST_MAX; ++i)
        {
            if (!g_toastLines[i])
            {
                std::ostringstream name;
                name << "KenshiTownToast" << i;
                g_toastLines[i] = gui->createWidgetReal<MyGUI::Button>("Kenshi_Button1", 0.6f, 0.5f, 0.3f, HUD_H,
                                                                       MyGUI::Align::Default, "Main", name.str());
                g_toastLines[i]->eventMouseButtonClick += MyGUI::newDelegate(onToastClick);
            }
            bool show = inGame && i < g_toasts.size();
            g_toastLines[i]->setVisible(show);
            if (!show)
                continue;
            const Toast& t = g_toasts[g_toasts.size() - 1 - i];
            float w = lineWidth(t.text);
            g_toastLines[i]->setRealCoord(COL_RIGHT - w, g_columnY, w, HUD_H);
            g_toastLines[i]->setCaption(t.text);
            g_columnY += HUD_H + 0.004f;
        }
    }
    void updateTray(bool inGame)
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;
        if (!g_trayLines[0])
            for (size_t i = 0; i <= TRAY_LINES; ++i)
            {
                std::ostringstream name;
                name << "KenshiTownTray" << i;
                g_trayLines[i] = gui->createWidgetReal<MyGUI::Button>("Kenshi_Button1", 0.6f, 0.5f, 0.2f, HUD_H,
                                                                      MyGUI::Align::Default, "Main", name.str());
                g_trayLines[i]->eventMouseButtonClick += MyGUI::newDelegate(onTrayClick);
            }
        if (inGame)
            computeNotices();
        for (size_t i = 0; i <= TRAY_LINES; ++i)
        {
            std::string text;
            if (i < TRAY_LINES && i < g_notices.size())
                text = g_notices[i].text;
            else if (i == TRAY_LINES && g_notices.size() > TRAY_LINES)
            {
                std::ostringstream t;
                t << "+" << g_notices.size() - TRAY_LINES << " more";
                text = t.str();
            }
            bool show = inGame && !text.empty();
            g_trayLines[i]->setVisible(show);
            if (!show)
                continue;
            float w = lineWidth(text);
            g_trayLines[i]->setRealCoord(COL_RIGHT - w, g_columnY, w, HUD_H);
            g_trayLines[i]->setCaption(text);
            g_columnY += HUD_H + 0.004f;
        }
        g_columnY += COL_GAP;
    }

    // ==== Task tracker ==========================================================
    //
    // Pinned at the top of the right-hand column, the way city builders show
    // objectives: a header with the chain's progress, the task, a one-line
    // hint, a bar with the count in words, and the reward and what comes next.
    // A finished task shows as complete (green, bar full, the reward) for a
    // few seconds before the next one takes its place. "-" folds it to its
    // header line. It sits in a Kenshi HUD panel (bare text over the world
    // was tried alongside it, 2026-11-22; Bill picked the panel).

    bool g_trackerFolded = false;
    MyGUI::Widget* g_trkPanel = NULL;
    MyGUI::TextBox* g_trkHeader = NULL;
    MyGUI::TextBox* g_trkTitle = NULL;
    MyGUI::TextBox* g_trkHint = NULL;
    MyGUI::TextBox* g_trkCount = NULL;
    MyGUI::TextBox* g_trkFoot = NULL;
    MyGUI::ProgressBar* g_trkBar = NULL;
    MyGUI::Button* g_trkFold = NULL;

    void updateTracker(bool inGame);

    void onTrackerFold(MyGUI::WidgetPtr)
    {
        g_trackerFolded = !g_trackerFolded;
        updateTracker(true);
    }

    MyGUI::TextBox* trackerText(const char* skin, const char* name)
    {
        MyGUI::TextBox* t = MyGUI::Gui::getInstancePtr()->createWidgetReal<MyGUI::TextBox>(
            skin, 0.6f, 0.1f, 0.2f, 0.02f, MyGUI::Align::Default, "Main", name);
        t->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
        t->setNeedMouseFocus(false);
        return t;
    }

    void place(MyGUI::Widget* w, float x, float y, float width, float height)
    {
        w->setRealCoord(x, y, width, height);
        w->setVisible(true);
    }

    void updateTracker(bool inGame)
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;
        if (!g_trkPanel)
        {
            g_trkPanel = gui->createWidgetReal<MyGUI::Widget>("Kenshi_FloatingPanelSkin", 0.6f, 0.1f, 0.2f, 0.1f,
                                                               MyGUI::Align::Default, "Main", "KenshiTownTrkPanel");
            g_trkPanel->setNeedMouseFocus(false);
            g_trkHeader = trackerText("Kenshi_TextboxStandardText_Small", "KenshiTownTrkHeader");
            g_trkTitle = trackerText("Kenshi_TextboxStandardText", "KenshiTownTrkTitle");
            g_trkHint = trackerText("Kenshi_TextboxStandardText_Small", "KenshiTownTrkHint");
            g_trkBar = gui->createWidgetReal<MyGUI::ProgressBar>("Kenshi_ProgressBarFill", 0.6f, 0.1f, 0.1f, 0.01f,
                                                                 MyGUI::Align::Default, "Main", "KenshiTownTrkBar");
            g_trkBar->setProgressRange(1000);
            g_trkBar->setNeedMouseFocus(false);
            g_trkCount = trackerText("Kenshi_TextboxStandardText_Small", "KenshiTownTrkCount");
            g_trkFoot = trackerText("Kenshi_TextboxStandardText_Small", "KenshiTownTrkFoot");
            g_trkFold = gui->createWidgetReal<MyGUI::Button>("Kenshi_Button1", 0.6f, 0.1f, 0.02f, 0.02f,
                                                             MyGUI::Align::Default, "Main", "KenshiTownTrkFold");
            g_trkFold->eventMouseButtonClick += MyGUI::newDelegate(onTrackerFold);
        }
        MyGUI::Widget* all[] = {g_trkPanel, g_trkHeader, g_trkTitle, g_trkHint, g_trkBar,
                                g_trkCount, g_trkFoot, g_trkFold};
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
            all[i]->setVisible(false);
        g_columnY = COL_TOP;

        // What to show: the task in progress, or the one just finished.
        Contract* c = inGame && g_hub ? currentContract() : NULL;
        bool ours = inGame && g_hub && hubIsOurs();
        bool justDone = g_questJustDone >= 0 && int(g_questDoneUntil - GetTickCount()) > 0;
        int step = justDone ? g_questJustDone : (c ? c->quest : 0);
        if (!inGame || (c && !justDone && c->quest >= QUEST_COUNT))
            return; // nothing to track
        if (ours && (!c || g_questWarm < 3) && !justDone)
            return; // still settling after a load

        const float x = COL_RIGHT - COL_W, pad = 0.008f, inner = COL_W - 2 * pad;
        float y = COL_TOP + pad;

        std::ostringstream head;
        if (!ours)
            head << "KENSHI TOWN";
        else
            head << "GETTING STARTED   " << step + 1 << " / " << QUEST_COUNT;
        g_trkHeader->setCaption(head.str());
        g_trkHeader->setTextColour(MyGUI::Colour(0.85f, 0.78f, 0.6f));
        place(g_trkHeader, x + pad, y, inner - 0.025f, 0.02f);
        g_trkFold->setCaption(g_trackerFolded ? "+" : "-");
        place(g_trkFold, x + COL_W - pad - 0.018f, y, 0.018f, 0.02f);
        y += 0.024f;

        if (!g_trackerFolded)
        {
            const MyGUI::Colour green(0.55f, 0.85f, 0.45f), dim(0.72f, 0.70f, 0.64f), plain(0.95f, 0.93f, 0.88f);
            std::string title, hint, count, foot;
            float frac = 0.0f;
            if (!ours)
            {
                title = "Claim the Hub";
                hint = "Hold the deed, or use Claim on the Ledger tab.";
            }
            else
            {
                const Quest& q = QUESTS[step];
                title = q.title;
                hint = q.hint;
                std::ostringstream f;
                if (justDone)
                {
                    count = "Complete";
                    frac = 1.0f;
                    f << "+" << q.reward << " cats";
                }
                else
                {
                    count = g_questShown.text;
                    frac = g_questShown.need > 0 ? float(g_questShown.have) / float(g_questShown.need) : 0.0f;
                    f << "Reward " << q.reward << " cats";
                }
                if (step + 1 < QUEST_COUNT)
                    f << "     Next: " << QUESTS[step + 1].title;
                foot = f.str();
            }
            g_trkTitle->setCaption(title);
            g_trkTitle->setTextColour(justDone ? green : plain);
            place(g_trkTitle, x + pad, y, inner, 0.026f);
            y += 0.027f;
            g_trkHint->setCaption(hint);
            g_trkHint->setTextColour(dim);
            place(g_trkHint, x + pad, y, inner, 0.02f);
            y += 0.024f;
            if (ours)
            {
                if (frac < 0.0f)
                    frac = 0.0f;
                if (frac > 1.0f)
                    frac = 1.0f;
                g_trkBar->setProgressPosition(size_t(frac * 1000.0f));
                place(g_trkBar, x + pad, y + 0.005f, inner * 0.5f, 0.011f);
                g_trkCount->setCaption(count);
                g_trkCount->setTextColour(justDone ? green : plain);
                place(g_trkCount, x + pad + inner * 0.5f + 0.006f, y, inner * 0.5f - 0.006f, 0.02f);
                y += 0.024f;
                g_trkFoot->setCaption(foot);
                g_trkFoot->setTextColour(justDone ? green : dim);
                place(g_trkFoot, x + pad, y, inner, 0.02f);
                y += 0.022f;
            }
        }
        y += pad;
        g_trkPanel->setRealCoord(x, COL_TOP, COL_W, y - COL_TOP);
        g_trkPanel->setVisible(true);
        g_columnY = y + COL_GAP;
    }

    // ---- HUD: one Town button, always on screen while a game is loaded.

    MyGUI::Button* g_hudTown = NULL;


    MyGUI::Button* hudButton(float y, const char* name, const char* caption, void (*click)(MyGUI::WidgetPtr))
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        MyGUI::Button* b = gui->createWidgetReal<MyGUI::Button>("Kenshi_Button1", HUD_X, y, HUD_W, HUD_H,
                                                                MyGUI::Align::Default, "Main", name);
        b->setCaption(caption);
        b->eventMouseButtonClick += MyGUI::newDelegate(click);
        return b;
    }

    void updateHud()
    {
        bool inGame = ou && ou->player && ou->player->getAllPlayerCharacters().size() > 0;
        if (!g_hudTown && inGame && MyGUI::Gui::getInstancePtr())
            g_hudTown = hudButton(HUD_Y, "KenshiTownHud", "Town", onTownClick);
        updateTracker(inGame); // the right-hand column, top down
        updateTray(inGame);
        updateToasts(inGame);
        if (g_hudTown)
        {
            g_hudTown->setVisible(inGame);
            std::ostringstream cap;
            cap << "Town";
            if (!g_gate.empty())
                cap << " (" << g_gate.size() << " at gate)";
            g_hudTown->setCaption(cap.str());
        }
    }

    void (*mainLoop_orig)(GameWorld*, float) = NULL;

    void mainLoop_hook(GameWorld* thisptr, float time)
    {
        mainLoop_orig(thisptr, time);

        ++g_frame;
        if (g_frame % FIND_HUB_EVERY_N_FRAMES == 1)
        {
            updateHubCache();
            rebuildCitizensFromNames();
            autoReapplyTick();
            arrivalsTick();
            deedTick();
            contractTick();
            questTick();
            hookBuildingPanels();
            if (hubIsOurs())
            {
                std::string merged = consolidateHub();
                if (!merged.empty())
                    actionLog("[hub] " + merged);
            }
            if (g_devFast)
                applyDevSpeed();
        }

        // Once, after the first in-Hub ore reading, log the scale we got.
        if (!g_loggedScale && (g_ore[STONE].hubCalls || g_ore[IRON].hubCalls || g_ore[COPPER].hubCalls))
        {
            g_loggedScale = true;
            logReport("first ore reading in Hub");
        }

        if (g_checkJobsAtFrame >= 0 && g_frame >= g_checkJobsAtFrame)
        {
            g_checkJobsAtFrame = -1;
            checkAssignedJobs();
        }

        // Escape closes our windows, like Kenshi's own panels.
        bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (esc && !g_escWasDown && kenshiHasFocus())
        {
            if (g_main)
                g_main->setVisible(false);
        }
        g_escWasDown = esc;
        if (g_frame % REFRESH_EVERY_N_FRAMES == 0)
        {
            refreshRoster();
            updateHud();
            if (g_frame % (REFRESH_EVERY_N_FRAMES * 4) == 0)
                refreshTown();
        }

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

    hookWhichTown();
    tryHook("buildingPlacementUpdate", KenshiLib::GetRealAddress(&PreviewBuilding::_NV_buildingPlacementUpdate),
            placementUpdate_hook, &placementUpdate_orig);

    tryHook("selectObject", KenshiLib::GetRealAddress(&PlayerInterface::selectObject),
            selectObject_hook, &selectObject_orig);
    tryHook("objectSelected", KenshiLib::GetRealAddress(&PlayerInterface::objectSelected),
            objectSelected_hook, &objectSelected_orig);

    g_createItem = (CreateItemFn)KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*, GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    g_createBuilding = (CreateBuildingFn)KenshiLib::GetRealAddress(&RootObjectFactory::createBuilding);
    g_createChar = (CreateCharFn)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomCharacter);

    loadProfiles();
    DebugLog("TownLedger: ready; Ledger and Roster buttons on the HUD");
}
