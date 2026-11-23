// Squads: keep Kenshi's squad bar in step with the Town Roster.
//
// Separate from TownLedger.cpp because it needs kenshi/Platoon.h, which
// can't be included alongside kenshi/Building/Building.h in KenshiLib 0.4.0
// (both define enum BuildingDesignation).
#pragma once

#include <string>

class Character;

// Move a player character into the player squad named `squad`, creating the
// squad if there isn't one. Returns a short account for the log; empty when
// the character was already there.
std::string putInSquad(Character* c, const std::string& squad);

// The name of the squad a character is in; empty if none.
std::string squadNameOf(Character* c);

// An opaque key that is the same for every member of a squad (NULL if none).
const void* squadKeyOf(Character* c);
