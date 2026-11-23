# Job D: fix the bugs found in the review

This folder is lsb-bots, a C++ bot module for LandSandBoat (an FFXI server). `DESIGN-review.md` section 4 lists
bugs found while reading it. Fix these, with the smallest clean change for each, in the existing code style:

1. `inspect` in `bot_ai.cpp`: the `orders` name array has 11 entries but `OrderKind` has 12 (`Ability` is
   missing), so `!bot <name>` reads past the end for an ability order. Add the name, and size the array from the
   enum so this can't happen again (a static_assert is fine).
2. `wantsParty` in `bot_chat.cpp`: check that the character AFTER the word is not a letter either (like `says` in
   `bot_orders.cpp`), so "inventory", "joining" and "options" don't queue an invite. Keep "lfp", "lfg", "pt" etc.
   working, including "pt?" and "inv pls".
3. The chat worker (`bot_chat.cpp`) and the brain worker (`bot_brain.cpp`) are detached threads with no
   try/catch, so one exception (for example `std::out_of_range` from `dechunk` in `bot_http.cpp`, or `std::stoi`
   in `JsonReader::parseString`) ends chat or orders until the server restarts. Catch exceptions per request,
   log them, and complete that request as failed so its caller falls back (the brain's `waiting` entry must get an
   outcome, so `planOrder` runs). Also make `dechunk` and the `\u` parse not throw on bad input.
4. `botRuns` and `partyQuietUntil` (`bot_chat.cpp`) are keyed by party pointer and never pruned, so a new party at
   the same address inherits old state. Key them by the party's id instead, or erase entries when a party ends.
5. Geomancer and Rune Fencer aren't part of this server's era. Make `createCharacter` (`bot_pool.cpp`) refuse a
   pool row whose job is GEO or RUN (log it and skip it). Don't rip the jobs out of the other tables.
6. `think` (`bot_ai.cpp`) logs every real player's emote at info level. Make that debug level.
7. `describe` (`bot_chat.cpp`) says subjobs start at 18. Use the same setting `subJobScript` uses
   (`main.SUBJOB_QUEST_LEVEL`) instead of the hard-coded 18.

Don't change anything else, don't reformat files, and don't add features. You can't compile here (the server
sources aren't in this folder), so be careful with names: only use functions and types that already appear in
this code. When done, write `JOB_D_NOTES.md`: one short paragraph per fix (what changed, file and function), plus
anything you weren't sure about.
