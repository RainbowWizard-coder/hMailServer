// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The append-only, hash-chained record of every administrative change.
//
// WHERE IT IS WRITTEN FROM, AND WHY THERE ARE SO FEW PLACES
//
// An administrative change is, nearly always, one of two things: a row in a
// configuration table, or a value in hm_settings. Every interface - COM (and so
// the Control Panel), REST (and so the Deck and hmctl) - reaches those through
// the same code, so the record is taken BELOW the interfaces rather than in
// them:
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
//   events    RecordEvent, for the administrative change that is neither. An
//             unlock, in every form it takes: an account let back in clears
//             an in-memory lockout and a POP3 mailbox lock, and writes no row
//             and no setting at all; Account.UnlockMailbox clears the POP3 lock
//             alone; and clearing the logon-failure list deletes rows of
//             hm_logon_failures, which is traffic and not an audited table.
//             Called where each change is made - POST
//             /api/v1/accounts/{address}/unlock and Application.UnlockAccount,
//             Account.UnlockMailbox, and POST /api/v1/settings/logon-failures/clear
//             and Settings.ClearLogonFailureList.
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

#pragma once

#include <vector>

namespace HM
{
   class SQLCommand;
   class AuditDescription;

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

      // The two recording chokepoints. Nothing else may call these; see the
      // header comment and build/check-audit-choke-point.py.
      void RecordStatement(const SQLCommand &command, bool succeeded);
      void RecordSettingChange(const String &name, const String &oldValue, const String &newValue);

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

      // Walks the chain from the first row. Returns true when every row's stored
      // hash is the hash its contents produce and each row carries the hash of
      // the row before it. On false, firstBroken names the first row that does
      // not, and reason says which of the two it failed.
      bool Verify(__int64 &firstBroken, __int64 &rowsChecked, String &reason);

      // Deletes rows older than the given number of days. 0 keeps everything,
      // which is the default: an audit trail that quietly forgets is not one.
      int PurgeOlderThan(int days);

      // True for a column or setting name whose value is a secret.
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
      static String HashRow_(const String &previousHash, __int64 time, const String &actor, const String &actorKind,
                             const String &interfaceName, const String &address, const String &objectType,
                             const String &objectName, const String &action, const String &detail);
      String ReadHeadHash_();
      static String Describe_(const SQLCommand &command, String &table, String &action, String &objectName);
      static String ColumnOfParameter_(const String &parameterName);

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
      ~AuditDescription();

   private:
      AuditDescription(const AuditDescription &);
      AuditDescription &operator=(const AuditDescription &);

      friend class AuditTrail;

      String table_;
      String object_name_;
      String detail_;
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
