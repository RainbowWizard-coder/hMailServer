// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PersonExport.h"
#include "ZipWriter.h"
#include "ContactStore.h"
#include "FileUtilities.h"
#include "Time.h"
#include "Unicode.h"
#include "XMLite.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../BO/Messages.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Sieve/SieveStorage.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString Utf8(const String &text)
      {
         AnsiString out;
         Unicode::WideToMultiByte(text, out);
         return out;
      }

      AnsiString JsonText(const String &text)
      {
         AnsiString raw = Utf8(text);
         AnsiString out;
         const char *bytes = raw.c_str();
         for (size_t i = 0; i < raw.size(); i++)
         {
            const unsigned char c = (unsigned char) bytes[i];
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  static const char digits[] = "0123456789abcdef";
                  out += "\\u00";
                  out += digits[c >> 4];
                  out += digits[c & 15];
               }
               else
                  out += (char) c;
            }
         }
         return out;
      }

      // A folder name is IMAP data, not a path, and inside the archive it is a
      // path: the characters an unpacker's file system refuses become '_', and
      // a name that is only dots - which would climb out of the archive on
      // extraction - becomes "_". Siblings that collide get " (2)", " (3)".
      String EntryName(const String &name)
      {
         String result = name;
         const TCHAR *forbidden = _T("\\/:*?\"<>|");
         for (const TCHAR *c = forbidden; *c != 0; ++c)
            result.Replace(*c, _T('_'));
         result.Trim();

         bool allDots = !result.IsEmpty();
         const TCHAR *characters = result.c_str();
         for (int i = 0; i < result.GetLength(); i++)
         {
            if (characters[i] != _T('.'))
            {
               allDots = false;
               break;
            }
         }

         if (result.IsEmpty() || allDots)
            result = _T("_");

         return result;
      }

      bool ReadBytes(const String &fileName, AnsiString &bytes)
      {
         boost::filesystem::path path(fileName.begin(), fileName.end());
         boost::filesystem::ifstream in(path, std::ios::in | std::ios::binary);
         if (!in)
            return false;

         std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
         bytes = content;
         return true;
      }

      bool ExportFolders(std::shared_ptr<Account> account, std::shared_ptr<IMAPFolders> folders, const String &prefix,
                         ZipWriter &zip, PersonExport::Summary &summary, String &error)
      {
         if (!folders)
            return true;

         // No Refresh here: IMAPFolders::Refresh loads the account's whole tree
         // into whichever collection it is called on (see MessageExport).
         std::set<String> usedNames;

         for (unsigned int i = 0; i < (unsigned int) folders->GetCount(); i++)
         {
            std::shared_ptr<IMAPFolder> folder = folders->GetItem(i);
            if (!folder)
               continue;

            String folderName = EntryName(folder->GetFolderName());
            String lowered = folderName;
            lowered.ToLower();

            if (usedNames.find(lowered) != usedNames.end())
            {
               for (int suffix = 2; ; suffix++)
               {
                  String candidate;
                  candidate.Format(_T("%s (%d)"), folderName.c_str(), suffix);
                  String candidateLower = candidate;
                  candidateLower.ToLower();
                  if (usedNames.find(candidateLower) == usedNames.end())
                  {
                     folderName = candidate;
                     lowered = candidateLower;
                     break;
                  }
               }
            }

            usedNames.insert(lowered);

            const String folderPath = prefix.IsEmpty() ? folderName : prefix + _T("/") + folderName;

            std::shared_ptr<Messages> messages = folder->GetMessages();
            if (messages)
            {
               messages->Refresh(false);

               AnsiString mbox;
               int count = 0;

               for (unsigned int j = 0; j < (unsigned int) messages->GetCount(); j++)
               {
                  std::shared_ptr<Message> message = messages->GetItem(j);
                  if (!message)
                     continue;

                  const String source = PersistentMessage::GetFileName(account, message);
                  AnsiString bytes;

                  if (!ReadBytes(source, bytes))
                  {
                     error.Format(_T("The message file %s could not be read. The export is incomplete and should be re-run."), source.c_str());
                     return false;
                  }

                  AnsiString from = Utf8(message->GetFromAddress());
                  if (from.IsEmpty())
                     from = "MAILER-DAEMON";

                  PersonExport::AppendMbox(mbox, bytes, from, Utf8(message->GetCreateTime()));
                  count++;
               }

               if (count > 0)
               {
                  if (!zip.Add(Utf8(_T("mail/") + folderPath + _T(".mbox")), mbox))
                  {
                     error = zip.Error();
                     return false;
                  }

                  summary.messages += count;
                  summary.folders++;
               }
            }

            if (!ExportFolders(account, folder->GetSubFolders(), folderPath, zip, summary, error))
               return false;
         }

         return true;
      }

      // The address book as vCard 3.0: the card a client sent where there is
      // one, a card made from the name and address otherwise.
      AnsiString ContactsAsVCard(__int64 accountId, int &count)
      {
         AnsiString out;
         std::vector<ContactRecord> contacts;
         count = 0;

         if (!ContactStore::List(accountId, contacts))
            return out;

         for (const ContactRecord &contact : contacts)
         {
            AnsiString card;

            if (!contact.vcard.IsEmpty())
               card = Utf8(contact.vcard);
            else
            {
               card = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:" + Utf8(contact.name) + "\r\nEMAIL:" + Utf8(contact.address) + "\r\nEND:VCARD";
            }

            while (card.size() > 0 && (card.c_str()[card.size() - 1] == '\r' || card.c_str()[card.size() - 1] == '\n'))
               card.erase(card.size() - 1);

            out += card + "\r\n";
            count++;
         }

         return out;
      }

      bool CalendarsIntoArchive(__int64 accountId, ZipWriter &zip, int &count, String &error)
      {
         count = 0;

         SQLCommand command("select hm_calendars.calendarname as rname, hm_calendars.calendardisplayname as rdisplay, "
                            "hm_calendarobjects.objecturi as ruri, hm_calendarobjects.objectdata as rdata "
                            "from hm_calendarobjects inner join hm_calendars on hm_calendarobjects.objectcalendarid = hm_calendars.calendarid "
                            "where hm_calendarobjects.objectaccountid = @ACCOUNTID and hm_calendarobjects.objectdeleted = 0");
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            error = _T("The calendars could not be read.");
            return false;
         }

         std::set<AnsiString> used;

         while (!recordset->IsEOF())
         {
            String calendar = recordset->GetStringValue("rdisplay");
            if (calendar.IsEmpty())
               calendar = recordset->GetStringValue("rname");
            String uri = recordset->GetStringValue("ruri");
            if (uri.IsEmpty())
               uri = _T("object.ics");

            AnsiString name = Utf8(_T("calendars/") + EntryName(calendar) + _T("/") + EntryName(uri));
            int suffix = 2;
            AnsiString candidate = name;
            while (used.find(candidate) != used.end())
            {
               AnsiString number;
               number.Format(" (%d)", suffix++);
               candidate = name + number;
            }
            used.insert(candidate);

            if (!zip.Add(candidate, Utf8(recordset->GetStringValue("rdata"))))
            {
               error = zip.Error();
               return false;
            }

            count++;
            recordset->MoveNext();
         }

         return true;
      }

      bool PreferencesAsJson(__int64 accountId, AnsiString &json)
      {
         SQLCommand command("select prefname, prefvalue from hm_accountprefs where prefaccountid = @ACCOUNTID order by prefname");
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         json = "{";
         bool first = true;

         while (!recordset->IsEOF())
         {
            json += (first ? "\n  \"" : ",\n  \"") + JsonText(recordset->GetStringValue("prefname")) + "\": \"" + JsonText(recordset->GetStringValue("prefvalue")) + "\"";
            first = false;
            recordset->MoveNext();
         }

         json += "\n}\n";
         return true;
      }
   }

   void
   PersonExport::AppendMbox(AnsiString &mbox, const AnsiString &message, const AnsiString &from, const AnsiString &date)
   {
      mbox += "From " + from + " " + date + "\r\n";

      // A body line that begins "From " is quoted; the first line is a header.
      size_t start = 0;
      while (start <= message.size())
      {
         size_t end = message.find('\n', start);
         AnsiString line = end == AnsiString::npos ? message.substr(start) : message.substr(start, end - start + 1);
         if (line.compare(0, 5, "From ") == 0)
            mbox += ">";
         mbox += line;
         if (end == AnsiString::npos)
            break;
         start = end + 1;
      }

      if (mbox.size() < 2 || mbox.substr(mbox.size() - 2) != "\r\n")
         mbox += "\r\n";
      mbox += "\r\n";
   }

   int
   PersonExport::LogLinesNaming(const String &address, size_t cap, AnsiString &lines)
   {
      lines = "";
      int count = 0;

      String logDirectory = IniFileSettings::Instance()->GetLogDirectory();
      if (logDirectory.IsEmpty())
         return 0;

      AnsiString needle = Utf8(address);
      std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
      if (needle.empty())
         return 0;

      boost::system::error_code errorCode;
      boost::filesystem::path directory(logDirectory.begin(), logDirectory.end());
      if (!boost::filesystem::is_directory(directory, errorCode))
         return 0;

      for (boost::filesystem::directory_iterator file(directory, errorCode);
           file != boost::filesystem::directory_iterator(); file.increment(errorCode))
      {
         boost::filesystem::path current = file->path();
         if (!boost::filesystem::is_regular_file(current, errorCode))
            continue;

         String fileName = String(current.filename().wstring().c_str());
         String lowerName = fileName;
         lowerName.ToLower();

         if (!((lowerName.StartsWith(_T("hmailserver_")) || lowerName.StartsWith(_T("error_hmailserver_"))) && lowerName.EndsWith(_T(".log"))))
            continue;

         boost::filesystem::ifstream in(current, std::ios::in | std::ios::binary);
         if (!in)
            continue;

         std::string line;
         while (std::getline(in, line))
         {
            std::string lowered = line;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            if (lowered.find(needle) == std::string::npos)
               continue;

            if (lines.size() + line.size() + fileName.GetLength() + 4 > cap)
               return count;

            AnsiString entry = Utf8(fileName);
            entry += ": ";
            entry += line.c_str();
            lines += entry;
            if (lines.size() == 0 || lines.c_str()[lines.size() - 1] != '\n')
               lines += "\r\n";
            count++;
         }
      }

      return count;
   }

   bool
   PersonExport::ExportArchive(std::shared_ptr<Account> account, const String &zipPath, Summary &summary, String &error)
   {
      summary = Summary();

      if (!account || account->GetID() == 0)
      {
         error = _T("No account.");
         return false;
      }

      ZipWriter zip;
      if (!zip.Open(zipPath))
      {
         error = zip.Error();
         return false;
      }

      // The mail, one mbox per folder with mail in it.
      std::shared_ptr<IMAPFolders> rootFolders = account->GetFolders();
      if (rootFolders)
         rootFolders->Refresh();

      if (!ExportFolders(account, rootFolders, String(), zip, summary, error))
         return false;

      // Everything the backup carries for the account, in the backup's own
      // form, so nothing the server holds is left out by being forgotten here.
      XDoc document;
      XNode *root = document.AppendChild(_T("hMailServerPersonExport"));
      root->AppendAttr(_T("Address"), account->GetAddress());
      root->AppendAttr(_T("Exported"), Time::GetCurrentDateTime());
      if (!account->XMLStore(root, 0))
      {
         error = _T("The account's settings could not be written out.");
         return false;
      }

      if (!zip.Add("account.xml", Utf8(document.GetXML())))
      {
         error = zip.Error();
         return false;
      }

      // And the human-readable forms of the parts a person can use directly.
      AnsiString contacts = ContactsAsVCard(account->GetID(), summary.contacts);
      if (!zip.Add("contacts.vcf", contacts))
      {
         error = zip.Error();
         return false;
      }

      if (!CalendarsIntoArchive(account->GetID(), zip, summary.calendarObjects, error))
         return false;

      AnsiString preferences;
      if (PreferencesAsJson(account->GetID(), preferences) && !zip.Add("preferences.json", preferences))
      {
         error = zip.Error();
         return false;
      }

      String sieve = SieveStorage::GetActiveScript(account->GetAddress());
      if (!sieve.IsEmpty() && !zip.Add("filters.sieve", Utf8(sieve)))
      {
         error = zip.Error();
         return false;
      }

      AnsiString logs;
      summary.logLines = LogLinesNaming(account->GetAddress(), 50 * 1024 * 1024, logs);
      if (!zip.Add("logs.txt", logs))
      {
         error = zip.Error();
         return false;
      }

      AnsiString manifest;
      manifest.Format("{\n  \"address\": \"%s\",\n  \"exported\": \"%s\",\n  \"messages\": %d,\n  \"folders\": %d,\n"
                      "  \"contacts\": %d,\n  \"calendar_objects\": %d,\n  \"log_lines\": %d,\n"
                      "  \"contents\": [\"mail/<folder>.mbox\", \"account.xml\", \"contacts.vcf\", \"calendars/<calendar>/<object>\", "
                      "\"preferences.json\", \"filters.sieve\", \"logs.txt\"]\n}\n",
                      JsonText(account->GetAddress()).c_str(), Utf8(Time::GetCurrentDateTime()).c_str(),
                      summary.messages, summary.folders, summary.contacts, summary.calendarObjects, summary.logLines);

      if (!zip.Add("manifest.json", manifest))
      {
         error = zip.Error();
         return false;
      }

      if (!zip.Close())
      {
         error = zip.Error();
         return false;
      }

      return true;
   }
}
