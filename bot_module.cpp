//
// Bots module: brings pooled bots online while real players are connected.
//
//   Settings are in settings/bots.lua, the pool in the bot_pool table.
//   Bot characters are created on startup for pool rows that don't have one yet.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/scheduler/scheduler.h"
#include "common/settings.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/map_constants.h"
#include "map/packet_system.h"
#include "map/enums/party_kind.h"
#include "map/packets/c2s/0x00a_login.h"
#include "map/packets/c2s/0x06e_group_solicit_req.h"
#include "map/persist_batch.h"
#include "map/utils/charutils.h"
#include "map/utils/moduleutils.h"
#include "map/utils/petutils.h"
#include "map/utils/zoneutils.h"
#include "map/status_effect_container.h"
#include "map/map_engine.h"
#include "map/zone.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <fmt/ranges.h>

namespace
{

constexpr auto kPresenceInterval = std::chrono::seconds(5);
constexpr auto kBalanceInterval  = std::chrono::seconds(30);
constexpr auto kGroupingInterval = std::chrono::seconds(4); // one recruit step: an invite, or a new party
constexpr auto kSlowStep         = std::chrono::milliseconds(200);

// A party fills by walking (and zoning) to the leader, not by already standing together. One invite each recruit pass.
constexpr uint8 kHealerLevel = 15; // at this level and up, a party wants a healer; below it, the healer slot is skipped

constexpr auto kSeekInviteDelay = std::chrono::seconds(5); // the tell, then the invite
constexpr auto kSeekerCooldown  = std::chrono::minutes(15);
constexpr auto kLeaderCooldown  = std::chrono::minutes(40);

// Same order partyHealer uses. It needs a party, and these bots are not in one yet.
auto healerRank(const xi::Job job) -> int
{
    return job == xi::Job::WHM ? 2 : job == xi::Job::RDM || job == xi::Job::SCH ? 1 : 0;
}

// One support after the healer: red mage, bard, corsair, geomancer. A red mage taken as the healer does not fill this.
auto isSupportJob(const xi::Job job) -> bool
{
    return job == xi::Job::RDM || job == xi::Job::BRD || job == xi::Job::COR || job == xi::Job::GEO;
}

// Same order mainTank uses, so the bot who leads is the one the party will treat as the tank.
auto tankScore(const xi::Job job) -> int
{
    switch (job)
    {
        case xi::Job::PLD: return 3;
        case xi::Job::NIN:
        case xi::Job::RUN: return 2;
        case xi::Job::WAR: return 1;
        default: return 0;
    }
}

auto chooseLeader(const std::vector<bots::Bot*>& cluster) -> bots::Bot*
{
    bots::Bot* tank = nullptr;
    for (auto* bot : cluster)
    {
        auto* PChar = bot->entity();
        if (PChar == nullptr || !bots::isTank(PChar->GetMJob()))
        {
            continue;
        }
        const auto score = tankScore(PChar->GetMJob());
        if (tank == nullptr || score > tankScore(tank->entity()->GetMJob()) ||
            (score == tankScore(tank->entity()->GetMJob()) && PChar->GetMLevel() > tank->entity()->GetMLevel()))
        {
            tank = bot;
        }
    }
    if (tank != nullptr)
    {
        return tank;
    }

    bots::Bot* highest = nullptr;
    for (auto* bot : cluster)
    {
        auto* PChar = bot->entity();
        if (PChar == nullptr)
        {
            continue;
        }
        if (highest == nullptr || PChar->GetMLevel() > highest->entity()->GetMLevel())
        {
            highest = bot;
        }
    }
    return highest;
}

// In the party's band. `center` is the leader's level when recruiting started; lo and hi are the party as it is.
// lo > hi when the only member is the leader and the caller passes the leader's level as both.
auto nearLeader(bots::Bot* leader, const std::vector<bots::Bot*>& bots, const int center, const int lo, const int hi) -> std::vector<bots::Bot*>
{
    std::vector<bots::Bot*> near;
    if (leader->entity() == nullptr)
    {
        return near;
    }
    for (auto* bot : bots)
    {
        auto* PChar = bot->entity();
        if (PChar == nullptr)
        {
            continue;
        }
        if (bot == leader || bots::fitsBand(center, lo, hi, static_cast<int>(PChar->GetMLevel())))
        {
            near.push_back(bot);
        }
    }
    return near;
}

// What the party already has. A red mage counted as the healer does not also count as the support.
struct Roles
{
    bool tank{};
    bool healer{};
    bool support{};
    int  highest{};
};

auto rolesOf(const std::vector<bots::Bot*>& group) -> Roles
{
    Roles roles;
    bool  healerTaken = false;
    for (const auto* bot : group)
    {
        auto* PChar = bot->entity();
        if (PChar == nullptr)
        {
            continue;
        }
        const auto job = PChar->GetMJob();
        roles.highest  = std::max(roles.highest, static_cast<int>(PChar->GetMLevel()));
        if (bots::isTank(job))
        {
            roles.tank = true;
        }
        const bool heal = healerRank(job) > 0;
        if (heal && !healerTaken)
        {
            healerTaken = true;
        }
        else if (isSupportJob(job))
        {
            roles.support = true;
        }
    }
    roles.healer = healerTaken;
    return roles;
}

// Tank, then a healer from 15 up, then one support, then damage. Best of that slot: tank score, healer rank, puller rank.
auto pickRole(const Roles& roles, const std::vector<bots::Bot*>& candidates) -> bots::Bot*
{
    const bool wantHealer = roles.highest >= kHealerLevel && !roles.healer;
    enum class Slot : uint8
    {
        Tank,
        Healer,
        Support,
        Any
    };
    std::vector<Slot> slots;
    if (!roles.tank)
    {
        slots.push_back(Slot::Tank);
    }
    if (wantHealer)
    {
        slots.push_back(Slot::Healer);
    }
    if (!roles.support)
    {
        slots.push_back(Slot::Support);
    }
    slots.push_back(Slot::Any);

    // true when a is the better fill. Puller rank is lower-is-better, the same order fillParty used to sort by.
    const auto better = [](const bots::Bot* a, const bots::Bot* b, const Slot slot) -> bool
    {
        auto* PCharA = a->entity();
        auto* PCharB = b->entity();
        if (slot == Slot::Healer)
        {
            const int ra = healerRank(PCharA->GetMJob());
            const int rb = healerRank(PCharB->GetMJob());
            if (ra != rb)
            {
                return ra > rb;
            }
        }
        else if (slot == Slot::Tank)
        {
            const int ra = tankScore(PCharA->GetMJob());
            const int rb = tankScore(PCharB->GetMJob());
            if (ra != rb)
            {
                return ra > rb;
            }
        }
        else
        {
            const int ra = bots::pullerRank(PCharA->GetMJob(), PCharA->GetMLevel());
            const int rb = bots::pullerRank(PCharB->GetMJob(), PCharB->GetMLevel());
            if (ra != rb)
            {
                return ra < rb;
            }
        }
        return PCharA->GetMLevel() > PCharB->GetMLevel();
    };

    for (const auto slot : slots)
    {
        bots::Bot* best     = nullptr;
        bots::Bot* fallback = nullptr;
        for (auto* bot : candidates)
        {
            auto* PChar = bot->entity();
            if (PChar == nullptr)
            {
                continue;
            }
            const auto job   = PChar->GetMJob();
            const bool tank  = bots::isTank(job);
            const bool heal  = healerRank(job) > 0;
            const bool supp  = isSupportJob(job);
            const bool fills = slot == Slot::Tank ? tank : slot == Slot::Healer ? heal : slot == Slot::Support ? supp : !tank && !heal && !supp;
            if (fills && (best == nullptr || better(bot, best, slot)))
            {
                best = bot;
            }
            if (slot == Slot::Any && (fallback == nullptr || better(bot, fallback, slot)))
            {
                fallback = bot;
            }
        }
        if (slot != Slot::Any)
        {
            if (best != nullptr)
            {
                return best;
            }
        }
        else
        {
            return best != nullptr ? best : fallback;
        }
    }
    return nullptr;
}

// Tank, then one healer (from 15 up), then one support, then damage dealers, up to 6.
// A party of one is the leader on their own: recruiting invites the rest, it does not wait for a healer to already be standing there.
auto fillParty(const std::vector<bots::Bot*>& pool) -> std::vector<bots::Bot*>
{
    auto* leader = chooseLeader(pool);
    if (leader == nullptr || leader->entity() == nullptr)
    {
        return {};
    }

    const int  center = static_cast<int>(leader->entity()->GetMLevel());
    const auto near   = nearLeader(leader, pool, center, center, center);

    std::vector<bots::Bot*> group{ leader };
    std::vector<bots::Bot*> rest;
    for (auto* bot : near)
    {
        if (bot != leader)
        {
            rest.push_back(bot);
        }
    }
    while (group.size() < 6)
    {
        auto* next = pickRole(rolesOf(group), rest);
        if (next == nullptr)
        {
            break;
        }
        group.push_back(next);
        std::erase(rest, next);
    }
    return group;
}

auto partyFrom(bots::Bot* seed, const std::vector<bots::Bot*>& zoneBots) -> std::vector<bots::Bot*>
{
    if (seed->entity() == nullptr)
    {
        return {};
    }
    const int  center = static_cast<int>(seed->entity()->GetMLevel());
    const auto around = nearLeader(seed, zoneBots, center, center, center);
    return fillParty(around);
}

auto jobAbbr(const xi::Job job) -> std::string
{
    static constexpr std::array<const char*, 23> jobs = {
        "", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
        "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN",
    };
    const auto index = static_cast<std::size_t>(job);
    const auto abbr  = index < jobs.size() && jobs[index][0] != '\0' ? std::string(jobs[index]) : std::string("DD");
    return bots::lowerCase(abbr);
}

// chars.nation, readable before the character is loaded. 0 San d'Oria, 1 Bastok, 2 Windurst. 255: not known.
auto charNation(const uint32 charId) -> uint8
{
    if (const auto rset = db::preparedStmt("SELECT nation FROM chars WHERE charid = ?", charId); rset && rset->next() && !rset->isNull("nation"))
    {
        const auto nation = rset->get<uint8>("nation");
        if (nation < 3)
        {
            return nation;
        }
    }
    return 255;
}

auto zoneText(const xi::ZoneId zone) -> std::string
{
    if (auto* PZone = zoneutils::GetZone(zone))
    {
        return bots::prettyName(PZone->getName());
    }
    return fmt::format("{}", static_cast<uint16>(zone));
}

// A cutscene, or a gather trade that has not opened one yet. Destroying the character here, or zoning it,
// leaves the event (or the trade that is about to open one) pointing at a character that is gone.
auto inCutscene(const bots::Bot& bot) -> bool
{
    const auto* PChar = bot.entity();
    return PChar != nullptr && (PChar->isInEvent() || (bot.gather && bot.gather->waiting));
}

} // namespace

// "LFM 18-24, need WHM + 2 DD". Empty when the party is already full. Role names are the job that slot is usually filled by.
auto bots::lfmLine(const CCharEntity* PChar, const uint8 center) -> std::string
{
    if (PChar == nullptr)
    {
        return {};
    }

    bool tank        = false;
    bool healerTaken = false;
    bool support     = false;
    int  count       = 0;
    int  highest     = static_cast<int>(PChar->GetMLevel());
    const auto note  = [&](const CBattleEntity* PMember)
    {
        ++count;
        highest            = std::max(highest, static_cast<int>(PMember->GetMLevel()));
        const auto job    = PMember->GetMJob();
        const bool heal   = healerRank(job) > 0;
        if (bots::isTank(job))
        {
            tank = true;
        }
        if (heal && !healerTaken)
        {
            healerTaken = true;
        }
        else if (isSupportJob(job))
        {
            support = true;
        }
    };
    if (PChar->PParty == nullptr)
    {
        note(PChar);
    }
    else
    {
        for (const auto* PMember : PChar->PParty->members)
        {
            note(PMember);
        }
    }
    if (count >= 6)
    {
        return {};
    }

    const int spread = bots::partySpread();
    const int lo     = std::max(1, static_cast<int>(center) - spread);
    const int hi     = std::min(75, static_cast<int>(center) + spread);
    const bool wantHealer = highest >= kHealerLevel && !healerTaken;

    std::vector<std::string> need;
    if (!tank)
    {
        need.emplace_back("PLD");
    }
    if (wantHealer)
    {
        need.emplace_back("WHM");
    }
    if (!support)
    {
        need.emplace_back("BRD");
    }
    const int open = 6 - count;
    const int reserved = (!tank ? 1 : 0) + (wantHealer ? 1 : 0) + (!support ? 1 : 0);
    const int dd = std::max(0, open - reserved);
    if (dd == 1)
    {
        need.emplace_back("1 DD");
    }
    else if (dd > 1)
    {
        need.emplace_back(fmt::format("{} DD", dd));
    }
    if (need.empty())
    {
        return {};
    }

    std::string listed;
    for (std::size_t i = 0; i < need.size(); ++i)
    {
        if (i != 0)
        {
            listed += " + ";
        }
        listed += need[i];
    }
    return fmt::format("LFM {}-{}, need {}", lo, hi, listed);
}

class BotModule : public CPPModule
{
    void OnInit() override
    {
        enabled_ = settings::get<bool>("bots.ENABLED");
        if (!enabled_)
        {
            ShowInfo("bots: disabled (settings/bots.lua)");
            return;
        }

        packets_ = std::make_unique<PacketSystem>();

        bots::ensureSchema();
        bots::clearStaleState();
        pool_ = bots::loadPool();
        bots::ensureLinkshell();

        for (const auto& entry : pool_)
        {
            poolIds_.insert(entry.charId);
            bots::registerPoolChar(entry.charId);
            poolNames_.insert(entry.name);
        }

        bots::startChat();

        // For the !bots and !rotatebots commands (bot_commands.lua).
        lua.set_function("BotsStatus",
                         [this]()
                         {
                             return sol::as_table(status());
                         });
        lua.set_function("BotsInspect",
                         [this](const std::string& name)
                         {
                             const auto bot = std::ranges::find_if(online_, [&](const bots::Bot& b) { return bots::lowerCase(b.name) == bots::lowerCase(name); });
                             return sol::as_table(bot != online_.end() ? bots::inspect(*bot) : std::vector<std::string>{ fmt::format("No bot called {} is online.", name) });
                         });
        lua.set_function("BotsRotate",
                         [this](const sol::optional<uint32> count)
                         {
                             return sol::as_table(rotate(count.value_or(0)));
                         });

        ShowInfoFmt("bots: {} in the pool, up to {} online", pool_.size(), settings::get<uint32>("bots.MAX_ONLINE"));
    }

    // The engine's scheduler is only reachable through a real player's session.
    void OnCharZoneIn(CCharEntity* PChar) override
    {
        if (!enabled_ || tickToken_ || poolIds_.contains(PChar->id) || !PChar->PSession || !PChar->PSession->scheduler)
        {
            return;
        }

        scheduler_ = PChar->PSession->scheduler;
        tickToken_.emplace(scheduler_->intervalOnMainThread(kLogicUpdateInterval,
                                                            [this]()
                                                            {
                                                                tick();
                                                            }));
    }

    // A real player's invite pulls the bot out of whatever party it's in, so the invite can land.
    auto OnIncomingPacket(MapSession* PSession, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        if (!enabled_ || packet.getType() != static_cast<uint16>(PacketC2S::GP_CLI_COMMAND_GROUP_SOLICIT_REQ) || poolIds_.contains(PChar->id))
        {
            return false;
        }

        auto*      invite = packet.as<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
        const auto it     = std::ranges::find_if(online_,
                                                 [&](const auto& bot)
                                                 {
                                                     return bot.charId == invite->UniqueNo;
                                                 });

        if (it == online_.end() || it->entity() == nullptr)
        {
            return false;
        }

        if (it->citizen)
        {
            return false;
        }

        // An invite from a search result goes out as an alliance invite when the target leads a party.
        // Bots never accept those, so it becomes a party invite.
        if (invite->Kind == PartyKind::Alliance)
        {
            ShowInfoFmt("bots: {}'s alliance invite to {} is taken as a party invite", PChar->getName(), it->name);
            invite->Kind = PartyKind::Party;
        }

        if (invite->Kind != PartyKind::Party)
        {
            return false;
        }

        if (it->entity()->PParty != nullptr && it->entity()->PParty != PChar->PParty)
        {
            ShowInfoFmt("bots: {} leaves its party for {}'s invite", it->name, PChar->getName());
            bots::leaveParty(*it, *packets_);
        }

        return false;
    }

    void tick()
    {
        const auto now = timer::now();

        // How long the bots take each tick, logged once a minute; the server watchdog fires at 2 s.
        struct Timing
        {
            BotModule&        module;
            timer::time_point start;
            ~Timing()
            {
                const auto took = timer::now() - start;
                module.tickCount_++;
                module.tickTotal_ += took;
                module.tickWorst_ = std::max(module.tickWorst_, took);
                if (timer::now() - module.tickReported_ >= std::chrono::minutes(1))
                {
                    const auto ms = [](const timer::duration d) { return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0; };
                    ShowInfoFmt("bots: tick {:.1f} ms average, {:.1f} ms worst over {} ticks, {} bots online", ms(module.tickTotal_ / std::max<int64>(1, module.tickCount_)),
                                ms(module.tickWorst_), module.tickCount_, module.online_.size());
                    module.tickReported_ = timer::now();
                    module.tickCount_    = 0;
                    module.tickTotal_    = {};
                    module.tickWorst_    = {};
                }
            }
        } timing{ *this, now };

        // Commands can log bots out between ticks.
        std::erase_if(online_,
                      [](const auto& bot)
                      {
                          return bot.entity() == nullptr;
                      });

        if (now >= nextPresenceCheck_)
        {
            nextPresenceCheck_ = now + kPresenceInterval;
            if (bots::countRealPlayers() > 0)
            {
                lastRealPlayer_ = now;
            }
        }

        const auto grace  = std::chrono::seconds(settings::get<uint32>("bots.LOGOUT_GRACE"));
        const bool wanted = lastRealPlayer_ != timer::time_point{} && now - lastRealPlayer_ <= kPresenceInterval + grace;
        const auto target = wanted ? std::min<std::size_t>(settings::get<uint32>("bots.MAX_ONLINE"), pool_.size()) : 0;

        if (online_.size() < target && now >= nextLogin_)
        {
            nextLogin_ = now + std::chrono::seconds(settings::get<uint32>("bots.LOGIN_INTERVAL"));
            loginNext();
        }
        else if (online_.size() > target && now >= nextLogout_)
        {
            // Over the cap with players still on: only bots outside a party go.
            nextLogout_   = now + std::chrono::seconds(1);
            const auto drop = [&](const bots::Bot& bot)
            {
                return target == 0 || isFree(bot);
            };
            auto it = online_.rend();
            if (target != 0)
            {
                it = std::ranges::find_if(online_.rbegin(), online_.rend(), [&](const bots::Bot& bot) { return drop(bot) && !bot.citizen && !inCutscene(bot); });
            }
            if (it == online_.rend())
            {
                it = std::ranges::find_if(online_.rbegin(), online_.rend(), [&](const bots::Bot& bot) { return drop(bot) && !inCutscene(bot); });
            }
            if (it != online_.rend())
            {
                logout(*it);
            }
        }

        if (wanted && now >= nextBalance_)
        {
            nextBalance_ = now + kBalanceInterval;
            balance();
        }

        if (wanted && now >= nextGrouping_)
        {
            nextGrouping_ = now + kGroupingInterval;
            formParties();
        }

        if (wanted && now >= nextSeek_)
        {
            nextSeek_ = now + std::chrono::seconds(30 + xirand::GetRandomNumber<uint32>(31));
            askSeekers(now);
        }

        if (wanted)
        {
            sendDueInvites(now);
        }

        if (const auto minutes = settings::get<uint32>("bots.ROTATE_MINUTES"); wanted && minutes > 0 && now >= nextRotate_)
        {
            if (nextRotate_ != timer::time_point{})
            {
                rotate(1);
            }
            nextRotate_ = now + std::chrono::minutes(minutes);
        }

        const auto occupied = bots::occupiedZones(poolIds_);
        // Once per tick, not once per bot. SOLO_BELOW 0 leaves the active line as it was.
        const auto soloBelow        = settings::get<uint32>("bots.SOLO_BELOW");
        const bool soloActive       = settings::get<bool>("bots.SOLO_ACTIVE");
        const bool activeEverywhere = settings::get<bool>("bots.ACTIVE_EVERYWHERE");

        // One session logout a tick: each logout saves the character, and several at once can blow the watchdog.
        bool endedSession = false;

        for (auto& bot : online_)
        {
            // Logged out this tick; a logout also leaves the session pending a zone, so check this first.
            if (bot.entity() == nullptr)
            {
                continue;
            }

            if (bot.session->blowfish.status == BLOWFISH_PENDING_ZONE)
            {
                if (!enterZone(bot))
                {
                    logout(bot, true);
                }
            }
            else if (auto* PChar = bot.entity(); PChar != nullptr)
            {
                // Anything slow enough to worry the server watchdog gets named in the log.
                const auto timed = [&](const char* what, auto&& step)
                {
                    const auto start = timer::now();
                    step();
                    if (const auto took = timer::now() - start; took > kSlowStep)
                    {
                        ShowWarningFmt("bots: {} for {} took {} ms", what, bot.name, std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
                    }
                };

                if (wanted && !endedSession && bot.sessionEnds != timer::time_point{} && now >= bot.sessionEnds && canEndSession(bot, PChar))
                {
                    ShowInfoFmt("bots: {}'s session is over", bot.name);
                    // Parked like rotate, or the next login often picks the same bot straight back (test, 2026-11-23).
                    resting_[bot.charId] = now + std::chrono::minutes(15);
                    logout(bot);
                    endedSession = true;
                    continue;
                }

                // Noted on the first tick in a zone, not in login(): early logins had no zone yet and wrote no line.
                if (!bot.loginNoted && PChar->loc.zone != nullptr)
                {
                    bot.loginNoted = true;
                    // Sessions log bots in often: a login replaces a previous login line, so the diary keeps real events.
                    if (!bot.diary.empty() && bot.diary.back().text.starts_with("logged in at "))
                    {
                        bot.diary.pop_back();
                    }
                    bots::noteEvent(bot, fmt::format("logged in at {}", bots::prettyName(PChar->loc.zone->getName())));
                }

                if (PChar->GetMLevel() != bot.preparedLevel && !PChar->PAI->IsEngaged() && PChar->loc.zone != nullptr)
                {
                    timed("prepare", [&] { bots::prepare(bot, lua); });
                }
                timed("restock", [&] { bots::restock(bot, lua, now); });
                timed("linkshell", [&] { bots::linkshellUpkeep(bot, lua, now); });
                // ACTIVE_EVERYWHERE (test): every bot plays as if a real player were in its zone, to measure what that costs.
                // SOLO_ACTIVE: a bot below SOLO_BELOW, alone in a starting area, hunts with nobody else in the zone.
                const bool soloHere = soloActive && soloBelow > 0 && !bot.citizen && !bot.withPlayer && PChar->PParty == nullptr &&
                                      static_cast<uint32>(PChar->GetMLevel()) < soloBelow && bots::inStartingArea(PChar->getZone());
                const bool active = bot.withPlayer || occupied.contains(PChar->getZone()) || activeEverywhere || soloHere;
                timed("think", [&] { bots::think(bot, *packets_, poolIds_, active, now); });
                if (bot.entity() != nullptr)
                {
                    timed("settle", [&] { bots::settle(bot); });
                }
            }
        }

        bots::handleChat(online_, poolNames_);
        bots::chatter(online_, poolNames_, now);
        bots::deliverChat(online_, *packets_);

        std::erase_if(online_,
                      [](const auto& bot)
                      {
                          return bot.entity() == nullptr;
                      });
    }

    // Least-represented job first, so the online set stays mixed.
    // Citizens (lowest char ids) log in before anyone else, and never at a camp.
    void loginNext()
    {
        const auto citizens = bots::citizenIds(pool_);
        if (!citizens.empty())
        {
            const bots::PoolEntry* citizen = nullptr;
            for (const auto& entry : pool_)
            {
                if (citizens.contains(entry.charId) && available(entry) && (citizen == nullptr || entry.charId < citizen->charId))
                {
                    citizen = &entry;
                }
            }
            if (citizen != nullptr)
            {
                bots::placeCitizen(citizen->charId);
                login(*citizen);
                return;
            }
        }

        std::map<uint8, std::size_t> jobsOnline;
        for (const auto& bot : online_)
        {
            jobsOnline[bot.job]++;
        }

        std::set<uint8> playerLevels;
        for (const auto& [zone, player] : bots::playerZones(poolIds_))
        {
            playerLevels.insert(player.level);
        }
        // Prefer a pool level within LEVEL_WINDOW of someone online. Balance uses the same window. Invites use PARTY_LEVEL_SPREAD.
        const auto window = static_cast<int>(settings::get<uint32>("bots.LEVEL_WINDOW"));
        const auto nearPlayer = [&](const bots::PoolEntry& entry)
        {
            return std::ranges::any_of(playerLevels, [&](const uint8 level) { return std::abs(static_cast<int>(entry.level) - static_cast<int>(level)) <= window; });
        };

        std::vector<const bots::PoolEntry*> candidates;
        std::size_t                         fewest   = std::numeric_limits<std::size_t>::max();
        bool                                bestNear = false;
        for (const auto& entry : pool_)
        {
            if (!available(entry) || citizens.contains(entry.charId))
            {
                continue;
            }

            const bool near  = nearPlayer(entry);
            const auto count = jobsOnline[entry.job];
            if ((near && !bestNear) || (near == bestNear && count < fewest))
            {
                fewest   = count;
                bestNear = near;
                candidates.clear();
            }
            if (near == bestNear && count == fewest)
            {
                candidates.push_back(&entry);
            }
        }

        if (candidates.empty())
        {
            return;
        }

        const auto* entry = candidates[xirand::GetRandomNumber<std::size_t>(candidates.size())];

        // Below SOLO_BELOW: a starting area, not a town and not a camp somewhere else. 0 keeps the camp / home roll.
        const auto soloBelow = settings::get<uint32>("bots.SOLO_BELOW");
        if (soloBelow > 0 && entry->level < soloBelow)
        {
            if (const auto place = bots::soloPlace(entry->level, charNation(entry->charId)))
            {
                bots::placeAt(entry->charId, *place);
                ShowInfoFmt("bots: {} solos in {} (level {})", entry->name, zoneText(place->zone), static_cast<int>(entry->level));
            }
            else
            {
                ShowWarningFmt("bots: no starting area for {} (level {}); left where they were", entry->name, static_cast<int>(entry->level));
            }
        }
        else
        {
            // Most go out to a camp for their level; the rest start at their pool home.
            const auto camps = bots::findCamps(entry->level);
            if (!camps.empty() && xirand::GetRandomNumber<uint32>(100) < settings::get<uint32>("bots.CAMP_CHANCE"))
            {
                const auto& camp = camps[xirand::GetRandomNumber<std::size_t>(camps.size())];
                bots::placeAt(entry->charId, { camp.zone, bots::spotNear(camp.zone, camp.pos, 5.0f, 25.0f) });
            }
            else
            {
                bots::placeAtHome(entry->charId);
            }
        }

        login(*entry);
    }

    // A recruiting party invites one bot at a time: same zone first, then an idle bot in another zone.
    // Tank, then healer from 15, then one support, then damage. The band is the leader's level at the start.
    void formParties()
    {
        if (!settings::get<bool>("bots.BOT_PARTIES"))
        {
            return;
        }

        const auto now              = timer::now();
        const auto minutes          = settings::get<uint32>("bots.PARTY_RECRUIT_MINUTES");
        const auto occupied         = bots::occupiedZones(poolIds_);
        const bool activeEverywhere = settings::get<bool>("bots.ACTIVE_EVERYWHERE");
        const auto soloBelow        = settings::get<uint32>("bots.SOLO_BELOW");

        const auto leveling = [&](const auto level) { return soloBelow > 0 && static_cast<uint32>(level) < soloBelow; };
        const auto idle     = [](const bots::Bot& bot)
        {
            auto* PChar = bot.entity();
            if (PChar == nullptr)
            {
                return false;
            }
            if (PChar->PAI != nullptr && (PChar->PAI->IsEngaged() || PChar->GetBattleTarget() != nullptr))
            {
                return false;
            }
            if (PChar->StatusEffectContainer != nullptr && PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
            {
                return false;
            }
            if (PChar->isInEvent() || bot.gather || bot.headingOut || bot.returnUntil != timer::time_point{})
            {
                return false;
            }
            return PChar->status == xi::Status::Normal && !PChar->isDead();
        };

        // An open party's current band. A full party is not still taking people.
        // Worked out once per leader per pass: held() asks this for every candidate, for every recruiting party.
        struct Band
        {
            bool open{};
            int  lo{};
            int  hi{};
        };
        std::map<uint32, Band> bands;
        const auto couldTake = [&](const bots::PartyRecruit& row, const int level) -> bool
        {
            auto found = bands.find(row.leaderId);
            if (found == bands.end())
            {
                Band       band{ true, static_cast<int>(row.center), static_cast<int>(row.center) };
                const auto leader = std::ranges::find_if(online_, [&](const bots::Bot& bot) { return bot.charId == row.leaderId; });
                if (leader != online_.end() && leader->entity() != nullptr && leader->entity()->PParty != nullptr)
                {
                    auto* PParty = leader->entity()->PParty;
                    band.open    = !(PParty->IsFull() || PParty->members.size() >= 6);
                    band.lo      = 255;
                    band.hi      = 0;
                    for (const auto* PMember : PParty->members)
                    {
                        band.lo = std::min(band.lo, static_cast<int>(PMember->GetMLevel()));
                        band.hi = std::max(band.hi, static_cast<int>(PMember->GetMLevel()));
                    }
                }
                found = bands.emplace(row.leaderId, band).first;
            }
            return found->second.open && bots::fitsBand(static_cast<int>(row.center), found->second.lo, found->second.hi, level);
        };
        // Same zone as this bot, and that party would still invite them. Left for it, not pulled across zones.
        const auto held = [&](const bots::Bot& candidate, const uint32 exceptId) -> bool
        {
            auto* PCand = candidate.entity();
            if (PCand == nullptr || PCand->loc.zone == nullptr)
            {
                return false;
            }
            const int level = static_cast<int>(PCand->GetMLevel());
            for (const auto other : bots::recruitLeaderIds())
            {
                if (other == exceptId)
                {
                    continue;
                }
                const auto* row = bots::recruitByLeader(other);
                if (row == nullptr || row->zone != PCand->getZone())
                {
                    continue;
                }
                if (couldTake(*row, level))
                {
                    return true;
                }
            }
            return false;
        };

        std::set<std::string> pending;
        for (const auto id : bots::recruitLeaderIds())
        {
            if (const auto* rec = bots::recruitByLeader(id))
            {
                const auto bot = std::ranges::find_if(online_, [&](const bots::Bot& candidate) { return candidate.charId == rec->leaderId; });
                if (bot != online_.end())
                {
                    for (const auto& name : bot->invites)
                    {
                        pending.insert(name);
                    }
                }
            }
        }

        // Existing parties first, so a new party doesn't take the bots they are about to invite.
        // The key stays on the bot who started it. When that bot has left, or logged out, move it to the bot who leads now.
        for (const auto id : bots::recruitLeaderIds())
        {
            auto* rec = bots::recruitByLeader(id);
            if (rec == nullptr)
            {
                continue;
            }

            const auto findBot = [&](const uint32 charId)
            {
                return std::ranges::find_if(online_, [&](const bots::Bot& candidate) { return candidate.charId == charId; });
            };
            const auto leads = [](const bots::Bot& candidate)
            {
                auto* P = candidate.entity();
                return P != nullptr && !candidate.citizen && P->PParty != nullptr && P->PParty->GetLeader() == P;
            };
            const auto solo = [&](const bots::Bot& candidate)
            {
                auto* P = candidate.entity();
                return P != nullptr && !candidate.citizen && P->PParty == nullptr && rec->partyId == 0 && rec->campAvg == 0 && !rec->atCamp;
            };
            const auto inThisParty = [&](const bots::Bot& candidate)
            {
                auto* P = candidate.entity();
                if (P == nullptr || P->PParty == nullptr)
                {
                    return false;
                }
                if (rec->partyId != 0 && P->PParty->GetPartyID() == rec->partyId)
                {
                    return true;
                }
                const auto* PLeader = P->PParty->GetLeader();
                return PLeader != nullptr && PLeader->id == id;
            };

            auto holder = findBot(id);
            if (holder == online_.end() || (!leads(*holder) && !solo(*holder)))
            {
                holder = online_.end();
                for (auto it = online_.begin(); it != online_.end(); ++it)
                {
                    if (leads(*it) && inThisParty(*it))
                    {
                        holder = it;
                        break;
                    }
                }
                if (holder == online_.end())
                {
                    bool member = false;
                    for (const auto& candidate : online_)
                    {
                        if (inThisParty(candidate))
                        {
                            member = true;
                            break;
                        }
                    }
                    if (!member)
                    {
                        bots::dropRecruit(id);
                    }
                    continue;
                }
                if (auto old = findBot(id); old != online_.end())
                {
                    old->invites.clear();
                }
                bots::recruitFor(holder->entity());
                rec = bots::recruitByLeader(holder->charId);
                if (rec == nullptr)
                {
                    continue;
                }
            }

            bots::Bot* bot   = &*holder;
            auto*      PChar = bot->entity();
            if (PChar->PParty != nullptr)
            {
                rec->partyId = PChar->PParty->GetPartyID();
            }

            const bool full = PChar->PParty != nullptr && (PChar->PParty->IsFull() || PChar->PParty->members.size() >= 6);
            if (full)
            {
                continue;
            }

            const bool heading = bot->headingOut || bot->returnUntil != timer::time_point{};
            if (!rec->atCamp && rec->campAvg == 0 && !heading && minutes > 0 && now - rec->since >= std::chrono::minutes(minutes))
            {
                ShowInfoFmt("bots: {}'s party disbands: short for {} minutes", bot->name, minutes);
                bot->invites.clear();
                if (PChar->PParty != nullptr)
                {
                    bots::disbandParty(*bot, *packets_);
                }
                bots::endRecruit(PChar->id, now);
                continue;
            }

            if (now < rec->nextInvite || !bot->invites.empty() || PChar->loc.zone == nullptr || PChar->isDead() || PChar->status != xi::Status::Normal || inCutscene(*bot))
            {
                continue;
            }

            int lo = static_cast<int>(PChar->GetMLevel());
            int hi = lo;
            if (PChar->PParty != nullptr)
            {
                lo = 255;
                hi = 0;
                for (auto* PMember : PChar->PParty->members)
                {
                    lo = std::min(lo, static_cast<int>(PMember->GetMLevel()));
                    hi = std::max(hi, static_cast<int>(PMember->GetMLevel()));
                }
            }

            const auto eligible = [&](bots::Bot& candidate, const bool otherZone)
            {
                auto* PCand = candidate.entity();
                if (PCand == nullptr || &candidate == &*bot || candidate.citizen || candidate.withPlayer || PCand->PParty != nullptr || PCand->loc.zone == nullptr ||
                    PCand->isDead() || PCand->status != xi::Status::Normal || leveling(PCand->GetMLevel()) || bots::recruitByLeader(candidate.charId) != nullptr ||
                    pending.contains(candidate.name) || !bots::fitsBand(rec->center, lo, hi, static_cast<int>(PCand->GetMLevel())))
                {
                    return false;
                }
                if (!otherZone)
                {
                    return PCand->getZone() == rec->zone;
                }
                // Idle, and not someone a party already recruiting in their own zone can still take.
                return PCand->getZone() != rec->zone && idle(candidate) && !held(candidate, PChar->id);
            };

            std::vector<bots::Bot*> same;
            std::vector<bots::Bot*> far;
            for (auto& candidate : online_)
            {
                if (eligible(candidate, false))
                {
                    same.push_back(&candidate);
                }
                else if (eligible(candidate, true))
                {
                    far.push_back(&candidate);
                }
            }

            // Same zone first. Roles come from everyone in the party, players included.
            Roles folded;
            bool  healerTaken = false;
            const auto addJob = [&](const CBattleEntity* PMember)
            {
                folded.highest = std::max(folded.highest, static_cast<int>(PMember->GetMLevel()));
                const auto job = PMember->GetMJob();
                if (bots::isTank(job))
                {
                    folded.tank = true;
                }
                if (healerRank(job) > 0 && !healerTaken)
                {
                    healerTaken = true;
                }
                else if (isSupportJob(job))
                {
                    folded.support = true;
                }
            };
            if (PChar->PParty == nullptr)
            {
                addJob(PChar);
            }
            else
            {
                for (auto* PMember : PChar->PParty->members)
                {
                    addJob(PMember);
                }
            }
            folded.healer = healerTaken;

            bots::Bot* pick = nullptr;
            if (PChar->PParty == nullptr)
            {
                auto pool = same;
                pool.push_back(&*bot);
                auto group = partyFrom(&*bot, pool);
                if (group.size() >= 2 && group.front() == &*bot)
                {
                    pick = group[1];
                }
            }
            if (pick == nullptr)
            {
                auto banded = nearLeader(&*bot, same, rec->center, lo, hi);
                std::erase(banded, &*bot);
                pick = pickRole(folded, banded);
                if (pick == nullptr)
                {
                    auto away = nearLeader(&*bot, far, rec->center, lo, hi);
                    std::erase(away, &*bot);
                    pick = pickRole(folded, away);
                }
            }
            if (pick != nullptr)
            {
                bot->invites.push_back(pick->name);
                pending.insert(pick->name);
            }
            bots::recruitInvited(PChar->id, now);
        }

        // One new party per pass. Bots a party in this zone can still take are left to it. A lone bot waits if any open party fits them.
        std::map<xi::ZoneId, std::vector<bots::Bot*>> byZone;
        for (auto& bot : online_)
        {
            auto* PChar = bot.entity();
            if (PChar == nullptr || bot.citizen || bot.withPlayer || PChar->PParty != nullptr || PChar->loc.zone == nullptr || PChar->isDead() ||
                PChar->status != xi::Status::Normal || leveling(PChar->GetMLevel()) || bots::recruitByLeader(bot.charId) != nullptr || inCutscene(bot))
            {
                continue;
            }
            byZone[PChar->getZone()].push_back(&bot);
        }

        xi::ZoneId bestZone{};
        int        bestCount = 0;
        for (auto& [zone, bots] : byZone)
        {
            if (!activeEverywhere && !occupied.contains(zone))
            {
                continue;
            }
            std::erase_if(bots, [&](const bots::Bot* candidate) { return held(*candidate, 0) || bots::leaderCooling(candidate->charId, now); });
            if (static_cast<int>(bots.size()) > bestCount)
            {
                bestCount = static_cast<int>(bots.size());
                bestZone  = zone;
            }
        }
        if (bestCount == 0)
        {
            return;
        }
        if (bestCount < 2)
        {
            auto*       only  = byZone[bestZone].front();
            const auto* POnly = only->entity();
            if (POnly != nullptr)
            {
                const int level = static_cast<int>(POnly->GetMLevel());
                for (const auto other : bots::recruitLeaderIds())
                {
                    if (const auto* row = bots::recruitByLeader(other); row != nullptr && couldTake(*row, level))
                    {
                        return;
                    }
                }
            }
        }

        auto seeds = byZone[bestZone];
        if (seeds.empty())
        {
            return;
        }

        auto* leader = chooseLeader(seeds);
        if (leader == nullptr || leader->entity() == nullptr)
        {
            return;
        }
        bots::beginRecruit(leader->entity(), now);
        const int center = static_cast<int>(leader->entity()->GetMLevel());
        const int spread = bots::partySpread();
        ShowInfoFmt("bots: {} recruiting (band {}-{}) in zone {}", leader->name, std::max(1, center - spread), std::min(75, center + spread),
                    zoneText(leader->entity()->getZone()));
    }

    // One seeking player, told once, then invited a few seconds later. A recruiting party asks while it is short.
    // A party already at a camp, or in a fight, still asks too. The level check is the party band, not LEVEL_WINDOW.
    void askSeekers(const timer::time_point now)
    {
        if (!settings::get<bool>("bots.INVITE_SEEKERS"))
        {
            return;
        }

        struct Offer
        {
            bots::Bot* leader{};
            uint8      lo{};
            uint8      hi{};
            uint8      center{};
            bool       needTank{};
            bool       needHealer{};
            bool       needSupport{};
            bool       needDamage{};
            bool       recruiting{};
        };

        std::vector<Offer> offers;
        for (auto& bot : online_)
        {
            auto* PChar = bot.entity();
            if (PChar == nullptr || bot.citizen || PChar->loc.zone == nullptr || !bot.invites.empty())
            {
                continue;
            }
            if (PChar->PParty != nullptr && (PChar->PParty->GetLeader() != PChar || PChar->PParty->IsFull() || PChar->PParty->members.size() >= 6))
            {
                continue;
            }

            const bool recruiting = bots::recruitFor(PChar) != nullptr && (PChar->PParty == nullptr || PChar->PParty->GetLeader() == PChar);
            if (!recruiting)
            {
                if (PChar->PParty == nullptr)
                {
                    continue;
                }
                if (const auto asked = leaderAsked_.find(bot.charId); asked != leaderAsked_.end() && now < asked->second)
                {
                    continue;
                }
                const bool hunting = std::ranges::any_of(PChar->PParty->members, [](const CBattleEntity* PMember)
                                                         {
                                                             return PMember->PAI->IsEngaged() && PMember->GetBattleTarget() != nullptr && PMember->GetBattleTarget()->objtype == TYPE_MOB;
                                                         });
                if (!bots::campSpot(PChar) && !hunting)
                {
                    continue;
                }
            }

            bool  hasTank     = false;
            bool  healerTaken = false;
            bool  hasSupport  = false;
            uint8 lo          = static_cast<uint8>(PChar->GetMLevel());
            uint8 hi          = lo;
            const auto note   = [&](const CBattleEntity* PMember)
            {
                lo = std::min(lo, static_cast<uint8>(PMember->GetMLevel()));
                hi = std::max(hi, static_cast<uint8>(PMember->GetMLevel()));
                const auto job = PMember->GetMJob();
                if (bots::isTank(job))
                {
                    hasTank = true;
                }
                if (healerRank(job) > 0 && !healerTaken)
                {
                    healerTaken = true;
                }
                else if (isSupportJob(job))
                {
                    hasSupport = true;
                }
            };
            if (PChar->PParty == nullptr)
            {
                note(PChar);
            }
            else
            {
                lo = 255;
                hi = 0;
                for (auto* PMember : PChar->PParty->members)
                {
                    note(PMember);
                }
            }

            const auto* rec    = bots::recruitFor(PChar);
            const auto  center = static_cast<uint8>(rec != nullptr ? rec->center : PChar->GetMLevel());
            const bool  wantHealer = std::max(lo, hi) >= kHealerLevel && !healerTaken;
            offers.push_back(Offer{ &bot, lo, hi, center, !hasTank, wantHealer, !hasSupport, true, recruiting });
        }
        if (offers.empty())
        {
            return;
        }

        std::vector<CCharEntity*> seekers;
        for (auto& zone : g_PZoneList)
        {
            auto& PZone = zone.second;
            if (PZone == nullptr || (static_cast<uint16>(PZone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::Instanced)) != 0)
            {
                continue;
            }

            PZone->ForEachChar(
                [&](CCharEntity* PChar)
                {
                    if (bots::isPoolChar(PChar->id) || PChar->inMogHouse() || PChar->status != xi::Status::Normal || PChar->PParty != nullptr || PChar->isDead() ||
                        PChar->PAI->IsEngaged() || !PChar->isSeekingParty() || bots::invitesRefused(PChar->id))
                    {
                        return;
                    }
                    if (const auto asked = seekerAsked_.find(PChar->id); asked != seekerAsked_.end() && now < asked->second)
                    {
                        return;
                    }
                    seekers.push_back(PChar);
                });
        }
        if (seekers.empty())
        {
            return;
        }

        std::ranges::shuffle(offers, std::mt19937{ xirand::GetRandomNumber<uint32>(std::numeric_limits<uint32>::max()) });
        std::ranges::shuffle(seekers, std::mt19937{ xirand::GetRandomNumber<uint32>(std::numeric_limits<uint32>::max()) });

        for (const auto& offer : offers)
        {
            auto* PLeader = offer.leader->entity();
            if (PLeader == nullptr || PLeader->loc.zone == nullptr)
            {
                continue;
            }

            // One tell. A missing tank beats a missing healer, then a support, then another damage dealer.
            CCharEntity* picked = nullptr;
            int          place  = 4;
            for (auto* PPlayer : seekers)
            {
                const auto level = static_cast<int>(PPlayer->GetMLevel());
                if (!bots::fitsBand(offer.center, offer.lo, offer.hi, level))
                {
                    continue;
                }

                const auto job     = PPlayer->GetMJob();
                const bool tank    = bots::isTank(job);
                const bool healer  = healerRank(job) > 0;
                const bool support = isSupportJob(job);
                int        role    = 4;
                if (tank && offer.needTank)
                {
                    role = 0;
                }
                else if (healer && offer.needHealer)
                {
                    role = 1;
                }
                else if (support && offer.needSupport)
                {
                    role = 2;
                }
                else if (offer.needDamage)
                {
                    role = 3;
                }
                if (role < place)
                {
                    picked = PPlayer;
                    place  = role;
                }
            }
            if (picked == nullptr)
            {
                continue;
            }

            const auto band = offer.lo == offer.hi ? fmt::format("{}", offer.lo) : fmt::format("{}-{}", offer.lo, offer.hi);
            const auto line = fmt::format("hi, need a {} for {} in {}, want to join?", jobAbbr(picked->GetMJob()), band,
                                          bots::lowerCase(bots::prettyName(PLeader->loc.zone->getName())));
            bots::tell(*offer.leader, picked->getName(), line);
            dueInvites_.push_back(DueInvite{ offer.leader->charId, picked->getName(), now + kSeekInviteDelay });
            seekerAsked_[picked->id] = now + kSeekerCooldown;
            // A party still filling can ask another player next time. Each player is still asked once per 15 minutes.
            if (!offer.recruiting)
            {
                leaderAsked_[offer.leader->charId] = now + kLeaderCooldown;
            }
            ShowInfoFmt("bots: {} asks {}: {}", offer.leader->name, picked->getName(), line);
            return;
        }
    }

    // The invite goes out a few seconds after the tell, through the same queue formParties uses.
    void sendDueInvites(const timer::time_point now)
    {
        std::erase_if(dueInvites_,
                      [&](const DueInvite& due)
                      {
                          if (now < due.when)
                          {
                              return false;
                          }

                          const auto bot = std::ranges::find_if(online_, [&](const bots::Bot& candidate) { return candidate.charId == due.charId; });
                          if (bot != online_.end() && !bot->citizen && bot->entity() != nullptr)
                          {
                              auto*       PLeader = bot->entity();
                              const auto* PPlayer = zoneutils::GetCharByName(due.name);
                              const bool  canLead = PLeader->PParty == nullptr || (PLeader->PParty->GetLeader() == PLeader && !PLeader->PParty->IsFull());
                              int         lo      = static_cast<int>(PLeader->GetMLevel());
                              int         hi      = lo;
                              int         center  = lo;
                              if (const auto* rec = bots::recruitFor(PLeader))
                              {
                                  center = static_cast<int>(rec->center);
                              }
                              if (PLeader->PParty != nullptr)
                              {
                                  lo = 255;
                                  hi = 0;
                                  for (const auto* PMember : PLeader->PParty->members)
                                  {
                                      lo = std::min(lo, static_cast<int>(PMember->GetMLevel()));
                                      hi = std::max(hi, static_cast<int>(PMember->GetMLevel()));
                                  }
                              }
                              const bool inBand = PPlayer != nullptr && bots::fitsBand(center, lo, hi, static_cast<int>(PPlayer->GetMLevel()));
                              if (PPlayer != nullptr && PPlayer->PParty == nullptr && !bots::invitesRefused(PPlayer->id) && canLead && inBand &&
                                  !std::ranges::contains(bot->invites, due.name))
                              {
                                  bot->invites.push_back(due.name);
                              }
                          }
                          return true;
                      });
    }

    // Bots rotated out sit out a while, unless nobody else is left.
    auto available(const bots::PoolEntry& entry) const -> bool
    {
        if (isOnline(entry.charId))
        {
            return false;
        }

        const auto rested = resting_.find(entry.charId);
        return rested == resting_.end() || timer::now() >= rested->second || !hasUnrested();
    }

    // Keeps a few free bots in every zone a real player is in: log one in there, or bring one over from an empty zone.
    void balance()
    {
        const auto wantedPerZone = settings::get<uint32>("bots.BOTS_PER_PLAYER_ZONE");
        if (wantedPerZone == 0)
        {
            return;
        }

        const auto window   = static_cast<int>(settings::get<uint32>("bots.LEVEL_WINDOW"));
        const auto leeway   = static_cast<float>(settings::get<uint32>("bots.PLAYER_LEEWAY"));
        const auto zones    = bots::playerZones(poolIds_);
        const auto citizens = bots::citizenIds(pool_);
        const auto soloBelow = settings::get<uint32>("bots.SOLO_BELOW");
        // A bot under SOLO_BELOW is out leveling. Don't log it into a player's zone or haul it out of a starting area.
        const auto leveling = [&](const auto level) { return soloBelow > 0 && static_cast<uint32>(level) < soloBelow; };
        const bool citizenWaiting = std::ranges::any_of(pool_, [&](const auto& entry) { return citizens.contains(entry.charId) && available(entry); });

        for (const auto& [zone, players] : zones)
        {
            const auto here = std::ranges::count_if(online_,
                                                    [&](const auto& bot)
                                                    {
                                                        auto* PChar = bot.entity();
                                                        return PChar != nullptr && !bot.citizen && !bot.withPlayer && PChar->getZone() == zone && !leveling(PChar->GetMLevel());
                                                    });
            if (static_cast<uint32>(here) >= wantedPerZone)
            {
                continue;
            }

            // At least PLAYER_LEEWAY from the player this spot is anchored on, then reject a point that
            // still landed on another real player (or on the anchor, when no navmesh point turned up).
            const float minRadius = std::max<float>(25.0f, leeway);
            const float maxRadius = std::max<float>(70.0f, minRadius + 30.0f);
            bots::Place place{ zone, players.pos };
            bool        clear = false;
            for (int attempt = 0; attempt < 4 && !clear; ++attempt)
            {
                place.pos = bots::spotNear(zone, players.pos, minRadius, maxRadius);
                clear     = !bots::nearRealPlayer(zone, place.pos, leeway);
            }
            if (!clear)
            {
                continue;
            }

            const auto gap = [&](const uint8 level)
            {
                return std::abs(static_cast<int>(level) - static_cast<int>(players.level));
            };

            if (!citizenWaiting && online_.size() < settings::get<uint32>("bots.MAX_ONLINE"))
            {
                const bots::PoolEntry* best = nullptr;
                for (const auto& entry : pool_)
                {
                    if (available(entry) && !citizens.contains(entry.charId) && !leveling(entry.level) && (best == nullptr || gap(entry.level) < gap(best->level)))
                    {
                        best = &entry;
                    }
                }

                // Closest pool level is still too far: leave the zone under-filled rather than send them.
                if (best != nullptr && gap(best->level) <= window)
                {
                    bots::placeAt(best->charId, place);
                    login(*best);
                    continue;
                }
            }

            bots::Bot* best = nullptr;
            for (auto& bot : online_)
            {
                auto* PChar = bot.entity();
                if (isFree(bot) && !bot.citizen && !inCutscene(bot) && !leveling(PChar->GetMLevel()) && PChar->loc.zone != nullptr && PChar->status == xi::Status::Normal && !zones.contains(PChar->getZone()) &&
                    (best == nullptr || gap(PChar->GetMLevel()) < gap(best->entity()->GetMLevel())))
                {
                    best = &bot;
                }
            }

            if (best != nullptr && gap(best->entity()->GetMLevel()) <= window)
            {
                ShowInfoFmt("bots: {} heads to {}", best->name, zone);
                bots::travel(best->entity(), place.zone, place.pos);
            }
        }
    }

    void login(const bots::PoolEntry& entry_)
    {
        const auto* entry = &entry_;

        bots::Bot bot{
            .charId = entry->charId,
            .accId  = entry->accId,
            .name   = entry->name,
            .job    = entry->job,
        };
        bot.citizen = bots::citizenIds(pool_).contains(entry->charId);
        if (bot.citizen)
        {
            bots::noteCitizenLogin(bot.charId);
        }

        bot.session                   = std::make_unique<MapSession>();
        bot.session->scheduler        = scheduler_;
        bot.session->client_ipp       = IPP(0, static_cast<uint16>(entry->charId));
        bot.session->charID           = entry->charId;
        bot.session->accountID        = entry->accId;
        bot.session->blowfish.status  = BLOWFISH_WAITING;

        bots::loadDiary(bot);
        if (!enterZone(bot))
        {
            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
            return;
        }

        // After enterZone's prepare, never inside LoadChar. A gain re-gears for the new level.
        if (bots::catchUp(bot) > 0)
        {
            if (const auto pooled = std::ranges::find(pool_, bot.charId, &bots::PoolEntry::charId); pooled != pool_.end())
            {
                if (auto* PChar = bot.entity())
                {
                    pooled->level = PChar->GetMLevel();
                }
            }
            bots::prepare(bot, lua);
        }

        const auto minutes = bot.citizen ? std::optional<uint32>{} : sessionMinutes();
        if (minutes)
        {
            bot.sessionEnds = timer::now() + std::chrono::minutes(*minutes);
            ShowInfoFmt("bots: {} logged in for {} minutes ({} online)", bot.name, *minutes, online_.size() + 1);
        }
        else
        {
            ShowInfoFmt("bots: {} logged in ({} online)", bot.name, online_.size() + 1);
        }
        online_.push_back(std::move(bot));
    }

    // Loads the character and runs the client's zone-in, for the first login and for every zone change.
    auto enterZone(bots::Bot& bot) -> bool
    {
        auto PChar = charutils::LoadChar(bot.charId);
        if (PChar == nullptr)
        {
            ShowErrorFmt("bots: could not load {}", bot.name);
            return false;
        }

        // LSB runs xi.player.charCreate on any login without the New Adventurer title. Its starter gear only covers
        // the six starting jobs, so a bot created as any other job errors there on every login and never gets the
        // title (issue #3). Those bots are marked set up before the login packet; starting jobs get the normal first login.
        constexpr uint16 kNewAdventurer = 206;
        const auto       mainJob        = PChar->GetMJob();
        const bool       startingJob    = mainJob == xi::Job::WAR || mainJob == xi::Job::MNK || mainJob == xi::Job::WHM || mainJob == xi::Job::BLM ||
                                 mainJob == xi::Job::RDM || mainJob == xi::Job::THF;
        if (!startingJob && !charutils::hasTitle(PChar.get(), kNewAdventurer))
        {
            charutils::addTitle(PChar.get(), kNewAdventurer);
            charutils::SaveTitles(PChar.get());
        }

        auto* PZone = zoneutils::GetZone(PChar->loc.destination);
        if (PZone == nullptr)
        {
            ShowErrorFmt("bots: {} is in zone {}, which this map server doesn't run", bot.name, PChar->loc.destination);
            return false;
        }

        const bool loggingIn = bot.session->blowfish.status == BLOWFISH_WAITING;
        if (loggingIn)
        {
            std::array<uint8, 20> key{};
            for (auto& byte : key)
            {
                byte = xirand::GetRandomNumber<uint8>(255);
            }

            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
            if (!db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, session_key, server_addr, server_port, client_addr, client_port) "
                                  "VALUES (?, ?, ?, ?, ?, 0, ?)",
                                  bot.accId, bot.charId, key, PZone->GetIP(), PZone->GetPort(), bot.session->client_ipp.getPort()))
            {
                ShowErrorFmt("bots: could not create a session for {}", bot.name);
                return false;
            }
        }

        PChar->PSession       = bot.session.get();
        PChar->PAI->PathFind  = bots::makePathFind(PChar.get());
        bot.session->PChar    = std::move(PChar);

        auto  packet          = bots::makePacket<GP_CLI_COMMAND_LOGIN>();
        packet->as<GP_CLI_COMMAND_LOGIN>()->UniqueNo = bot.charId;
        bots::send(bot, *packets_, *packet);

        auto* PEntity = bot.entity();
        if (PEntity->loc.zone == nullptr)
        {
            ShowErrorFmt("bots: {} did not get into zone {}", bot.name, PEntity->loc.destination);
            return false;
        }

        bot.home = PEntity->loc.p;

        if (loggingIn)
        {
            bots::prepare(bot, lua);
        }

        // Rested while offline; also covers characters created at a level above their stored HP.
        if (loggingIn && !PEntity->isDead())
        {
            PEntity->addHP(PEntity->GetMaxHP());
            PEntity->addMP(PEntity->GetMaxMP());
        }

        // The character was just loaded; the next think sends the seeking flag again.
        bot.seekingApplied = false;

        return true;
    }

    // Mirrors the session cleanup a timed-out client gets.
    // force is the failed zone-in path: the character never finished arriving, so it cannot wait out a cutscene.
    void logout(bots::Bot& bot, const bool force = false)
    {
        auto* PChar = bot.entity();
        if (!force && inCutscene(bot))
        {
            return;
        }

        if (PChar != nullptr && PChar->loc.zone != nullptr)
        {
            // Bots level in parties; the next placement goes by the new level.
            if (const auto entry = std::ranges::find(pool_, bot.charId, &bots::PoolEntry::charId); entry != pool_.end())
            {
                entry->level = PChar->GetMLevel();
            }

            persist::flush(PChar, IsLogout::Yes);
            charutils::SaveCharPosition(PChar);

            if (PChar->PPet != nullptr && PChar->PPet->objtype == TYPE_MOB)
            {
                petutils::DespawnPet(PChar);
            }

            PChar->status = xi::Status::Shutdown;
            charutils::removeCharFromZone(PChar);
        }

        if (PChar != nullptr)
        {
            bots::breakCamp(PChar);
        }
        bots::dropOuting(bot.charId);
        // Another bot is still in this party: keep the row so the next formParties moves it onto whoever leads.
        bool keepRecruit = false;
        if (PChar != nullptr && PChar->PParty != nullptr && bots::recruitByLeader(bot.charId) != nullptr)
        {
            for (const auto* PMember : PChar->PParty->members)
            {
                if (PMember != PChar && PMember->objtype == TYPE_PC && bots::isPoolChar(PMember->id))
                {
                    keepRecruit = true;
                    break;
                }
            }
            if (keepRecruit)
            {
                if (auto* rec = bots::recruitByLeader(bot.charId); rec != nullptr && rec->partyId == 0)
                {
                    rec->partyId = PChar->PParty->GetPartyID();
                }
            }
        }
        if (!keepRecruit)
        {
            bots::dropRecruit(bot.charId);
        }

        bots::noteLastSeen(bot);
        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
        bot.session->PChar.reset();

        ShowInfoFmt("bots: {} logged out ({} online)", bot.name, online_.size() - 1);
    }

    // Inclusive minutes in [SESSION_MIN, SESSION_MAX]. Null when SESSION_MAX is 0 (sessions off).
    auto sessionMinutes() const -> std::optional<uint32>
    {
        auto lo = settings::get<uint32>("bots.SESSION_MIN");
        auto hi = settings::get<uint32>("bots.SESSION_MAX");
        if (hi == 0)
        {
            return std::nullopt;
        }
        if (hi < lo)
        {
            const auto tmp = lo;
            lo             = hi;
            hi             = tmp;
        }

        const auto span = hi - lo;
        return span == 0 ? lo : lo + xirand::GetRandomNumber<uint32>(span + 1);
    }

    // A session that has already ended may log the bot out. Same bar as rotate (no party), and not
    // mid-fight. withPlayer keeps a real player's party from ever timing out, including while they zone.
    // returnUntil is the same bar: a party walking back from a wipe is not free.
    auto canEndSession(const bots::Bot& bot, const CCharEntity* PChar) const -> bool
    {
        if (bot.citizen || bot.withPlayer || !isFree(bot))
        {
            return false;
        }
        if (PChar->PAI == nullptr || PChar->isDead() || PChar->PAI->IsEngaged() || inCutscene(bot))
        {
            return false;
        }
        return PChar->status != xi::Status::Disappear && PChar->status != xi::Status::Shutdown;
    }

    // Not in a party. Walking back from a wipe counts as not free too: rotate, balance, and session
    // logout all use this, and a zone tick can drop PParty.
    static auto isFree(const bots::Bot& bot) -> bool
    {
        const auto* PChar = bot.entity();
        // A leader standing alone while the first invite is out is not free: balance and rotate would walk them off the spot.
        return PChar != nullptr && PChar->PParty == nullptr && bot.returnUntil == timer::time_point{} && bots::recruitByLeader(bot.charId) == nullptr;
    }

    auto isOnline(const uint32 charId) const -> bool
    {
        return std::ranges::any_of(online_,
                                   [&](const auto& bot)
                                   {
                                       return bot.charId == charId && bot.entity() != nullptr;
                                   });
    }

    auto hasUnrested() const -> bool
    {
        const auto now = timer::now();
        return std::ranges::any_of(pool_,
                                   [&](const auto& entry)
                                   {
                                       const auto rested = resting_.find(entry.charId);
                                       return !isOnline(entry.charId) && (rested == resting_.end() || now >= rested->second);
                                   });
    }

    // Logs out up to count bots that aren't in a party (0: all of them); replacements follow at the login interval.
    auto rotate(const uint32 count) -> std::vector<std::string>
    {
        std::vector<bots::Bot*> free;
        for (auto& bot : online_)
        {
            if (isFree(bot) && !bot.citizen && !inCutscene(bot))
            {
                free.push_back(&bot);
            }
        }

        std::ranges::shuffle(free, std::mt19937{ xirand::GetRandomNumber<uint32>(std::numeric_limits<uint32>::max()) });
        if (count > 0 && free.size() > count)
        {
            free.resize(count);
        }

        std::vector<std::string> names;
        for (auto* bot : free)
        {
            names.push_back(bot->name);
            resting_[bot->charId] = timer::now() + std::chrono::minutes(15);
            logout(*bot);
        }

        nextLogin_ = timer::now();
        if (names.empty())
        {
            return { "No bots outside a party to rotate." };
        }

        std::vector<std::string> lines{ fmt::format("Rotating out {} bots; others will log in to replace them:", names.size()) };
        for (std::size_t i = 0; i < names.size(); i += 5)
        {
            const auto end = std::min(names.size(), i + 5);
            lines.push_back(fmt::format("  {}", fmt::join(names.begin() + i, names.begin() + end, ", ")));
        }
        return lines;
    }

    // One line per bot, so the chat log doesn't cut it off.
    auto status() const -> std::vector<std::string>
    {
        static constexpr std::array<const char*, 23> jobs = {
            "",    "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
            "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN",
        };

        std::vector<std::string> lines;
        for (const auto& bot : online_)
        {
            const auto* PChar = bot.entity();
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                continue;
            }

            auto zone = PChar->loc.zone->getName();
            std::ranges::replace(zone, '_', ' ');

            const auto job = static_cast<std::size_t>(PChar->GetMJob());
            lines.push_back(fmt::format("  {} - {}{} - {}{}", bot.name, job < jobs.size() ? jobs[job] : "?", PChar->GetMLevel(), zone,
                                        PChar->PParty ? " (in a party)" : ""));
        }

        lines.insert(lines.begin(), fmt::format("Bots online: {} of {}", lines.size(), pool_.size()));
        return lines;
    }

    int64                         tickCount_{};
    timer::duration               tickTotal_{};
    timer::duration               tickWorst_{};
    timer::time_point             tickReported_{};
    bool                          enabled_{ false };
    Scheduler*                    scheduler_{ nullptr };
    std::optional<Scheduler::Token> tickToken_;
    std::unique_ptr<PacketSystem> packets_; // its rate limiter reads Lua settings, so not at static init
    std::vector<bots::PoolEntry>  pool_;
    std::set<uint32>              poolIds_;
    std::set<std::string>         poolNames_;
    std::vector<bots::Bot>        online_;
    timer::time_point             nextPresenceCheck_{};
    timer::time_point             lastRealPlayer_{};
    timer::time_point             nextLogin_{};
    timer::time_point             nextLogout_{};
    timer::time_point             nextRotate_{};
    timer::time_point             nextBalance_{};
    timer::time_point             nextGrouping_{};
    timer::time_point             nextSeek_{};
    struct DueInvite
    {
        uint32            charId{};
        std::string       name;
        timer::time_point when{};
    };
    std::vector<DueInvite>              dueInvites_;
    std::map<uint32, timer::time_point> seekerAsked_; // real player char id, not asked again until this time
    std::map<uint32, timer::time_point> leaderAsked_; // bot leader char id
    std::map<uint32, timer::time_point> resting_; // rotated-out bots and when they may return
};

REGISTER_CPP_MODULE(BotModule);
