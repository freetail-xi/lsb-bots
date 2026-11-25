//
// Bot pool: the bot_pool table, and the accounts and characters behind it.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"

#include "map/entities/char_entity.h"
#include "map/utils/charutils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <map>
#include <string_view>

#include <fmt/format.h>

namespace bots
{

namespace
{

// char_jobs columns, indexed by job id
constexpr std::array<std::string_view, 23> jobColumns = {
    "",
    "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
    "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run",
};

// A job's level, unlocked and genkai, indexed by job id - 1 (queries must be literals)
constexpr std::array<db::Query, 22> jobLevelQueries = {
    "UPDATE char_jobs SET unlocked = unlocked | ?, war = ?, genkai = GREATEST(genkai, war) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, mnk = ?, genkai = GREATEST(genkai, mnk) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, whm = ?, genkai = GREATEST(genkai, whm) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, blm = ?, genkai = GREATEST(genkai, blm) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, rdm = ?, genkai = GREATEST(genkai, rdm) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, thf = ?, genkai = GREATEST(genkai, thf) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, pld = ?, genkai = GREATEST(genkai, pld) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, drk = ?, genkai = GREATEST(genkai, drk) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, bst = ?, genkai = GREATEST(genkai, bst) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, brd = ?, genkai = GREATEST(genkai, brd) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, rng = ?, genkai = GREATEST(genkai, rng) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, sam = ?, genkai = GREATEST(genkai, sam) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, nin = ?, genkai = GREATEST(genkai, nin) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, drg = ?, genkai = GREATEST(genkai, drg) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, smn = ?, genkai = GREATEST(genkai, smn) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, blu = ?, genkai = GREATEST(genkai, blu) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, cor = ?, genkai = GREATEST(genkai, cor) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, pup = ?, genkai = GREATEST(genkai, pup) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, dnc = ?, genkai = GREATEST(genkai, dnc) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, sch = ?, genkai = GREATEST(genkai, sch) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, geo = ?, genkai = GREATEST(genkai, geo) WHERE charid = ?",
    "UPDATE char_jobs SET unlocked = unlocked | ?, run = ?, genkai = GREATEST(genkai, run) WHERE charid = ?",
};

auto nextId(const db::Query query) -> uint32
{
    const auto rset = db::preparedStmt(db::selected(query));
    FOR_DB_SINGLE_RESULT(rset)
    {
        return rset->get<uint32>("id") + 1;
    }

    return 0;
}

auto nameTaken(const std::string& name) -> bool
{
    const auto rset = db::preparedStmt("SELECT charid FROM chars WHERE charname = ? LIMIT 1", name);
    return !rset || rset->rowsCount() > 0;
}

// Mirrors loginHelpers::saveCharacter, plus the level, position and home point the pool row asks for.
auto createCharacter(const std::string& name) -> bool
{
    const auto rset = db::preparedStmt("SELECT race, face, size, nation, job, level, zone, pos_x, pos_y, pos_z, pos_rot "
                                       "FROM bot_pool WHERE name = ?",
                                       name);
    if (!rset || !rset->next())
    {
        return false;
    }

    const auto race   = rset->get<uint8>("race");
    const auto face   = rset->get<uint8>("face");
    const auto size   = rset->get<uint8>("size");
    const auto nation = rset->get<uint8>("nation");
    const auto job    = rset->get<uint8>("job");
    const auto level  = rset->get<uint8>("level");
    const auto zone   = rset->get<uint16>("zone");
    const auto x      = rset->get<float>("pos_x");
    const auto y      = rset->get<float>("pos_y");
    const auto z      = rset->get<float>("pos_z");
    const auto rot    = rset->get<uint8>("pos_rot");

    if (job == 0 || job >= jobColumns.size())
    {
        ShowErrorFmt("bots: {} has invalid job {}", name, job);
        return false;
    }

    if (nameTaken(name))
    {
        ShowErrorFmt("bots: the name {} is already taken by another character", name);
        return false;
    }

    const auto accId  = nextId("SELECT MAX(id) AS id FROM accounts");
    const auto charId = nextId("SELECT MAX(charid) AS id FROM chars");
    if (accId == 0 || charId == 0)
    {
        return false;
    }

    // status 0 and an impossible hash: the login server refuses this account.
    if (!db::preparedStmt("INSERT INTO accounts (id, login, password, timecreate, status) VALUES (?, ?, '!', NOW(), 0)",
                          accId,
                          fmt::format("bot{}", accId)))
    {
        return false;
    }

    const bool ok =
        db::preparedStmt("INSERT INTO chars (charid, accid, charname, pos_zone, nation, pos_x, pos_y, pos_z, pos_rot, "
                         "home_zone, home_x, home_y, home_z, home_rot) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                         charId, accId, name, zone, nation, x, y, z, rot, zone, x, y, z, rot) &&
        db::preparedStmt("INSERT INTO char_look (charid, face, race, size) VALUES (?, ?, ?, ?)", charId, face, race, size) &&
        db::preparedStmt("INSERT INTO char_stats (charid, mjob) VALUES (?, ?)", charId, job) &&
        // The chars insert trigger already made most of these rows.
        db::preparedStmt("INSERT INTO char_exp (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_flags (charid) VALUES (?) ON DUPLICATE KEY UPDATE disconnecting = disconnecting", charId) &&
        db::preparedStmt("INSERT INTO char_jobs (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_points (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_unlocks (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_profile (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_storage (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("DELETE FROM char_inventory WHERE charid = ?", charId) &&
        db::preparedStmt("INSERT INTO char_inventory (charid) VALUES (?)", charId) &&
        db::preparedStmt(db::selected(jobLevelQueries[job - 1]), 1u << job, level, charId) &&
        // char_stats.mlvl stays 1 until the first login saves it, and loadPool reads it: set it now.
        db::preparedStmt("UPDATE char_stats SET mlvl = ? WHERE charid = ?", level, charId) &&
        db::preparedStmt("UPDATE bot_pool SET charid = ?, accid = ? WHERE name = ?", charId, accId, name);

    if (!ok)
    {
        ShowErrorFmt("bots: creating {} failed part way (account {}, char {})", name, accId, charId);
        return false;
    }

    ShowInfoFmt("bots: created {} (account {}, char {}, job {} level {})", name, accId, charId, jobColumns[job], level);
    return true;
}

} // namespace

void ensureSchema()
{
    db::preparedStmt("CREATE TABLE IF NOT EXISTS bot_pool ("
                     "name varchar(15) NOT NULL, "
                     "enabled tinyint(1) unsigned NOT NULL DEFAULT 1, "
                     "charid int(10) unsigned NOT NULL DEFAULT 0, "
                     "accid int(10) unsigned NOT NULL DEFAULT 0, "
                     "race tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "face tinyint(2) unsigned NOT NULL DEFAULT 0, "
                     "size tinyint(1) unsigned NOT NULL DEFAULT 0, "
                     "nation tinyint(1) unsigned NOT NULL DEFAULT 0, "
                     "job tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "level tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "zone smallint(3) unsigned NOT NULL DEFAULT 230, "
                     "pos_x float NOT NULL DEFAULT 0, "
                     "pos_y float NOT NULL DEFAULT 0, "
                     "pos_z float NOT NULL DEFAULT 0, "
                     "pos_rot tinyint(3) unsigned NOT NULL DEFAULT 0, "
                     "personality text NULL, "
                     "chattiness tinyint(3) unsigned NULL, "
                     "diary text NULL, "
                     "last_seen int(10) unsigned NULL, "
                     "play_style tinyint(3) unsigned NULL, "
                     "PRIMARY KEY (name)"
                     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");

    // Added after the table first shipped.
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS chattiness tinyint(3) unsigned NULL AFTER personality");
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS subjob tinyint(2) unsigned NULL AFTER level");
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS diary text NULL AFTER chattiness");
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS last_seen int(10) unsigned NULL AFTER diary");
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS play_style tinyint(3) unsigned NULL AFTER last_seen");
}

namespace
{
std::map<uint32, std::string> diaryStored; // bot_pool.diary as loaded, and as noteEvent last saved it
} // namespace

auto loadPool() -> std::vector<PoolEntry>
{
    std::vector<std::string> missing;

    if (const auto rset = db::preparedStmt("SELECT name FROM bot_pool WHERE enabled = 1 AND charid = 0"))
    {
        while (rset->next())
        {
            missing.emplace_back(rset->get<std::string>("name"));
        }
    }

    for (const auto& name : missing)
    {
        createCharacter(name);
    }

    std::vector<PoolEntry> pool;

    const auto rset = db::preparedStmt("SELECT b.name, b.charid, b.accid, b.diary, s.mjob, GREATEST(s.mlvl, b.level) AS mlvl FROM bot_pool b "
                                       "JOIN chars c ON c.charid = b.charid "
                                       "JOIN char_stats s ON s.charid = b.charid "
                                       "WHERE b.enabled = 1");
    FOR_DB_MULTIPLE_RESULTS(rset)
    {
        const auto charId = rset->get<uint32>("charid");
        diaryStored[charId] = rset->isNull("diary") ? std::string{} : rset->get<std::string>("diary");
        pool.push_back(PoolEntry{
            .name   = rset->get<std::string>("name"),
            .charId = charId,
            .accId  = rset->get<uint32>("accid"),
            .job    = rset->get<uint8>("mjob"),
            .level  = rset->get<uint8>("mlvl"),
        });
    }

    return pool;
}

// A crashed map server leaves bot sessions and party rows behind.
void clearStaleState()
{
    db::preparedStmt("DELETE s FROM accounts_sessions s JOIN bot_pool b ON b.charid = s.charid");
    db::preparedStmt("DELETE p FROM accounts_parties p JOIN bot_pool b ON b.charid = p.charid");
}

auto countRealPlayers() -> uint32
{
    const auto rset = db::preparedStmt("SELECT COUNT(*) AS n FROM accounts_sessions s "
                                       "LEFT JOIN bot_pool b ON b.charid = s.charid WHERE b.charid IS NULL");
    FOR_DB_SINGLE_RESULT(rset)
    {
        return rset->get<uint32>("n");
    }

    return 0;
}

namespace
{
std::set<uint32> poolChars;
} // namespace

void registerPoolChar(const uint32 charId)
{
    poolChars.insert(charId);
}

auto isPoolChar(const uint32 charId) -> bool
{
    return poolChars.contains(charId);
}

namespace
{

constexpr std::size_t kDiaryCap    = 8;
constexpr std::size_t kPromptLines = 3;

auto whenPhrase(const int64 at) -> const char*
{
    const auto nowT  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto thenT = static_cast<std::time_t>(at);
    std::tm    nowTm{};
    std::tm    thenTm{};
    if (localtime_r(&nowT, &nowTm) == nullptr || localtime_r(&thenT, &thenTm) == nullptr || thenT >= nowT)
    {
        return thenT >= nowT ? "earlier today" : "earlier";
    }

    if (nowTm.tm_year == thenTm.tm_year && nowTm.tm_yday == thenTm.tm_yday)
    {
        return "earlier today";
    }

    // The previous calendar day, including New Year's Day looking back at December.
    const int gap = nowTm.tm_yday - thenTm.tm_yday;
    if ((nowTm.tm_year == thenTm.tm_year && gap == 1) ||
        (nowTm.tm_year == thenTm.tm_year + 1 && nowTm.tm_yday == 0 && thenTm.tm_yday >= 364))
    {
        return "yesterday";
    }

    return "a few days ago";
}

auto parseDiary(const std::string& stored) -> std::deque<DiaryLine>
{
    std::deque<DiaryLine> lines;
    std::size_t           pos = 0;
    while (pos < stored.size())
    {
        auto end = stored.find('\n', pos);
        if (end == std::string::npos)
        {
            end = stored.size();
        }
        auto line = stored.substr(pos, end - pos);
        pos       = end + 1;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front())))
        {
            continue;
        }

        int64       at = 0;
        std::size_t i  = 0;
        while (i < line.size() && i < 18 && std::isdigit(static_cast<unsigned char>(line[i])))
        {
            at = at * 10 + (line[i] - '0');
            ++i;
        }
        if (i >= line.size() || line[i] != ' ')
        {
            continue;
        }
        auto text = line.substr(i + 1);
        if (text.empty())
        {
            continue;
        }
        lines.push_back(DiaryLine{ at, std::move(text) });
    }

    while (lines.size() > kDiaryCap)
    {
        lines.pop_front();
    }
    return lines;
}

auto formatDiary(const std::deque<DiaryLine>& diary) -> std::string
{
    std::string out;
    for (const auto& line : diary)
    {
        if (!out.empty())
        {
            out += '\n';
        }
        out += fmt::format("{} {}", line.at, line.text);
    }
    return out;
}

} // namespace

void loadDiary(Bot& bot)
{
    bot.diary.clear();
    if (const auto it = diaryStored.find(bot.charId); it != diaryStored.end())
    {
        bot.diary = parseDiary(it->second);
    }
}

void noteEvent(Bot& bot, std::string fact)
{
    for (auto& c : fact)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }
    if (fact.empty())
    {
        return;
    }

    const auto at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    bot.diary.push_back(DiaryLine{ static_cast<int64>(at), std::move(fact) });
    while (bot.diary.size() > kDiaryCap)
    {
        bot.diary.pop_front();
    }

    const auto stored        = formatDiary(bot.diary);
    diaryStored[bot.charId]  = stored;
    if (!db::preparedStmt("UPDATE bot_pool SET diary = ? WHERE charid = ?", stored, bot.charId))
    {
        ShowErrorFmt("bots: could not save {}'s diary", bot.name);
    }
    else
    {
        ShowInfoFmt("bots: {} diary: {}", bot.name, bot.diary.back().text);
    }
}

auto recentEvents(const Bot& bot) -> std::string
{
    if (bot.diary.empty())
    {
        return {};
    }

    const auto start = bot.diary.size() > kPromptLines ? bot.diary.size() - kPromptLines : 0;
    std::string out;
    for (std::size_t i = start; i < bot.diary.size(); ++i)
    {
        if (!out.empty())
        {
            out += "; ";
        }
        out += fmt::format("{}, {}", whenPhrase(bot.diary[i].at), bot.diary[i].text);
    }
    return out;
}

namespace
{

constexpr int64 kAwayMin   = 60 * 60;          // an hour, or the gap is not a catch-up
constexpr int64 kAwayMax   = 3 * 24 * 60 * 60; // three days of time, and no more
constexpr uint8 kLevelCap  = 75;
// Levels per real day at level 20. Above 20 this shrinks by 20/level.
constexpr std::array<double, 3> kPerDay = { 1.0, 2.0, 4.0 }; // casual, regular, hardcore

constexpr std::array<const char*, 3> kStyleName = { "casual", "regular", "hardcore" };

constexpr std::array<const char*, 23> kJobAbbrev = {
    "",    "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
    "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN",
};

auto unixNow() -> uint32
{
    return static_cast<uint32>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Levels per day at this level: the level-20 pace, and fewer once the level is past 20.
auto levelsPerDay(const uint8 style, const uint8 level) -> double
{
    const auto base = style < kPerDay.size() ? kPerDay[style] : kPerDay[1];
    return base * 20.0 / static_cast<double>(std::max<uint8>(level, 20));
}

auto storeLastSeen(const Bot& bot, const uint32 now) -> bool
{
    if (!db::preparedStmt("UPDATE bot_pool SET last_seen = ? WHERE charid = ?", now, bot.charId))
    {
        ShowErrorFmt("bots: could not store {}'s last seen time", bot.name);
        return false;
    }
    return true;
}

} // namespace

void noteLastSeen(const Bot& bot)
{
    storeLastSeen(bot, unixNow());
}

// One grant per login. last_seen is written after the exp, so a retry cannot grant it again.
// A NULL last_seen has never logged out and grants nothing.
auto catchUp(Bot& bot) -> uint8
{
    if (!settings::get<bool>("bots.CATCH_UP"))
    {
        return 0;
    }

    auto* PChar = bot.entity();
    if (PChar == nullptr || PChar->loc.zone == nullptr)
    {
        return 0;
    }

    const auto rset = db::preparedStmt("SELECT last_seen, play_style FROM bot_pool WHERE charid = ?", bot.charId);
    if (!rset || !rset->next() || rset->isNull("last_seen"))
    {
        return 0;
    }

    const auto now  = unixNow();
    const auto seen = rset->get<uint32>("last_seen");
    if (now < seen)
    {
        return 0;
    }
    const auto gap = static_cast<int64>(now) - static_cast<int64>(seen);
    if (gap < kAwayMin)
    {
        return 0;
    }

    // Rolled once. A later login reads the stored value.
    uint8 style = 1;
    if (rset->isNull("play_style"))
    {
        style = xirand::GetRandomNumber<uint8>(3);
        if (!db::preparedStmt("UPDATE bot_pool SET play_style = ? WHERE charid = ? AND play_style IS NULL", style, bot.charId))
        {
            ShowErrorFmt("bots: could not store {}'s play style", bot.name);
        }
    }
    else
    {
        style = rset->get<uint8>("play_style");
    }

    const auto before = PChar->GetMLevel();
    const auto capped = std::min(gap, kAwayMax);
    uint32     owed   = 0;
    if (before < kLevelCap)
    {
        const auto bar = charutils::GetExpNEXTLevel(before);
        if (bar == 0)
        {
            ShowErrorFmt("bots: no exp to the next level for {} at {}, skipping catch-up", bot.name, before);
            return 0;
        }

        auto   level     = before;
        double hoursLeft = static_cast<double>(capped) / 3600.0;
        while (hoursLeft > 1e-6 && level < kLevelCap)
        {
            const auto tnl    = charutils::GetExpNEXTLevel(level);
            const auto perDay = levelsPerDay(style, level);
            if (tnl == 0 || perDay <= 0.0)
            {
                break;
            }
            const auto hoursPerLevel = 24.0 / perDay;
            const auto take          = std::min(hoursLeft, hoursPerLevel);
            if (take + 1e-4 >= hoursPerLevel)
            {
                owed += tnl;
                ++level;
            }
            else
            {
                owed += static_cast<uint32>(static_cast<double>(tnl) * (take / hoursPerLevel));
            }
            hoursLeft -= take;
        }
    }

    uint32 spent = 0;
    // AddExperiencePoints applies at most one level per call and drops exp past the next bar, so one call per level.
    for (uint8 step = 0; owed > 0 && PChar->GetMLevel() < kLevelCap && step < kLevelCap; ++step)
    {
        const auto tnl = charutils::GetExpNEXTLevel(PChar->GetMLevel());
        if (tnl == 0)
        {
            break;
        }
        const auto grant  = std::min(owed, tnl);
        const auto level  = PChar->GetMLevel();
        // fromScripts skips map.EXP_RATE. awardRegionPoints is off: this is not a kill.
        // A dead bot still has to take the exp; the raise flag is what lets a corpse receive it.
        charutils::AddExperiencePoints(PChar->isDead(), false, true, PChar, PChar, grant);
        owed -= grant;
        spent += grant;
        if (PChar->GetMLevel() == level)
        {
            break;
        }
    }

    // Stamp before the diary write, so a failure there cannot grant this gap twice.
    storeLastSeen(bot, now);

    const auto after  = PChar->GetMLevel();
    const auto gained = after > before ? static_cast<uint8>(after - before) : 0;
    ShowInfoFmt("bots: {} was away {:.1f} hours ({}), catch-up {} exp, now level {}", bot.name, static_cast<double>(capped) / 3600.0,
                style < kStyleName.size() ? kStyleName[style] : kStyleName[1], spent, after);

    if (gained > 0)
    {
        const auto id   = static_cast<std::size_t>(PChar->GetMJob());
        const auto abbr = id < kJobAbbrev.size() && kJobAbbrev[id][0] != '\0' ? kJobAbbrev[id] : "?";
        noteEvent(bot, fmt::format("gained {} level{} while away, now {} {}", gained, gained == 1 ? "" : "s", abbr, after));
        // The prepare that follows would otherwise also diary "reached level".
        bot.preparedLevel = after;
    }
    return gained;
}

} // namespace bots
