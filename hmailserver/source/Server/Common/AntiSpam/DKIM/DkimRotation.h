// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

namespace HM
{
   class Domain;

   // DKIM keys that rotate: the part with I/O.
   //
   // The rules - when a rotation starts, when signing moves, when an old selector
   // goes - are DkimRotationPlan's, as functions of values. This class reads the
   // domain and its hm_dkimrotations row, checks records from the server's own
   // resolver, generates keys, writes them with protected permissions, moves the
   // domain's selectors and removes the keys it generated once nothing uses them.
   //
   // WHAT MOVES SIGNING. Only a switch, and a switch only when the next key's record
   // resolves from DNSResolver - the resolver DKIM verification itself uses - with
   // exactly that key's public half, checked in the same pass, and has done so for the
   // whole propagation period. "Force" skips the period and never the check. The
   // signer is untouched: DKIMSigner signs with the domain's primary selector as it
   // always has, and a switch is the primary becoming the secondary's values.
   //
   // THE CLOCK. Every entry point takes now. The scheduled task passes time(nullptr);
   // Utilities.RunDkimRotation and POST /api/v1/dkim/rotation/run pass it with a
   // number of seconds added, which is how the regression suite walks a rotation of
   // three days and a week in a few seconds. The offset itself is one call's, but
   // what that pass does is not: a switch or a retire it makes is real and early,
   // and every time it records - a first sighting, the moment a key became current
   // or started retiring, a check - is the moved clock's and stays in the row. The
   // real-clock passes after it read those times as they are, so a sighting it
   // recorded makes the switch wait the offset longer, a switch it made holds the
   // next rotation and the retiring slot back by the offset, and a retire it made
   // has already dropped the old selector. That is a test's tool, and the IDL, the
   // OpenAPI document and both command-line clients say so.
   //
   // THE WRITES, AND WHY IN THAT ORDER. SQL Server Compact has no transactions
   // (PersistentDomain::DeleteObject says so), so a switch is two writes that cannot
   // be made one: the row first, then the domain. Each change to hm_domains is a
   // compare-and-set on the selectors it expects, read back to confirm, so a rotation
   // on a second node, or an administrator's save in between, loses nothing - the
   // loser restores its row and says so in the application log. The retiring slot can
   // never name the domain's current key, so a failure between the two writes leaves
   // a row the next pass reconciles, and never a key deleted that still signs.
   //
   // WHAT IS DELETED. Only a key file the rotation generated (a flag no interface can
   // set), inside <DataDirectory>/DKIM, that parses as a private key, and that no
   // domain's primary or secondary - the retiring domain's own included - and no
   // other rotation's retiring slot names. A key file an administrator supplied is
   // left where it is, which is what the manual walk has always done, and the status
   // says so. A restore deletes every domain before it loads the backup's, and
   // deletes no key file doing it (KeepKeysOnDomainDelete).
   //
   // WHAT IS NOT AN ERROR. A record not found, a record that differs, a lookup that
   // failed: those are the ordinary states of a rotation waiting for somebody to
   // publish a record, and they are status and application-log lines. ReportError is
   // for faults only - OpenSSL refusing to make a key (6610), a key file that could
   // not be written with its permissions (6611), a directory that could not be made
   // (6612), a row that could not be read or written (6613), the domain update failing
   // in the database (6614), a generated key that could not be deleted (6615).
   class DkimRotation
   {
   public:
      // The answer to one operation. http_status is what REST answers with and what
      // COM turns into an error: 200 or 201 carries the status document in json; 400,
      // 404, 409 and 500 carry the sentence in error, and 409 carries the status
      // document too, with the waiting reason word that refused it in reason.
      struct Result
      {
         Result() : http_status(200) {}

         int http_status;
         AnsiString json;
         String error;
         AnsiString reason;

         bool Succeeded() const { return http_status >= 200 && http_status < 300; }
      };

      struct RunSummary
      {
         RunSummary() : as_of(0), domains(0), generated(0), checked(0), switched(0), retired(0), errors(0) {}

         __int64 as_of;
         int domains;
         int generated;
         int checked;
         int switched;
         int retired;
         int errors;

         AnsiString ToJson() const;
      };

      // One pass over every domain in flight - the schedule on, or a next or retiring
      // selector in the row - advanced to now.
      static RunSummary RunAll(__int64 now);

      // The status document of one domain, as REST's GET and COM's DKIMRotationStatus
      // answer it. Nothing is written and nothing is looked up.
      static Result Status(__int64 domainId, __int64 now);

      // Rotate now: a key and a selector (the one given, or the suggestion), written
      // under <DataDirectory>/DKIM and staged as the domain's secondary.
      static Result Start(__int64 domainId, const AnsiString &selector, __int64 now);

      // Checks the next, current and retiring records from the resolver now.
      static Result CheckNow(__int64 domainId, __int64 now);

      // Moves signing to the next key. Refused unless its record matches in a check
      // made now; force skips the propagation period and nothing else.
      static Result Switch(__int64 domainId, bool force, __int64 now);

      // Retires the old selector now, rather than at the end of the retention period.
      static Result Retire(__int64 domainId, __int64 now);

      // Abandons the next key: the domain's secondary is emptied and a key the
      // rotation generated for it is removed. Signing is not touched.
      static Result Cancel(__int64 domainId, __int64 now);

      // The key files a domain's rotation generated, as PersistentDomain::DeleteObject
      // collects them before the domain's row goes (the rotation's row cascades
      // from it, and is what says which files the rotation made) and hands them
      // back once the delete has succeeded. Empty while a KeepKeysOnDomainDelete is
      // alive on this thread.
      struct DeletedDomainKeys
      {
         DeletedDomainKeys() : domain_id(0) {}

         __int64 domain_id;
         std::vector<String> files;
      };

      static DeletedDomainKeys KeysOfDomainBeingDeleted(__int64 domainId);

      // Removes them, under the guard above, once the domain is gone. Never before:
      // a delete that fails leaves a domain still signing with its files.
      static void RemoveKeysOfDeletedDomain(const DeletedDomainKeys &keys);

      // While one of these is alive on the thread, deleting a domain removes no key
      // file. BackupExecuter holds one across a restore, which deletes every domain
      // before it loads the backup's: the domains it puts back name the same key
      // files, and a restore without the data directory has no way to put a
      // deleted private key back.
      class KeepKeysOnDomainDelete
      {
      public:
         KeepKeysOnDomainDelete();
         ~KeepKeysOnDomainDelete();

         static bool Active();

      private:
         KeepKeysOnDomainDelete(const KeepKeysOnDomainDelete &);
         KeepKeysOnDomainDelete &operator=(const KeepKeysOnDomainDelete &);

         static thread_local int depth_;
      };

      // Whether the rotation manages this domain's keys: its schedule is on, or a
      // next or retiring selector is in flight.
      static bool IsManaged(const Domain &stored);

      // The stale-save guard. PersistentDomain::SaveObject is the one place a COM
      // Save and a REST PUT both pass, and a whole-domain save built from values read
      // before the rotation last moved would put the old selector back - after the
      // old key is retired, that is a domain signing with a key file that no longer
      // exists. True, with the sentence, when proposed changes any of the four key
      // columns of a managed domain. Restores and renames do not come here.
      static bool RefusesKeyChange(const Domain &stored, const Domain &proposed, String &reason);

      // For the dkim.rotation.stalled alert: every domain whose next key's record has
      // not been seen for thresholdDays, or whose rotation is running (schedule on,
      // or a next or retiring selector in flight) and whose last attempt to move it
      // failed - DkimRotationPlan::Stalled. Stored state only - no lookup. True when
      // there is one; summary names them.
      static bool CollectStalled(__int64 now, int thresholdDays, String &summary);

      // <DataDirectory>/DKIM, or empty when the data directory is not configured.
      static String KeyDirectory();
   };
}
