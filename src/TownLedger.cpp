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

#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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
            int score = (exact ? 1000000 : 0) + (active ? 100000 : 0) + int(n);
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
        }
        return best;
    }

    // Keep every twin owned by whoever owns the Hub, so a check that lands on
    // the twin sees the same owner. Runs from the main loop and after Claim.
    void syncHubTwins()
    {
        if (!g_hub)
            return;
        Faction* owner = g_hub->getFaction();
        for (size_t i = 0; i < g_hubTwins.size(); ++i)
        {
            TownBase* t = g_hubTwins[i];
            if (t->getFaction() == owner)
                continue;
            DebugLog("TownLedger: twin Hub " + factionName(t->getFaction()) + " -> " + factionName(owner));
            t->setFaction(owner, NULL);
        }
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

    void onMakeCitizenClick(MyGUI::WidgetPtr)
    {
        std::vector<Character*> chars;
        selectedCharacters(chars);
        std::ostringstream note;
        note << "make citizen:";
        for (size_t i = 0; i < chars.size(); ++i)
        {
            g_citizens.insert(chars[i]);
            ou->player->objectSelected(chars[i], false);
            note << " " << chars[i]->getName();
        }
        if (chars.empty())
            note << " nobody selected";
        actionLog("[" + note.str() + "]");
    }

    void onFreeCitizensClick(MyGUI::WidgetPtr)
    {
        std::ostringstream note;
        note << "freed " << g_citizens.size() << " citizens";
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
                c->addJob(g_role[j].type, g_role[j].subject, false, true, g_role[j].location);
            note << "\n  " << c->getName() << ": " << describeJobs(c);
        }
        actionLog("[" + note.str() + "]");
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
        return g_hub && ou && ou->player && g_hub->getFaction() == ou->player->getFaction();
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
        DebugLog("TownLedger: setFaction " + factionName(g_hub->getFaction()) + " -> " + factionName(to));
        g_hub->setFaction(to, NULL);
        syncHubTwins();

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
            "Kenshi_Button1", 0.03f + col * (width + gap), 0.38f + row * 0.115f, width, 0.1f,
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

    void createWindow()
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui)
            return;

        g_window = gui->createWidgetReal<MyGUI::Window>(
            "Kenshi_WindowCX", 0.68f, 0.2f, 0.3f, 0.55f,
            MyGUI::Align::Default, "Window", "TownLedgerWindow");
        g_window->setCaption("Town Ledger");
        g_window->eventWindowButtonPressed += MyGUI::newDelegate(onWindowButton);

        MyGUI::Widget* client = g_window->getClientWidget();
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

    DebugLog("TownLedger: ready, press F8 in game");
}
