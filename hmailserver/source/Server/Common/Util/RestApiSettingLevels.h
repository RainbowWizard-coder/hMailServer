// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// One value, three levels over REST (Application/EffectiveSetting.h,
// docs/SettingLevels.md): what the domain and account routes add to their
// answers and take in their bodies, and GET /api/v1/settings/levels.
//
// THE ANSWER. For every setting that may be set below the server, a domain's
// and an account's entry carry <name>_effective (the value in effect there) and
// <name>_source (server, domain or account). Where the level is the overlay's
// they also carry <name>, the level's own value or null when it inherits; where
// the level is a dedicated column the entry already carries that column under
// the same key, as it always has, with the column's own "take the level above"
// (0 or -1) rather than null - a client written before this reads what it read.
//
// THE BODY. <name> with a value of the setting's own JSON type (a whole number,
// true or false, a string) sets the level's own value, and null makes it
// inherit - for the levels the overlay holds; a dedicated column keeps its key's
// contract (null leaves it alone; its 0 or -1 inherits). A limit looser than the
// level above is refused (400, the sentence naming the level above and its
// value); a gate asked for by a key confined to named domains is refused (403).
// <name>_effective and <name>_source are answered and IGNORED when sent back, so
// that an entry read and sent back as it is is taken. Everything is read and
// judged before anything is written, and the overlay rows are written after the
// domain or the account itself has been saved.

#include <vector>

namespace HM
{
   class Domain;
   class Account;
   class JsonValue;

   namespace SettingLevelsRest
   {
      struct Change
      {
         String name;
         bool clear = false;
         String value;
      };

      // The fields the entry adds: a comma first, then each name's members.
      // account null is the domain's entry.
      AnsiString EntryFields(std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);

      // Whether the key is a level the overlay holds at this scope (1 domain, 2
      // account) - a member the route takes in addition to its own fields.
      bool IsOverlayKey(const std::string &key, int scope);

      // Whether the key is <name>_effective or <name>_source of a setting with
      // levels: answered, and ignored when a body carries it back.
      bool IsReadOnlyKey(const std::string &key);

      // Reads and judges the overlay members of a body for the owner (the
      // domain's or the account's id; the level above it is what a limit is held
      // to). False with the sentence on the first refusal, forbidden set when the
      // refusal is the caller's authority (a gate, asked for by a credential
      // confined to named domains). Nothing is written.
      bool Read(const JsonValue &body, int scope, __int64 ownerId, bool serverAuthority, std::vector<Change> &changes, AnsiString &error, bool &forbidden);

      // Writes what Read accepted, for the owner. False with the sentence when
      // the database did not take a row.
      bool Apply(const std::vector<Change> &changes, int scope, __int64 ownerId, AnsiString &error);

      // GET /api/v1/settings/levels. Its OpenAPI path is
      // RestApiServer::OpenApiSettingLevelsPaths_, beside this.
      AnsiString TableJson();

      // The sentence the domain and account routes' descriptions end with.
      const char *DescriptionSentence();
   }
}
