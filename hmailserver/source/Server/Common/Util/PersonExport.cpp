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
#include "XmlTree.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../BO/Message.h"
#include "../BO/Messages.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
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

      // A message's original bytes, whatever its stored file holds
      // (Persistence/MessageStore.h).
      bool ReadBytes(const String &fileName, AnsiString &bytes)
      {
         return MessageStore::ReadAll(fileName, bytes);
      }

      // A folder's mbox is written in parts of at most this many bytes, as the
      // eDiscovery export writes its own: ZipWriter takes an entry whole, with
      // its deflated copy beside it while it is added, so a folder of two
      // gigabytes held as one mbox would ask the mail server's process for
      // three times that at once.
      const size_t MaxMboxBytes = 256 * 1024 * 1024;

      // One part of a folder's mbox into the archive: mail/<path>.mbox, and
      // mail/<path>-part2.mbox and on. An entry name already written - a
      // folder that happens to be called "INBOX-part2" beside a large INBOX -
      // takes " (2)", " (3)" before its extension, so no entry is written
      // twice under one name.
      bool AddMbox(ZipWriter &zip, std::set<AnsiString> &entries, const String &folderPath, int part,
                   const AnsiString &mbox, String &error)
      {
         String base = _T("mail/") + folderPath;
         if (part > 1)
         {
            String suffix;
            suffix.Format(_T("-part%d"), part);
            base += suffix;
         }

         AnsiString name = Utf8(base + _T(".mbox"));
         AnsiString lowered = name;
         std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);

         for (int number = 2; entries.find(lowered) != entries.end(); number++)
         {
            String candidate;
            candidate.Format(_T("%s (%d).mbox"), base.c_str(), number);
            name = Utf8(candidate);
            lowered = name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
         }

         entries.insert(lowered);

         if (!zip.Add(name, mbox))
         {
            error = zip.Error();
            return false;
         }

         return true;
      }

      bool ExportFolders(std::shared_ptr<Account> account, std::shared_ptr<IMAPFolders> folders, const String &prefix,
                         ZipWriter &zip, std::set<AnsiString> &entries, PersonExport::Summary &summary, String &error)
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
               int part = 1;

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

                  // The part so far goes into the archive before this message
                  // would take it past the ceiling; a single message larger
                  // than the ceiling is a part of its own.
                  if (!mbox.empty() && mbox.size() + bytes.size() > MaxMboxBytes)
                  {
                     if (!AddMbox(zip, entries, folderPath, part, mbox, error))
                        return false;

                     mbox = AnsiString();
                     part++;
                  }

                  PersonExport::AppendMbox(mbox, bytes, from, Utf8(message->GetCreateTime()));
                  count++;
               }

               if (count > 0)
               {
                  if (!AddMbox(zip, entries, folderPath, part, mbox, error))
                     return false;

                  summary.messages += count;
                  summary.folders++;
               }
            }

            if (!ExportFolders(account, folder->GetSubFolders(), folderPath, zip, entries, summary, error))
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
            else if (ContactStore::IsGroupAddress(contact.address))
            {
               // A group (ContactGroups.h) has no address of its own: its row
               // holds the marker there, which must not be written out as one.
               // Every group has a card, so this is the row whose card was
               // lost - served as an empty group here as it is over CardDAV.
               card = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:" + Utf8(contact.name) + "\r\nX-ADDRESSBOOKSERVER-KIND:group\r\nEND:VCARD";
            }
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

      void RemoveAttribute(XNode *node, const String &name)
      {
         // A value an AccountStores row could not hold as an attribute is
         // written under the column's name with ".base64" after it.
         const String names[2] = { name, name + _T(".base64") };
         for (const String &candidate : names)
         {
            LPXAttr attribute = node->GetAttr(candidate.c_str());
            if (attribute)
               node->RemoveAttr(attribute);
         }
      }

      // What account.xml leaves out of the backup's form: the secrets that
      // prove who someone is or unlock something elsewhere, which a backup
      // must carry to be restored from and a person's copy of their data has
      // no use for. The password's hash and its scheme, the authenticator's
      // seed, each app password's and recovery code's hash, the hashes of the
      // password history and of a file sent behind a password, the fetch
      // accounts' passwords and OAuth tokens (sealed with a key built into
      // every copy of the server, so anyone can open them), a browser's push
      // secret, and the mailbox key of an account encrypted at rest, wrapped
      // by the password: the mail itself is in the archive in the clear, so
      // the key opens nothing the person needs and is only something to
      // crack. The account's own OpenPGP and S/MIME keys stay: they are
      // wrapped under secrets this server never had, and they are what reads
      // the mail that was encrypted to them - the person's to take along.
      void LeaveOutCredentials(XNode *node)
      {
         if (!node)
            return;

         if (node->name == _T("Account"))
         {
            RemoveAttribute(node, _T("Password"));
            RemoveAttribute(node, _T("PasswordEncryption"));
            RemoveAttribute(node, _T("TotpSecret"));
         }
         else if (node->name == _T("FetchAccount"))
         {
            RemoveAttribute(node, _T("Password"));
            RemoveAttribute(node, _T("OAuthToken"));
         }
         else if (node->name == _T("AppPassword"))
         {
            RemoveAttribute(node, _T("Hash"));
         }
         else if (node->name == _T("Row") && node->parent && node->parent->name == _T("Store"))
         {
            static const struct { const TCHAR *table; const TCHAR *column; } secrets[] =
            {
               { _T("hm_passwordhistory"), _T("phhash") },
               { _T("hm_recoverycodes"), _T("rkhash") },
               { _T("hm_files"), _T("filepasswordhash") },
               { _T("hm_pushsubscriptions"), _T("pskeyauth") },
               { _T("hm_accountkeys"), _T("akpassword") },
               { _T("hm_accountkeys"), _T("akrecovery") },
               { _T("hm_accountkeys"), _T("akpending") },
            };

            const String table = node->parent->GetAttrValue(_T("Name"));
            for (const auto &secret : secrets)
            {
               if (table.CompareNoCase(secret.table) == 0)
                  RemoveAttribute(node, secret.column);
            }
         }

         for (int i = 0; i < node->GetChildCount(); i++)
            LeaveOutCredentials(node->GetChild((unsigned int) i));
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
            // The address as a whole address: ann@example.com's export is not
            // handed the lines of joann@example.com or ann@example.com.au.
            std::string lowered = line;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            if (StringParser::FindWholeAddress(lowered, needle) == std::string::npos)
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

      std::set<AnsiString> mboxEntries;
      if (!ExportFolders(account, rootFolders, String(), zip, mboxEntries, summary, error))
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

      // Less the credentials: this is the person's data, not an archive to
      // restore from (LeaveOutCredentials says which, and why).
      LeaveOutCredentials(root);

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
