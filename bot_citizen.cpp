//
// Townsfolk cut from the pool. They live in one city, walk between its NPCs, and talk in /say.
//
//   Adventuring stays in the other files and is skipped for these bots. CITIZENS 0 marks nobody,
//   and then none of this runs. Later this moves onto real NPC entities.
//

#include "bots.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/entities/npc_entity.h"
#include "map/packets/c2s/0x074_group_solicit_res.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace bots
{

namespace
{

// {name} is the NPC they are stopped at. Skipped at home, and when that stop has no name.
constexpr std::array<const char*, 40> kLines = {
    "The cathedral bells carry over the square.",
    "An Elvaan's word is not given lightly.",
    "The knights keep the watch, as ever.",
    "Prices in the market have crept up again.",
    "Fine weather for the old stone to dry.",
    "Orcs on the plains, or so the gate says.",
    "San d'Oria stands, as it always has.",
    "These cobbles have outlasted kingdoms.",
    "Glory to the kingdom, and to the king.",
    "I have no errand beyond these walls.",
    "The west gate was busy at dawn.",
    "Rain on the ramparts, and the banners heavy.",
    "Clear skies over the castle today.",
    "Keep a hand on your purse in the market.",
    "The Orcs will find us ready, if they come.",
    "Light's blessing on this city.",
    "A knight rode out while the street was quiet.",
    "The shops open early. So do I.",
    "The cathedral marks the hour, and I with it.",
    "Our walls have never fallen. I like that.",
    "The auction house wants too much for iron.",
    "A cool wind off the mountains this morning.",
    "I saw the watch change at the gate.",
    "Elvaan steel, Elvaan stone, Elvaan pride.",
    "Better a quiet street than a loud field.",
    "My place is here, among our own.",
    "The king would call this a proper morning.",
    "Fresh bread, and the smell reaches the square.",
    "Mind the wagons if you cross the plaza.",
    "A copper saved is a copper the Orcs don't take.",
    "Good morning, {name}.",
    "Fair day to your stall, {name}.",
    "You are open early, {name}.",
    "Busy day for you, {name}?",
    "Save me a fair price, {name}.",
    "The usual, when you have a moment, {name}.",
    "Still the finest shop on the street, {name}.",
    "Morning, {name}. The knights are out in numbers.",
    "How is trade, {name}?",
    "Mind the cobbles, {name}. They are slick today.",
};

constexpr float kArrive = 3.0f;
constexpr auto  kWalkCap = std::chrono::minutes(2);
constexpr auto  kSayMemory = std::chrono::minutes(1);
constexpr float kSayHear = 15.0f;

struct Routine
{
    enum class Phase : uint8
    {
        Wait,
        Walk,
        Linger,
    };

    Phase             phase{ Phase::Wait };
    bool              live{};
    bool              held{}; // Wait is a pause in a walk already chosen, not a new stop
    uint32            stopId{};
    bool              home{};
    std::string       name;
    position_t        pos{};
    timer::time_point waitUntil{};
    timer::time_point walkFrom{};
    timer::time_point lingerUntil{};
    timer::time_point emoteAt{};
    timer::time_point sitAt{};
    timer::time_point sitUntil{};
    float             lastDistance{};
    int               lastLine{ -1 };
    timer::time_point nextSay{};
    timer::time_point nextReply{};
};

struct Said
{
    timer::time_point when{};
    xi::ZoneId        zone{};
    position_t        pos{};
    int               line{};
    uint32            charId{};
};

std::map<uint32, Routine> routines;
std::vector<Said>         recentSays;

auto hasLetter(const std::string& name) -> bool
{
    return std::ranges::any_of(name, [](const char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
}

auto wantsName(const char* line) -> bool
{
    return std::string_view(line).find("{name}") != std::string_view::npos;
}

// Same half-to-one-and-a-half spread chatter uses, so "about every N minutes" stays about that.
auto sayJitter(const uint32 minutes) -> std::chrono::seconds
{
    return std::chrono::seconds(xirand::GetRandomNumber<uint32>(minutes * 30, minutes * 90 + 1));
}

auto playerWithin(CCharEntity* PChar, const float range) -> bool
{
    bool near = false;
    PChar->loc.zone->ForEachChar(
        [&](CCharEntity* POther)
        {
            if (near || POther == PChar || isPoolChar(POther->id) || POther->inMogHouse())
            {
                return;
            }
            if (distance(PChar->loc.p, POther->loc.p) <= range)
            {
                near = true;
            }
        });
    return near;
}

// A visible named NPC, the same status and name-hidden test wander uses. No distance cap: the city is the whole walk.
auto pickStop(Bot& bot, Routine& routine) -> bool
{
    auto*                   PChar = bot.entity();
    std::vector<CNpcEntity*> npcs;
    PChar->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (PNpc->status != xi::Status::Normal || PNpc->IsNameHidden() || !hasLetter(prettyName(PNpc->getName())))
            {
                return;
            }
            if (!routine.home && PNpc->id == routine.stopId)
            {
                return;
            }
            npcs.push_back(PNpc);
        });

    // Sometimes home. Never the stop they just left, NPC or home.
    if (!routine.home && (npcs.empty() || xirand::GetRandomNumber<uint32>(100) < 25))
    {
        routine.stopId = 0;
        routine.home   = true;
        routine.name   = "home";
        routine.pos    = bot.home;
        return true;
    }
    if (npcs.empty())
    {
        return false;
    }

    auto* PNpc     = npcs[xirand::GetRandomNumber<std::size_t>(npcs.size())];
    routine.stopId = PNpc->id;
    routine.home   = false;
    routine.name   = prettyName(PNpc->getName());
    routine.pos    = PNpc->loc.p;
    return true;
}

void beginLinger(Routine& routine, const timer::time_point tick)
{
    routine.phase        = Routine::Phase::Linger;
    routine.lingerUntil  = tick + std::chrono::seconds(xirand::GetRandomNumber<uint32>(30, 121));
    routine.emoteAt      = xirand::GetRandomNumber<uint32>(100) < 50 ? tick + std::chrono::seconds(xirand::GetRandomNumber<uint32>(4, 18)) : timer::time_point{};
    routine.sitAt        = {};
    routine.sitUntil     = {};
    if (routine.home && xirand::GetRandomNumber<uint32>(100) < 40)
    {
        routine.sitAt    = tick + std::chrono::seconds(xirand::GetRandomNumber<uint32>(5, 16));
        routine.sitUntil = routine.sitAt + std::chrono::seconds(xirand::GetRandomNumber<uint32>(15, 36));
    }
}

void speak(Bot& bot, Routine& routine, CCharEntity* PChar, const timer::time_point tick)
{
    const auto minutes = settings::get<uint32>("bots.CITIZEN_SAY_MINUTES");
    if (minutes == 0 || routine.nextSay == timer::time_point{} || tick < routine.nextSay || !playerWithin(PChar, 30.0f))
    {
        return;
    }

    std::erase_if(recentSays, [&](const Said& said) { return tick - said.when >= kSayMemory; });

    std::vector<int> choices;
    for (int i = 0; i < static_cast<int>(kLines.size()); ++i)
    {
        if (i == routine.lastLine || (wantsName(kLines[i]) && (routine.home || routine.name.empty() || routine.name == "home")))
        {
            continue;
        }
        const bool heard = std::ranges::any_of(recentSays, [&](const Said& said)
                                               {
                                                   return said.line == i && said.charId != bot.charId && said.zone == PChar->getZone() &&
                                                          distance(said.pos, PChar->loc.p) <= kSayHear;
                                               });
        if (!heard)
        {
            choices.push_back(i);
        }
    }
    if (choices.empty())
    {
        routine.nextSay = tick + std::chrono::seconds(20);
        return;
    }

    const int index = choices[xirand::GetRandomNumber<std::size_t>(choices.size())];
    std::string text = kLines[index];
    if (const auto at = text.find("{name}"); at != std::string::npos)
    {
        text.replace(at, 6, routine.name);
    }
    if (!sayCanned(bot, text))
    {
        routine.nextSay = tick + std::chrono::seconds(30);
        return;
    }

    routine.lastLine = index;
    routine.nextSay  = tick + sayJitter(minutes);
    recentSays.push_back(Said{ tick, PChar->getZone(), PChar->loc.p, index, bot.charId });
}

} // namespace

auto citizenIds(const std::vector<PoolEntry>& pool) -> std::set<uint32>
{
    const auto          count = settings::get<uint32>("bots.CITIZENS");
    std::set<uint32>    ids;
    if (count == 0 || pool.empty())
    {
        return ids;
    }

    std::vector<uint32> sorted;
    sorted.reserve(pool.size());
    for (const auto& entry : pool)
    {
        sorted.push_back(entry.charId);
    }
    std::ranges::sort(sorted);

    const auto keep = std::min<std::size_t>(count, sorted.size());
    for (std::size_t i = 0; i < keep; ++i)
    {
        ids.insert(sorted[i]);
    }
    return ids;
}

void placeCitizen(const uint32 charId)
{
    const auto zone  = static_cast<xi::ZoneId>(settings::get<uint32>("bots.CITIZEN_ZONE"));
    auto*      PZone = zoneutils::GetZone(zone);
    if (PZone == nullptr)
    {
        ShowWarningFmt("bots: citizen zone {} is not on this map", zone);
        return;
    }

    std::vector<position_t> spots;
    PZone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (PNpc->status == xi::Status::Normal && !PNpc->IsNameHidden() && hasLetter(prettyName(PNpc->getName())))
            {
                spots.push_back(PNpc->loc.p);
            }
        });

    position_t around{};
    if (!spots.empty())
    {
        around = spots[xirand::GetRandomNumber<std::size_t>(spots.size())];
    }
    else
    {
        ShowWarningFmt("bots: citizen zone {} has no named NPC; using a spot near the origin", zone);
    }

    // Beside the NPC. The wider ring in loginNext is for camps.
    const auto pos = spots.empty() ? spotNear(zone, around, 0.0f, 30.0f) : spotNear(zone, around, 1.0f, 3.0f);
    placeAt(charId, Place{ zone, pos });
}

void noteCitizenLogin(const uint32 charId)
{
    const auto keep = routines[charId].nextReply;
    routines[charId] = Routine{};
    routines[charId].nextReply = keep;
}

void citizenPause(Bot& bot)
{
    if (const auto it = routines.find(bot.charId); it != routines.end())
    {
        it->second.live = false;
    }
}

void declineInvite(Bot& bot, PacketSystem& packets)
{
    auto packet                                         = makePacket<GP_CLI_COMMAND_GROUP_SOLICIT_RES>();
    packet->as<GP_CLI_COMMAND_GROUP_SOLICIT_RES>()->Res = static_cast<uint8_t>(GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Decline);
    send(bot, packets, *packet);
}

auto citizenReplyAllowed(const Bot& bot) -> bool
{
    const auto it = routines.find(bot.charId);
    return it == routines.end() || it->second.nextReply == timer::time_point{} || timer::now() >= it->second.nextReply;
}

void citizenReplyUsed(const Bot& bot)
{
    routines[bot.charId].nextReply = timer::now() + std::chrono::seconds(20);
}

auto sayAddressedToOther(CCharEntity* PSpeaker, const std::string& text) -> bool
{
    if (PSpeaker == nullptr || PSpeaker->loc.zone == nullptr)
    {
        return false;
    }

    const auto hay = lowerCase(text);
    auto       hit = [&](const std::string& name)
    {
        const auto pretty = prettyName(name);
        // Short names ("A", "To") would match ordinary sentences.
        return pretty.size() >= 3 && saysPhrase(hay, { lowerCase(pretty) });
    };

    bool found = false;
    PSpeaker->loc.zone->ForEachChar(
        [&](CCharEntity* PChar)
        {
            if (!found && PChar != PSpeaker && hit(PChar->getName()))
            {
                found = true;
            }
        });
    if (found)
    {
        return true;
    }

    PSpeaker->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (!found && PNpc->status == xi::Status::Normal && !PNpc->IsNameHidden() && hit(PNpc->getName()))
            {
                found = true;
            }
        });
    return found;
}

void citizenAct(Bot& bot, PacketSystem& packets, const timer::time_point tick)
{
    auto* PChar = bot.entity();
    if (PChar == nullptr || PChar->loc.zone == nullptr || PChar->PAI == nullptr || PChar->PAI->PathFind == nullptr)
    {
        return;
    }

    auto& routine = routines[bot.charId];
    if (!routine.live)
    {
        routine.live = true;
        const auto gap = std::chrono::seconds((bot.charId % 23) + xirand::GetRandomNumber<uint32>(13));
        if (routine.phase == Routine::Phase::Walk)
        {
            routine.held = true;
        }
        else
        {
            routine.held = false;
        }
        routine.phase     = Routine::Phase::Wait;
        routine.waitUntil = tick + gap;
        if (routine.nextSay != timer::time_point{} && routine.nextSay <= tick)
        {
            routine.nextSay = tick + std::chrono::seconds(bot.charId % 40);
        }
    }
    if (routine.nextSay == timer::time_point{})
    {
        if (const auto minutes = settings::get<uint32>("bots.CITIZEN_SAY_MINUTES"); minutes > 0)
        {
            routine.nextSay = tick + std::chrono::seconds(xirand::GetRandomNumber<uint32>(20, minutes * 60 + 1) + (bot.charId % 47));
        }
    }

    const auto city = static_cast<xi::ZoneId>(settings::get<uint32>("bots.CITIZEN_ZONE"));
    if (PChar->getZone() != city)
    {
        // Travel is not used to change zones. Outside the city they stand.
        PChar->PAI->PathFind->Clear();
        return;
    }

    speak(bot, routine, PChar, tick);

    if (routine.phase == Routine::Phase::Wait)
    {
        if (tick < routine.waitUntil)
        {
            return;
        }
        if (!routine.held && !pickStop(bot, routine))
        {
            routine.waitUntil = tick + std::chrono::seconds(10);
            return;
        }
        if (!routine.held)
        {
            ShowInfoFmt("bots: {} (citizen) walks to {}", bot.name, routine.name);
        }
        routine.held         = false;
        routine.phase        = Routine::Phase::Walk;
        routine.walkFrom     = tick;
        routine.lastDistance = 1.0e9f;
    }

    if (routine.phase == Routine::Phase::Walk)
    {
        if (bot.sitting)
        {
            setSitting(bot, packets, false);
        }

        const float dist = distance(PChar->loc.p, routine.pos);
        walkTo(PChar, routine.pos, kArrive, tick);
        if (dist <= kArrive)
        {
            PChar->PAI->PathFind->Clear();
            PChar->PAI->PathFind->LookAt(routine.pos);
            beginLinger(routine, tick);
            return;
        }

        const bool timedOut = tick - routine.walkFrom >= kWalkCap;
        const bool stuck    = tick - routine.walkFrom >= std::chrono::seconds(10) && !PChar->PAI->PathFind->IsFollowingPath() &&
                           routine.lastDistance - dist < 0.4f;
        routine.lastDistance = dist;
        if (timedOut || stuck)
        {
            routine.phase     = Routine::Phase::Wait;
            routine.held      = false;
            routine.waitUntil = tick + std::chrono::seconds(bot.charId % 5);
        }
        return;
    }

    if (tick >= routine.lingerUntil)
    {
        if (bot.sitting)
        {
            setSitting(bot, packets, false);
        }
        routine.phase     = Routine::Phase::Wait;
        routine.held      = false;
        routine.waitUntil = tick + std::chrono::seconds(bot.charId % 5);
        return;
    }

    if (!bot.sitting)
    {
        PChar->PAI->PathFind->LookAt(routine.pos);
    }
    if (bot.sitting && (routine.sitUntil == timer::time_point{} || tick >= routine.sitUntil))
    {
        setSitting(bot, packets, false);
    }
    else if (!bot.sitting && routine.sitAt != timer::time_point{} && tick >= routine.sitAt && tick < routine.sitUntil)
    {
        setSitting(bot, packets, true);
    }
    else if (!bot.sitting && routine.emoteAt != timer::time_point{} && tick >= routine.emoteAt)
    {
        static constexpr std::array<xi::Emote, 8> idle = { xi::Emote::Wave, xi::Emote::Think, xi::Emote::Doze, xi::Emote::Laugh,
                                                           xi::Emote::Yes,  xi::Emote::Sigh,  xi::Emote::Cheer, xi::Emote::Stare };
        emote(bot, packets, idle[xirand::GetRandomNumber<std::size_t>(idle.size())]);
        routine.emoteAt = {};
    }
    else if (routine.emoteAt != timer::time_point{} && tick >= routine.emoteAt)
    {
        routine.emoteAt = {};
    }
}

} // namespace bots
