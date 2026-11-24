//
// Bot behaviour, run once per logic tick for each online bot.
//

#include "bots.h"

#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/entities/npc_entity.h"
#include "map/enums/party_kind.h"
#include "map/event_info.h"
#include "map/notoriety_container.h"
#include "map/packet_system.h"
#include "map/packets/c2s/0x01a_action.h"
#include "map/packets/c2s/0x036_item_transfer.h"
#include "map/packets/c2s/0x037_item_use.h"
#include "map/battlefield.h"
#include "map/packets/c2s/0x05b_eventend.h"
#include "map/packets/c2s/0x05c_eventendxzy.h"
#include "map/packets/c2s/0x06e_group_solicit_req.h"
#include "map/packets/c2s/0x06f_group_leave.h"
#include "map/packets/c2s/0x070_group_breakup.h"
#include "map/packets/c2s/0x074_group_solicit_res.h"
#include "map/packets/c2s/0x0dc_config.h"
#include "data/enums/emote.h"
#include "data/enums/emote_mode.h"
#include "map/packets/c2s/0x05d_motion.h"
#include "map/packets/c2s/0x0e8_camp.h"
#include "map/packets/c2s/0x0ea_sit.h"
#include "map/packets/s2c/0x017_chat_std.h"
#include "map/packets/s2c/0x05a_motionmes.h"
#include "map/packets/s2c/0x11d_partyreq.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/status_effect_container.h"
#include "map/utils/charutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstring>
#include <string_view>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace bots
{

namespace
{

constexpr float kFollowDistance  = 3.0f;
constexpr float kRegroupDistance = 50.0f; // further than this from the leader, warp to them
constexpr float kCasterDistance  = 15.0f; // casters keep about this far from their target
constexpr float kArcherDistance  = 9.0f;  // close enough for full ranged damage
constexpr float kToTankDistance  = 4.0f;  // how close to the tank a bot runs when it pulls hate
constexpr float kRaiseRange      = 18.0f; // a little inside casting range
constexpr float kCampSpread      = 6.0f;  // how far from the camp spot bots stand
constexpr float kEmoteNear       = 6.0f;  // an emote at nobody in particular is answered this close
constexpr uint32 kEmoteChance    = 60;    // percent, for those
constexpr float kCampHold        = 10.0f; // a monster further out than this from camp is waited for
constexpr uint8 kHelpBelowHPP    = 40;    // unless whoever it's on is below this
constexpr uint8 kFightRestMPP    = 30;    // a WHM or BLM below this rests mid-fight
constexpr uint8 kFightResumeMPP  = 50;    // and a BLM gets back up to nuke at this
constexpr float kWanderRadius    = 6.0f;
constexpr float kThreatRange     = 40.0f; // how far away a monster attacking the party gets noticed
constexpr float kHuntRange       = 30.0f; // how far a bot on its own looks for something to fight
constexpr auto  kPreyDry         = std::chrono::seconds(60); // solo under SOLO_BELOW: nothing to hunt for this long, then move
constexpr auto  kPreyMove        = std::chrono::minutes(2);  // and not more often than this
constexpr float kPreyNear        = 5.0f;                     // close enough to the new spot to start hunting again
constexpr float kGatherRange     = 80.0f;                    // a visible HELM point at least this close, or the bot does not start
constexpr float kGatherNear      = 4.0f;                     // stand this close; the server accepts the trade out to 6
constexpr auto  kGatherGap       = std::chrono::seconds(4);  // between trades
constexpr auto  kGatherEvent     = std::chrono::seconds(5);  // a trade that opens no event ends the trip
constexpr auto  kGatherTrip      = std::chrono::minutes(5);
constexpr auto  kGatherRest      = std::chrono::minutes(10); // after a trip, before another
constexpr uint8 kGatherTrades    = 8;
constexpr uint8 kGatherFree      = 3; // stop once fewer inventory slots than this are free
constexpr auto  kHomepointDelay  = std::chrono::seconds(60);
constexpr auto  kRaiseWait       = std::chrono::minutes(3);
constexpr auto  kReturnCap       = std::chrono::minutes(10); // a wiped bot party may walk back this long, then idle
constexpr float kHeadOutNear     = 30.0f; // a trip out of town ends this close to the camp, not merely in its zone
constexpr auto  kHeadOutWait     = std::chrono::minutes(5); // one look for a camp to leave town, per leader
constexpr auto  kSettleTime      = std::chrono::seconds(2); // the leader has stopped for this long
constexpr auto  kPlayerGoneGrace = std::chrono::seconds(20);
constexpr uint32 kAttackKey      = 0x30000;
constexpr uint32 kOutOfMPKey     = 0xB0000; // the mid-fight rest log, once a fight

void sendAction(Bot& bot, PacketSystem& packets, const GP_CLI_COMMAND_ACTION_ACTIONID actionId, const CBaseEntity* PTarget, const uint32 param = 0)
{
    // The server refuses these while /healing, and while the sit animation is up ("Character in invalid animation state").
    // Resting decides when to stand (rest(), a monster on the bot); until then the action waits. A /sit stands up here,
    // and that packet clears the animation before the action is sent.
    using enum GP_CLI_COMMAND_ACTION_ACTIONID;
    const bool blockedResting = actionId == Attack || actionId == CastMagic || actionId == JobAbility || actionId == Shoot || actionId == Weaponskill;
    if (blockedResting && bot.entity() != nullptr)
    {
        if (bot.entity()->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            return;
        }
        if (bot.sitting)
        {
            setSitting(bot, packets, false);
        }
    }

    auto  packet     = makePacket<GP_CLI_COMMAND_ACTION>();
    auto* action     = packet->as<GP_CLI_COMMAND_ACTION>();
    action->UniqueNo = PTarget->id;
    action->ActIndex = PTarget->targid;
    action->ActionID = actionId;
    action->ActionBuf[0] = param;
    send(bot, packets, *packet);
}

void acceptInvite(Bot& bot, PacketSystem& packets)
{
    auto packet                                         = makePacket<GP_CLI_COMMAND_GROUP_SOLICIT_RES>();
    packet->as<GP_CLI_COMMAND_GROUP_SOLICIT_RES>()->Res = static_cast<uint8_t>(GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Accept);
    send(bot, packets, *packet);
}

// Ends a cutscene with its default choice, as a client skipping through it would.
void finishEvent(Bot& bot, PacketSystem& packets, CCharEntity* PChar)
{
    const auto* PTarget = PChar->currentEvent->targetEntity;

    auto  packet   = makePacket<GP_CLI_COMMAND_EVENTEND>();
    auto* end      = packet->as<GP_CLI_COMMAND_EVENTEND>();
    end->UniqueNo  = PTarget ? PTarget->id : PChar->id;
    end->ActIndex  = PTarget ? PTarget->targid : PChar->targid;
    end->Mode      = static_cast<uint16_t>(GP_CLI_COMMAND_EVENTEND_MODE::End);
    end->EventNum  = static_cast<uint16_t>(PChar->getZone());
    end->EventPara = static_cast<uint16_t>(PChar->currentEvent->eventId);
    send(bot, packets, *packet);
}

// Answers a join request (the newer /prcmd system) with a normal invite.
void invite(Bot& bot, PacketSystem& packets, const uint32 charId, const uint16 targId)
{
    auto  packet      = makePacket<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
    auto* request     = packet->as<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
    request->UniqueNo = charId;
    request->ActIndex = targId;
    request->Kind     = PartyKind::Party;
    send(bot, packets, *packet);
}

// A leader breaking up takes the whole party with it; leaving passes the lead on.
void leaveOrBreakUp(Bot& bot, PacketSystem& packets, const bool breakUp)
{
    auto* PChar = bot.entity();
    if (breakUp && PChar->PParty->GetLeader() == PChar)
    {
        noteEvent(bot, "broke up the party");
        auto packet                                      = makePacket<GP_CLI_COMMAND_GROUP_BREAKUP>();
        packet->as<GP_CLI_COMMAND_GROUP_BREAKUP>()->Kind = PartyKind::Party;
        send(bot, packets, *packet);
    }
    else
    {
        noteEvent(bot, "left the party");
        auto packet                                    = makePacket<GP_CLI_COMMAND_GROUP_LEAVE>();
        packet->as<GP_CLI_COMMAND_GROUP_LEAVE>()->Kind = PartyKind::Party;
        send(bot, packets, *packet);
    }
}

// The /sea "seeking party" flag. On while the bot has no party and a real player is in its zone.
void setSeeking(Bot& bot, PacketSystem& packets, const bool seeking)
{
    auto  packet      = makePacket<GP_CLI_COMMAND_CONFIG>();
    auto* config      = packet->as<GP_CLI_COMMAND_CONFIG>();
    config->InviteFlg = 1;
    config->SetFlg    = static_cast<uint8_t>(seeking ? GP_CLI_COMMAND_CONFIG_SETFLG::On : GP_CLI_COMMAND_CONFIG_SETFLG::Off);
    send(bot, packets, *packet);
    bot.seeking = seeking;
}

void setResting(Bot& bot, PacketSystem& packets, const bool resting)
{
    // /heal is refused while engaged. Standing back up is still sent.
    if (resting)
    {
        auto* PChar = bot.entity();
        if (PChar != nullptr && PChar->PAI != nullptr && (PChar->PAI->IsEngaged() || PChar->GetBattleTarget() != nullptr))
        {
            return;
        }
    }

    auto packet                                = makePacket<GP_CLI_COMMAND_CAMP>();
    packet->as<GP_CLI_COMMAND_CAMP>()->Mode    = static_cast<uint32_t>(resting ? GP_CLI_COMMAND_CAMP_MODE::On : GP_CLI_COMMAND_CAMP_MODE::Off);
    send(bot, packets, *packet);
}

auto hasRealPlayer(const CParty* PParty, const std::set<uint32>& botIds) -> bool
{
    return std::ranges::any_of(PParty->members,
                               [&](const CBattleEntity* PMember)
                               {
                                   return PMember->objtype == TYPE_PC && !botIds.contains(PMember->id);
                               });
}

auto leaderOf(CCharEntity* PChar) -> CCharEntity*
{
    if (PChar->PParty == nullptr)
    {
        return nullptr;
    }

    auto* PLeader = dynamic_cast<CCharEntity*>(PChar->PParty->GetLeader());
    return PLeader == PChar ? nullptr : PLeader;
}

// The party member a bot was told to follow or assist, or the leader. A member zoning drops off the list for a moment, so
// the choice is only forgotten once the bot is out of the party.
auto partner(CCharEntity* PChar, uint32& id, CCharEntity* PLeader) -> CCharEntity*
{
    if (PChar->PParty == nullptr)
    {
        id = 0;
    }
    if (id == 0)
    {
        return PLeader;
    }
    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->id == id)
        {
            return dynamic_cast<CCharEntity*>(PMember);
        }
    }
    return PLeader;
}

auto inParty(const CCharEntity* PChar, const CBattleEntity* PEntity) -> bool
{
    if (PEntity == PChar)
    {
        return true;
    }
    return PChar->PParty != nullptr && std::ranges::contains(withTrusts(PChar), PEntity);
}

// The leader's target, or else the nearest mob attacking anyone in the party.
auto combatTarget(Bot& bot, CCharEntity* PChar, CCharEntity* PLeader) -> CBattleEntity*
{
    // Asked to leave this one alone.
    const auto valid = [&](CBattleEntity* PMob)
    {
        return PMob != nullptr && PMob->objtype == TYPE_MOB && !PMob->isDead() && PMob->getZone() == PChar->getZone() && PMob->id != bot.holdTarget;
    };

    // Asked to attack this one; a monster's target index is the low 12 bits of its id.
    if (bot.focusTarget != 0)
    {
        auto* PFocus = dynamic_cast<CBattleEntity*>(PChar->GetEntity(static_cast<uint16>(bot.focusTarget & 0xFFF), TYPE_MOB));
        if (PFocus != nullptr && PFocus->id == bot.focusTarget && valid(PFocus))
        {
            return PFocus;
        }
        bot.focusTarget = 0;
    }

    if (PLeader != nullptr && PLeader->PAI->IsEngaged() && valid(PLeader->GetBattleTarget()))
    {
        return PLeader->GetBattleTarget();
    }

    // A monster that aggros engages before anyone is on its hate list, so look for whatever is attacking the party.
    // A sleeping link waits until nothing awake is left.
    CBattleEntity* PNearest = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (!valid(PMob) || !PMob->PAI->IsEngaged() || distance(PChar->loc.p, PMob->loc.p) >= kThreatRange || !inParty(PChar, PMob->GetBattleTarget()) ||
                beingPulled(PChar, PMob))
            {
                return;
            }
            const bool better = PNearest == nullptr || (asleep(PNearest) && !asleep(PMob)) ||
                                (asleep(PNearest) == asleep(PMob) && distance(PChar->loc.p, PMob->loc.p) < distance(PChar->loc.p, PNearest->loc.p));
            if (better)
            {
                PNearest = PMob;
            }
        });
    return PNearest;
}

auto navMeshOf(const CCharEntity* PChar) -> NavMesh*
{
    return PChar->loc.zone != nullptr ? PChar->loc.zone->navMesh() : nullptr;
}

// A path to pos, or else to the nearest point on the navmesh to it: a spot worked out from angles can be inside a wall.
auto pathNear(CCharEntity* PChar, const position_t& pos, const float within) -> bool
{
    auto&      path  = *PChar->PAI->PathFind;
    const auto start = [&](const position_t& to) { return within > 0.0f ? path.PathAround(to, within, PATHFLAG_RUN) : path.PathTo(to, PATHFLAG_RUN); };
    if (start(pos))
    {
        return true;
    }
    const auto* mesh = navMeshOf(PChar);
    const auto  near = mesh != nullptr ? mesh->findClosestValidPoint(pos) : std::nullopt;
    return near && distance(*near, pos) > 0.5f && start(*near);
}

// No path found: a bot on the navmesh steps along its surface, which stops at walls and ledges; one off it (in a wall,
// up a cliff) goes back onto it. Only a zone without a navmesh is walked in a straight line.
void stepSafely(CCharEntity* PChar, const position_t& pos)
{
    auto&       path = *PChar->PAI->PathFind;
    const auto* mesh = navMeshOf(PChar);
    if (position_t reached{}; mesh != nullptr && mesh->moveAlongSurface(PChar->loc.p, pos, reached))
    {
        if (distance(reached, PChar->loc.p) > 0.1f)
        {
            path.StepTo(reached, true);
        }
        return;
    }
    if (const auto back = mesh != nullptr ? mesh->findClosestValidPoint(PChar->loc.p) : std::nullopt)
    {
        path.WarpTo(*back, 0.0f);
        return;
    }
    path.StepTo(pos, true);
}

void moveToward(CCharEntity* PChar, const position_t& pos, const float within, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (distance(PChar->loc.p, pos) > within)
    {
        if ((!path.IsFollowingPath() || distance(path.GetDestination(), pos) > within) && !pathNear(PChar, pos, within * 0.75f))
        {
            stepSafely(PChar, pos);
        }
    }
    else if (path.IsFollowingPath())
    {
        path.Clear();
    }

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
    }
}

// Walks to an exact spot, repathing only when the spot has moved on (the target turned or moved).
void moveToSpot(CCharEntity* PChar, const position_t& spot, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (distance(PChar->loc.p, spot) > 0.8f)
    {
        if ((!path.IsFollowingPath() || distance(path.GetDestination(), spot) > 1.0f) && !pathNear(PChar, spot, 0.0f))
        {
            stepSafely(PChar, spot);
        }
    }
    else if (path.IsFollowingPath())
    {
        path.Clear();
    }

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
    }
}

void follow(CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick)
{
    if (PLeader->loc.zone == nullptr || PLeader->status != xi::Status::Normal || PLeader->inMogHouse())
    {
        return;
    }

    if (PLeader->getZone() != PChar->getZone())
    {
        travel(PChar, PLeader->getZone(), PLeader->loc.p);
        return;
    }

    if (distance(PChar->loc.p, PLeader->loc.p) > kRegroupDistance)
    {
        // Behind the leader, on the navmesh: the spot behind them can be inside a wall.
        PChar->PAI->PathFind->WarpTo(PLeader->loc.p, kFollowDistance);
        const auto* mesh = navMeshOf(PChar);
        if (const auto onMesh = mesh != nullptr ? mesh->findClosestValidPoint(PChar->loc.p) : std::nullopt; onMesh && distance(*onMesh, PChar->loc.p) > 1.0f)
        {
            PChar->PAI->PathFind->WarpTo(*onMesh, 0.0f);
        }
        return;
    }

    moveToward(PChar, PLeader->loc.p, kFollowDistance, tick);
}

// A fitting emote back for a player's, or none.
auto emoteReply(const xi::Emote theirs) -> std::optional<xi::Emote>
{
    using E = xi::Emote;
    static const std::map<E, std::vector<E>> replies = {
        { E::Wave, { E::Wave } },                     { E::Bow, { E::Bow } },
        { E::Salute, { E::Salute } },                 { E::Goodbye, { E::Goodbye, E::Wave } },
        { E::Welcome, { E::Bow, E::Smile } },         { E::Cheer, { E::Cheer, E::Hurray } },
        { E::Joy, { E::Joy, E::Cheer } },             { E::Hurray, { E::Hurray, E::Cheer } },
        { E::Clap, { E::Bow, E::Smile } },            { E::Praise, { E::Blush, E::Bow } },
        { E::Smile, { E::Smile } },                   { E::Grin, { E::Grin, E::Laugh } },
        { E::Laugh, { E::Laugh, E::Grin } },          { E::Cry, { E::Comfort } },
        { E::Sulk, { E::Comfort } },                  { E::Sigh, { E::Comfort, E::Sigh } },
        { E::Comfort, { E::Smile, E::Bow } },         { E::Panic, { E::Comfort } },
        { E::Poke, { E::Huh, E::Stare, E::Doubt } },  { E::Slap, { E::Angry, E::Fume, E::Cry } },
        { E::Stare, { E::Stare, E::Huh } },           { E::Blush, { E::Smile } },
        { E::Angry, { E::Panic, E::Doubt, E::Sulk } }, { E::Fume, { E::Comfort, E::Doubt } },
        { E::Point, { E::Huh } },                     { E::Kneel, { E::Bow } },
        { E::Dance, { E::Dance, E::Clap, E::Cheer } }, { E::Doze, { E::Poke } },
        { E::Surprised, { E::Surprised, E::Huh } },   { E::Amazed, { E::Surprised, E::Huh } },
        { E::Shocked, { E::Surprised, E::Huh } },     { E::Doubt, { E::Think, E::Doubt } },
        { E::Think, { E::Think } },                   { E::Huh, { E::Huh, E::Think } },
        { E::Yes, { E::Yes } },                       { E::No, { E::Doubt, E::No } },
        { E::Psych, { E::Huh } },                     { E::Disgusted, { E::Doubt, E::Sigh } },
    };
    const auto it = replies.find(theirs);
    if (it == replies.end())
    {
        return std::nullopt;
    }
    return it->second[xirand::GetRandomNumber<std::size_t>(it->second.size())];
}

// In town a bot strolls from one NPC to another (shops, the auction house, guards) and hangs about there,
// sometimes sitting or using an emote. Elsewhere it roams a little around where it logged in.
constexpr uint16 kEntryEvent = 32000; // a battlefield entrance's menu
constexpr uint8  kEntryTries = 3;
constexpr uint32 kEntryKey   = 0xA0000;

// The leader is already inside: the entry menu gets their battlefield and arena, and the bot comes out of it beside them,
// where a player's client would send the arena's own spot.
auto answerEntry(Bot& bot, PacketSystem& packets, CCharEntity* PChar) -> bool
{
    auto*      PLeader = leaderOf(PChar);
    const auto options = PChar->GetLocalVar("[BCNM]MenuOptions");
    if (PChar->currentEvent->eventId != kEntryEvent || bot.entryAnswered || PLeader == nullptr || PLeader->PBattlefield == nullptr || options == 0)
    {
        return false;
    }

    const auto* PTarget = PChar->currentEvent->targetEntity;
    auto        packet  = makePacket<GP_CLI_COMMAND_EVENTENDXZY>();
    auto*       answer  = packet->as<GP_CLI_COMMAND_EVENTENDXZY>();
    answer->x           = PLeader->loc.p.x;
    answer->y           = PLeader->loc.p.y;
    answer->z           = PLeader->loc.p.z;
    answer->dir         = static_cast<int8_t>(PLeader->loc.p.rotation);
    answer->UniqueNo    = PTarget ? PTarget->id : PChar->id;
    answer->ActIndex    = PTarget ? PTarget->targid : PChar->targid;
    answer->EndPara     = (static_cast<uint32>(std::countr_zero(options)) << 4) | PLeader->PBattlefield->GetArea();
    answer->EventNum    = static_cast<uint16_t>(PChar->getZone());
    answer->EventPara   = kEntryEvent;
    answer->Mode        = 1;
    send(bot, packets, *packet);
    bot.entryAnswered = true;
    return true;
}

// Into the leader's battlefield through the entrance they used, once the game has cleared the bot for it.
// Left outside (not cleared, or it's full) the bot waits where it is until the leader comes back out.
auto followIn(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick) -> bool
{
    if (PLeader == nullptr)
    {
        return false;
    }
    if (PLeader->isInEvent() && PLeader->currentEvent->eventId == kEntryEvent && PLeader->currentEvent->targetEntity != nullptr)
    {
        bot.entranceId     = PLeader->currentEvent->targetEntity->id;
        bot.entranceTargid = PLeader->currentEvent->targetEntity->targid;
    }
    if (PLeader->PBattlefield == nullptr || PLeader->getZone() != PChar->getZone())
    {
        bot.entryTries = 0;
        return false;
    }
    if (PChar->PBattlefield != nullptr)
    {
        return false;
    }

    auto* PDoor = bot.entranceId != 0 ? PChar->GetEntity(bot.entranceTargid, TYPE_NPC) : nullptr;
    if (PDoor == nullptr || PDoor->id != bot.entranceId || bot.entryTries >= kEntryTries ||
        !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Battlefield))
    {
        PChar->PAI->PathFind->Clear();
        return true;
    }
    if (distance(PChar->loc.p, PDoor->loc.p) > 4.0f)
    {
        moveToward(PChar, PDoor->loc.p, 3.0f, tick);
        return true;
    }
    if (throttle(bot, kEntryKey, PDoor->id, std::chrono::seconds(6), tick))
    {
        ++bot.entryTries;
        PChar->PAI->PathFind->Clear();
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Talk, PDoor);
    }
    return true;
}

void wander(Bot& bot, PacketSystem& packets, CCharEntity* PChar, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
        return;
    }

    if (tick < bot.nextWander)
    {
        return;
    }

    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
    if (!town)
    {
        bot.nextWander = tick + std::chrono::seconds(xirand::GetRandomNumber(20, 90));
        if (path.RoamAround(bot.home, kWanderRadius, 1, 3))
        {
            path.FollowPath(tick);
        }
        return;
    }

    bot.nextWander = tick + std::chrono::seconds(xirand::GetRandomNumber(45, 180));

    // Lingering: sit down or emote now and then before moving on.
    if (const auto roll = xirand::GetRandomNumber<uint32>(100); roll < 35)
    {
        if (roll < 15 && !bot.sitting)
        {
            setSitting(bot, packets, true);
        }
        else if (!bot.sitting)
        {
            static constexpr std::array<xi::Emote, 8> idle = { xi::Emote::Wave, xi::Emote::Think, xi::Emote::Doze, xi::Emote::Laugh,
                                                              xi::Emote::Yes,  xi::Emote::Sigh,  xi::Emote::Cheer, xi::Emote::Stare };
            emote(bot, packets, idle[xirand::GetRandomNumber<std::size_t>(idle.size())]);
        }
        return;
    }

    std::vector<position_t> spots;
    // Same names citizens skip, so a stroll doesn't stop on a furnace, a door, or a home point.
    PChar->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (PNpc->status == xi::Status::Normal && !PNpc->IsNameHidden() && !skippedNpc(prettyName(PNpc->getName())) &&
                distance(PChar->loc.p, PNpc->loc.p) < 100.0f)
            {
                spots.push_back(PNpc->loc.p);
            }
        });
    if (spots.empty())
    {
        return;
    }

    if (bot.sitting)
    {
        setSitting(bot, packets, false);
    }
    if (path.PathAround(spots[xirand::GetRandomNumber<std::size_t>(spots.size())], 2.5f))
    {
        path.FollowPath(tick);
    }
}

auto partyCanRaise(CCharEntity* PChar) -> bool
{
    return PChar->PParty != nullptr && std::ranges::any_of(PChar->PParty->members,
                                                           [&](CBattleEntity* PMember)
                                                           {
                                                               return PMember != PChar && !PMember->isDead() &&
                                                                      (spell::CanUseSpell(PMember, SpellID::Raise) || spell::CanUseSpell(PMember, SpellID::Raise_II));
                                                           });
}

// A group that is together and healthy enough for the next pull.
auto partyReady(CCharEntity* PLeader) -> bool
{
    return std::ranges::all_of(withTrusts(PLeader),
                               [&](const CBattleEntity* PMember)
                               {
                                   return PMember->getZone() == PLeader->getZone() && !PMember->isDead() && PMember->GetHPP() >= 70 &&
                                          distance(PMember->loc.p, PLeader->loc.p) < 15.0f;
                               });
}

// A bot on its own picks an unclaimed monster near its level and goes for it, like a player soloing.
// A mob a real player is fighting or has claimed, or one standing within PLAYER_LEEWAY of a real player
// outside this bot's party, is left alone. Something that attacks the bot is still fought (combatTarget).
auto huntTarget(Bot& bot, CCharEntity* PChar, const timer::time_point tick) -> CBattleEntity*
{
    if (tick < bot.nextHunt)
    {
        return nullptr;
    }
    bot.nextHunt = tick + std::chrono::seconds(xirand::GetRandomNumber(8, 25));

    const auto type = static_cast<uint16>(PChar->loc.zone->GetTypeMask());
    if ((type & (static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon))) == 0 ||
        PChar->GetHPP() < 80 || (PChar->GetMaxMP() > 0 && PChar->GetMPP() < 50))
    {
        return nullptr;
    }

    const int   level    = PChar->GetMLevel();
    const int   toughest = PChar->PParty != nullptr && withTrusts(PChar).size() >= 3 ? 3 : 1;
    CMobEntity* PBest    = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (PMob->isDead() || !PMob->PAI->IsSpawned() || PMob->PAI->IsEngaged() || PMob->m_OwnerID.isSet() || PMob->m_Type != xi::MobType::Normal ||
                leaveForPlayers(PChar, PMob))
            {
                return;
            }

            // Easy prey up to an even match.
            const int gap = PMob->GetMLevel() - level;
            if (gap < -6 || gap > toughest || distance(PChar->loc.p, PMob->loc.p) > kHuntRange)
            {
                return;
            }

            if (PBest == nullptr || distance(PChar->loc.p, PMob->loc.p) < distance(PChar->loc.p, PBest->loc.p))
            {
                PBest = PMob;
            }
        });
    return PBest;
}

void fight(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CBattleEntity* PTarget, const timer::time_point tick)
{
    if (!PChar->PAI->IsEngaged())
    {
        // A refused engage (someone else's claim) shouldn't be retried every tick.
        if (throttle(bot, kAttackKey, PTarget->id, std::chrono::seconds(3), tick))
        {
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Attack, PTarget);
        }
    }
    else if (PChar->GetBattleTarget() != PTarget)
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::ChangeTarget, PTarget);
    }

    const float reach = PChar->GetMeleeRange(PTarget);
    if (const auto spot = combatSpot(PChar, PTarget, reach * 0.7f))
    {
        moveToSpot(PChar, *spot, tick);
    }
    else
    {
        moveToward(PChar, PTarget->loc.p, reach * 0.8f, tick);
    }

    if (!PChar->PAI->PathFind->IsFollowingPath())
    {
        PChar->PAI->PathFind->LookAt(PTarget->loc.p);
    }
}

// Casters and archers fan out behind the target at range; one that has hate holds its ground and takes it.
// Holds its ground while the distance still works: re-placing each time the monster turns kept archers and casters
// running, and a shot or cast started on the move is interrupted.
void keepRange(CCharEntity* PChar, CBattleEntity* PTarget, const float range, const timer::time_point tick)
{
    auto&      path   = *PChar->PAI->PathFind;
    const auto inBand = [&](const position_t& at)
    {
        const float gap = distance(at, PTarget->loc.p);
        return gap >= range * 0.6f && gap <= range + 3.0f;
    };
    if (path.IsFollowingPath() ? inBand(path.GetDestination()) : inBand(PChar->loc.p))
    {
        if (path.IsFollowingPath())
        {
            path.FollowPath(tick);
        }
        else
        {
            path.LookAt(PTarget->loc.p);
        }
        return;
    }

    if (const auto spot = combatSpot(PChar, PTarget, range))
    {
        moveToSpot(PChar, *spot, tick);
    }
    else
    {
        PChar->PAI->PathFind->Clear();
    }

    if (!PChar->PAI->PathFind->IsFollowingPath())
    {
        PChar->PAI->PathFind->LookAt(PTarget->loc.p);
    }
}

// /heal when low and the leader has stopped, or whenever the leader sits down to /heal; stand up when full or the leader moves on.
// Casters take every spare moment for a tick, missing anything at all; an avatar out would cancel it.
// Tanks and damage dealers low on HP rest if it costs no TP (Signet working here, or none to lose) or nobody can cure them;
// otherwise they ask the healer for a cure. Low on MP, they rest once their TP is gone. And whenever told to.
auto rest(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick) -> bool
{
    const bool resting = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const bool full    = PChar->GetHPP() == 100 && (PChar->GetMaxMP() == 0 || PChar->GetMPP() == 100);
    const bool caster  = (isCaster(PChar->GetMJob()) || PChar->GetMJob() == xi::Job::RDM) && PChar->PPet == nullptr;
    const bool lowHP   = PChar->GetHPP() < kRestHPP;
    const bool lowMP   = PChar->GetMaxMP() > 0 && PChar->GetMPP() < kRestMPP;
    const bool keepsTP = signetWorks(PChar) || PChar->health.tp == 0;
    const bool healer  = PChar->PParty != nullptr && std::ranges::any_of(withTrusts(PChar),
                                                                        [&](const CBattleEntity* PMember)
                                                                        {
                                                                            const auto job = PMember->GetMJob();
                                                                            return PMember != PChar && !PMember->isDead() &&
                                                                                   (job == xi::Job::WHM || job == xi::Job::RDM || job == xi::Job::SCH);
                                                                        });
    const bool low     = bot.restNow || (caster ? !full : (lowHP && (keepsTP || !healer)) || (lowMP && (PChar->health.tp == 0 || !healer)));
    if (full)
    {
        bot.restNow = false;
    }
    if (!caster && lowHP && healer && !keepsTP && !resting)
    {
        askForCure(bot, tick);
    }

    // Sitting down waits for the leader to stop; once down, the bot stays down until the leader actually walks off.
    bool settled       = true;
    bool near          = true;
    bool leaderResting = false;
    if (PLeader != nullptr)
    {
        leaderResting = PLeader->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
        const auto camp = campSpot(PChar);
        near            = camp ? distance(PChar->loc.p, *camp) <= kCampSpread
                               : PLeader->getZone() == PChar->getZone() && distance(PChar->loc.p, PLeader->loc.p) <= kFollowDistance + 2.0f;
        settled         = near && (camp || leaderResting || tick - bot.leaderMovedAt >= kSettleTime);
    }

    // Healing comes before resting: spells can't be cast sitting down.
    const bool heal = healingWanted(PChar);
    if (resting)
    {
        if (full || !near || heal)
        {
            setResting(bot, packets, false);
            return false;
        }
        return true;
    }

    // Losing HP stands a resting character up, so a bot draining (Sublimation charging, poison) doesn't kneel into it.
    const bool draining = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SublimationActivated) || PChar->getMod(xi::Mod::REGEN_DOWN) > 0;
    if ((low || (caster && leaderResting && !full)) && !heal && settled && !draining && canAct(PChar))
    {
        // Still engaged, or still holding a target: /heal would be refused. Skip this tick.
        if (PChar->PAI->IsEngaged() || PChar->GetBattleTarget() != nullptr)
        {
            return false;
        }
        PChar->PAI->PathFind->Clear();
        setResting(bot, packets, true);
        return true;
    }
    return false;
}

// A White or Black Mage out of MP sits down mid-fight. It gets up to heal (WHM), once there's MP to nuke again (BLM),
// to sleep an add it has the MP for, or when something starts hitting it.
auto restInFight(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    const auto job = PChar->GetMJob();
    if (job != xi::Job::WHM && job != xi::Job::BLM)
    {
        return false;
    }

    const bool resting  = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const bool attacked = PTarget->GetBattleTarget() == PChar || chasedOffTank(PChar);
    const bool wanted   = (job == xi::Job::WHM ? healingWanted(PChar) : PChar->GetMPP() >= kFightResumeMPP) || addNeedsSleep(PChar, PTarget);

    if (resting)
    {
        if (attacked || wanted || PChar->GetMPP() == 100)
        {
            setResting(bot, packets, false);
            return false;
        }
        return true;
    }

    if (PChar->GetMPP() < kFightRestMPP && !attacked && !wanted && canAct(PChar))
    {
        // The server refuses /heal while engaged. Disengage first; camp once that has landed.
        if (PChar->PAI->IsEngaged() || PChar->GetBattleTarget() != nullptr)
        {
            if (PChar->PAI->IsEngaged())
            {
                disengage(bot, packets);
            }
            if (PChar->PAI->IsEngaged() || PChar->GetBattleTarget() != nullptr)
            {
                return true;
            }
        }

        if (throttle(bot, kOutOfMPKey, PTarget->id, std::chrono::minutes(5), tick))
        {
            ShowInfoFmt("bots: {} ({} {}) sits down at {}% MP fighting {} ({}% HP)", PChar->getName(), job == xi::Job::WHM ? "WHM" : "BLM", PChar->GetMLevel(),
                        PChar->GetMPP(), PTarget->getName(), PTarget->GetHPP());
        }
        PChar->PAI->PathFind->Clear();
        setResting(bot, packets, true);
        return true;
    }
    return false;
}

// No real player in the party. withPlayer still covers the grace after one leaves, so that party does not walk back.
auto partyIsBots(const Bot& bot, const CCharEntity* PChar, const std::set<uint32>& botIds) -> bool
{
    return PChar->PParty != nullptr && !bot.withPlayer && !hasRealPlayer(PChar->PParty, botIds);
}

auto campZoneText(const CCharEntity* PChar, const xi::ZoneId zone) -> std::string
{
    if (PChar->loc.zone != nullptr && PChar->getZone() == zone)
    {
        return prettyName(PChar->loc.zone->getName());
    }
    if (auto* PZone = zoneutils::GetZone(zone)) // CZone::getName is not const
    {
        return prettyName(PZone->getName());
    }
    return "somewhere";
}

// Copies the Pull camp while a bot-only party is dead. A raise before the homepoint drops it again.
void rememberCamp(Bot& bot, const CCharEntity* PChar, const std::set<uint32>& botIds)
{
    if (bot.returnUntil != timer::time_point{} || bot.returnCamp || !partyIsBots(bot, PChar, botIds))
    {
        return;
    }
    if (const auto camp = campPlace(PChar))
    {
        bot.returnCamp = camp;
    }
}

// Homepoint of a bot-only party that had a camp: walk back, for at most kReturnCap. A second homepoint does not extend it.
void startReturn(Bot& bot, CCharEntity* PChar, const std::set<uint32>& botIds, const timer::time_point tick)
{
    if (bot.returnUntil != timer::time_point{})
    {
        return;
    }
    if (!partyIsBots(bot, PChar, botIds))
    {
        bot.returnCamp.reset();
        return;
    }
    if (!bot.returnCamp)
    {
        bot.returnCamp = campPlace(PChar);
    }
    if (!bot.returnCamp)
    {
        return;
    }
    bot.returnUntil = tick + kReturnCap;
    const auto where = campZoneText(PChar, bot.returnCamp->zone);
    noteEvent(bot, fmt::format("wiped at {}, heading back", where));
    ShowInfoFmt("bots: {} is heading back to {}", PChar->getName(), where);
}

// A camp a bot-only party in town is walking to. Members copy it so the return path takes them along.
struct Outing
{
    Place             place{};
    timer::time_point until{};
};

std::map<uint32, Outing> outings;

auto highestLevel(const CCharEntity* PChar) -> int
{
    int highest = 1;
    for (const auto* PMember : PChar->PParty->members)
    {
        highest = std::max(highest, static_cast<int>(PMember->GetMLevel()));
    }
    return highest;
}

// Same zones autoCamp refuses: not an outdoor field or a dungeon, or a city.
auto notHunting(const CCharEntity* PChar) -> bool
{
    if (PChar->loc.zone == nullptr)
    {
        return false;
    }
    const auto type    = static_cast<uint16>(PChar->loc.zone->GetTypeMask());
    const bool hunting = (type & (static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon))) != 0 &&
                         (type & static_cast<uint16>(xi::ZoneType::City)) == 0;
    return !hunting;
}

void endHeadOut(Bot& bot, CCharEntity* PChar)
{
    if (!bot.headingOut)
    {
        return;
    }
    const auto* PLead = PChar->PParty != nullptr ? PChar->PParty->GetLeader() : nullptr;
    if (PLead != nullptr && PLead != PChar)
    {
        if (const auto it = outings.find(PLead->id); it != outings.end())
        {
            bot.nextHeadOut = it->second.until;
        }
    }
    outings.erase(PChar->id);
    bot.headingOut = false;
}

// A member of a party already walking out copies the leader's camp onto the return path.
void joinOuting(Bot& bot, CCharEntity* PChar, const std::set<uint32>& botIds, const timer::time_point tick)
{
    if (bot.returnUntil != timer::time_point{} || bot.nextHeadOut > tick || !partyIsBots(bot, PChar, botIds) || PChar->PParty->GetLeader() == PChar)
    {
        return;
    }
    const auto it = outings.find(PChar->PParty->GetLeader()->id);
    if (it == outings.end() || tick >= it->second.until)
    {
        return;
    }
    bot.returnCamp  = it->second.place;
    bot.returnUntil = it->second.until;
    bot.headingOut  = true;
}

// Leader of a bot-only party standing where autoCamp will not hunt: pick a camp and walk there. At most once per 5 minutes.
void startHeadOut(Bot& bot, CCharEntity* PChar, const std::set<uint32>& botIds, const timer::time_point tick)
{
    if (PChar->PParty == nullptr || PChar->PParty->GetLeader() != PChar || bot.returnUntil != timer::time_point{} || tick < bot.nextHeadOut ||
        !partyIsBots(bot, PChar, botIds) || !notHunting(PChar) || hasPull(PChar))
    {
        return;
    }

    const int  level = highestLevel(PChar);
    bot.nextHeadOut  = tick + kHeadOutWait;
    const auto camps = findCamps(static_cast<uint8>(level));
    if (camps.empty())
    {
        ShowInfoFmt("bots: {}'s party has no camp to head to (level {})", PChar->getName(), level);
        return;
    }

    const auto& camp   = camps[xirand::GetRandomNumber<std::size_t>(camps.size())];
    bot.returnCamp     = camp;
    bot.returnUntil    = tick + kReturnCap;
    bot.headingOut     = true;
    outings[PChar->id] = Outing{ camp, bot.returnUntil };
    ShowInfoFmt("bots: {}'s party heads out to {} (level {})", PChar->getName(), campZoneText(PChar, camp.zone), level);
}

auto campsIn(const std::vector<Place>& camps, const xi::ZoneId zone) -> std::vector<Place>
{
    std::vector<Place> spots;
    for (const auto& camp : camps)
    {
        if (camp.zone == zone)
        {
            spots.push_back(camp);
        }
    }
    return spots;
}

// Another camp, preferably past hunt range so the walk is worth it. Same ring login uses.
auto pickPreySpot(const std::vector<Place>& spots, const position_t& from, const bool elsewhere) -> std::optional<Place>
{
    if (spots.empty())
    {
        return std::nullopt;
    }

    std::vector<Place> far;
    if (!elsewhere)
    {
        for (const auto& spot : spots)
        {
            if (distance(spot.pos, from) > kHuntRange)
            {
                far.push_back(spot);
            }
        }
    }

    const auto& pool = far.empty() ? spots : far;
    const auto& camp = pool[xirand::GetRandomNumber<std::size_t>(pool.size())];
    return Place{ camp.zone, spotNear(camp.zone, camp.pos, 5.0f, 25.0f) };
}

// Solo under SOLO_BELOW, in a starting area, with nothing in hunt range: walk to another camp in the zone.
// No camp that fits means the zone is outgrown: travel to the nation's other starting zone. At most one new
// spot per 2 minutes. A bot in a real player's party never reaches this.
auto seekPrey(Bot& bot, CCharEntity* PChar, const timer::time_point tick) -> bool
{
    const auto soloBelow = settings::get<uint32>("bots.SOLO_BELOW");
    if (bot.citizen || bot.withPlayer || PChar->PParty != nullptr || soloBelow == 0 || static_cast<uint32>(PChar->GetMLevel()) >= soloBelow ||
        !inStartingArea(PChar->getZone()))
    {
        bot.preySpot.reset();
        bot.preyDrySince = {};
        return false;
    }

    if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) || PChar->PAI->IsEngaged())
    {
        return false;
    }

    if (bot.preySpot && tick < bot.nextPreyMove)
    {
        if (PChar->getZone() != bot.preySpot->zone)
        {
            travel(PChar, bot.preySpot->zone, bot.preySpot->pos);
            bot.preySpot.reset();
            return true;
        }
        if (distance(PChar->loc.p, bot.preySpot->pos) > kPreyNear)
        {
            walkTo(PChar, bot.preySpot->pos, kPreyNear, tick);
            return true;
        }
        bot.preySpot.reset();
        bot.preyDrySince = tick;
        bot.home         = PChar->loc.p;
        return false;
    }

    if (bot.preyDrySince == timer::time_point{})
    {
        bot.preyDrySince = tick;
        return false;
    }
    if (tick - bot.preyDrySince < kPreyDry || tick < bot.nextPreyMove)
    {
        return false;
    }

    const auto level     = static_cast<uint8>(PChar->GetMLevel());
    const auto camps     = findCamps(level);
    auto       spots     = campsIn(camps, PChar->getZone());
    bool       elsewhere = false;
    if (spots.empty())
    {
        const auto nation = static_cast<uint8>(static_cast<std::size_t>(PChar->profile.nation));
        if (const auto other = otherStartingZone(nation, PChar->getZone()))
        {
            spots     = campsIn(camps, *other);
            elsewhere = true;
        }
    }

    const auto picked = pickPreySpot(spots, PChar->loc.p, elsewhere);
    if (!picked)
    {
        // Nowhere that fits. Look again later, not every tick (findCamps walks every zone).
        bot.nextPreyMove = tick + kPreyMove;
        return false;
    }

    bot.nextPreyMove = tick + kPreyMove;
    bot.preyDrySince = tick;
    ShowInfoFmt("bots: {} moves on to find prey (level {})", bot.name, static_cast<int>(level));

    if (picked->zone != PChar->getZone())
    {
        travel(PChar, picked->zone, picked->pos);
        bot.preyDrySince = {};
        return true;
    }

    bot.preySpot = picked;
    bot.home     = picked->pos;
    walkTo(PChar, picked->pos, kPreyNear, tick);
    return true;
}

// Harvesting, logging, mining, excavation. The name is what prettyName makes of the NPC ("Harvesting_Point").
struct Helm
{
    const char* name;
    uint16      tool;
    const char* toolName;
};

constexpr Helm kHelm[] = {
    { "Harvesting Point", 1020, "sickle" },
    { "Logging Point", 1021, "hatchet" },
    { "Mining Point", 605, "pickaxe" },
    { "Excavation Point", 605, "pickaxe" },
};

auto toolCount(CCharEntity* PChar, const uint16 itemId) -> int32
{
    return static_cast<int32>(charutils::getItemCount(PChar, itemId));
}

void stopGather(Bot& bot, const timer::time_point tick, const char* reason)
{
    if (!bot.gather)
    {
        return;
    }
    ShowInfoFmt("bots: {} stops gathering: {}", bot.name, reason);
    bot.gather.reset();
    bot.nextGather = tick + kGatherRest;
}

// The yield is already in the inventory. End the cutscene the way any other one ends, then count it.
void finishGather(Bot& bot, PacketSystem& packets, CCharEntity* PChar, const timer::time_point tick)
{
    auto& trip     = *bot.gather;
    trip.waiting   = false;
    trip.tradedAt  = {};
    finishEvent(bot, packets, PChar);

    const auto left = toolCount(PChar, trip.tool);
    if (trip.tools - left == 1)
    {
        ShowInfoFmt("bots: {}'s {} broke", bot.name, trip.toolName);
    }
    ++trip.done;
    ShowInfoFmt("bots: {} gathered at {} ({} of 8)", bot.name, trip.pointName, static_cast<int>(trip.done));

    if (trip.done >= kGatherTrades)
    {
        stopGather(bot, tick, "8 trades");
        return;
    }
    if (tick >= trip.until)
    {
        stopGather(bot, tick, "5 minutes");
        return;
    }
    if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() < kGatherFree)
    {
        stopGather(bot, tick, "fewer than 3 free slots");
        return;
    }
    if (left <= 0)
    {
        stopGather(bot, tick, "no tool left");
        return;
    }
    trip.tools     = left;
    trip.nextTrade = tick + kGatherGap;
}

auto helmOf(CNpcEntity* PNpc) -> const Helm*
{
    if (PNpc->status != xi::Status::Normal)
    {
        return nullptr;
    }
    const auto name = prettyName(PNpc->getName());
    for (const auto& point : kHelm)
    {
        if (name == point.name)
        {
            return &point;
        }
    }
    return nullptr;
}

auto helmVisible(CNpcEntity* PNpc) -> bool
{
    return PNpc != nullptr && PNpc->status == xi::Status::Normal;
}

auto npcById(CCharEntity* PChar, const uint32 id) -> CNpcEntity*
{
    CNpcEntity* found = nullptr;
    PChar->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (found == nullptr && PNpc->id == id)
            {
                found = PNpc;
            }
        });
    return found;
}

struct HelmPick
{
    CNpcEntity* npc{};
    const Helm* kind{};
};

// The nearest visible point inside kGatherRange. Hidden points (relocating) are skipped.
auto nearestHelm(CCharEntity* PChar) -> HelmPick
{
    HelmPick best;
    float    bestDist = kGatherRange;
    PChar->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            const auto* kind = helmOf(PNpc);
            if (kind == nullptr)
            {
                return;
            }
            const float dist = distance(PChar->loc.p, PNpc->loc.p);
            if (dist <= kGatherRange && (best.npc == nullptr || dist < bestDist))
            {
                best.npc  = PNpc;
                best.kind = kind;
                bestDist  = dist;
            }
        });
    return best;
}

// Where seekPrey would run, once per hunt look that found nothing: sometimes walk to a point instead.
// The roll is that look, not every follow tick. No point within 80 yalms, or the roll misses: seekPrey runs as before.
auto startGather(Bot& bot, CCharEntity* PChar, const timer::time_point tick) -> bool
{
    if (bot.citizen || bot.withPlayer || PChar->PParty != nullptr || PChar->PAI->IsEngaged() ||
        PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        return false;
    }
    if (tick < bot.nextGather)
    {
        return false;
    }

    const auto chance = settings::get<uint32>("bots.GATHER_CHANCE");
    if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() < kGatherFree)
    {
        return false;
    }

    const auto picked = nearestHelm(PChar);
    if (picked.npc == nullptr)
    {
        return false;
    }
    if (xirand::GetRandomNumber<uint32>(100) >= chance)
    {
        return false;
    }

    Gather trip;
    trip.pointId   = picked.npc->id;
    trip.targid    = picked.npc->targid;
    trip.pointName = picked.kind->name;
    trip.tool      = picked.kind->tool;
    trip.toolName  = picked.kind->toolName;
    trip.tools     = toolCount(PChar, trip.tool);
    trip.until     = tick + kGatherTrip;
    trip.nextTrade = tick;
    if (trip.tools <= 0)
    {
        trip.giveTool = trip.tool;
    }

    bot.preySpot.reset();
    bot.preyDrySince = {};
    bot.gather       = std::move(trip);
    ShowInfoFmt("bots: {} goes gathering at {} in {}", bot.name, bot.gather->pointName, campZoneText(PChar, PChar->getZone()));
    return true;
}

// Walk to the point, trade one tool, and wait out the event. False once the trip has ended.
auto onGatherTrip(Bot& bot, PacketSystem& packets, CCharEntity* PChar, const timer::time_point tick) -> bool
{
    auto& trip = *bot.gather;
    if (tick >= trip.until)
    {
        stopGather(bot, tick, "5 minutes");
        return false;
    }
    if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        stopGather(bot, tick, "resting");
        return false;
    }

    auto* PNpc = npcById(PChar, trip.pointId);
    if (trip.waiting)
    {
        if (tick - trip.tradedAt >= kGatherEvent)
        {
            const auto left = toolCount(PChar, trip.tool);
            if (trip.tools - left == 1)
            {
                ShowInfoFmt("bots: {}'s {} broke", bot.name, trip.toolName);
            }
            stopGather(bot, tick, "no event");
            return false;
        }
        if (helmVisible(PNpc))
        {
            PChar->PAI->PathFind->Clear();
            PChar->PAI->PathFind->LookAt(PNpc->loc.p);
        }
        return true;
    }

    if (!helmVisible(PNpc))
    {
        stopGather(bot, tick, PNpc == nullptr ? "point gone" : "point hidden");
        return false;
    }

    const auto have = toolCount(PChar, trip.tool);
    if (have <= 0 && trip.giveTool == 0)
    {
        stopGather(bot, tick, "no tool left");
        return false;
    }

    if (distance(PChar->loc.p, PNpc->loc.p) > kGatherNear)
    {
        walkTo(PChar, PNpc->loc.p, kGatherNear, tick);
        return true;
    }

    if (PChar->PAI->PathFind->IsFollowingPath())
    {
        PChar->PAI->PathFind->Clear();
    }
    PChar->PAI->PathFind->LookAt(PNpc->loc.p);

    // The stack arrives from restock on the tick after the trip starts. Stand here until then, and between trades.
    if (have <= 0 || tick < trip.nextTrade)
    {
        return true;
    }
    if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() < kGatherFree)
    {
        stopGather(bot, tick, "fewer than 3 free slots");
        return false;
    }
    if (bot.sitting)
    {
        setSitting(bot, packets, false);
        return true;
    }
    if (PChar->isInEvent())
    {
        return true;
    }

    // Slot index for the trade, the same one useItem sends from SearchItem.
    const auto slot = PChar->getStorage(LOC_INVENTORY)->SearchItem(trip.tool);
    if (slot == ERROR_SLOTID)
    {
        stopGather(bot, tick, "no tool left");
        return false;
    }

    trip.tools       = have;
    auto  packet     = makePacket<GP_CLI_COMMAND_ITEM_TRANSFER>();
    auto* trade      = packet->as<GP_CLI_COMMAND_ITEM_TRANSFER>();
    trade->UniqueNo  = PNpc->id;
    trade->ActIndex  = PNpc->targid;
    trade->ItemNum   = 1;
    for (uint8 i = 0; i < 10; ++i)
    {
        trade->ItemNumTbl[i]           = 0;
        trade->PropertyItemIndexTbl[i] = 0;
    }
    trade->ItemNumTbl[0]           = 1;
    trade->PropertyItemIndexTbl[0] = static_cast<uint8>(slot);
    send(bot, packets, *packet);
    trip.waiting  = true;
    trip.tradedAt = tick;

    // The handler usually opens the event before send returns. End it now; the next think does it if not.
    if (PChar->isInEvent() && PChar->currentEvent->eventId != kEntryEvent)
    {
        finishGather(bot, packets, PChar, tick);
    }
    return true;
}

} // namespace

void emote(Bot& bot, PacketSystem& packets, const xi::Emote emote, const CBaseEntity* PTarget)
{
    auto  packet       = makePacket<GP_CLI_COMMAND_MOTION>();
    auto* motion       = packet->as<GP_CLI_COMMAND_MOTION>();
    // No target for an emote at nobody: aimed at itself it reads "looks at <name>".
    motion->UniqueNo   = PTarget != nullptr ? PTarget->id : 0;
    motion->ActIndex   = PTarget != nullptr ? PTarget->targid : 0;
    motion->Number     = static_cast<uint8_t>(emote);
    motion->Mode       = static_cast<uint8_t>(xi::EmoteMode::All);
    send(bot, packets, *packet);
}

void setSitting(Bot& bot, PacketSystem& packets, const bool sitting)
{
    auto packet                            = makePacket<GP_CLI_COMMAND_SIT>();
    packet->as<GP_CLI_COMMAND_SIT>()->Mode = static_cast<uint32_t>(sitting ? GP_CLI_COMMAND_SIT_MODE::On : GP_CLI_COMMAND_SIT_MODE::Off);
    send(bot, packets, *packet);
    bot.sitting = sitting;
}

auto Bot::entity() const -> CCharEntity*
{
    return session ? session->PChar.get() : nullptr;
}

void send(Bot& bot, PacketSystem& packets, CBasicPacket& packet)
{
    auto* PChar = bot.entity();
    packets.dispatch(packet.getType(), bot.session.get(), PChar, packet);
    PChar->flushEquipChanges();
}

void castSpell(Bot& bot, PacketSystem& packets, const SpellID spellId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::CastMagic, PTarget, static_cast<uint32>(spellId));
}

void useAbility(Bot& bot, PacketSystem& packets, const uint16 abilityId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::JobAbility, PTarget, abilityId);
}

// An item from the inventory on a target; false when the bot has none.
auto useItem(Bot& bot, PacketSystem& packets, const uint16 itemId, const CBaseEntity* PTarget) -> bool
{
    auto*      PChar = bot.entity();
    const auto slot  = PChar->getStorage(LOC_INVENTORY)->SearchItem(itemId);
    if (slot == ERROR_SLOTID)
    {
        return false;
    }

    auto  packet            = makePacket<GP_CLI_COMMAND_ITEM_USE>();
    auto* use               = packet->as<GP_CLI_COMMAND_ITEM_USE>();
    use->UniqueNo           = PTarget->id;
    use->ActIndex           = PTarget->targid;
    use->PropertyItemIndex  = slot;
    use->Category           = LOC_INVENTORY;
    send(bot, packets, *packet);
    return true;
}

void rangedAttack(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Shoot, PTarget);
}

void engage(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget)
{
    if (throttle(bot, kAttackKey, PTarget->id, std::chrono::seconds(3), timer::now()))
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Attack, PTarget);
    }
}

void disengage(Bot& bot, PacketSystem& packets)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, bot.entity());
}

void walkTo(CCharEntity* PChar, const position_t& pos, const float within, const timer::time_point tick)
{
    moveToward(PChar, pos, within, tick);
}

void weaponSkill(Bot& bot, PacketSystem& packets, const uint16 skillId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Weaponskill, PTarget, skillId);
}

// The same request setPos makes; the zone hands the bot over on its next tick.
void travel(CCharEntity* PChar, const xi::ZoneId zone, const position_t& pos)
{
    PChar->loc.p               = pos;
    PChar->loc.destination     = zone;
    PChar->loc.boundary        = 0;
    PChar->m_moghouseID        = 0;
    PChar->status              = xi::Status::Disappear;
    PChar->requestedZoneChange = true;

    if (PChar->shouldPetPersistThroughZoning())
    {
        PChar->setPetZoningInfo();
    }
}

// !bot <name>: what a bot is up to and why, a line each.
auto inspect(const Bot& bot) -> std::vector<std::string>
{
    auto* PChar = bot.entity();
    if (PChar == nullptr || PChar->loc.zone == nullptr)
    {
        return { fmt::format("{} isn't online.", bot.name) };
    }

    static constexpr std::array<const char*, static_cast<std::size_t>(OrderKind::Ability) + 1> orders = {
        "cast", "provoke", "attack", "stop", "rest", "stay", "follow", "pull", "stop pulling", "camp", "assist", "ability",
    };
    static_assert(orders.back() != nullptr, "one name per OrderKind");

    auto* PLeader = leaderOf(PChar);
    auto* PTarget = PChar->GetBattleTarget();

    std::string doing;
    if (PChar->isDead())
    {
        doing = "dead";
    }
    else if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        doing = "resting";
    }
    else if (!PChar->PAI->CanChangeState())
    {
        doing = "casting or using an ability";
    }
    else if (PChar->PAI->IsEngaged() && PTarget != nullptr)
    {
        doing = fmt::format("fighting {}", prettyName(PTarget->getName()));
    }
    else if (PChar->PAI->PathFind->IsFollowingPath())
    {
        doing = "moving";
    }
    else
    {
        doing = "standing";
    }

    std::vector<std::string> lines;
    lines.push_back(fmt::format("{} {}{}: HP {}% MP {}% TP {}, {}, {}", bot.name, static_cast<int>(PChar->GetMJob()), PChar->GetSJob() != xi::Job::NONE ? "+sub" : "",
                                PChar->GetHPP(), PChar->GetMPP(), PChar->health.tp, prettyName(PChar->loc.zone->getName()),
                                PChar->PParty == nullptr ? "no party" : PLeader != nullptr ? fmt::format("following {}", PLeader->getName()) : "leading"));
    const auto named = [](const uint32 id, const char* what)
    {
        const auto* POther = id != 0 ? zoneutils::GetChar(id) : nullptr;
        return POther != nullptr ? fmt::format(", {} {}", what, POther->getName()) : std::string();
    };
    lines.push_back(fmt::format("Now: {}{}{}{}{}", doing, bot.stay ? ", told to stay" : "", named(bot.followId, "following"), named(bot.assistId, "assisting"),
                                bot.withPlayer ? ", in a player's party" : ""));
    lines.push_back(bot.order ? fmt::format("Order: {}{}", orders[static_cast<std::size_t>(bot.order->kind)], bot.queued.empty() ? "" : fmt::format(" (+{} queued)", bot.queued.size()))
                              : "Order: none");
    if (PChar->PParty != nullptr)
    {
        lines.push_back(pullStatus(PChar));
    }
    lines.push_back(hateStatus(PChar, PTarget));
    lines.push_back(fmt::format("Heal wanted: {}; rests below {}% HP / {}% MP{}", healingWanted(PChar) ? "yes" : "no", kRestHPP, kRestMPP,
                                isCaster(PChar->GetMJob()) || PChar->GetMJob() == xi::Job::RDM ? ", and at every chance (caster)" : ""));
    return lines;
}

void leaveParty(Bot& bot, PacketSystem& packets)
{
    if (bot.entity()->PParty != nullptr)
    {
        leaveOrBreakUp(bot, packets, false);
    }
}

void think(Bot& bot, PacketSystem& packets, const std::set<uint32>& botIds, const bool active, const timer::time_point tick)
{
    auto* PChar = bot.entity();

    // Only chat and join requests are read; drop the rest without clearPacketList, which also resets the spawn lists.
    std::optional<std::pair<uint32, uint16>> joinRequest;
    while (!PChar->isPacketListEmpty())
    {
        const auto packet = PChar->popPacket();
        if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_CHAT_STD))
        {
            using Data             = GP_SERV_COMMAND_CHAT_STD::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            const auto*    from    = reinterpret_cast<const char*>(&packet->ref<uint8>(payload + offsetof(Data, sName)));
            const auto*    text    = reinterpret_cast<const char*>(&packet->ref<uint8>(payload + offsetof(Data, Mes)));

            // Auto-translate phrases (0xFD ... 0xFD, with id bytes that can look like letters) and other non-ASCII bytes are dropped.
            std::string message;
            bool        phrase = false;
            for (const char c : std::string_view(text, strnlen(text, sizeof(Data::Mes))))
            {
                if (static_cast<unsigned char>(c) == 0xFD)
                {
                    phrase = !phrase;
                    continue;
                }
                if (!phrase && std::isprint(static_cast<unsigned char>(c)))
                {
                    message += c;
                }
            }

            bot.heard.push_back(Heard{ packet->ref<uint8>(payload + offsetof(Data, Kind)), std::string(from, strnlen(from, sizeof(Data::sName))), std::move(message) });
        }
        else if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_MOTIONMES))
        {
            using Data             = GP_SERV_COMMAND_MOTIONMES::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            const auto     caster  = packet->ref<uint32>(payload + offsetof(Data, CasUniqueNo));
            const auto     target  = packet->ref<uint32>(payload + offsetof(Data, TarUniqueNo));
            const auto     theirs  = static_cast<xi::Emote>(packet->ref<uint16>(payload + offsetof(Data, MesNum)));
            auto*          PCaster = caster != PChar->id && !isPoolChar(caster) ? zoneutils::GetChar(caster) : nullptr;

            if (PCaster != nullptr)
            {
                // The same emote is queued for every bot in range, and think() runs once per bot. Log the first copy this tick.
                const auto tarIndex = packet->ref<uint16>(payload + offsetof(Data, TarActIndex));
                const auto casIndex = packet->ref<uint16>(payload + offsetof(Data, CasActIndex));
                const auto mode     = static_cast<uint8>(packet->ref<uint8>(payload + offsetof(Data, Mode)));
                const auto param    = packet->ref<uint16>(payload + offsetof(Data, Param));
                const auto number   = static_cast<uint16>(theirs);
                struct Seen
                {
                    uint32 caster{};
                    uint32 target{};
                    uint16 tarIndex{};
                    uint16 casIndex{};
                    uint16 number{};
                    uint8  mode{};
                    uint16 param{};
                    bool   operator==(const Seen&) const = default;
                };
                static timer::time_point seenTick{};
                static std::vector<Seen> seen;
                if (seenTick != tick)
                {
                    seen.clear();
                    seenTick = tick;
                }
                const Seen key{ caster, target, tarIndex, casIndex, number, mode, param };
                if (!std::ranges::contains(seen, key))
                {
                    seen.push_back(key);
                    ShowDebugFmt("bots: emote from {}: target {} index {}, caster index {}, emote {}, mode {}, param {}", PCaster->getName(), target,
                                tarIndex, casIndex, number, mode, param);
                }
            }

            const bool atMe   = target == PChar->id;
            const bool nearby = (target == 0 || target == caster) && PCaster != nullptr && distance(PCaster->loc.p, PChar->loc.p) <= kEmoteNear &&
                                xirand::GetRandomNumber<uint32>(100) < kEmoteChance;
            if (PCaster != nullptr && (atMe || nearby))
            {
                if (const auto reply = emoteReply(theirs))
                {
                    bot.emoteBack = static_cast<uint8>(*reply);
                    bot.emoteTo   = caster;
                    bot.emoteAt   = tick + std::chrono::milliseconds(xirand::GetRandomNumber(800, 2500));
                }
            }
        }
        else if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_PARTYREQ))
        {
            using Data             = GP_SERV_COMMAND_PARTYREQ::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            if (packet->ref<uint8>(payload + offsetof(Data, Status)) == static_cast<uint8>(GP_SERV_COMMAND_PARTYREQ_STATUS::Requesting))
            {
                joinRequest = { packet->ref<uint32>(payload + offsetof(Data, UniqueNo)), packet->ref<uint16>(payload + offsetof(Data, ActIndex)) };
            }
        }
    }

    if (PChar->loc.zone == nullptr || PChar->status == xi::Status::Disappear)
    {
        return;
    }
    watchPull(PChar);

    if (PChar->isDead())
    {
        if (bot.gather)
        {
            stopGather(bot, tick, "died");
        }
        if (bot.diedAt == timer::time_point{})
        {
            bot.diedAt = tick;
            if (!bot.deathNoted)
            {
                bot.deathNoted = true;
                noteEvent(bot, fmt::format("died in {}", prettyName(PChar->loc.zone->getName())));
            }
        }

        // Before anyone homepoints. A party with a real player does not take a copy. Citizens have no camp to copy.
        if (!bot.citizen)
        {
            rememberCamp(bot, PChar, botIds);
        }

        // Take a raise when one is offered; wait longer for one when someone in the party, or close by, could cast it.
        if (PChar->m_hasRaise)
        {
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::RaiseMenu, PChar);
        }
        else if (tick - bot.diedAt > (partyCanRaise(PChar) || raiserNearby(PChar) ? kRaiseWait : kHomepointDelay))
        {
            bot.diedAt = {};
            if (!bot.homeNoted)
            {
                bot.homeNoted = true;
                noteEvent(bot, "homepointed");
            }
            if (!bot.citizen)
            {
                startReturn(bot, PChar, botIds, tick);
            }
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::HomepointMenu, PChar);
        }
        return;
    }
    if (bot.deathNoted && !bot.homeNoted)
    {
        noteEvent(bot, "got raised");
        if (bot.returnUntil == timer::time_point{})
        {
            bot.returnCamp.reset();
        }
    }
    bot.diedAt     = {};
    bot.deathNoted = false;
    bot.homeNoted  = false;

    // Bots always carry Signet: faster resting, no TP lost to it, and the crystals and conquest points it brings.
    if (!PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Signet))
    {
        PChar->StatusEffectContainer->AddStatusEffectSilent(xi::StatusEffect::Signet, static_cast<uint16>(xi::StatusEffect::Signet), 0, 0s, 0s);
    }

    if (PChar->isInEvent())
    {
        // A gather trade opened this. End it the same way as any other cutscene, then count the yield.
        // A battlefield menu, and any event the bot did not just trade for, stays on the path below.
        if (bot.gather && bot.gather->waiting && PChar->currentEvent->eventId != kEntryEvent)
        {
            finishGather(bot, packets, PChar, tick);
            return;
        }
        if (!answerEntry(bot, packets, PChar))
        {
            finishEvent(bot, packets, PChar);
        }
        return;
    }
    bot.entryAnswered = false;

    // Bots may party among themselves, but one a real player has left breaks up.
    // A zoning player drops out of the party's member list for a moment, so only a real absence counts.
    if (PChar->PParty == nullptr)
    {
        bot.withPlayer   = false;
        bot.playerGoneAt = {};
    }
    else if (hasRealPlayer(PChar->PParty, botIds))
    {
        if (!bot.withPlayer)
        {
            std::string with;
            for (auto* PMember : PChar->PParty->members)
            {
                if (PMember->objtype == TYPE_PC && !botIds.contains(PMember->id))
                {
                    with = PMember->getName();
                    break;
                }
            }
            if (!with.empty())
            {
                noteEvent(bot, fmt::format("joined a party with {}", with));
            }
            greet(bot);
        }
        bot.withPlayer   = true;
        bot.playerGoneAt = {};
    }
    else if (bot.withPlayer)
    {
        if (bot.playerGoneAt == timer::time_point{})
        {
            bot.playerGoneAt = tick;
        }
        else if (tick - bot.playerGoneAt > kPlayerGoneGrace)
        {
            bot.withPlayer   = false;
            bot.playerGoneAt = {};
            leaveOrBreakUp(bot, packets, true);
            return;
        }
    }

    // A real player's party is not the solo grind. Drop a half-finished walk so leaving later starts clean.
    if (bot.withPlayer || PChar->PParty != nullptr)
    {
        bot.preySpot.reset();
        bot.preyDrySince = {};
        if (bot.gather)
        {
            stopGather(bot, tick, "joined a party");
        }
    }

    if (bot.citizen)
    {
        bot.invites.clear();
        if (PChar->InvitePending.entity.UniqueNo != 0 && PChar->PParty == nullptr)
        {
            declineInvite(bot, packets);
        }
    }
    else if (PChar->InvitePending.entity.UniqueNo != 0 && PChar->PParty == nullptr)
    {
        acceptInvite(bot, packets);
    }
    else if (joinRequest && (PChar->PParty == nullptr || PChar->PParty->GetLeader() == PChar))
    {
        invite(bot, packets, joinRequest->first, joinRequest->second);
    }
    else if (!bot.invites.empty() && (PChar->PParty == nullptr || (PChar->PParty->GetLeader() == PChar && !PChar->PParty->IsFull())))
    {
        const auto name = bot.invites.front();
        bot.invites.erase(bot.invites.begin());
        if (auto* PTarget = zoneutils::GetCharByName(name); PTarget != nullptr && PTarget->PParty == nullptr && PTarget != PChar)
        {
            invite(bot, packets, PTarget->id, PTarget->getZone() == PChar->getZone() ? PTarget->targid : 0);
        }
    }
    else if (PChar->PParty != nullptr && PChar->PParty->GetLeader() != PChar)
    {
        bot.invites.clear();
    }

    // Idle in a zone with nobody real: don't advertise on /sea. Those bots won't move until someone arrives.
    const bool wantSeek = active && PChar->PParty == nullptr && !bot.citizen;
    if (!bot.seekingApplied || bot.seeking != wantSeek)
    {
        setSeeking(bot, packets, wantSeek);
        bot.seekingApplied = true;
    }


    auto* PLeader = leaderOf(PChar);
    auto* PGuide  = partner(PChar, bot.followId, PLeader);
    if (bot.citizen)
    {
        bot.focusTarget = 0;
    }
    auto* PTarget = combatTarget(bot, PChar, partner(PChar, bot.assistId, PLeader));

    if (bot.sitting && (PTarget != nullptr || PLeader != nullptr))
    {
        setSitting(bot, packets, false);
    }

    // Back in the camp zone, out of time, or a real player joined: drop the copy and behave as today.
    // A trip out of town waits until the leader is near the camp spot, or autoCamp would camp at the zone line.
    if (bot.returnUntil != timer::time_point{})
    {
        const bool inZone  = bot.returnCamp && PChar->getZone() == bot.returnCamp->zone;
        bool       arrived = inZone;
        if (bot.headingOut && inZone)
        {
            const bool atSpot = distance(PChar->loc.p, bot.returnCamp->pos) <= kHeadOutNear;
            if (PChar->PParty != nullptr && PChar->PParty->GetLeader() == PChar)
            {
                arrived = atSpot;
            }
            else if (const auto* PLead = PChar->PParty != nullptr ? PChar->PParty->GetLeader() : nullptr; PLead != nullptr && outings.find(PLead->id) != outings.end())
            {
                // The leader is still on the way. Stay with the return path so a quiet zone does not idle the member at the line.
                arrived = atSpot;
            }
            else
            {
                auto*      PFollow  = leaderOf(PChar);
                const bool withLead = PFollow != nullptr && PFollow->getZone() == PChar->getZone() &&
                                      distance(PChar->loc.p, PFollow->loc.p) <= kFollowDistance + 3.0f;
                arrived = atSpot || withLead;
            }
        }
        const bool capped  = tick >= bot.returnUntil;
        if (capped || arrived || bot.withPlayer || !bot.returnCamp)
        {
            if (capped && !arrived && !bot.withPlayer && bot.returnCamp)
            {
                ShowInfoFmt("bots: {} stopped heading {} to {}", PChar->getName(), bot.headingOut ? "out" : "back", campZoneText(PChar, bot.returnCamp->zone));
            }
            endHeadOut(bot, PChar);
            bot.returnCamp.reset();
            bot.returnUntil = {};
        }
    }

    if (!bot.citizen)
    {
        joinOuting(bot, PChar, botIds, tick);
        startHeadOut(bot, PChar, botIds, tick);
    }

    // No real player around to see it: the bot stands idle, only fighting back if something attacks it.
    // A party still walking back to its camp is exempt until the clear above.
    if (!active && bot.returnUntil == timer::time_point{} && (PTarget == nullptr || PTarget->GetBattleTarget() != PChar))
    {
        if (bot.citizen)
        {
            citizenPause(bot);
        }
        PChar->PAI->PathFind->Clear();
        return;
    }

    // The leader zones to the copied camp. Members follow, which zones to the leader once the leader is there.
    if (bot.returnUntil != timer::time_point{} && bot.returnCamp && PChar->getZone() != bot.returnCamp->zone)
    {
        if (PChar->PParty != nullptr && PChar->PParty->GetLeader() == PChar)
        {
            travel(PChar, bot.returnCamp->zone, bot.returnCamp->pos);
        }
        else if (auto* PLead = leaderOf(PChar);
                 PLead != nullptr && !PLead->isDead() && (PLead->getZone() == bot.returnCamp->zone || PLead->getZone() == PChar->getZone()))
        {
            follow(PChar, PLead, tick);
        }
        else
        {
            PChar->PAI->PathFind->Clear();
        }
        return;
    }

    // In the camp's zone, but not at the spot yet. A wipe return stops at the zone line, above.
    if (bot.headingOut && bot.returnUntil != timer::time_point{} && bot.returnCamp && PChar->getZone() == bot.returnCamp->zone &&
        distance(PChar->loc.p, bot.returnCamp->pos) > kHeadOutNear)
    {
        if (PChar->PParty != nullptr && PChar->PParty->GetLeader() == PChar)
        {
            walkTo(PChar, bot.returnCamp->pos, kHeadOutNear, tick);
        }
        else if (auto* PLead = leaderOf(PChar);
                 PLead != nullptr && !PLead->isDead() && (PLead->getZone() == bot.returnCamp->zone || PLead->getZone() == PChar->getZone()))
        {
            follow(PChar, PLead, tick);
        }
        else
        {
            PChar->PAI->PathFind->Clear();
        }
        return;
    }

    // Moved: across the ground, from where it last stood. Height and small jitter in a client's position don't count.
    if (PGuide != nullptr)
    {
        const float dx = PGuide->loc.p.x - bot.leaderLast.x;
        const float dz = PGuide->loc.p.z - bot.leaderLast.z;
        if (dx * dx + dz * dz >= 0.3f * 0.3f)
        {
            bot.leaderMovedAt = tick;
            bot.leaderLast    = PGuide->loc.p;
        }
    }

    // Mid-cast (or ability, weapon skill, item): a player stands still and waits, or the cast is interrupted.
    if (!PChar->PAI->CanChangeState())
    {
        PChar->PAI->PathFind->Clear();
        return;
    }

    if (bot.emoteTo != 0 && tick >= bot.emoteAt)
    {
        if (auto* PTo = zoneutils::GetChar(bot.emoteTo); PTo != nullptr && PTo->getZone() == PChar->getZone() && distance(PTo->loc.p, PChar->loc.p) < 30.0f)
        {
            PChar->PAI->PathFind->LookAt(PTo->loc.p);
            emote(bot, packets, static_cast<xi::Emote>(bot.emoteBack), PTo);
        }
        bot.emoteTo = 0;
    }

    // Something attacking a bot left outside a battlefield still gets fought.
    if (!bot.citizen && followIn(bot, packets, PChar, PLeader, tick) && PTarget == nullptr)
    {
        return;
    }

    if (!bot.citizen && followOrder(bot, packets, tick))
    {
        PChar->PAI->PathFind->Clear();
        return;
    }

    if (!bot.citizen && pull(bot, packets, PChar, PLeader, tick))
    {
        return;
    }

    if (PTarget != nullptr)
    {
        if (bot.gather)
        {
            stopGather(bot, tick, "attacked");
        }
        bot.foughtAt = tick;

        if (restInFight(bot, packets, PChar, PTarget, tick))
        {
            return;
        }

        // Camped: a monster still on its way in is waited for, not met. The tank opens on it from here.
        if (const auto camp = campSpot(PChar); camp && distance(PTarget->loc.p, *camp) > kCampHold && PTarget->GetBattleTarget() != nullptr &&
                                               PTarget->GetBattleTarget()->GetHPP() >= kHelpBelowHPP)
        {
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }

            // On the one it's after: back off across camp, still inside it. A melee monster follows all the way in;
            // an archer or caster stops at its range from here, which puts it inside camp too.
            if (PTarget->GetBattleTarget() == PChar)
            {
                const float dx  = camp->x - PTarget->loc.p.x;
                const float dz  = camp->z - PTarget->loc.p.z;
                const float len = std::max(0.01f, std::sqrt(dx * dx + dz * dz));

                position_t away = *camp;
                away.x += dx / len * (kCampHold - 1.0f);
                away.z += dz / len * (kCampHold - 1.0f);
                if (distance(PChar->loc.p, away) > 1.5f)
                {
                    moveToward(PChar, away, 1.0f, tick);
                    return;
                }
                PChar->PAI->PathFind->Clear();
                PChar->PAI->PathFind->LookAt(PTarget->loc.p);
                act(bot, packets, PTarget, false, tick);
                return;
            }

            if (distance(PChar->loc.p, *camp) > kCampSpread)
            {
                moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
                return;
            }
            PChar->PAI->PathFind->Clear();
            PChar->PAI->PathFind->LookAt(PTarget->loc.p);
            act(bot, packets, PTarget, false, tick);
            return;
        }

        if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            setResting(bot, packets, false);
        }

        // Casters and archers hit by a link run to the tank too; melee keep swinging while the tank grabs it.
        if (pulledHate(PChar, PTarget) || ((isCaster(PChar->GetMJob()) || isRanged(PChar)) && chasedOffTank(PChar)))
        {
            // Hate came off the tank: stop adding to it and bring the monster back to them.
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }
            if (auto* PTank = mainTank(PChar); distance(PChar->loc.p, PTank->loc.p) > kToTankDistance)
            {
                moveToward(PChar, PTank->loc.p, kToTankDistance / 2, tick);
            }
            else if (!PChar->PAI->PathFind->IsFollowingPath())
            {
                PChar->PAI->PathFind->LookAt(PTarget->loc.p);
            }
        }
        else if (isCaster(PChar->GetMJob()) || isRanged(PChar))
        {
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }
            if (tick >= bot.songWalkUntil)
            {
                keepRange(PChar, PTarget, isRanged(PChar) ? kArcherDistance : kCasterDistance, tick);
            }
        }
        else
        {
            fight(bot, packets, PChar, PTarget, tick);
        }

        act(bot, packets, PTarget, false, tick);
        return;
    }

    if (PChar->PAI->IsEngaged())
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
    }

    // Townsfolk: the city routine instead of hunt, wander, camp, and follow. A fight above still runs.
    if (bot.citizen)
    {
        citizenAct(bot, packets, tick);
        return;
    }

    // Following a leader who is moving, or still catching up: no casting that would stop the bot.
    const bool travelling = PGuide != nullptr && !campSpot(PChar) && (tick - bot.leaderMovedAt < kSettleTime ||
                                                  distance(PChar->loc.p, PGuide->loc.p) > kFollowDistance + 3.0f || PGuide->getZone() != PChar->getZone());

    if (act(bot, packets, nullptr, travelling, tick) || (PGuide != nullptr && conceal(bot, packets, PGuide, tick)) || rest(bot, packets, PChar, PLeader, tick))
    {
        if (bot.gather && PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            stopGather(bot, tick, "resting");
        }
        return;
    }

    if (PGuide != nullptr)
    {
        if (const auto camp = campSpot(PChar))
        {
            if (distance(PChar->loc.p, *camp) > kCampSpread)
            {
                moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
            }
        }
        else if (!bot.stay)
        {
            follow(PChar, PGuide, tick);
        }
    }
    else if (PChar->PParty != nullptr && PChar->PParty->GetLeader() == PChar && (campSpot(PChar) || partyReady(PChar)) && autoCamp(PChar, bot.withPlayer))
    {
        // A bot-led party at its camp: the leader stays there and the puller brings the fights in.
        if (const auto camp = campSpot(PChar); camp && distance(PChar->loc.p, *camp) > kCampSpread)
        {
            moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
        }
    }
    else if (PChar->PParty == nullptr || (PChar->PParty->GetLeader() == PChar && partyReady(PChar)))
    {
        // A body nearby: go over and raise it before anything else.
        if (auto* PDead = deadToRaise(PChar); PDead != nullptr && distance(PChar->loc.p, PDead->loc.p) > kRaiseRange)
        {
            moveToward(PChar, PDead->loc.p, kRaiseRange - 3.0f, tick);
            return;
        }

        // GATHERING false: the flag is read and both branches are skipped, and hunt, seek, and wander are unchanged.
        const bool gatheringOn = settings::get<bool>("bots.GATHERING");
        if (gatheringOn && bot.gather && onGatherTrip(bot, packets, PChar, tick))
        {
            return;
        }

        if (settings::get<bool>("bots.HUNT"))
        {
            const bool looked = tick >= bot.nextHunt;
            if (auto* PPrey = huntTarget(bot, PChar, tick))
            {
                bot.focusTarget  = PPrey->id;
                bot.preyDrySince = {};
                return;
            }
            // Nothing to hunt (this is where seekPrey runs). One roll per hunt look, and only if a point is close.
            if (looked && gatheringOn && !bot.gather && startGather(bot, PChar, tick) && onGatherTrip(bot, packets, PChar, tick))
            {
                return;
            }
            // Nothing within 30 yalms: after a minute, walk to another camp. A player's party never gets here.
            if (seekPrey(bot, PChar, tick))
            {
                return;
            }
        }
        wander(bot, packets, PChar, tick);
    }
}

} // namespace bots
