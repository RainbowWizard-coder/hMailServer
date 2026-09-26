// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The append-only, hash-chained record of every administrative change.
//
// WHERE IT IS WRITTEN FROM, AND WHY THERE ARE SO FEW PLACES
//
// An administrative change is, nearly always, one of three things: a row in a
// configuration table, a value in hm_settings, or a value in hm_inisettings
// (the [Settings] keys that left hMailServer.INI at schema 6042). Every
// interface - COM (and so the Control Panel), REST (and so the Deck and hmctl)
// - reaches those through the same code, so the record is taken BELOW the
// interfaces rather than in them:
//
//   objects   DatabaseConnectionManager::Execute, the single statement
//             chokepoint the whole server already runs every write through.
//             RecordStatement is called there, after the connection has been
//             released, for an INSERT, UPDATE or DELETE against a table in
//             AuditedTable_ below.
//
//   settings  PropertySet::SetLong/SetBool/SetString, which is where the old
//             value is still readable. hm_settings is deliberately NOT an
//             audited table, so a setting is recorded once, with its old value
//             and its new one, and not twice.
//
//   [Settings] IniSettingStore::WriteSetting and ::RemoveSetting, the two
//             administrative writes of hm_inisettings, which every COM
//             SetIniSetting/DeleteIniSetting, every /api/v1/settings/ini route
//             and every /api/v1/settings field backed by one of those keys goes
//             through. The stored value is read there before it is replaced,
//             and hm_inisettings is not an audited table either, for the same
//             reason as hm_settings.
//
//   events    RecordEvent, for the administrative change that is neither. An
//             unlock, in every form it takes: an account let back in clears
//             an in-memory lockout and a POP3 mailbox lock, and writes no row
//             and no setting at all; Account.UnlockMailbox clears the POP3 lock
//             alone; and clearing the logon-failure list deletes rows of
//             hm_logon_failures, which is traffic and not an audited table.
//             Called where each change is made - POST
//             /api/v1/accounts/{address}/unlock and Application.UnlockAccount,
//             Account.UnlockMailbox, and POST /api/v1/settings/logon-failures/clear
//             and Settings.ClearLogonFailureList. And a restore asked for:
//             Backup.StartRestore and POST /api/v1/backup/restore, each on the
//             thread that knows who asked. And a setting profile applied,
//             reverted, its way back discarded or none wanted (Profiles), recorded once with
//             its counts beside the row the settings chokepoint writes for each
//             setting it changed.
//
// WHAT A RESTORE RECORDS. The request, and not what it replaces. The work runs
// on the maintenance work queue, which no actor is ever installed on, so every
// delete and insert it makes reaches the statement chokepoint with nobody to
// attribute it to and none of the places above writes a row for it. So the two
// callers write one row each for the request itself - who asked, which archive,
// which parts - before the queue takes it, and that is the half that says whose
// decision this was. docs/AlertsAndAuditTrail.md says which half is which.
//
// Nothing else may call Record*: build/check-audit-choke-point.py fails the
// build if it does, and fails it if any of those call sites goes away.
//
// WHO IT SAYS DID IT
//
// A row is written only while an ACTOR is installed on the calling thread, and
// an actor is installed at exactly one place per interface:
//
//   REST  RestApiServer::ProcessRequest_, from the authenticated Caller and the
//         peer address, for the length of one request.
//   COM   COMAuthentication::InstallActor_, reached from every authorisation
//         question a COM write asks before it writes - GetIsServerAdmin,
//         GetIsDomainAdmin, and the Allows questions the narrow roles' entry
//         points ask. They run on the thread that is about to do the writing,
//         which a thread-local set at authentication time would not.
//
// Both install RoleManager's actor at the same moment, from the same grant, so
// the authority a change was decided on and the name it is recorded under are
// one credential and not two readings of it.
//
// COM's install is never taken away again, which is safe on COM's own pool
// threads - the next question replaces it - and was not safe where COM is used
// in process: an event script runs on the server thread that fired the event,
// and the credential it signed in to COM with stayed on that thread for the
// server's next change. ScriptServer runs every script inside a
// ScriptActorBoundary, which takes both actors off the thread first and puts
// back what was there once the script is gone.
//
// That is also what keeps the server's own writes out of the record: delivery,
// retention, greylisting and the rest run with no actor, and an audit trail
// that says "the mail server changed something" every minute is one nobody
// reads. It is what stops the recursion, too - the row this class writes is
// itself a statement through the chokepoint, and it carries no actor.
//
// WHAT IT NEVER RECORDS
//
// A password, a key, a token, a secret or a hash is recorded as "(changed)" and
// never as a value, by the name of the column or the setting - see
// IsSecretName. An audit trail that copies every password an administrator sets
// into a table the Deck renders is worse than no audit trail.
//
// THE CHAIN
//
// Each row carries the SHA-256 of the row before it (auditprevhash) and its own
// (audithash), computed over the previous hash followed by this row's fields.
// Deleting a row, or editing one, therefore breaks the chain at that point and
// at every point after it; Verify walks it and names the first row where the
// stored hash is not the hash the row's own contents produce. It does not make
// the table tamper-PROOF - anyone who can write the table can recompute the
// whole chain - it makes tampering DETECTABLE without a second copy.
//
// The fields are hashed as UTF-8, and the hash is stored in upper-case hex.
// Until 26 September 2026 they were hashed through AnsiString(String), which
// on Windows wrote NUL bytes from the first character above U+00FF to the
// end and on Linux wrote each such character as '?', so an edit after or of
// such a character left the hash unchanged. Those rows were stored in lower-
// case hex, and Verify checks a row by the scheme its own hash is written in:
// lower case the old one, anything else UTF-8. Turning a row's hash to lower
// case to have it checked the old way changes the back-hash the next row
// carries, which is an edit the chain catches like any other.

#pragma once

#include <vector>

namespace HM
{
   class SQLCommand;
   class AuditDescription;
   class DALRecordset;

   class AuditTrail : public Singleton<AuditTrail>
   {
   public:

      AuditTrail();
      ~AuditTrail();

      // Who is making the change, and over what. Installed by AuditScope.
      struct Actor
      {
         Actor() : present(false) {}

         bool present;
         String name;       // "Administrator", "user@example.com", "key:reporting"
         String kind;       // administrator | account | apikey | system
         String interface_name;  // COM | REST | Deck | Portal
         String address;    // the peer's address, empty where there is no peer

         // The role the credential acted under - "serveradmin", "helpdesk", a
         // custom role's name - and empty for an account that holds none. Read
         // from the same RoleGrant the decision was made from. The row has no
         // column for it, so Write_ records it at the end of the detail, where
         // the chain's hash covers it like every other field.
         String role;
      };

      struct Entry
      {
         Entry() : id(0), time(0) {}

         __int64 id;
         __int64 time;      // seconds since the epoch, UTC
         String actor;
         String actor_kind;
         String interface_name;
         String address;
         String object_type;
         String object_name;
         String action;     // created | updated | deleted | unlocked
         String detail;
         String previous_hash;
         String hash;
      };

      struct Filter
      {
         Filter() : since(0), until(0), limit(100), offset(0) {}

         String actor;        // substring, case-insensitive
         String object_type;  // exact
         String action;       // exact
         __int64 since;
         __int64 until;
         int limit;
         int offset;
      };

      // The recording chokepoints. Nothing else may call these; see the header
      // comment and build/check-audit-choke-point.py. A setting's action is
      // "updated", or "deleted" for a [Settings] key returned to its default.
      void RecordStatement(const SQLCommand &command, bool succeeded);
      // withheld: the value is secret although its name does not say so - a file of
      // the server's environment supplied it (EnvironmentSettings::IsWithheld) - and
      // is recorded as changed, as a secret name's is.
      void RecordSettingChange(const String &name, const String &oldValue, const String &newValue,
                               const String &action = String(_T("updated")), bool withheld = false);

      // An administrative change that lands in no audited table and no
      // setting, and so passes neither chokepoint: an account unlocked, a
      // mailbox lock cleared, the logon-failure list cleared. Recorded exactly
      // as the other two record - only with an actor installed on the calling
      // thread, only while recording is on (or being switched), one line per
      // field, and with the role the credential acted under at the end of the
      // detail. The caller says what it did in the trail's own terms:
      // objectType as AuditedTable spells it ("account") or, for a thing no
      // table names, a noun of its own ("logon failures"), objectName the name
      // a person knows the object by, action a past-tense verb ("unlocked",
      // "cleared"). Named only at the call sites
      // build/check-audit-choke-point.py lists.
      void RecordEvent(const String &objectType, const String &objectName, const String &action, const String &detail);

      bool Query(const Filter &filter, std::vector<Entry> &entries, int &total);

      // One row by its id; false when there is none.
      bool Read(__int64 id, Entry &entry);

      // A row as the REST API and the COM report both write it: id, time, actor,
      // actor_type, interface, address, object_type, object, action, detail,
      // hash and previous_hash.
      static AnsiString EntryJson(const Entry &entry);

      // Walks the chain from the first row. Returns true when every row's stored
      // hash is the hash its contents produce and each row carries the hash of
      // the row before it. On false, firstBroken names the first row that does
      // not, and reason says which of the two it failed.
      bool Verify(__int64 &firstBroken, __int64 &rowsChecked, String &reason);

      // Deletes rows older than the given number of days. 0 keeps everything,
      // which is the default: an audit trail that quietly forgets is not one.
      int PurgeOlderThan(int days);

      // True for a column or setting name whose value is a secret: a name
      // carrying password, secret, token, hash, salt, private key and the rest,
      // except the few settings named in NotSecretNames that only look like one.
      static bool IsSecretName(const String &name);

      // The thread's actor. Installed by AuditScope, which is the only thing
      // that should call Set/Clear.
      static void SetActor(const Actor &actor);
      static void ClearActor();
      static Actor GetActor();
      static bool HasActor();

      // "hm_domains" -> "domain". Empty for a table that is not audited.
      static String AuditedTable(const String &table);

   private:

      bool Write_(const String &objectType, const String &objectName, const String &action, const String &detail);
      static String HashRow_(bool legacyEncoding, const String &previousHash, __int64 time, const String &actor, const String &actorKind,
                             const String &interfaceName, const String &address, const String &objectType,
                             const String &objectName, const String &action, const String &detail);
      static bool IsLegacyHash_(const String &storedHash);
      String ReadHeadHash_();
      static String Describe_(const SQLCommand &command, String &table, String &action, String &objectName);
      static String ColumnOfParameter_(const String &parameterName);
      static void ReadRow_(std::shared_ptr<DALRecordset> recordset, Entry &entry);

      boost::recursive_mutex mutex_;

      // True while this thread is inside Write_, so that the INSERT into
      // hm_audit - a statement through the same chokepoint - does not record
      // itself for ever.
      static thread_local bool writing_;

      static thread_local Actor actor_;

      // The words the writer of the statement on this thread supplied, or
      // nothing. See AuditDescription.
      friend class AuditDescription;
      static thread_local const AuditDescription *description_;
   };

   // The words for a change whose statement carries only numbers.
   //
   // RecordStatement names an object by a column ending in "name" or "address"
   // and details it by the columns the statement carried, which reads well for
   // a domain or an account and not at all for a row that is nothing but ids:
   // "raaccountid=12, raroleid=3" is a role given to somebody, and nobody
   // reading the trail can say who or which. Worse, what the row said BEFORE an
   // update or a delete is gone by the time the statement has run, so the
   // recorder could not look it up even if it tried.
   //
   // The code that makes such a change knows both, so it says them: for the
   // length of this scope, a statement against this table that is recorded on
   // this thread is recorded with this object name and this detail in place of
   // the ones read from its parameters. It changes the words and nothing else -
   // whether a row is written at all is still the actor's and the chokepoint's
   // decision, so a change made with no actor installed is still not recorded,
   // and a statement against another table is described as it always was.
   // Scopes nest, and the innermost for the table wins.
   //
   // build/check-audit-choke-point.py names the files that may supply words:
   // words that are wrong are a record that lies.
   class AuditDescription
   {
   public:
      AuditDescription(const String &table, const String &objectName, const String &detail);

      // The NAME only, with the statement's own columns left to make the detail.
      // For a row the recorder describes correctly and NAMES unrecognisably,
      // which is what a column holding an encoding rather than a name does to it:
      // a greylisting white address typed as 10.78.0.? is stored as the SQL LIKE
      // pattern 10.78.0._, so the trail named an address that appears nowhere in
      // the Control Panel, the REST list or hmctl. The detail is still the
      // columns, and so still says what was written.
      AuditDescription(const String &table, const String &objectName);

      ~AuditDescription();

   private:
      AuditDescription(const AuditDescription &);
      AuditDescription &operator=(const AuditDescription &);

      friend class AuditTrail;

      String table_;
      String object_name_;
      String detail_;
      bool detail_supplied_;
      const AuditDescription *previous_;
   };

   // Installs an actor on the calling thread for the length of a scope, and
   // restores whatever was there before - so a nested scope (a COM object used
   // from inside a REST handler) leaves the outer one intact.
   class AuditScope
   {
   public:
      explicit AuditScope(const AuditTrail::Actor &actor);
      ~AuditScope();

   private:
      AuditScope(const AuditScope &);
      AuditScope &operator=(const AuditScope &);

      AuditTrail::Actor previous_;
   };
}
