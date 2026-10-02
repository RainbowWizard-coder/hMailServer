// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PreReqGreyListKeys.h"
#include "PrerequisiteList.h"

#include <algorithm>
#include <stdexcept>

#include "../DALConnection.h"
#include "../DALRecordset.h"
#include "../SQLCommand.h"
#include "../DatabaseSettings.h"
#include "../../BO/GreyListTriplet.h"
#include "../../Persistence/SchemaUpgrader.h"

#ifndef HM_PLATFORM_POSIX
#include "../DALConnectionFactory.h"
#include "../SQLCEConnection.h"
#include "../SQLScriptRunner.h"
#include "../../Application/IniFileSettings.h"
#include "../../Util/FileUtilities.h"
#include "../../Util/GUIDCreator.h"
#include "../../Util/PasswordGenerator.h"
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      struct UnkeyedRow_
      {
         __int64 id = 0;
         String sender;
         String recipient;
      };

      String Number_(__int64 value)
      {
         return Formatter::Format(_T("{0}"), value);
      }

      // TOP (n) on SQL Server and Compact, which wants the parentheses; LIMIT n
      // on MySQL and PostgreSQL. By the connection's own type rather than the
      // ini's, since the tester's connection is not the server's.
      String SelectLimited_(std::shared_ptr<DALConnection> connection, const String &columns, const String &rest, int rows)
      {
         switch (connection->GetSettings()->GetType())
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            return Formatter::Format(_T("select top ({0}) {1} from hm_greylisting_triplets {2}"), rows, columns, rest);
         default:
            return Formatter::Format(_T("select {0} from hm_greylisting_triplets {1} limit {2}"), columns, rest, rows);
         }
      }

      bool Open_(std::shared_ptr<DALConnection> connection, const SQLCommand &command, std::shared_ptr<DALRecordset> &recordset, String &error)
      {
         recordset = connection->CreateRecordset();
         if (!recordset || !recordset->Open(connection, command))
         {
            error = _T("This statement failed (the error log has the database's own words): ") + command.GetQueryString();
            return false;
         }

         return true;
      }

      String IdList_(const std::vector<__int64> &ids, size_t begin, size_t end)
      {
         String list;
         for (size_t i = begin; i < end; i++)
         {
            if (i > begin)
               list += _T(", ");
            list += Number_(ids[i]);
         }

         return list;
      }

      // The rows named, deleted a statement at a time. `onlyUnkeyed` keeps the
      // statement to rows that still have no key, so that a row a server still
      // running on the database keyed in the meantime is not taken with them.
      bool DeleteIds_(std::shared_ptr<DALConnection> connection, const std::vector<__int64> &ids, int statementRows, bool onlyUnkeyed, String &error)
      {
         for (size_t begin = 0; begin < ids.size(); begin += statementRows)
         {
            const size_t end = std::min(ids.size(), begin + (size_t) statementRows);

            String sql = _T("delete from hm_greylisting_triplets where glid in (") + IdList_(ids, begin, end) + _T(")");
            if (onlyUnkeyed)
               sql += _T(" and (glsenderhash is null or glrecipienthash is null)");

            if (!connection->Execute(SQLCommand(sql), error, 0, 0))
               return false;
         }

         return true;
      }

      // Half an hour a statement for the pass, as SQLScriptRunner allows a
      // script's, and the server-wide limit back on the way out: the bounds and
      // the grouping each read the whole table once, which on a large one can
      // take longer than DatabaseStatementTimeout allows an ordinary statement.
      class PassTimeout_
      {
      public:
         explicit PassTimeout_(std::shared_ptr<DALConnection> connection) :
            connection_(connection)
         {
            connection_->SetTimeout(60 * 30);
         }

         ~PassTimeout_()
         {
            try
            {
               connection_->ApplyDefaultTimeout();
            }
            catch (...)
            {
               // Never out of a destructor: the connection keeps the half hour
               // until the next ApplyDefaultTimeout, which is all it costs.
               LOG_DEBUG("PreReqGreyListKeys: the statement timeout could not be put back after the pass.");
            }
         }

      private:
         std::shared_ptr<DALConnection> connection_;
      };

      bool ReadVersion_(std::shared_ptr<DALConnection> connection, int &version, String &error)
      {
         std::shared_ptr<DALRecordset> recordset;
         if (!Open_(connection, SQLCommand(_T("select value from hm_dbversion")), recordset, error))
            return false;

         version = recordset->IsEOF() ? 0 : (int) recordset->GetLongValue("value");
         return true;
      }

      // Every row with a NULL key: deleted when it cannot be keyed honestly,
      // keyed otherwise. The rows are walked by ranges of glid rather than by
      // "the next n rows without a key", which no index answers (the key's
      // index leads with the address): a range is a seek on the primary key on
      // every backend, and an empty one doubles the next, so a table whose
      // legacy rows are few and far apart is crossed in a few dozen queries.
      bool KeyTheUnkeyed_(std::shared_ptr<DALConnection> connection, int batchRows, int statementRows, int maxBatches, PreReqGreyListKeys::Counts &counts, bool &stoppedEarly, String &error)
      {
         stoppedEarly = false;

         std::shared_ptr<DALRecordset> bounds;
         if (!Open_(connection, SQLCommand(_T("select min(glid) as low, max(glid) as high from hm_greylisting_triplets where glsenderhash is null or glrecipienthash is null")), bounds, error))
            return false;

         if (bounds->IsEOF() || bounds->GetIsNull("low"))
            return true;

         const __int64 low = bounds->GetInt64Value("low");
         const __int64 high = bounds->GetInt64Value("high");
         bounds.reset();

         const __int64 widest = (__int64) 1 << 24;
         __int64 width = batchRows;
         __int64 from = low - 1;
         int batches = 0;

         while (from < high)
         {
            if (maxBatches >= 0 && batches >= maxBatches)
            {
               stoppedEarly = true;
               return true;
            }

            const __int64 to = (high - from > width) ? from + width : high;

            String rest = Formatter::Format(_T("where glid > {0} and glid <= {1} and (glsenderhash is null or glrecipienthash is null) order by glid"), from, to);
            SQLCommand select(SelectLimited_(connection, _T("glid, glsenderaddress, glrecipientaddress"), rest, batchRows));

            std::vector<UnkeyedRow_> rows;
            {
               std::shared_ptr<DALRecordset> recordset;
               if (!Open_(connection, select, recordset, error))
                  return false;

               while (!recordset->IsEOF())
               {
                  UnkeyedRow_ row;
                  row.id = recordset->GetInt64Value("glid");
                  row.sender = recordset->GetStringValue("glsenderaddress");
                  row.recipient = recordset->GetStringValue("glrecipientaddress");
                  rows.push_back(row);
                  recordset->MoveNext();
               }
            }

            std::vector<__int64> truncated;
            std::vector<__int64> unkeyable;
            std::vector<PreReqGreyListKeys::KeyedRow> keyed;

            for (const UnkeyedRow_ &row : rows)
            {
               PreReqGreyListKeys::KeyedRow keys;
               keys.id = row.id;

               switch (PreReqGreyListKeys::Classify(row.sender, row.recipient, keys.senderKey, keys.recipientKey))
               {
               case PreReqGreyListKeys::Key:
                  keyed.push_back(keys);
                  break;
               case PreReqGreyListKeys::DeleteTruncated:
                  truncated.push_back(row.id);
                  break;
               default:
                  unkeyable.push_back(row.id);
                  break;
               }
            }

            if (!DeleteIds_(connection, truncated, statementRows, true, error) ||
                !DeleteIds_(connection, unkeyable, statementRows, true, error))
               return false;

            for (size_t begin = 0; begin < keyed.size(); begin += statementRows)
            {
               const size_t end = std::min(keyed.size(), begin + (size_t) statementRows);
               std::vector<PreReqGreyListKeys::KeyedRow> chunk(keyed.begin() + begin, keyed.begin() + end);

               const String statement = PreReqGreyListKeys::KeyStatement(chunk);
               if (statement.IsEmpty())
               {
                  error = _T("A greylisting key was computed that is not 32 hexadecimal characters; nothing was written for its rows.");
                  return false;
               }

               if (!connection->Execute(SQLCommand(statement), error, 0, 0))
                  return false;
            }

            counts.truncated += (__int64) truncated.size();
            counts.unkeyable += (__int64) unkeyable.size();
            counts.keyed += (__int64) keyed.size();
            batches++;

            if ((int) rows.size() >= batchRows)
            {
               // The range holds more than one batch: go on from the last row
               // read, in the same range.
               from = rows.back().id;
            }
            else
            {
               from = to;
               width = rows.empty() ? std::min(width * 2, widest) : (__int64) batchRows;
            }
         }

         return true;
      }

      // Every triplet with more than one row keeps one. Rows still without a key
      // are not grouped: two of them with the same address are two different
      // senders until they are keyed.
      bool OneRowPerTriplet_(std::shared_ptr<DALConnection> connection, int statementRows, PreReqGreyListKeys::Counts &counts, String &error)
      {
         struct Group
         {
            __int64 ip1 = 0;
            bool ipv4 = false;
            __int64 ip2 = 0;
            String senderKey;
            String recipientKey;
         };

         std::vector<Group> groups;
         {
            std::shared_ptr<DALRecordset> recordset;
            if (!Open_(connection, SQLCommand(_T("select glipaddress1, glipaddress2, glsenderhash, glrecipienthash from hm_greylisting_triplets "
                                                  "where glsenderhash is not null and glrecipienthash is not null "
                                                  "group by glipaddress1, glipaddress2, glsenderhash, glrecipienthash having count(*) > 1")), recordset, error))
               return false;

            while (!recordset->IsEOF())
            {
               Group group;
               group.ip1 = recordset->GetInt64Value("glipaddress1");
               group.ipv4 = recordset->GetIsNull("glipaddress2");
               group.ip2 = group.ipv4 ? 0 : recordset->GetInt64Value("glipaddress2");
               group.senderKey = recordset->GetStringValue("glsenderhash");
               group.recipientKey = recordset->GetStringValue("glrecipienthash");
               groups.push_back(group);
               recordset->MoveNext();
            }
         }

         for (const Group &group : groups)
         {
            String sql = Formatter::Format(_T("select glid, glpassedcount from hm_greylisting_triplets where glipaddress1 = {0} and glipaddress2 {1} and glsenderhash = @SENDERHASH and glrecipienthash = @RECIPIENTHASH"),
               group.ip1, group.ipv4 ? String(_T("is null")) : _T("= ") + Number_(group.ip2));

            SQLCommand select(sql);
            select.AddParameter("@SENDERHASH", group.senderKey);
            select.AddParameter("@RECIPIENTHASH", group.recipientKey);

            std::vector<PreReqGreyListKeys::Copy> copies;
            {
               std::shared_ptr<DALRecordset> recordset;
               if (!Open_(connection, select, recordset, error))
                  return false;

               while (!recordset->IsEOF())
               {
                  PreReqGreyListKeys::Copy copy;
                  copy.id = recordset->GetInt64Value("glid");
                  copy.passed = (int) recordset->GetLongValue("glpassedcount");
                  copies.push_back(copy);
                  recordset->MoveNext();
               }
            }

            if (copies.size() < 2)
               continue;

            const size_t keep = PreReqGreyListKeys::ChooseSurvivor(copies);

            std::vector<__int64> others;
            for (size_t i = 0; i < copies.size(); i++)
            {
               if (i != keep)
                  others.push_back(copies[i].id);
            }

            if (!DeleteIds_(connection, others, statementRows, false, error))
               return false;

            counts.duplicates += (__int64) others.size();
         }

         return true;
      }
   }

   bool
   PreReqGreyListKeys::Ensure(std::shared_ptr<DALConnection> connection, const PrerequisiteReport &report, String &sErrorMessage)
   {
      Counts counts;
      String error;

      if (!Run(connection, 5000, 100, -1, counts, error))
      {
         sErrorMessage = _T("The greylisting triplets could not be given their keys (schema 6107's prerequisite). ")
                         _T("The rows keyed outside a transaction stay keyed, and running the upgrade again continues from the first row still without its key. ") + error;
         return false;
      }

      if (counts.skipped)
         return true;

      // To the walker that ran it - the output of --upgrade-database, the
      // UpgradeOnStart lines - or, on the COM route DBUpdater takes, to the
      // application log: kept until the service has read its settings, since
      // a service that refused the database's version has not, and its logger
      // drops every line until it has (SchemaUpgrader::Note).
      const String line = Summary(counts);
      if (report)
         report(line);
      else
         SchemaUpgrader::Note(line);

      return true;
   }

   bool
   PreReqGreyListKeys::Run(std::shared_ptr<DALConnection> connection, int batchRows, int statementRows, int maxBatches, Counts &counts, String &error)
   {
      counts = Counts();

      if (batchRows < 1)
         batchRows = 1;
      if (statementRows < 1)
         statementRows = 1;

      int version = 0;
      if (!ReadVersion_(connection, version, error))
         return false;

      if (version < KeyColumnsSchema)
      {
         counts.skipped = true;
         return true;
      }

      PassTimeout_ timeout(connection);

      bool stoppedEarly = false;
      if (!KeyTheUnkeyed_(connection, batchRows, statementRows, maxBatches, counts, stoppedEarly, error))
         return false;

      // A pass stopped part-way leaves rows without their key, and those are not
      // grouped; the pass that finishes groups everything.
      if (stoppedEarly)
         return true;

      if (!OneRowPerTriplet_(connection, statementRows, counts, error))
         return false;

      counts.complete = true;
      return true;
   }

   PreReqGreyListKeys::Fate
   PreReqGreyListKeys::Classify(const String &sender, const String &recipient, String &senderKey, String &recipientKey)
   {
      senderKey.Empty();
      recipientKey.Empty();

      // At or past the old width: the address was cut when it was written, and
      // the row may stand for several senders or recipients at once.
      if (sender.GetLength() >= LegacyAddressWidth || recipient.GetLength() >= LegacyAddressWidth)
         return DeleteTruncated;

      // The function every new row is written with (PersistentGreyList::AddObject)
      // and every lookup asks with (GreyListing::GetVerdict).
      const String sKey = GreyListTriplet::ComputeKey(sender);
      const String rKey = GreyListTriplet::ComputeKey(recipient);

      if (!IsKey(sKey) || !IsKey(rKey))
         return DeleteUnkeyable;

      senderKey = sKey;
      recipientKey = rKey;
      return Key;
   }

   size_t
   PreReqGreyListKeys::ChooseSurvivor(const std::vector<Copy> &copies)
   {
      size_t best = 0;
      for (size_t i = 1; i < copies.size(); i++)
      {
         const Copy &candidate = copies[i];
         const Copy &kept = copies[best];

         if (candidate.passed > kept.passed || (candidate.passed == kept.passed && candidate.id < kept.id))
            best = i;
      }

      return best;
   }

   bool
   PreReqGreyListKeys::IsKey(const String &value)
   {
      if (value.GetLength() != 32)
         return false;

      for (const wchar_t c : value)
      {
         const bool digit = c >= L'0' && c <= L'9';
         const bool hex = c >= L'a' && c <= L'f';
         if (!digit && !hex)
            return false;
      }

      return true;
   }

   String
   PreReqGreyListKeys::KeyStatement(const std::vector<KeyedRow> &rows)
   {
      if (rows.empty())
         return String();

      String senderCase = _T("case glid");
      String recipientCase = _T("case glid");
      String ids;

      for (size_t i = 0; i < rows.size(); i++)
      {
         const KeyedRow &row = rows[i];

         if (!IsKey(row.senderKey) || !IsKey(row.recipientKey))
            return String();

         senderCase += Formatter::Format(_T(" when {0} then '{1}'"), row.id, row.senderKey);
         recipientCase += Formatter::Format(_T(" when {0} then '{1}'"), row.id, row.recipientKey);

         if (i > 0)
            ids += _T(", ");
         ids += Number_(row.id);
      }

      return _T("update hm_greylisting_triplets set glsenderhash = ") + senderCase + _T(" end, glrecipienthash = ") + recipientCase +
             _T(" end where glid in (") + ids + _T(") and (glsenderhash is null or glrecipienthash is null)");
   }

   String
   PreReqGreyListKeys::Summary(const Counts &counts)
   {
      if (counts.keyed == 0 && counts.truncated == 0 && counts.unkeyable == 0 && counts.duplicates == 0)
         return _T("Greylisting triplets (schema 6107): every row has its key and every triplet one row; nothing to change.");

      return Formatter::Format(_T("Greylisting triplets (schema 6107): {0} given their key; {1} deleted because an address in them had been cut to 200 characters; ")
                               _T("{2} deleted because no key could be computed for them; {3} deleted as second rows of a triplet that keeps one."),
                               counts.keyed, counts.truncated, counts.unkeyable, counts.duplicates);
   }

   // ---- the tester -----------------------------------------------------------

   namespace
   {
      void Fail_(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(sentence).c_str()));
      }

      void Expect_(bool condition, const String &sentence)
      {
         if (!condition)
            Fail_(sentence);
      }

      String Repeat_(wchar_t c, int count)
      {
         String text;
         for (int i = 0; i < count; i++)
            text += c;
         return text;
      }

      // The pure half: the key itself, what becomes of a row, the survivor, and
      // the statement with its refusal.
      void TestThePureParts_()
      {
         // The key is the documented function (docs/Greylisting.md), held to MD5
         // vectors computed outside this code: whitespace off both ends, the
         // domain folded, the local part kept, UTF-8.
         Expect_(GreyListTriplet::ComputeKey(_T(" Alice@Example.COM\t")) == _T("a0d0dfd20e4338c402ebd4084ea8bd91"),
            _T("ComputeKey(\" Alice@Example.COM\\t\") is not the MD5 of \"Alice@example.com\"."));
         Expect_(GreyListTriplet::ComputeKey(_T("")) == _T("d41d8cd98f00b204e9800998ecf8427e"),
            _T("ComputeKey of the null sender is not the MD5 of nothing."));
         Expect_(GreyListTriplet::ComputeKey(_T("j\u00f6ran@EXAMPLE.org")) == _T("1f7d69619a780636402b5f10c7831286"),
            _T("ComputeKey of a non-ASCII local part is not the MD5 of its UTF-8."));
         Expect_(GreyListTriplet::ComputeKey(_T("ALICE@example.com")) != GreyListTriplet::ComputeKey(_T("Alice@example.com")),
            _T("ComputeKey folded the local part, which the key keeps as received."));

         // The fate of a row without its key: 199 characters is an address, 200 is
         // the old columns' cut, on either side.
         const String at = _T("@example.test");
         const String sender199 = Repeat_(L'a', 199 - at.GetLength()) + at;
         const String sender200 = Repeat_(L'a', 200 - at.GetLength()) + at;
         Expect_(sender199.GetLength() == 199 && sender200.GetLength() == 200, _T("The tester's addresses are not the lengths it says."));

         String senderKey, recipientKey;
         Expect_(PreReqGreyListKeys::Classify(sender199, _T("bob@example.test"), senderKey, recipientKey) == PreReqGreyListKeys::Key,
            _T("A 199-character address was not keyed."));
         Expect_(senderKey == GreyListTriplet::ComputeKey(sender199) && recipientKey == _T("94dbbb40db96c96463c3e6402e50a40c"),
            _T("A keyed row's keys are not the ones new rows are written with."));
         Expect_(PreReqGreyListKeys::Classify(sender200, _T("bob@example.test"), senderKey, recipientKey) == PreReqGreyListKeys::DeleteTruncated,
            _T("A 200-character sender, the old columns' cut, was not deleted."));
         Expect_(senderKey.IsEmpty() && recipientKey.IsEmpty(), _T("A row to delete came back with keys."));
         Expect_(PreReqGreyListKeys::Classify(_T("bob@example.test"), sender200, senderKey, recipientKey) == PreReqGreyListKeys::DeleteTruncated,
            _T("A 200-character recipient, the old columns' cut, was not deleted."));
         Expect_(PreReqGreyListKeys::Classify(_T(""), _T("bob@example.test"), senderKey, recipientKey) == PreReqGreyListKeys::Key,
            _T("The null sender's row was not keyed."));

         // One row of a triplet is kept: the most passed, then the oldest. A
         // confirmed row beats an older pending one.
         std::vector<PreReqGreyListKeys::Copy> copies;
         copies.push_back({ 5, 0 });
         copies.push_back({ 3, 0 });
         copies.push_back({ 9, 0 });
         Expect_(PreReqGreyListKeys::ChooseSurvivor(copies) == 1, _T("Of pending copies, the oldest was not kept."));
         copies.push_back({ 12, 2 });
         copies.push_back({ 10, 2 });
         Expect_(PreReqGreyListKeys::ChooseSurvivor(copies) == 4, _T("Of copies that passed, the most passed and then the oldest was not kept."));
         copies.push_back({ 1, 7 });
         Expect_(PreReqGreyListKeys::ChooseSurvivor(copies) == 5, _T("The copy that passed most was not kept."));

         Expect_(PreReqGreyListKeys::IsKey(_T("a0d0dfd20e4338c402ebd4084ea8bd91")), _T("A key was not taken for one."));
         Expect_(!PreReqGreyListKeys::IsKey(_T("A0D0DFD20E4338C402EBD4084EA8BD91")), _T("Upper-case hex was taken for a key."));
         Expect_(!PreReqGreyListKeys::IsKey(_T("a0d0dfd20e4338c402ebd4084ea8bd9")), _T("31 characters were taken for a key."));
         Expect_(!PreReqGreyListKeys::IsKey(_T("a0d0dfd20e4338c402ebd4084ea8bd9'")), _T("A quote was taken for part of a key."));
         Expect_(!PreReqGreyListKeys::IsKey(_T("")), _T("Nothing was taken for a key."));

         std::vector<PreReqGreyListKeys::KeyedRow> rows;
         rows.push_back({ 7, _T("a0d0dfd20e4338c402ebd4084ea8bd91"), _T("94dbbb40db96c96463c3e6402e50a40c") });
         rows.push_back({ 12, _T("e85bdea72f763d76279a16f7090151a4"), _T("858a7476b348b87420b2b0ca2e2c8972") });
         const String expected =
            _T("update hm_greylisting_triplets set glsenderhash = case glid when 7 then 'a0d0dfd20e4338c402ebd4084ea8bd91' when 12 then 'e85bdea72f763d76279a16f7090151a4' end, ")
            _T("glrecipienthash = case glid when 7 then '94dbbb40db96c96463c3e6402e50a40c' when 12 then '858a7476b348b87420b2b0ca2e2c8972' end ")
            _T("where glid in (7, 12) and (glsenderhash is null or glrecipienthash is null)");
         Expect_(PreReqGreyListKeys::KeyStatement(rows) == expected, _T("The key statement is not the one expected: ") + PreReqGreyListKeys::KeyStatement(rows));

         rows.push_back({ 13, _T("x'; drop table hm_accounts; --"), _T("858a7476b348b87420b2b0ca2e2c8972") });
         Expect_(PreReqGreyListKeys::KeyStatement(rows).IsEmpty(), _T("A value that is not a key reached the key statement."));
         Expect_(PreReqGreyListKeys::KeyStatement(std::vector<PreReqGreyListKeys::KeyedRow>()).IsEmpty(), _T("No rows made a statement."));

         PreReqGreyListKeys::Counts counts;
         counts.keyed = 11;
         counts.truncated = 2;
         counts.duplicates = 3;
         const String summary = PreReqGreyListKeys::Summary(counts);
         Expect_(summary.Find(_T("11 given their key")) >= 0 && summary.Find(_T("2 deleted because an address")) >= 0 &&
                 summary.Find(_T("0 deleted because no key")) >= 0 && summary.Find(_T("3 deleted as second rows")) >= 0,
            _T("The summary does not say what the pass did: ") + summary);

         // The summary's way to the application log where nobody is listening
         // yet (SchemaUpgrader::Note, the COM route DBUpdater takes): kept while
         // the logger's mask has no application log - 0 until the service has
         // read its settings - and written at once where it has; kept in order,
         // MaxNotes at most, handed over once. Whatever was kept before the
         // tester ran is put back.
         Expect_(SchemaUpgrader::KeepsForLater(0), _T("A line was not kept for later under the mask a service has before it reads its settings."));
         Expect_(SchemaUpgrader::KeepsForLater(Logger::LSEnabled | Logger::LSDebug), _T("A line was not kept for later with the application log off."));
         Expect_(!SchemaUpgrader::KeepsForLater(Logger::LSEnabled | Logger::LSApplication), _T("A line was kept for later with the application log on."));

         const std::vector<String> keptBefore = SchemaUpgrader::TakeNotes();
         SchemaUpgrader::Keep(_T("first"));
         SchemaUpgrader::Keep(_T("second"));
         const std::vector<String> taken = SchemaUpgrader::TakeNotes();
         Expect_(taken.size() == 2 && taken[0] == _T("first") && taken[1] == _T("second"), _T("The lines kept were not handed over, or not in the order they came."));
         Expect_(SchemaUpgrader::TakeNotes().empty(), _T("Lines handed over were handed over again."));
         for (size_t i = 0; i < SchemaUpgrader::MaxNotes + 5; i++)
            SchemaUpgrader::Keep(_T("line"));
         const size_t keptAtMost = SchemaUpgrader::TakeNotes().size();
         Expect_(keptAtMost == SchemaUpgrader::MaxNotes, Formatter::Format(_T("{0} lines were kept, not the {1} at most."), (__int64) keptAtMost, (__int64) SchemaUpgrader::MaxNotes));
         for (const String &line : keptBefore)
            SchemaUpgrader::Keep(line);
      }

#ifndef HM_PLATFORM_POSIX
      std::shared_ptr<DALConnection> Connection_;

      void Exec_(const String &sql)
      {
         String error;
         if (!Connection_->Execute(SQLCommand(sql), error, 0, 0))
            Fail_(_T("The tester's statement failed: ") + sql + _T(" - ") + error);
      }

      bool TryExec_(const SQLCommand &command)
      {
         // The statement's own failure is what is being tested, so the error log
         // is told nothing: TryExecute is the call underneath Execute's retries
         // and its HM5032 report.
         String error;
         return Connection_->TryExecute(command, error, 0, 0) == DALConnection::DALSuccess;
      }

      __int64 Scalar_(const String &sql)
      {
         std::shared_ptr<DALRecordset> recordset = Connection_->CreateRecordset();
         if (!recordset || !recordset->Open(Connection_, SQLCommand(sql)) || recordset->IsEOF())
            Fail_(_T("The tester's query failed: ") + sql);
         return recordset->GetIsNull("v") ? -1 : recordset->GetInt64Value("v");
      }

      String Text_(const String &sql)
      {
         std::shared_ptr<DALRecordset> recordset = Connection_->CreateRecordset();
         if (!recordset || !recordset->Open(Connection_, SQLCommand(sql)))
            Fail_(_T("The tester's query failed: ") + sql);
         if (recordset->IsEOF() || recordset->GetIsNull("v"))
            return _T("<none>");
         return recordset->GetStringValue("v");
      }

      SQLCommand Insert_(__int64 ip1, bool ipv4, __int64 ip2, const String &sender, const String &recipient, const String &senderKey, const String &recipientKey, int passed)
      {
         String sql = Formatter::Format(_T("insert into hm_greylisting_triplets (glcreatetime, glblockendtime, gldeletetime, glipaddress1, glipaddress2, glsenderaddress, glrecipientaddress, ")
                                        _T("glblockedcount, glpassedcount, glsenderhash, glrecipienthash) values (getdate(), getdate(), getdate(), {0}, {1}, @SENDER, @RECIPIENT, 0, {2}, {3}, {4})"),
            ip1, ipv4 ? String(_T("null")) : Number_(ip2), passed,
            senderKey.IsEmpty() ? String(_T("null")) : String(_T("@SENDERHASH")),
            recipientKey.IsEmpty() ? String(_T("null")) : String(_T("@RECIPIENTHASH")));

         SQLCommand command(sql);
         command.AddParameter("@SENDER", sender);
         command.AddParameter("@RECIPIENT", recipient);
         if (!senderKey.IsEmpty())
            command.AddParameter("@SENDERHASH", senderKey);
         if (!recipientKey.IsEmpty())
            command.AddParameter("@RECIPIENTHASH", recipientKey);
         return command;
      }

      __int64 Add_(__int64 ip1, bool ipv4, __int64 ip2, const String &sender, const String &recipient, const String &senderKey, const String &recipientKey, int passed)
      {
         String error;
         if (!Connection_->Execute(Insert_(ip1, ipv4, ip2, sender, recipient, senderKey, recipientKey, passed), error, 0, 0))
            Fail_(_T("The tester could not write a triplet: ") + error);
         return Scalar_(_T("select max(glid) as v from hm_greylisting_triplets"));
      }

      bool Exists_(__int64 id)
      {
         return Scalar_(Formatter::Format(_T("select count(*) as v from hm_greylisting_triplets where glid = {0}"), id)) == 1;
      }

      String KeyOf_(__int64 id, const String &column)
      {
         return Text_(Formatter::Format(_T("select {0} as v from hm_greylisting_triplets where glid = {1}"), column, id));
      }

      // The 6107 script as it ships. The service on a development bench runs
      // from the build output, whose DBScripts folder the build does not
      // refresh, so the source tree's is tried after the configured one.
      String FindStepScript_()
      {
         const String name = _T("Upgrade6106to6107MSSQLCE.sql");
         const String configured = IniFileSettings::Instance()->GetDBScriptDirectory() + FileUtilities::PathSeparator + name;
         if (FileUtilities::Exists(configured))
            return configured;

         const String source = IniFileSettings::Instance()->GetProgramDirectory() + _T("..\\..\\..\\..\\DBScripts\\") + name;
         if (FileUtilities::Exists(source))
            return source;

         Fail_(_T("The 6107 step's Compact script is neither at ") + configured + _T(" nor at ") + source);
         return String();
      }

      // The pass and the step, on a Compact database at 6106 holding every kind of
      // row the upgrade meets.
      void TestThePassAndTheStep_(const String &directory)
      {
         const String name = _T("GreyListKeysTest");
         const String password = PasswordGenerator::Generate();
         String error;
         if (!SQLCEConnection::CreateDatabase(directory, name, password, error))
            Fail_(_T("The tester's Compact database could not be created: ") + error);

         const String empty;
         std::shared_ptr<DatabaseSettings> settings(new DatabaseSettings(empty, empty, name, empty, password, directory, empty, DatabaseSettings::TypeMSSQLCompactEdition, 0));
         Connection_ = DALConnectionFactory::CreateConnection(settings);
         if (!Connection_ || Connection_->Connect(error) != DALConnection::Connected)
            Fail_(_T("The tester's Compact database could not be connected to: ") + error);

         // The table as schema 6072 to 6106 have it: keys that may be NULL, an
         // index that is not unique.
         Exec_(_T("create table hm_dbversion (value int)"));
         Exec_(_T("insert into hm_dbversion (value) values (6106)"));
         Exec_(_T("create table hm_greylisting_triplets (glid bigint identity(1,1) not null, glcreatetime datetime not null, glblockendtime datetime not null, ")
               _T("gldeletetime datetime not null, glipaddress1 bigint not null, glipaddress2 bigint null, glsenderaddress nvarchar(320) not null, ")
               _T("glrecipientaddress nvarchar(320) not null, glblockedcount int not null, glpassedcount int not null, glsenderhash nvarchar(32) null, glrecipienthash nvarchar(32) null)"));
         Exec_(_T("ALTER TABLE hm_greylisting_triplets ADD CONSTRAINT hm_glid_pk PRIMARY KEY (glid)"));
         Exec_(_T("CREATE INDEX idx_hm_greylisting_triplets_key ON hm_greylisting_triplets (glipaddress1, glipaddress2, glsenderhash, glrecipienthash)"));

         const String at = _T("@example.test");
         const String stub = Repeat_(L's', 200 - at.GetLength()) + at;
         const String none;
         const __int64 ipA = 3405803777LL;            // 203.0.113.1
         const __int64 ipB = 0x20010db800000000LL;    // 2001:db8::/64, upper half
         const __int64 ipB2 = 5;                      // ... ::5

         const String carolKey = _T("e85bdea72f763d76279a16f7090151a4");
         const String daveKey = _T("858a7476b348b87420b2b0ca2e2c8972");
         const String bobKey = _T("94dbbb40db96c96463c3e6402e50a40c");

         // A row from before 6072, keyed by the pass from the address it holds.
         const __int64 alice = Add_(ipA, true, 0, _T(" Alice@Example.COM"), _T("bob@example.test"), none, none, 0);
         // Two rows whose addresses were cut to the old columns' 200: deleted.
         const __int64 cutSender = Add_(ipA, true, 0, stub, _T("bob@example.test"), none, none, 4);
         const __int64 cutRecipient = Add_(ipA, true, 0, _T("bob@example.test"), stub, none, none, 0);
         // A row written since 6072 with a 200-character address and its key: an
         // address like another, kept. (The work item's rule would delete it.)
         const __int64 keyedStub = Add_(ipA, true, 0, stub, _T("bob@example.test"), GreyListTriplet::ComputeKey(stub), bobKey, 1);
         // A pending row from before 6072, older than the confirmed row the
         // server keyed since for the same triplet (the domain's case differs,
         // which the key folds): the pass keys it into a copy, and the
         // confirmed row is the one kept although it is the newer.
         const __int64 carolOld = Add_(ipA, true, 0, _T("carol@EXAMPLE.org"), _T("dave@example.test"), none, none, 0);
         const __int64 carolConfirmed = Add_(ipA, true, 0, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 3);
         // Two pending copies of an IPv6 triplet, as a race left them: the older kept.
         const __int64 raceFirst = Add_(ipB, false, ipB2, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 0);
         const __int64 raceSecond = Add_(ipB, false, ipB2, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 0);
         // The same keys from an IPv4 address equal to the IPv6 one's upper half:
         // another triplet (glipaddress2 NULL is not 5), kept.
         const __int64 otherFamily = Add_(ipB, true, 0, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 0);
         // Enough rows from before 6072 to need several batches.
         std::vector<__int64> many;
         for (int i = 0; i < 9; i++)
            many.push_back(Add_(ipA + 1 + i, true, 0, Formatter::Format(_T("sender{0}@example.org"), i), _T("bob@example.test"), none, none, 0));
         // ... and one with only its recipient's key, as nothing should write but
         // the pass must still finish.
         const __int64 halfKeyed = Add_(ipA, true, 0, _T("half@example.org"), _T("bob@example.test"), none, bobKey, 0);

         const __int64 unkeyedAtFirst = Scalar_(_T("select count(*) as v from hm_greylisting_triplets where glsenderhash is null or glrecipienthash is null"));
         Expect_(unkeyedAtFirst == 14, Formatter::Format(_T("The tester wrote {0} rows without a key, not 14."), unkeyedAtFirst));

         // Before 6072 there is nothing to key: the pass touches nothing.
         Exec_(_T("update hm_dbversion set value = 6071"));
         PreReqGreyListKeys::Counts counts;
         if (!PreReqGreyListKeys::Run(Connection_, 3, 2, -1, counts, error))
            Fail_(_T("The pass failed on a database at 6071: ") + error);
         Expect_(counts.skipped && !counts.complete && counts.keyed == 0, _T("The pass did not stand aside on a database older than the key columns."));
         Expect_(Scalar_(_T("select count(*) as v from hm_greylisting_triplets where glsenderhash is null or glrecipienthash is null")) == unkeyedAtFirst,
            _T("The pass changed rows on a database older than the key columns."));
         Exec_(_T("update hm_dbversion set value = 6106"));

         // Stopped after one batch, as an interrupted upgrade stops: what it did is
         // kept, and it said it had not finished.
         if (!PreReqGreyListKeys::Run(Connection_, 3, 2, 1, counts, error))
            Fail_(_T("The interrupted pass failed: ") + error);
         Expect_(!counts.complete && !counts.skipped, _T("A pass stopped after one batch said it had finished."));
         const __int64 doneByFirst = counts.keyed + counts.truncated + counts.unkeyable;
         Expect_(doneByFirst == 3, Formatter::Format(_T("One batch of three dealt with {0} rows."), doneByFirst));
         const __int64 unkeyedAfterFirst = Scalar_(_T("select count(*) as v from hm_greylisting_triplets where glsenderhash is null or glrecipienthash is null"));
         Expect_(unkeyedAfterFirst == unkeyedAtFirst - 3, _T("What the interrupted pass did was not kept."));
         Expect_(KeyOf_(alice, _T("glsenderhash")) == _T("a0d0dfd20e4338c402ebd4084ea8bd91") && KeyOf_(alice, _T("glrecipienthash")) == bobKey,
            _T("The first row's keys are not the ones a lookup asks with."));
         Expect_(!Exists_(cutSender) && !Exists_(cutRecipient), _T("A row cut to 200 characters was kept."));

         // Run again, it resumes and finishes: the rest keyed, the copies gone.
         if (!PreReqGreyListKeys::Run(Connection_, 3, 2, -1, counts, error))
            Fail_(_T("The resumed pass failed: ") + error);
         Expect_(counts.complete, _T("The resumed pass did not finish."));
         Expect_(counts.keyed + counts.truncated + counts.unkeyable == unkeyedAfterFirst,
            Formatter::Format(_T("The resumed pass dealt with {0} rows, not the {1} left."), counts.keyed + counts.truncated + counts.unkeyable, unkeyedAfterFirst));
         Expect_(counts.truncated == 0 && counts.unkeyable == 0, _T("The resumed pass deleted rows it should have keyed."));
         Expect_(counts.duplicates == 2, Formatter::Format(_T("The pass deleted {0} second rows, not 2."), counts.duplicates));
         Expect_(Scalar_(_T("select count(*) as v from hm_greylisting_triplets where glsenderhash is null or glrecipienthash is null")) == 0,
            _T("A row is still without its key after the pass."));

         Expect_(Exists_(keyedStub) && KeyOf_(keyedStub, _T("glsenderhash")) == GreyListTriplet::ComputeKey(stub),
            _T("A keyed row with a 200-character address was deleted or changed."));
         Expect_(Exists_(carolConfirmed) && !Exists_(carolOld), _T("Of a confirmed row and an older pending copy, the confirmed one was not the one kept."));
         Expect_(Exists_(raceFirst) && !Exists_(raceSecond), _T("Of two pending copies, the older was not the one kept."));
         Expect_(Exists_(otherFamily), _T("An IPv4 triplet was taken for a copy of an IPv6 one."));
         Expect_(KeyOf_(halfKeyed, _T("glsenderhash")) == GreyListTriplet::ComputeKey(_T("half@example.org")), _T("A row with one key was not given the other."));
         for (__int64 id : many)
            Expect_(KeyOf_(id, _T("glsenderhash")) != _T("<none>"), _T("A row of the many was not keyed."));

         // A third pass finds nothing to do.
         if (!PreReqGreyListKeys::Run(Connection_, 3, 2, -1, counts, error))
            Fail_(_T("The third pass failed: ") + error);
         Expect_(counts.complete && counts.keyed == 0 && counts.duplicates == 0, _T("A pass over a finished table changed something."));

         // Through the list, as the walkers call it: the step's prerequisite runs
         // before the transaction and reports; the 5200 one does not (it would
         // fail here, on a database with no hm_imapfolders).
         Exec_(_T("update hm_greylisting_triplets set glsenderhash = null where glid = ") + Number_(alice));
         std::vector<String> lines;
         PrerequisiteReport report = [&lines](const String &line) { lines.push_back(line); };
         PrerequisiteList list;
         if (!list.EnsureBeforeTransaction(Connection_, 6107, report, error))
            Fail_(_T("The 6107 prerequisite failed through the list: ") + error);
         Expect_(lines.size() == 1 && lines[0].Find(_T("1 given their key")) >= 0, _T("The 6107 prerequisite did not report what it did through the list."));
         if (!list.EnsureBeforeTransaction(Connection_, 5200, report, error))
            Fail_(_T("A prerequisite that does not run before the transaction was run there: ") + error);

         // With no walker to report to - the COM route DBUpdater takes - the line
         // goes to SchemaUpgrader::Note: kept until the service has read its
         // settings where the application log is not on yet, written at once
         // where it is. Whatever was kept before is put back.
         Exec_(_T("update hm_greylisting_triplets set glsenderhash = null where glid = ") + Number_(alice));
         const std::vector<String> keptBefore = SchemaUpgrader::TakeNotes();
         if (!list.EnsureBeforeTransaction(Connection_, 6107, PrerequisiteReport(), error))
            Fail_(_T("The 6107 prerequisite failed through the list with no report: ") + error);
         const std::vector<String> kept = SchemaUpgrader::TakeNotes();
         for (const String &line : keptBefore)
            SchemaUpgrader::Keep(line);
         if (SchemaUpgrader::KeepsForLater(Logger::Instance()->GetLogMask()))
            Expect_(kept.size() == 1 && kept[0].Find(_T("1 given their key")) >= 0, _T("With no report and the application log not on, the 6107 line was not kept for later."));
         else
            Expect_(kept.empty(), _T("With the application log on, the 6107 line was kept rather than written."));
         Expect_(KeyOf_(alice, _T("glsenderhash")) == _T("a0d0dfd20e4338c402ebd4084ea8bd91"), _T("The 6107 prerequisite with no report did not key the row."));

         // Before the step a row may still be written without a key, and a second
         // row for a triplet: the controls for what the step then refuses.
         const SQLCommand nullKey = Insert_(ipA + 100, true, 0, _T("x@example.org"), _T("y@example.org"), none, bobKey, 0);
         Expect_(TryExec_(nullKey), _T("The 6106 table refused a NULL key, so the refusal below proves nothing."));
         Exec_(_T("delete from hm_greylisting_triplets where glipaddress1 = ") + Number_(ipA + 100));
         const SQLCommand secondCopy = Insert_(ipA, true, 0, _T(" Alice@Example.COM"), _T("bob@example.test"), _T("a0d0dfd20e4338c402ebd4084ea8bd91"), bobKey, 0);
         Expect_(TryExec_(secondCopy), _T("The 6106 table refused a second row, so the refusal below proves nothing."));
         Exec_(_T("delete from hm_greylisting_triplets where glid = ") + Number_(Scalar_(_T("select max(glid) as v from hm_greylisting_triplets"))));

         // The step's script as it ships.
         const String script = FindStepScript_();
         SQLScriptRunner runner;
         if (!runner.ExecuteScript(Connection_, script, error))
            Fail_(_T("The 6107 Compact script failed on a keyed table: ") + error);
         Expect_(Scalar_(_T("select value as v from hm_dbversion")) == 6107, _T("The 6107 script did not stamp 6107."));

         Expect_(!TryExec_(nullKey), _T("After 6107 a row was written without its key."));
         Expect_(!TryExec_(secondCopy), _T("After 6107 a second row was written for an IPv4 triplet."));
         Expect_(!TryExec_(Insert_(ipB, false, ipB2, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 0)),
            _T("After 6107 a second row was written for an IPv6 triplet."));
         Expect_(TryExec_(Insert_(ipB, false, ipB2 + 1, _T("carol@example.org"), _T("dave@example.test"), carolKey, daveKey, 0)),
            _T("After 6107 another triplet - another address in the same /64 - could not be written."));

         // Compact commits each statement of a script, so a step stopped before
         // its last statement is run again whole: the script must allow that.
         Exec_(_T("update hm_dbversion set value = 6106"));
         if (!runner.ExecuteScript(Connection_, script, error))
            Fail_(_T("The 6107 Compact script failed when run a second time over its own work: ") + error);
         Expect_(Scalar_(_T("select value as v from hm_dbversion")) == 6107, _T("The 6107 script run again did not stamp 6107."));
         Expect_(!TryExec_(secondCopy), _T("The 6107 script run again left the key without its unique index."));

         Connection_->Disconnect();
         Connection_.reset();
      }
#endif
   }

   void
   PreReqGreyListKeysTester::Test()
   {
      TestThePureParts_();

#ifndef HM_PLATFORM_POSIX
      const String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("GreyListKeysTest-") + GUIDCreator::GetGUID();
      if (!FileUtilities::CreateDirectory(directory))
         Fail_(_T("The tester's directory could not be created under the temp folder."));

      try
      {
         TestThePassAndTheStep_(directory);
      }
      catch (...)
      {
         if (Connection_)
         {
            Connection_->Disconnect();
            Connection_.reset();
         }

         FileUtilities::DeleteDirectory(directory, true);
         throw;
      }

      FileUtilities::DeleteDirectory(directory, true);
#endif
   }
}
