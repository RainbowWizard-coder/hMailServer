// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../TCPIP/IPAddress.h"
#include "PersistenceMode.h"

namespace HM
{
   class SecurityRange;

   class PersistentSecurityRange
   {
   public:
      PersistentSecurityRange();
      virtual ~PersistentSecurityRange();

      static bool DeleteObject(std::shared_ptr<SecurityRange> pSR);
      static bool SaveObject(std::shared_ptr<SecurityRange> pSR);
      static bool SaveObject(std::shared_ptr<SecurityRange> pSR, String &result,  PersistenceMode mode);

      // SaveObject for a new range under a name the caller chose as free, which
      // the unique index on the name may still refuse: a range of that name can
      // have been written between the choice and the insert, by this server or by
      // another sharing the database, and the index, not the choice, decides: it
      // is asked alone, without the name check SaveObject makes first
      // (PreSaveLimitationsCheck). Its refusal is not reported as an error: it
      // answers false with nameTaken set,
      // so that the caller can write the range under another name. Every other
      // failure is reported and answered as SaveObject's, nameTaken false.
      static bool SaveNewObject(std::shared_ptr<SecurityRange> pSR, bool &nameTaken);

      // The most characters a range's name is stored with: the rangename column
      // is varchar(100) on MySQL and PostgreSQL and nvarchar(100) on SQL Server
      // and SQL Server Compact, UNIQUE on each.
      static const int MaxNameLength = 100;

      // text cut to at most units of its own (UTF-16 code units where wchar_t is
      // 16 bits, as on Windows, code points where it is 32), one fewer where the
      // cut would keep the first half of a surrogate pair without the second.
      // Every engine's column counts a character as no more than that, so text
      // cut to MaxNameLength fits the column on each, and the cut never leaves
      // half a character for the database to refuse or replace.
      static String CutName(const String &text, int units);

      // A range's name as the column stores it: CutName(name, MaxNameLength).
      // SaveObject writes it, and the object is given it, and Exists and
      // ReadObject(name) ask about it, so that a name longer than the column is
      // the name of the row it becomes - for an auto-ban and for a name an
      // administrator gave over COM or the REST API alike.
      static String StoredName(const String &name);

      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, std::shared_ptr<DALRecordset> pRS);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, const SQLCommand &command);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, __int64 ObjectID);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, const String &name);

      static std::shared_ptr<SecurityRange> ReadMatchingIP(const IPAddress &ipaddress);
      
      static bool DeleteExpired();

      // Whether a range is stored under name, as the column stores it
      // (StoredName), compared as the unique index compares: the name is bound
      // as a parameter, so that SQL Server and Compact compare it as nvarchar
      // under the column's own collation, and MySQL and PostgreSQL are given
      // the literal the INSERT is written with.
      static bool Exists(const String &name);

      // An auto-ban's row widened to lower..upper (Util/AutoBanWidener.h): the
      // row with that id, and only while it is still the single expiring IPv4
      // address it was written as - a row an administrator has edited since, or
      // one already lifted, is not touched. All three addresses IPv4. True when
      // the statement ran; the caller reads the row back to learn whether it
      // matched.
      static bool WidenAutoBan(__int64 id, const IPAddress &address, const IPAddress &lower, const IPAddress &upper);

   private:

      static bool Validate(std::shared_ptr<SecurityRange> pSR, String &result);

      // SaveObject's and SaveNewObject's write. nameTaken, when given, asks for
      // a refusal by the database's own rules on an insert to be forgiven
      // (DALConnection::DALErrorInSQL) and said there rather than reported.
      static bool Save_(std::shared_ptr<SecurityRange> pSR, String &result, PersistenceMode mode, bool *nameTaken);
   };

}
