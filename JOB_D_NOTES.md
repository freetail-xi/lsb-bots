`inspect` (`bot_ai.cpp`) now names all 12 `OrderKind` values, including `"ability"`. The array length is `OrderKind::Ability + 1`, and a `static_assert` rejects a short initializer (the spare slots would be null). This assumes `Ability` stays the last enumerator: a kind added after it would not grow the array. Deduction of `std::array` from the string literals would not compile, because the literals have different lengths, so the size is written out.

`wantsParty` (`bot_chat.cpp`) now requires the character after the word to be a non-letter, the same rule `says` uses in `bot_orders.cpp`. "inventory" and "joining" no longer match. "lfp", "lfg", "pt", "pt?", and "inv pls" still do. "options" was already rejected by the check on the character before the word, because "pt" there sits after "o".

`worker` in `bot_chat.cpp` and `worker` in `bot_brain.cpp` catch each request. The exception is logged and the thread keeps going. A failed chat request pushes no reply, which is the same outcome as a model miss. A failed brain request is still published, with `answered` cleared, so `brainOutcomes` wakes the `waiting` entry and `handleChat` runs `planOrder`. `dechunk` (`bot_http.cpp`) stops on a bad or truncated chunk instead of calling `string::append` past the body. `JsonReader::parseString` (`bot_brain.cpp`) reads `\u` with `std::from_chars` and returns nullopt on bad hex, instead of `std::stoi`.

`botRuns` (`handleChat` in `bot_chat.cpp`) keys a party channel by `GetPartyID()` rather than `fmt::ptr` of the party. `partyQuietUntil` (`chatter` in `bot_chat.cpp`) is a `std::map<uint32, time_point>` keyed the same way. Entries are not erased when a party ends, so the maps can still grow, the way `partyLines` does. A new party at a recycled address does not inherit the old count or quiet time. A party message with a null party uses id 0.

`createCharacter` (`bot_pool.cpp`) logs and returns false when the pool row's job column is `"geo"` or `"run"` (indexes 21 and 22). The other job tables are unchanged. `xi::Job` is only forward-declared in this folder, so the check uses the column names already in `jobColumns`.

`think` (`bot_ai.cpp`) logs a real player's emote with `ShowDebugFmt` instead of `ShowInfoFmt`. That name is the debug counterpart of the `ShowInfoFmt` / `ShowWarningFmt` / `ShowErrorFmt` macros already used here. It could not be compiled against the server headers.

`describe` (`bot_chat.cpp`) fills the subjob line from `settings::get<uint8>("main.SUBJOB_QUEST_LEVEL")`, the same call `subJobScript` uses in `bot_gear.cpp`, instead of the hard-coded 18.
