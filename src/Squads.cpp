// Squads: keep Kenshi's squad bar in step with the Town Roster. See Squads.h.

#include "Squads.h"

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Platoon.h>

#include <set>
#include <sstream>

namespace
{
    // The player's squads, found through their members: every squad the
    // player has holds at least one character, and Kenshi removes empty ones.
    ActivePlatoon* findPlayerSquad(const std::string& name)
    {
        const lektor<Character*>& people = ou->player->getAllPlayerCharacters();
        for (uint32_t i = 0; i < people.size(); ++i)
        {
            ActivePlatoon* p = people[i] ? people[i]->platoon : NULL;
            if (p && p->getName() == name)
                return p;
        }
        return NULL;
    }
}

std::string putInSquad(Character* c, const std::string& squad)
{
    if (!c || !ou || !ou->player)
        return "no game";
    ActivePlatoon* from = c->platoon;
    if (from && from->getName() == squad)
        return "";

    std::ostringstream out;
    ActivePlatoon* to = findPlayerSquad(squad);
    if (!to)
    {
        to = ou->player->createSquad();
        if (!to)
            return "could not create squad " + squad;
        to->setName(squad);
        out << "new squad " << squad << "; ";
    }

    // The game's own move: append to the new squad. If the character is still
    // in the old squad afterwards, take them out of it and add them again.
    to->addCharacterAt(c, to->getSquadSize());
    if (c->platoon != to)
    {
        if (from)
            from->removeObject(c);
        to->addActiveObject(c);
    }
    out << "moved to squad " << squad << (c->platoon == to ? "" : " (FAILED)");
    return out.str();
}

std::string squadNameOf(Character* c)
{
    return (c && c->platoon) ? c->platoon->getName() : std::string();
}

const void* squadKeyOf(Character* c)
{
    return c ? static_cast<const void*>(c->platoon) : NULL;
}
