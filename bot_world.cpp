//
// Where bots go: level-appropriate camps, and spots near real players.
//
//   A camp is the spawn point of an ordinary monster close to the bot's level,
//   in an outdoor zone or dungeon. Read from the live zones, so it follows
//   whatever content the server has enabled.
//

#include "bots.h"

#include "common/database.h"
#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/map_engine.h"
#include "map/navmesh/navmesh.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>

namespace bots
{

namespace
{

constexpr auto kFieldZones = static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon);

// Zones from after the 75 era (June 2008 on): the Abyssea areas, and everything numbered 256 and up
// (Adoulin, Escha, Reisenjima, Dynamis - Divergence...). A test-server login put a bot in Abyssea - La Theine.
auto afterEra(const uint16 zoneId) -> bool
{
    static constexpr std::array<uint16, 10> abyssea = { 15, 45, 132, 215, 216, 217, 218, 253, 254, 255 };
    return zoneId >= 256 || std::ranges::find(abyssea, zoneId) != abyssea.end();
}

auto isField(const CZone* PZone) -> bool
{
    const auto type = static_cast<uint16>(PZone->GetTypeMask());
    return (type & kFieldZones) != 0 && (type & static_cast<uint16>(xi::ZoneType::Instanced)) == 0;
}

// Nation 0 San d'Oria, 1 Bastok, 2 Windurst. Same values as chars.nation and profile.nation.
// West Ronfaure, East Ronfaure, North Gustaberg, South Gustaberg, West Sarutabaruta, East Sarutabaruta.
constexpr std::array<std::array<xi::ZoneId, 2>, 3> kStartingAreas{ {
    { static_cast<xi::ZoneId>(100), static_cast<xi::ZoneId>(101) },
    { static_cast<xi::ZoneId>(106), static_cast<xi::ZoneId>(107) },
    { static_cast<xi::ZoneId>(115), static_cast<xi::ZoneId>(116) },
} };

auto zonesFor(const uint8 nation) -> std::vector<xi::ZoneId>
{
    std::vector<xi::ZoneId> zones;
    if (nation < kStartingAreas.size())
    {
        zones.push_back(kStartingAreas[nation][0]);
        zones.push_back(kStartingAreas[nation][1]);
        return zones;
    }

    for (const auto& pair : kStartingAreas)
    {
        zones.push_back(pair[0]);
        zones.push_back(pair[1]);
    }
    return zones;
}

auto inZones(const xi::ZoneId zone, const std::vector<xi::ZoneId>& zones) -> bool
{
    return std::ranges::find(zones, zone) != zones.end();
}

// findCamps first. Failing that, a Normal spawn that is not entirely above the bot (min level <= bot + 1).
auto spotsFor(const uint8 level, const std::vector<xi::ZoneId>& zones) -> std::vector<Place>
{
    std::vector<Place> spots;
    for (const auto& camp : findCamps(level))
    {
        if (inZones(camp.zone, zones))
        {
            spots.push_back(camp);
        }
    }
    if (!spots.empty())
    {
        return spots;
    }

    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        const auto zone = static_cast<xi::ZoneId>(zoneId);
        if (PZone == nullptr || !inZones(zone, zones))
        {
            continue;
        }

        PZone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (PMob->m_Type == xi::MobType::Normal && PMob->m_minLevel <= level + 1)
                {
                    spots.push_back(Place{ zone, PMob->m_SpawnPoint });
                }
            });
    }
    return spots;
}

auto chooseSpot(const std::vector<Place>& spots) -> std::optional<Place>
{
    if (spots.empty())
    {
        return std::nullopt;
    }

    const auto& camp = spots[xirand::GetRandomNumber<std::size_t>(spots.size())];
    return Place{ camp.zone, spotNear(camp.zone, camp.pos, 5.0f, 25.0f) };
}

} // namespace

auto findCamps(const uint8 level) -> std::vector<Place>
{
    std::vector<Place> camps;

    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr || !isField(PZone) || afterEra(static_cast<uint16>(zoneId)))
        {
            continue;
        }

        PZone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (PMob->m_Type == xi::MobType::Normal && PMob->m_minLevel + 1 >= level && PMob->m_maxLevel <= level + 2)
                {
                    camps.push_back(Place{ zoneId, PMob->m_SpawnPoint });
                }
            });
    }

    return camps;
}

auto campZoneNames(const uint8 level, const std::size_t count) -> std::vector<std::string>
{
    std::map<xi::ZoneId, std::size_t> spawns;
    for (const auto& camp : findCamps(level))
    {
        spawns[camp.zone]++;
    }

    std::vector<std::pair<std::size_t, xi::ZoneId>> ranked;
    for (const auto& [zone, n] : spawns)
    {
        ranked.emplace_back(n, zone);
    }
    std::ranges::sort(ranked, std::greater{});

    std::vector<std::string> names;
    for (const auto& [n, zone] : ranked)
    {
        if (names.size() >= count)
        {
            break;
        }
        if (auto* PZone = zoneutils::GetZone(zone))
        {
            names.push_back(PZone->getName());
        }
    }
    return names;
}

auto inStartingArea(const xi::ZoneId zone) -> bool
{
    for (const auto& pair : kStartingAreas)
    {
        if (zone == pair[0] || zone == pair[1])
        {
            return true;
        }
    }
    return false;
}

// Nation under 3 is chars.nation / profile.nation. Anything else: the other zone of the pair `here` belongs to.
auto otherStartingZone(const uint8 nation, const xi::ZoneId here) -> std::optional<xi::ZoneId>
{
    if (nation < kStartingAreas.size())
    {
        if (here == kStartingAreas[nation][0])
        {
            return kStartingAreas[nation][1];
        }
        if (here == kStartingAreas[nation][1])
        {
            return kStartingAreas[nation][0];
        }
        return std::nullopt;
    }

    for (const auto& pair : kStartingAreas)
    {
        if (here == pair[0])
        {
            return pair[1];
        }
        if (here == pair[1])
        {
            return pair[0];
        }
    }
    return std::nullopt;
}

// Where a bot below SOLO_BELOW logs in. Nation under 3 keeps to that nation's two zones; otherwise any of the six.
auto soloPlace(const uint8 level, const uint8 nation) -> std::optional<Place>
{
    return chooseSpot(spotsFor(level, zonesFor(nation)));
}

// A point on the navmesh between minRadius and maxRadius from around, or around itself if none turns up.
auto spotNear(const xi::ZoneId zone, const position_t& around, const float minRadius, const float maxRadius) -> position_t
{
    auto* PZone = zoneutils::GetZone(zone);
    if (PZone != nullptr && PZone->navMesh() != nullptr)
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (const auto spot = PZone->navMesh()->findRandomPosition(around, maxRadius); spot && distance(*spot, around) >= minRadius)
            {
                auto pos     = *spot;
                pos.rotation = xirand::GetRandomNumber<uint8>(255);
                return pos;
            }
        }
    }
    return around;
}

// A real player (not a pool bot, and not in a Mog House) standing within leeway of pos.
// A zone that can't be looked up counts as near, so a camp spot is not used unchecked.
auto nearRealPlayer(const xi::ZoneId zone, const position_t& pos, const float leeway) -> bool
{
    auto* PZone = zoneutils::GetZone(zone);
    if (PZone == nullptr)
    {
        return true;
    }

    bool near = false;
    PZone->ForEachChar(
        [&](CCharEntity* PChar)
        {
            if (!near && !isPoolChar(PChar->id) && !PChar->inMogHouse() && distance(pos, PChar->loc.p) < leeway)
            {
                near = true;
            }
        });
    return near;
}

// Leave the mob for a real player who is fighting it or has claimed it, or who is standing within
// PLAYER_LEEWAY and is not in this character's party. Another bot nearby does not count.
auto leaveForPlayers(const CCharEntity* PChar, const CMobEntity* PMob) -> bool
{
    if (PChar == nullptr || PMob == nullptr || PChar->loc.zone == nullptr)
    {
        return false;
    }

    const float leeway = static_cast<float>(settings::get<uint32>("bots.PLAYER_LEEWAY"));
    bool        leave  = false;
    PChar->loc.zone->ForEachChar(
        [&](CCharEntity* POther)
        {
            if (leave || POther == PChar || isPoolChar(POther->id) || POther->inMogHouse())
            {
                return;
            }

            const bool claimed  = PMob->m_OwnerID.isSet() && PMob->m_OwnerID.UniqueNo == POther->id;
            const bool fighting = (POther->PAI->IsEngaged() && POther->GetBattleTarget() == PMob) ||
                                  (PMob->PAI->IsEngaged() && PMob->GetBattleTarget() == POther);
            if (claimed || fighting)
            {
                leave = true;
                return;
            }

            // The bot's own party is not "someone else's pull".
            if (PChar->PParty != nullptr && POther->PParty == PChar->PParty)
            {
                return;
            }
            if (distance(PMob->loc.p, POther->loc.p) < leeway)
            {
                leave = true;
            }
        });
    return leave;
}

// The next login lands here; LoadChar reads the position back from the database.
void placeAt(const uint32 charId, const Place& place)
{
    db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_prevzone = ?, pos_x = ?, pos_y = ?, pos_z = ?, pos_rot = ?, moghouse = 0, boundary = 0 "
                     "WHERE charid = ?",
                     place.zone, place.zone, place.pos.x, place.pos.y, place.pos.z, place.pos.rotation, charId);
}

void placeAtHome(const uint32 charId)
{
    db::preparedStmt("UPDATE chars c JOIN bot_pool b ON b.charid = c.charid "
                     "SET c.pos_zone = b.zone, c.pos_prevzone = b.zone, c.pos_x = b.pos_x, c.pos_y = b.pos_y, c.pos_z = b.pos_z, "
                     "c.pos_rot = b.pos_rot, c.moghouse = 0, c.boundary = 0 WHERE c.charid = ?",
                     charId);
}

// Zones with real players in them, each with the highest of their levels.
auto playerZones(const std::set<uint32>& botIds) -> std::map<xi::ZoneId, PlayerZone>
{
    std::map<xi::ZoneId, PlayerZone> zones;

    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr || (static_cast<uint16>(PZone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::Instanced)) != 0)
        {
            continue;
        }

        PZone->ForEachChar(
            [&](CCharEntity* PChar)
            {
                if (botIds.contains(PChar->id) || PChar->inMogHouse() || PChar->status != xi::Status::Normal)
                {
                    return;
                }

                auto& entry = zones[zoneId];
                if (PChar->GetMLevel() >= entry.level)
                {
                    entry.level = PChar->GetMLevel();
                    entry.pos   = PChar->loc.p;
                }
            });
    }

    return zones;
}

// Zones with any real player in them, Mog Houses included; bots elsewhere go idle.
auto occupiedZones(const std::set<uint32>& botIds) -> std::set<xi::ZoneId>
{
    std::set<xi::ZoneId> zones;
    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr)
        {
            continue;
        }

        PZone->ForEachChar(
            [&](CCharEntity* PChar)
            {
                if (!botIds.contains(PChar->id))
                {
                    zones.insert(zoneId);
                }
            });
    }
    return zones;
}

} // namespace bots
