// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PersistentArchiveIndex.h"

#include "../SQL/SQLStatement.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../Util/DiscoveryHolds.h"

namespace HM
{
   namespace
   {
      // A row-limited SELECT for each backend: TOP (n) on SQL Server and Compact,
      // which requires the parentheses, LIMIT n on MySQL and PostgreSQL - the
      // same knowledge AuditTrail and PersistentBounce apply.
      String SelectWithLimit(const String &columns, const String &from, const String &where, const String &orderBy, int rows)
      {
         DatabaseSettings::SQLDBType type = IniFileSettings::Instance()->GetDatabaseType();

         String statement;

         switch (type)
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            statement.Format(_T("select top (%d) %s from %s"), rows, columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            break;
         default:
            statement.Format(_T("select %s from %s"), columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            statement += Formatter::Format(" limit {0}", rows);
            break;
         }

         return statement;
      }

      // The domain and mailbox directories an archived file lies in, for a file
      // the index has no row for: <ArchiveDir>\<domain>\<mailbox>\<file>. False
      // for a file outside the archive or directly in its Inbound directory.
      bool PlaceOfFile(const String &path, String &domain, String &mailbox)
      {
         String root = IniFileSettings::Instance()->GetArchiveDir();
         if (root.IsEmpty() || path.GetLength() <= root.GetLength() || !path.StartsWith(root))
            return false;

         String rest = path.Mid(root.GetLength());
         rest.Replace(_T("\\"), _T("/"));
         while (rest.StartsWith(_T("/")))
            rest = rest.Mid(1);

         std::vector<String> parts = StringParser::SplitString(rest, _T("/"));
         if (parts.size() != 3)
            return false;

         domain = parts[0];
         mailbox = parts[1];
         return !domain.IsEmpty() && !mailbox.IsEmpty();
      }

      // A search term is used inside a LIKE, so its own wildcards are literal.
      String Contains(const String &term)
      {
         String escaped = term;
         escaped.Replace(_T("%"), _T("[%]"));
         escaped.Replace(_T("_"), _T("[_]"));
         return _T("%") + escaped + _T("%");
      }

      String Truncate(const String &value, int maxLength)
      {
         return value.GetLength() > maxLength ? value.Mid(0, maxLength) : value;
      }

      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString result;
         for (int i = 0; i < value.GetLength(); i++)
         {
            char c = value[i];
            switch (c)
            {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
               if (static_cast<unsigned char>(c) >= 0x20)
                  result += c;
               break;
            }
         }
         return result;
      }
   }

   __int64
   PersistentArchiveIndex::Record(int direction, const String &domain, const String &mailbox, const String &sender,
                                  const String &recipients, const String &subject, const String &messageId,
                                  const String &path, __int64 size)
   {
      SQLStatement statement;
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetTable("hm_archiveindex");
      statement.SetIdentityColumn("archiveid");
      statement.AddColumnCommand("archivetime", SQLStatement::GetCurrentTimestamp());
      statement.AddColumn("archivedomain", Truncate(domain, 255).ToLower());
      statement.AddColumn("archivemailbox", Truncate(mailbox, 255).ToLower());
      statement.AddColumn("archivedirection", (long) direction);
      statement.AddColumn("archivesender", Truncate(sender, 255));
      statement.AddColumn("archiverecipients", Truncate(recipients, 1000));
      statement.AddColumn("archivesubject", Truncate(subject, 255));
      statement.AddColumn("archivemessageid", Truncate(messageId, 255));
      statement.AddColumn("archivepath", Truncate(path, 1000));
      statement.AddColumnInt64("archivesize", size);
      statement.AddColumn("archivehold", (long) 0);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id))
         return 0;
      return id;
   }

   bool
   PersistentArchiveIndex::Read_(std::shared_ptr<DALRecordset> recordset, Entry &entry)
   {
      entry.id = recordset->GetInt64Value("archiveid");
      entry.time = recordset->GetStringValue("archivetime");
      entry.domain = recordset->GetStringValue("archivedomain");
      entry.mailbox = recordset->GetStringValue("archivemailbox");
      entry.direction = recordset->GetLongValue("archivedirection");
      entry.sender = recordset->GetStringValue("archivesender");
      entry.recipients = recordset->GetStringValue("archiverecipients");
      entry.subject = recordset->GetStringValue("archivesubject");
      entry.messageId = recordset->GetStringValue("archivemessageid");
      entry.path = recordset->GetStringValue("archivepath");
      entry.size = recordset->GetInt64Value("archivesize");
      entry.hold = recordset->GetLongValue("archivehold") != 0;
      return true;
   }

   bool
   PersistentArchiveIndex::Search(const Criteria &criteria, std::vector<Entry> &entries)
   {
      entries.clear();

      int maxRows = criteria.maxRows;
      if (maxRows < 1)
         maxRows = 1;
      if (maxRows > 1000)
         maxRows = 1000;

      // Every user-supplied value goes in as a parameter; the SQL text holds
      // only column names and the shape of the query.
      String sql = _T("select archiveid, archivetime, archivedomain, archivemailbox, archivedirection, archivesender, ")
                   _T("archiverecipients, archivesubject, archivemessageid, archivepath, archivesize, archivehold ")
                   _T("from hm_archiveindex where 1 = 1");
      SQLCommand command;

      if (!criteria.domain.IsEmpty())
         sql += _T(" and archivedomain = @DOMAIN");
      if (!criteria.mailbox.IsEmpty())
         sql += _T(" and archivemailbox = @MAILBOX");
      if (!criteria.sender.IsEmpty())
         sql += _T(" and archivesender like @SENDER");
      if (!criteria.recipient.IsEmpty())
         sql += _T(" and archiverecipients like @RECIPIENT");
      if (!criteria.subject.IsEmpty())
         sql += _T(" and archivesubject like @SUBJECT");
      if (!criteria.since.IsEmpty())
         sql += _T(" and archivetime >= @SINCE");
      if (!criteria.until.IsEmpty())
         sql += _T(" and archivetime <= @UNTIL");
      if (criteria.holdOnly)
         sql += _T(" and archivehold = 1");
      sql += _T(" order by archivetime desc, archiveid desc");

      command.SetQueryString(sql);
      if (!criteria.domain.IsEmpty())
         command.AddParameter("@DOMAIN", String(criteria.domain).ToLower());
      if (!criteria.mailbox.IsEmpty())
         command.AddParameter("@MAILBOX", String(criteria.mailbox).ToLower());
      if (!criteria.sender.IsEmpty())
         command.AddParameter("@SENDER", Contains(criteria.sender));
      if (!criteria.recipient.IsEmpty())
         command.AddParameter("@RECIPIENT", Contains(criteria.recipient));
      if (!criteria.subject.IsEmpty())
         command.AddParameter("@SUBJECT", Contains(criteria.subject));
      if (!criteria.since.IsEmpty())
         command.AddParameter("@SINCE", criteria.since);
      if (!criteria.until.IsEmpty())
         command.AddParameter("@UNTIL", criteria.until);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF() && (int) entries.size() < maxRows)
      {
         Entry entry;
         Read_(recordset, entry);
         entries.push_back(entry);
         recordset->MoveNext();
      }
      return true;
   }

   bool
   PersistentArchiveIndex::Get(__int64 id, Entry &entry)
   {
      SQLCommand command(_T("select archiveid, archivetime, archivedomain, archivemailbox, archivedirection, archivesender, ")
                         _T("archiverecipients, archivesubject, archivemessageid, archivepath, archivesize, archivehold ")
                         _T("from hm_archiveindex where archiveid = @ID"));
      command.AddParameter("@ID", id);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      return Read_(recordset, entry);
   }

   bool
   PersistentArchiveIndex::SetHold(__int64 id, bool hold)
   {
      SQLCommand command(_T("update hm_archiveindex set archivehold = @HOLD where archiveid = @ID"));
      command.AddParameter("@HOLD", hold ? 1 : 0);
      command.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentArchiveIndex::ListAfter(const String &domain, const String &mailbox, __int64 afterId, int maxRows, std::vector<Entry> &entries)
   {
      entries.clear();

      if (maxRows < 1)
         maxRows = 1;

      String where = _T("archiveid > @AFTER");
      if (!domain.IsEmpty())
         where += _T(" and archivedomain = @DOMAIN");
      if (!domain.IsEmpty() && !mailbox.IsEmpty())
         where += _T(" and archivemailbox = @MAILBOX");

      SQLCommand command(SelectWithLimit(_T("archiveid, archivetime, archivedomain, archivemailbox, archivedirection, archivesender, ")
                                         _T("archiverecipients, archivesubject, archivemessageid, archivepath, archivesize, archivehold"),
                                         _T("hm_archiveindex"), where, _T("archiveid"), maxRows));
      command.AddParameter("@AFTER", afterId);
      if (!domain.IsEmpty())
         command.AddParameter("@DOMAIN", String(domain).ToLower());
      if (!domain.IsEmpty() && !mailbox.IsEmpty())
         command.AddParameter("@MAILBOX", String(mailbox).ToLower());

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Entry entry;
         Read_(recordset, entry);
         entries.push_back(entry);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentArchiveIndex::IsHeld(const String &path)
   {
      // Every row the path has - a file is indexed once, but a question about a
      // file must not depend on that - and its hold: the row's own flag, or an
      // eDiscovery hold on the row, its account or its domain.
      SQLCommand command(_T("select archiveid, archivedomain, archivemailbox, archivehold from hm_archiveindex where archivepath = @PATH"));
      command.AddParameter("@PATH", Truncate(path, 1000));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      // A question the database cannot answer is answered held: the sweep and
      // the eraser leave the file for the next pass, which is what a hold asks.
      if (!recordset)
         return true;

      bool anyRow = false;

      while (!recordset->IsEOF())
      {
         anyRow = true;

         if (recordset->GetLongValue("archivehold") != 0)
            return true;

         if (DiscoveryHolds::IsArchiveEntryHeld(recordset->GetInt64Value("archiveid"),
                                                recordset->GetStringValue("archivedomain"),
                                                recordset->GetStringValue("archivemailbox")))
            return true;

         recordset->MoveNext();
      }

      // A file with no row is still an archived message; its account's or its
      // domain's hold is read from where it lies.
      if (!anyRow)
      {
         String domain, mailbox;
         if (PlaceOfFile(path, domain, mailbox) && DiscoveryHolds::IsArchiveEntryHeld(0, domain, mailbox))
            return true;
      }

      return false;
   }

   bool
   PersistentArchiveIndex::RemoveByPath(const String &path)
   {
      SQLCommand command(_T("delete from hm_archiveindex where archivepath = @PATH"));
      command.AddParameter("@PATH", Truncate(path, 1000));
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   int
   PersistentArchiveIndex::RemoveByAddress(const String &address)
   {
      // Rows whose sender is the address, or whose recipient list names it.
      // The count is what the eraser reports; rows for held copies are kept,
      // because a hold is a promise that nothing removes the record.
      String lowered = address;
      lowered.ToLower();

      // The rows one at a time rather than in one delete: a row an eDiscovery
      // hold covers - by its own hold, its account's or its domain's - is kept
      // as a flagged one is, and only the question can tell which those are.
      SQLCommand candidates(_T("select archiveid, archivedomain, archivemailbox from hm_archiveindex where archivehold = 0 and (archivesender = @ADDRESS or archiverecipients like @CONTAINS)"));
      candidates.AddParameter("@ADDRESS", lowered);
      candidates.AddParameter("@CONTAINS", Contains(lowered));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(candidates);
      if (!recordset)
         return 0;

      std::vector<__int64> removable;
      while (!recordset->IsEOF())
      {
         const __int64 id = recordset->GetInt64Value("archiveid");
         if (!DiscoveryHolds::IsArchiveEntryHeld(id, recordset->GetStringValue("archivedomain"), recordset->GetStringValue("archivemailbox")))
            removable.push_back(id);
         recordset->MoveNext();
      }

      int rows = 0;
      for (const __int64 id : removable)
      {
         SQLCommand remove(_T("delete from hm_archiveindex where archiveid = @ID and archivehold = 0"));
         remove.AddParameter("@ID", id);
         if (Application::Instance()->GetDBManager()->Execute(remove))
            rows++;
      }

      return rows;
   }

   AnsiString
   PersistentArchiveIndex::ToJson(const Entry &entry)
   {
      const char *direction = entry.direction == DirectionSent ? "sent"
                            : entry.direction == DirectionReceived ? "received" : "inbound";
      AnsiString json;
      json.Format("{\"id\":%I64d,\"time\":\"%hs\",\"domain\":\"%hs\",\"mailbox\":\"%hs\",\"direction\":\"%hs\",\"sender\":\"%hs\",",
         entry.id,
         JsonEscape(AnsiString(entry.time)).c_str(),
         JsonEscape(AnsiString(entry.domain)).c_str(),
         JsonEscape(AnsiString(entry.mailbox)).c_str(),
         direction,
         JsonEscape(AnsiString(entry.sender)).c_str());
      AnsiString rest;
      rest.Format("\"recipients\":\"%hs\",\"subject\":\"%hs\",\"message_id\":\"%hs\",\"path\":\"%hs\",\"size\":%I64d,\"hold\":%hs}",
         JsonEscape(AnsiString(entry.recipients)).c_str(),
         JsonEscape(AnsiString(entry.subject)).c_str(),
         JsonEscape(AnsiString(entry.messageId)).c_str(),
         JsonEscape(AnsiString(entry.path)).c_str(),
         entry.size,
         entry.hold ? "true" : "false");
      return json + rest;
   }

   AnsiString
   PersistentArchiveIndex::ToJson(const std::vector<Entry> &entries)
   {
      AnsiString json = "[";
      for (size_t i = 0; i < entries.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += ToJson(entries[i]);
      }
      json += "]";
      return json;
   }
}
