// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// One value, three levels (docs/SettingLevels.md).
//
// A setting that may be set below the server is resolved here and nowhere else:
// the account's own value when it has one, else its domain's, else the
// server's - the effective value, and the level it came from. The shape a
// hosted product is expected to have, and the only honest way to answer "why
// is this account not doing what the domain says".
//
// THREE KINDS OF SETTING, and every name is exactly one (the Class below, a
// column of the table, and docs/SettingLevels.md with the reason for each):
//
//    A LIMIT protects the server or the other tenants - the message size, the
//    recall window, the sending limits, the session lifetimes. A level below
//    may tighten the level above and never loosen it: a looser value is
//    refused when it is written, and a level whose value the level above has
//    since tightened past is capped when it is read. Anybody who administers
//    the level may tighten it.
//
//    A GATE is a capability the server grants - zero-access, the OpenPGP
//    portal, a directory people edit themselves, plus addressing. Only the
//    server administrator sets it, at any level, either way.
//
//    A PREFERENCE is the tenant's own - the digest's hour, the external tag,
//    the retention of its own mail. The deepest level that holds a value wins,
//    and the domain administrator sets it.
//
// Two older contracts are kept, so that nothing configured before these levels
// resolves differently: the domain's own FIELD of a limit that always could go
// either way from the server's (the session policies, the quota grace and
// send-stop shares) still can - the ceiling binds the levels below it - and the
// maximum message size's domain field keeps the server's value as its ceiling,
// as it always did. The sending limits' [SendingLimitsOverrides] line for an
// address is that address's SERVER level: the server administrator's own
// exception, which the levels below may tighten.
//
// WHERE EACH LEVEL LIVES. The server level is where it always was: a row of
// hm_settings, a key of the settings store that was hMailServer.ini's
// [Settings] section, the [SendingLimits] section of the file, or - for the
// settings that were only ever the domain's - the value the domain column is
// created with. The domain and account levels are, name by name, either a
// DEDICATED COLUMN that already held them (hm_domains.domainmaxmessagesize,
// hm_accounts.accountmessageretentiondays, ...) or the OVERLAY,
// hm_settingoverrides (schema 6088), which holds only the levels that had
// nowhere to live. The dedicated columns stay the storage of their level; no
// value moved.
//
// WHAT "UNSET" MEANS IN A COLUMN. Each column has, or has not, a value that
// means "take the level above": 0 for the session seconds, the domain's
// retention days, the message size and the account's retention days; -1 for
// the three quota percentages; 0 (with 1 on and 2 off) for the two session
// switches. A column that was never optional - the digest, its hour, the tags,
// the impersonation action, plus addressing, the country policy - is always
// its domain's own value, and the overlay supplies the account level below it.
// The table in EffectiveSetting.cpp says which, per name; a domain or account
// with no row in the overlay resolves exactly as it did before the overlay
// existed. A value in a column or a row that the interfaces would refuse - put
// there by hand - is treated as unset rather than honoured, as SessionPolicies
// always treated one.
//
// NAMES are the ones REST uses for the domain's and the account's fields, and
// COM, hmctl and the overlay's soname use the same spelling: max_message_size_kb,
// recall_days, quarantine_digest_hour. At most thirty characters, the width of
// soname.
//
// A READ SITE of one of these settings asks here, with the account when it has
// one. build/check-setting-levels.py holds every reader of a domain column or a
// server getter behind a three-level name to a list, so a new reader that goes
// round the resolver is refused by name.

#pragma once

namespace HM
{
   class Domain;
   class Account;

   class EffectiveSetting
   {
   public:
      // Where an effective value came from.
      enum Source
      {
         SourceServer = 0,
         SourceDomain = 1,
         SourceAccount = 2
      };

      // The deepest level a name may be set at: every name in the table below
      // reaches the account; the mark `server` is every other setting there is.
      enum Levels
      {
         LevelsServer = 0,
         LevelsDomain = 1,
         LevelsAccount = 2
      };

      enum Type
      {
         TypeInteger = 0,
         TypeBoolean = 1,
         TypeText = 2
      };

      // What the server level is read from, for the checker and the documents.
      enum ServerKind
      {
         ServerDefault = 0,     // no server setting: the shipped value of the domain column
         ServerProperty = 1,    // a row of hm_settings
         ServerIniKey = 2,      // a key of the settings store ([Settings])
         ServerIniSection = 3   // a key of a section the store does not hold ([SendingLimits])
      };

      // What kind of setting a name is (above): what decides how its levels
      // combine and who may set them. Every name is exactly one.
      enum Class
      {
         ClassLimit = 0,
         ClassGate = 1,
         ClassPreference = 2
      };

      // Which way a limit tightens. Lower: a smaller number is tighter, 0 the
      // tightest of all (the recall window, a reader maximum). LowerZeroNone:
      // a smaller number is tighter and 0 is no limit at all (the message size,
      // the sending limits, the send-stop share). On: a switch that is tighter
      // on (the session switches). None: not a limit.
      enum Tight
      {
         TightNone = 0,
         TightLower = 1,
         TightLowerZeroNone = 2,
         TightOn = 3
      };

      struct Definition
      {
         const char *name;
         Type type;
         Levels levels;
         ServerKind server_kind;
         // The ini key or hm_settings name behind the server level, for the
         // levels mark (build/ini-settings-baseline.txt, build/settings-levels.txt)
         // and the documents. Empty for ServerDefault.
         const char *server_key;
         Class klass;
         Tight tight;
         // The server's answer, in the value's text form.
         String (*server)();
         // The server's answer for one account where the server holds one per
         // address - the sending limits' [SendingLimitsOverrides] line - or null.
         bool (*server_for_account)(const Account &account, String &value);
         // The level held in a dedicated column: true, with the value, when the
         // column holds one; false when it holds its "take the level above".
         // Null when the overlay holds that level.
         bool (*domain_column)(const Domain &domain, String &value);
         // For a limit whose domain level is a column: whether the server's value
         // has always capped that column (the message size), or the column has
         // always been free to go either way from it (the session policies, the
         // quota shares) - which it still is, the ceiling binding the account.
         bool domain_column_capped;
         bool (*account_column)(const Account &account, String &value);
         // Whether the overlay holds the level.
         bool domain_overlay;
         bool account_overlay;
         // Canonicalises a value written into the overlay, or refuses it with a
         // sentence. Also run over a stored value when it is read, so a row put in
         // by hand that the interfaces would refuse is treated as unset.
         bool (*canonicalise)(const String &value, String &canonical, String &refusal);

         // A limit: a level below may tighten the level above and not loosen it.
         bool ceiling() const { return klass == ClassLimit; }

         // A gate: the server administrator's to set at a domain or an account;
         // a key restricted to domains, and a domain administrator over COM, are
         // refused it. The levels held in the overlay are gated so; a level held
         // in a dedicated field keeps the authority that field always had
         // (docs/SettingLevels.md says, name by name).
         bool server_administrators_only() const { return klass == ClassGate; }
      };

      struct Answer
      {
         Answer() : source(SourceServer) {}
         String value;
         Source source;
      };

      // Every name, in the order the documents list them.
      static const Definition *Definitions(size_t &count);

      // The name's definition, matched without case; null for a name that is
      // not three-level.
      static const Definition *Find(const String &name);

      // The effective value. The account may be null (the domain's answer), and
      // so may the domain (the server's); an account given without its domain
      // is resolved under its own domain.
      static Answer Resolve(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);
      static Answer Resolve(const String &name, __int64 domainId, __int64 accountId);

      static int ResolveInt(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);
      static bool ResolveBool(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);
      static String ResolveText(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account);

      // A limit resolved for one account under a domain, and held as well to a
      // second account's answer - its own value, else its domain's, held to
      // the server's: the signed-in account's, where the message names another
      // address (or none) as its sender. A limit on an account or on its
      // domain is a limit on what the account sends, and what address it
      // sends as does not lift it; with no second account, the answer is
      // exactly ResolveInt's.
      static int ResolveIntHeldTo(const String &name, std::shared_ptr<const Domain> domain, std::shared_ptr<const Account> account,
                                  std::shared_ptr<const Account> signedIn);

      // A level's own value: true with the value when the level has one (its
      // column or its overlay row), false when it inherits.
      static bool DomainValue(const Definition &definition, std::shared_ptr<const Domain> domain, String &value);
      static bool AccountValue(const Definition &definition, std::shared_ptr<const Account> account, String &value);

      // Whether the level named by scope (1 domain, 2 account) is the overlay's
      // for this name - the only levels SetOverride and ClearOverride write.
      static bool OverlayHolds(const Definition &definition, int scope);

      // Writes or clears an overlay row. The value is canonicalised first and
      // refused with a sentence; a level held in a dedicated column is refused
      // with the sentence that names the field that writes it; an owner not
      // saved yet is refused; and a limit looser than the level above it is
      // refused with the sentence that names the level above and its value -
      // unless holdToCeiling is false, which only a restore passes (the ceiling
      // still caps the value when it is read). Applies at once: a read site
      // resolves at use.
      static bool SetOverride(const String &name, int scope, __int64 ownerId, const String &value, String &refusal, bool holdToCeiling = true);
      static bool ClearOverride(const String &name, int scope, __int64 ownerId, String &refusal);

      // The level held for this scope by a dedicated field rather than by the
      // overlay: the sentence that says so, for a caller that must refuse
      // before asking who may set it.
      static String ColumnSentence(const Definition &definition, int scope);

      // What the level above an owner's holds for a name: for a domain the
      // server's, for an account its domain's effective value (the server's
      // per-address value included). What a limit written at that owner may
      // not loosen.
      static Answer Above(const Definition &definition, int scope, __int64 ownerId);

      // Whether value is looser than above for a limit; false for any other kind.
      static bool Looser(const Definition &definition, const String &value, const String &above);

      // For a limit: false, with the sentence, when the canonical value would
      // loosen what the level above the owner holds. True for any other kind.
      // SetOverride asks it; REST asks it before it saves anything, so that a
      // refused value is a 400 and not a half-done save.
      static bool HoldsToCeiling(const Definition &definition, int scope, __int64 ownerId, const String &canonical, String &refusal);

      // The sentence for a name that is not three-level: set for the whole
      // server only, or not a setting at all.
      static String NotThreeLevelSentence(const String &name);

      static const char *SourceWord(Source source);
      static const char *LevelsWord(Levels levels);
      static const char *ClassWord(Class klass);
      static const char *TightWord(Tight tight);
      static const char *TypeWord(Type type);
      static const char *ServerKindWord(ServerKind kind);

      // A value's JSON form for REST: a number, true/false, or a quoted string.
      static AnsiString JsonText(const Definition &definition, const String &value);

      // The whole table as JSON, for GET /api/v1/settings/levels.
      static AnsiString TableJson();

      // The overlay of one owner in the XML backup, and back.
      static bool XMLStore(XNode *parentNode, int scope, __int64 ownerId);
      static bool XMLLoad(XNode *parentNode, int scope, __int64 ownerId);

      // The largest and the smallest value of an integer (or boolean) name
      // anywhere - the server's and every overlay row's - read through the
      // cache of one name's rows, never the database per call: the OpenPGP
      // portal's module is served when any level anywhere has the portal on,
      // and the recover-deleted sweep looks from the shortest window.
      static int LargestAnywhere(const String &name);
      static int SmallestAnywhere(const String &name);
   };
}
