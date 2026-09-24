// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ListArchive.h"
#include "ListManager.h"
#include "File.h"
#include "GUIDCreator.h"
#include "Time.h"
#include "Unicode.h"
#include "Utilities.h"

#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/MessageData.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../SQL/DatabaseSettings.h"

#include <ctime>
#include <set>
#include <stdexcept>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A digest carries at most this many messages and this many bytes of
      // them; what does not fit goes in the next one.
      const size_t MostDigestMessages = 100;
      const __int64 MostDigestBytes = 10 * 1024 * 1024;

      // Twenty hours rather than twenty-four, as the quarantine digest does, so
      // that a clock that drifts, or an hour an administrator changed, does not
      // skip a day.
      const __int64 DigestSpacingSeconds = 20 * 3600;

      // Howard Hinnant's civil calendar arithmetic, the proleptic Gregorian
      // calendar in days since 1970-01-01, the same on every platform - a
      // month boundary must not depend on whether the build has timegm.
      __int64 DaysFromCivil(__int64 y, unsigned m, unsigned d)
      {
         y -= m <= 2 ? 1 : 0;
         const __int64 era = (y >= 0 ? y : y - 399) / 400;
         const unsigned yoe = (unsigned) (y - era * 400);
         const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
         const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
         return era * 146097 + (__int64) doe - 719468;
      }

      void CivilFromDays(__int64 z, __int64 &y, unsigned &m, unsigned &d)
      {
         z += 719468;
         const __int64 era = (z >= 0 ? z : z - 146096) / 146097;
         const unsigned doe = (unsigned) (z - era * 146097);
         const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
         y = (__int64) yoe + era * 400;
         const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
         const unsigned mp = (5 * doy + 2) / 153;
         d = doy - (153 * mp + 2) / 5 + 1;
         m = mp < 10 ? mp + 3 : mp - 9;
         y += m <= 2 ? 1 : 0;
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String OneLine(const String &value, int most)
      {
         String flat = value;
         flat.Replace(_T("\r"), _T(" "));
         flat.Replace(_T("\n"), _T(" "));
         flat.Replace(_T("\t"), _T(" "));
         flat.Trim();

         if (flat.GetLength() > most)
            flat = flat.Mid(0, most);

         return flat;
      }

      bool ReadEntry(std::shared_ptr<DALRecordset> recordset, ListArchiveEntry &entry)
      {
         entry.id = recordset->GetInt64Value("laid");
         entry.list_id = recordset->GetInt64Value("lalistid");
         entry.message_id = recordset->GetStringValue("lamessageid");
         entry.subject = recordset->GetStringValue("lasubject");
         entry.sender = recordset->GetStringValue("lasender");
         entry.sent = recordset->GetInt64Value("lasent");
         entry.size = (int) recordset->GetLongValue("lasize");
         entry.file = recordset->GetStringValue("lafile");
         return true;
      }

      // The stored file name is ours - a GUID and .eml - so anything else in
      // the column (a row somebody edited by hand) is not opened.
      // A row-limited SELECT for each backend: TOP (n) on SQL Server and
      // Compact, LIMIT n on MySQL and PostgreSQL - DomainReports' shape.
      String SelectWithLimit(const String &columns, const String &from, const String &where, const String &orderBy, int rows)
      {
         String statement;

         switch (IniFileSettings::Instance()->GetDatabaseType())
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            statement.Format(_T("select top (%d) %s from %s where %s order by %s"), rows, columns.c_str(), from.c_str(), where.c_str(), orderBy.c_str());
            break;
         default:
            statement.Format(_T("select %s from %s where %s order by %s"), columns.c_str(), from.c_str(), where.c_str(), orderBy.c_str());
            statement += Formatter::Format(_T(" limit {0}"), rows);
            break;
         }

         return statement;
      }

      // The largest id the list's archive holds, or 0.
      __int64 NewestId(__int64 listId)
      {
         SQLCommand command("select max(laid) as highest from hm_listarchive where lalistid = @LISTID");
         command.AddParameter("@LISTID", listId);

         // A max() over no rows is one row holding NULL.
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF() || recordset->GetIsNull("highest"))
            return 0;

         return recordset->GetInt64Value("highest");
      }

      bool IsOwnFileName(const String &file)
      {
         if (file.IsEmpty() || file.GetLength() > 64)
            return false;

         for (int i = 0; i < file.GetLength(); i++)
         {
            const wchar_t c = file.GetAt(i);
            const bool allowed = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'.';
            if (!allowed)
               return false;
         }

         return file.Find(_T("..")) < 0;
      }
   }

   String
   ListArchive::Directory(__int64 listId)
   {
      const String lists = FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Lists"));
      return FileUtilities::Combine(lists, StringParser::IntToString(listId));
   }

   String
   ListArchive::PathOf(const ListArchiveEntry &entry)
   {
      if (!IsOwnFileName(entry.file))
         return String();

      return FileUtilities::Combine(Directory(entry.list_id), entry.file);
   }

   String
   ListArchive::MonthOf(__int64 unixTime)
   {
      __int64 y = 0;
      unsigned m = 0, d = 0;

      const __int64 days = unixTime >= 0 ? unixTime / 86400 : (unixTime - 86399) / 86400;
      CivilFromDays(days, y, m, d);

      String month;
      month.Format(_T("%04d-%02d"), (int) y, (int) m);
      return month;
   }

   __int64
   ListArchive::MonthStart(const String &month)
   {
      if (month.GetLength() != 7 || month.GetAt(4) != L'-')
         return -1;

      for (int i = 0; i < 7; i++)
      {
         if (i != 4 && (month.GetAt(i) < L'0' || month.GetAt(i) > L'9'))
            return -1;
      }

      const int year = _ttoi(month.Mid(0, 4));
      const int monthNumber = _ttoi(month.Mid(5, 2));

      if (year < 1970 || year > 9999 || monthNumber < 1 || monthNumber > 12)
         return -1;

      return DaysFromCivil(year, (unsigned) monthNumber, 1) * 86400;
   }

   bool
   ListArchive::Store(std::shared_ptr<const DistributionList> list, const String &copyFile, std::shared_ptr<MessageData> data)
   {
      if (!list || !data)
         return false;

      const String directory = Directory(list->GetID());
      if (!FileUtilities::Exists(directory) && !FileUtilities::CreateDirectory(directory))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7252, "ListArchive::Store",
            Formatter::Format("The archive folder {0} of list {1} could not be made, so a posting was distributed without being archived.", directory, list->GetAddress()));
         return false;
      }

      String name = GUIDCreator::GetGUID();
      name.Replace(_T("{"), _T(""));
      name.Replace(_T("}"), _T(""));
      name += _T(".eml");

      const String path = FileUtilities::Combine(directory, name);

      if (!FileUtilities::Copy(copyFile, path, true))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7252, "ListArchive::Store",
            Formatter::Format("A posting to list {0} could not be copied into its archive, so it was distributed without being archived.", list->GetAddress()));
         return false;
      }

      SQLStatement statement;
      statement.SetTable("hm_listarchive");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("laid");
      statement.AddColumnInt64("lalistid", list->GetID());
      statement.AddColumn("lamessageid", OneLine(data->GetFieldValue(_T("Message-ID")), 255));
      statement.AddColumn("lasubject", OneLine(data->GetSubject(), 255));
      statement.AddColumn("lasender", OneLine(data->GetFrom(), 255));
      const __int64 sent = (__int64) time(nullptr);
      statement.AddColumnInt64("lasent", sent);
      statement.AddColumn("lasize", (long) FileUtilities::FileSize(path));
      statement.AddColumn("lafile", name);

      // The month, so that the archive's pages ask for a month's rows and its
      // list of months without reading every row; and whether the archive kept
      // it - a copy stored only for the digest, while the archive is off, is
      // never shown and goes once the digest has carried it.
      statement.AddColumn("lamonth", MonthOf(sent));
      statement.AddColumn("laarchived", (long) (list->GetArchive() != DistributionList::ArchiveNone ? 1 : 0));

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         FileUtilities::DeleteFile(path);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7252, "ListArchive::Store",
            Formatter::Format("A posting to list {0} could not be recorded in its archive, so it was distributed without being archived.", list->GetAddress()));
         return false;
      }

      return true;
   }

   std::vector<String>
   ListArchive::Months(__int64 listId)
   {
      std::vector<String> months;

      // One row a month, from the column that holds it: never every row of a
      // large archive for each view of its front page.
      SQLCommand command("select distinct lamonth from hm_listarchive where lalistid = @LISTID and laarchived = 1 order by lamonth desc");
      command.AddParameter("@LISTID", listId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return months;

      while (!recordset->IsEOF())
      {
         const String month = recordset->GetStringValue("lamonth");
         if (!month.IsEmpty())
            months.push_back(month);

         // Twenty years of months is more than any page will list.
         if (months.size() >= 240)
            break;

         recordset->MoveNext();
      }

      return months;
   }

   std::vector<ListArchiveEntry>
   ListArchive::List(__int64 listId, const String &month, int limit)
   {
      std::vector<ListArchiveEntry> entries;

      if (limit <= 0)
         return entries;

      if (month.IsEmpty())
      {
         // The newest, read newest first and handed back oldest first; the
         // database is asked for as many as are wanted and no more.
         SQLCommand command(SelectWithLimit(_T("*"), _T("hm_listarchive"), _T("lalistid = @LISTID and laarchived = 1"), _T("laid desc"), limit));
         command.AddParameter("@LISTID", listId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return entries;

         while (!recordset->IsEOF() && (int) entries.size() < limit)
         {
            ListArchiveEntry entry;
            ReadEntry(recordset, entry);
            entries.push_back(entry);
            recordset->MoveNext();
         }

         std::vector<ListArchiveEntry> oldestFirst(entries.rbegin(), entries.rend());
         return oldestFirst;
      }

      if (MonthStart(month) < 0)
         return entries;

      SQLCommand command(SelectWithLimit(_T("*"), _T("hm_listarchive"), _T("lalistid = @LISTID and lamonth = @MONTH and laarchived = 1"), _T("laid asc"), limit));
      command.AddParameter("@LISTID", listId);
      command.AddParameter("@MONTH", month);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return entries;

      while (!recordset->IsEOF() && (int) entries.size() < limit)
      {
         ListArchiveEntry entry;
         ReadEntry(recordset, entry);
         entries.push_back(entry);
         recordset->MoveNext();
      }

      return entries;
   }

   bool
   ListArchive::Get(__int64 listId, __int64 id, ListArchiveEntry &entry)
   {
      SQLCommand command("select * from hm_listarchive where lalistid = @LISTID and laid = @ID and laarchived = 1");
      command.AddParameter("@LISTID", listId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      return ReadEntry(recordset, entry);
   }

   bool
   ListArchive::DeleteEntries_(const String &where, __int64 listId, __int64 bound)
   {
      // The files first, then the rows: a row whose file is gone is a page that
      // says so; a file whose row is gone is a file nothing will ever remove.
      // `where` is one of this file's own fragments, never anybody's input, and
      // names @BOUND at most once after @LISTID.
      const bool bounded = where.Find(_T("@BOUND")) >= 0;

      SQLCommand select(_T("select laid, lalistid, lafile from hm_listarchive where lalistid = @LISTID") + where);
      select.AddParameter("@LISTID", listId);
      if (bounded)
         select.AddParameter("@BOUND", bound);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(select);
      if (!recordset)
         return false;

      int files = 0;
      while (!recordset->IsEOF())
      {
         ListArchiveEntry entry;
         entry.id = recordset->GetInt64Value("laid");
         entry.list_id = recordset->GetInt64Value("lalistid");
         entry.file = recordset->GetStringValue("lafile");

         const String path = PathOf(entry);
         if (!path.IsEmpty() && FileUtilities::Exists(path))
         {
            FileUtilities::DeleteFile(path);
            files++;
         }

         recordset->MoveNext();
      }

      SQLCommand remove(_T("delete from hm_listarchive where lalistid = @LISTID") + where);
      remove.AddParameter("@LISTID", listId);
      if (bounded)
         remove.AddParameter("@BOUND", bound);

      return Application::Instance()->GetDBManager()->Execute(remove);
   }

   void
   ListArchive::DeleteAll(__int64 listId)
   {
      if (listId <= 0)
         return;

      DeleteEntries_(String(), listId, 0);

      const String directory = Directory(listId);
      if (FileUtilities::Exists(directory) && !FileUtilities::DeleteDirectory(directory, true))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7257, "ListArchive::DeleteAll",
            Formatter::Format("The archive folder {0} of a deleted list could not be removed.", directory));
      }
   }

   int
   ListArchive::Sweep()
   {
      int removed = 0;

      // Nothing to do on a server with no managed list, which is every server
      // until somebody makes one: the pass reads one cached answer and stops.
      if (!ListManager::AnyManagedList())
         return removed;

      const __int64 now = (__int64) time(nullptr);

      // Requests nobody confirmed, and requests the owner never approved, lapse
      // after a week - what the confirmation and the approval mail say. A row
      // saying an address has left is not a request and never lapses.
      {
         SQLCommand lapsed("delete from hm_distributionlistsrecipients where (distributionlistrecipientconfirmed = 0 or distributionlistrecipientconfirmed = 2) and distributionlistrecipientsubscribed < @CUTOFF");
         lapsed.AddParameter("@CUTOFF", (__int64) (now - (__int64) ListManager::RequestLifetimeDays * 86400));

         if (Application::Instance()->GetDBManager()->Execute(lapsed))
            removed++;
      }

      std::vector<__int64> listIds;
      {
         SQLCommand managed("select distributionlistid from hm_distributionlists where distributionlistmanaged = 1");
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(managed);
         if (!recordset)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7259, "ListArchive::Sweep",
               "The managed lists could not be read, so no list's archive was trimmed on this pass.");
            return removed;
         }

         while (!recordset->IsEOF())
         {
            listIds.push_back(recordset->GetInt64Value("distributionlistid"));
            recordset->MoveNext();
         }
      }

      for (__int64 listId : listIds)
      {
         std::shared_ptr<DistributionList> list = ListManager::ListById(listId);
         if (!list)
            continue;

         const bool digestOn = list->GetDigest();
         bool done = true;

         // The copies kept only for the digest: gone once the digest has
         // carried them, and all of them when the digest is off - a digest
         // switched on again starts from the newest message, not from these.
         if (digestOn)
            done = DeleteEntries_(_T(" and laarchived = 0 and laid <= @BOUND"), listId, list->GetDigestLastId());
         else
            done = DeleteEntries_(_T(" and laarchived = 0"), listId, 0);

         // The archive's own: kept for archive_days, whether or not the archive
         // is on - an administrator who switches it off takes the page away and
         // keeps what it held until its days run out, and gets it back by
         // switching it on again. 0 keeps everything. Never one the digest has
         // not sent yet.
         if (done && list->GetArchiveDays() > 0)
         {
            const __int64 cutoff = now - (__int64) list->GetArchiveDays() * 86400;

            if (digestOn)
            {
               // Two bounds would need two parameters; the older of the two
               // limits is taken by the id, which grows with the time.
               SQLCommand oldest("select laid from hm_listarchive where lalistid = @LISTID and laarchived = 1 and lasent < @CUTOFF order by laid desc");
               oldest.AddParameter("@LISTID", listId);
               oldest.AddParameter("@CUTOFF", cutoff);

               std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(oldest);
               if (recordset && !recordset->IsEOF())
               {
                  __int64 bound = recordset->GetInt64Value("laid");
                  if (bound > list->GetDigestLastId())
                     bound = list->GetDigestLastId();

                  done = DeleteEntries_(_T(" and laarchived = 1 and laid <= @BOUND"), listId, bound);
               }
            }
            else
               done = DeleteEntries_(_T(" and laarchived = 1 and lasent < @BOUND"), listId, cutoff);
         }

         if (!done)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7259, "ListArchive::Sweep",
               Formatter::Format("The archive of list {0} could not be trimmed to its retention on this pass.", list->GetAddress()));
         }
         else
            removed++;
      }

      return removed;
   }

   int
   ListArchive::SendDigest(std::shared_ptr<const DistributionList> list, bool force)
   {
      if (!list || !list->GetManaged() || !list->GetDigest() || !list->GetActive())
         return 0;

      // A pass that is not forced has already been told the hour is right; the
      // once-a-day rule is asked here, so that no path sends two in a day.
      const __int64 now = (__int64) time(nullptr);
      if (!force && list->GetDigestLastTime() > 0 && now - list->GetDigestLastTime() < DigestSpacingSeconds)
         return 0;

      std::vector<ListArchiveEntry> entries;
      __int64 highest = list->GetDigestLastId();
      {
         SQLCommand command("select * from hm_listarchive where lalistid = @LISTID and laid > @LASTID order by laid asc");
         command.AddParameter("@LISTID", list->GetID());
         command.AddParameter("@LASTID", list->GetDigestLastId());

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7256, "ListArchive::SendDigest",
               Formatter::Format("The archive of list {0} could not be read, so no digest was sent on this pass.", list->GetAddress()));
            return 0;
         }

         __int64 bytes = 0;
         while (!recordset->IsEOF() && entries.size() < MostDigestMessages)
         {
            ListArchiveEntry entry;
            ReadEntry(recordset, entry);

            // What does not fit waits for the next digest - unless it is the
            // first, which goes whatever its size, or no digest would ever move on.
            if (!entries.empty() && bytes + entry.size > MostDigestBytes)
               break;

            bytes += entry.size;
            entries.push_back(entry);
            if (entry.id > highest)
               highest = entry.id;

            recordset->MoveNext();
         }
      }

      // Nothing new, nothing sent: a digest that says nothing happened is how
      // people learn to filter the digest.
      if (entries.empty())
         return 0;

      struct Recipient
      {
         String address;
         std::shared_ptr<DistributionListRecipient> row;
      };

      std::vector<Recipient> recipients;
      {
         std::shared_ptr<DistributionListRecipients> rows = list->GetMembers();
         const std::vector<std::shared_ptr<DistributionListRecipient> > snapshot = rows ? rows->GetSnapshot() : std::vector<std::shared_ptr<DistributionListRecipient> >();

         for (const std::shared_ptr<DistributionListRecipient> &row : snapshot)
         {
            if (!row || !row->GetIsMember() || row->GetDelivery() != DistributionListRecipient::DeliveryDigest)
               continue;

            if (ListManager::IsListOwnAddress(row->GetAddress(), list->GetAddress()))
               continue;

            Recipient recipient;
            recipient.address = row->GetAddress();
            recipient.row = row;
            recipients.push_back(recipient);
         }
      }

      // Nobody takes the digest: the position moves on to the newest message
      // the archive holds - not merely past this batch of a hundred - so that
      // a member who chooses it tomorrow starts from tomorrow.
      if (recipients.empty())
      {
         const __int64 newest = NewestId(list->GetID());
         PersistentDistributionList::SaveDigestState(list->GetID(), newest > highest ? newest : highest, now);
         return 0;
      }

      String outerBoundary = GUIDCreator::GetGUID();
      outerBoundary.Replace(_T("{"), _T(""));
      outerBoundary.Replace(_T("}"), _T(""));
      const AnsiString outer = "=_digest_" + AnsiString(outerBoundary);
      const AnsiString inner = "=_parts_" + AnsiString(outerBoundary);

      const String listAddress = list->GetAddress();
      const AnsiString list7 = AnsiString(listAddress);

      AnsiString subject;
      subject.Format("Digest of %s, %d message%s", list7.c_str(), (int) entries.size(), entries.size() == 1 ? "" : "s");

      AnsiString head;
      head += "From: <" + AnsiString(ListManager::CommandAddress(listAddress, ListManager::CommandRequest)) + ">\r\n";
      head += "To: <" + list7 + ">\r\n";
      head += "Subject: " + subject + "\r\n";
      head += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
      head += "Message-ID: " + AnsiString(Utilities::GenerateMessageID()) + "\r\n";
      head += "MIME-Version: 1.0\r\n";
      head += "Content-Type: multipart/mixed; boundary=\"" + outer + "\"\r\n";
      head += "\r\n";
      head += "This is a digest in MIME format.\r\n";
      head += "\r\n";
      head += "--" + outer + "\r\n";
      head += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      head += "Content-Transfer-Encoding: 8bit\r\n";
      head += "\r\n";
      head += subject + "\r\n";
      head += "\r\n";

      for (size_t i = 0; i < entries.size(); i++)
      {
         AnsiString line;
         line.Format("%3d. ", (int) i + 1);
         head += line + Utf8(OneLine(entries[i].subject.IsEmpty() ? String(_T("(no subject)")) : entries[i].subject, 200));
         head += "\r\n     " + Utf8(OneLine(entries[i].sender, 200)) + "\r\n";
      }

      head += "\r\n";
      head += "--" + outer + "\r\n";
      head += "Content-Type: multipart/digest; boundary=\"" + inner + "\"\r\n";
      head += "\r\n";

      const String digestFile = FileUtilities::Combine(Directory(list->GetID()), outerBoundary + _T(".digest"));

      bool written = false;
      try
      {
         const String directory = Directory(list->GetID());
         if (!FileUtilities::Exists(directory))
            FileUtilities::CreateDirectory(directory);

         File out;
         written = out.Open(digestFile, File::OTCreate) && out.Write(head);

         for (const ListArchiveEntry &entry : entries)
         {
            if (!written)
               break;

            const String path = PathOf(entry);
            File part;
            if (path.IsEmpty() || !part.Open(path, File::OTReadOnly))
            {
               // A copy the archive lost is listed in the contents and left out
               // of the parts, rather than holding up everybody's digest.
               continue;
            }

            // The default type of a part of a multipart/digest is
            // message/rfc822, so a part is a blank line and the message.
            written = out.Write(AnsiString("--" + inner + "\r\n\r\n")) && out.Write(part) && out.Write(AnsiString("\r\n"));
            part.Close();
         }

         if (written)
            written = out.Write(AnsiString("--" + inner + "--\r\n\r\n--" + outer + "--\r\n")) && out.FlushToDisk();

         out.Close();
      }
      catch (...)
      {
         written = false;
      }

      if (!written)
      {
         FileUtilities::DeleteFile(digestFile);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7256, "ListArchive::SendDigest",
            Formatter::Format("The digest of list {0} could not be written, so none was sent on this pass.", listAddress));
         return 0;
      }

      int sent = 0;
      for (const Recipient &recipient : recipients)
      {
         ListManager::EnsureToken(recipient.row);

         if (ListManager::QueueCopy(list, recipient.address, recipient.row->GetToken(), digestFile, false))
            sent++;
      }

      FileUtilities::DeleteFile(digestFile);

      // Recorded after the digests are in the queue: the other order would
      // mark a digest sent that was never written.
      PersistentDistributionList::SaveDigestState(list->GetID(), highest, now);

      if (sent > 0)
         Application::Instance()->SubmitPendingEmail();

      LOG_APPLICATION(Formatter::Format("ListManager: the digest of {0} ({1} message(s), up to archive entry {2}) was sent to {3} member(s).",
         listAddress, (int) entries.size(), highest, sent));

      return sent;
   }

   int
   ListArchive::RunDigests()
   {
      // No managed list, no digest: the one cached question and nothing more.
      if (!ListManager::AnyManagedList())
         return 0;

      std::time_t now = std::time(nullptr);
      struct tm local;

#ifdef HM_PLATFORM_POSIX
      if (::localtime_r(&now, &local) == nullptr)
         return 0;
#else
      if (localtime_s(&local, &now) != 0)
         return 0;
#endif

      std::vector<__int64> listIds;
      {
         SQLCommand command("select distributionlistid, distributionlistdigesthour from hm_distributionlists where distributionlistmanaged = 1 and distributionlistdigest = 1");
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return 0;

         while (!recordset->IsEOF())
         {
            if (recordset->GetLongValue("distributionlistdigesthour") == local.tm_hour)
               listIds.push_back(recordset->GetInt64Value("distributionlistid"));

            recordset->MoveNext();
         }
      }

      int sent = 0;
      for (__int64 listId : listIds)
      {
         std::shared_ptr<DistributionList> list = ListManager::ListById(listId);
         if (list)
            sent += SendDigest(list, false);
      }

      return sent;
   }

   void
   ListArchiveTester::Test()
   {
      if (ListArchive::MonthOf(0) != _T("1970-01"))
         throw std::logic_error("The Unix epoch is not in January 1970.");

      // 2026-09-24 12:00:00 UTC.
      if (ListArchive::MonthOf(1790251200) != _T("2026-09"))
         throw std::logic_error("A day in September 2026 was put in another month.");

      if (ListArchive::MonthStart(_T("2026-09")) != 1788220800)
         throw std::logic_error("September 2026 does not start at 1788220800.");

      if (ListArchive::MonthOf(ListArchive::MonthStart(_T("2024-02"))) != _T("2024-02") ||
          ListArchive::MonthOf(ListArchive::MonthStart(_T("2024-03")) - 1) != _T("2024-02"))
         throw std::logic_error("The boundaries of a leap February are wrong.");

      if (ListArchive::MonthStart(_T("2026-13")) != -1 || ListArchive::MonthStart(_T("26-09")) != -1 || ListArchive::MonthStart(_T("2026/09")) != -1)
         throw std::logic_error("Something that is not a month was read as one.");
   }
}
