// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See DirectoryStore.h. Everything here is one domain's names, read through
// the same SQLCommand path as every other table.
//
// The listing joins hm_accounts (and hm_distributionlists) with hm_directory
// in one query per kind, rather than reading a row per account, because the
// webmail's completion asks for this on a keystroke and a domain of a thousand
// mailboxes would otherwise be a thousand round trips. The filtering the
// completion asks for is done here in memory, as the address book's is and for
// the same reason: a case-insensitive substring match is not spelled the same
// on the four backends.
//
// The photo is kept base64 in a text column rather than as a blob. The three
// create scripts already carry PEM certificates, armoured keys and whole
// vCards that way, the four DALs agree about a text parameter and do not agree
// about a binary one, and the bytes are handed to a browser base64-decoded on
// this side either way.

#include "StdAfx.h"

#include <algorithm>
#include <openssl/sha.h>

#include "DirectoryStore.h"
#include "VCard.h"
#include "Unicode.h"
#include "Time.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/Constants.h"
#include "Encoding/Base64.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int DirectoryStore::MaximumTitleLength;
   const int DirectoryStore::MaximumDepartmentLength;
   const int DirectoryStore::MaximumPhoneLength;
   const int DirectoryStore::MaximumOfficeLength;
   const int DirectoryStore::MaximumNoteLength;
   const int DirectoryStore::MaximumPhotoBytes;

   namespace
   {
      const char *DirectoryColumns =
         "dirid, diraccountid, dirtitle, dirdepartment, dirphone, dirmobile, diroffice, "
         "dirphototype, dirphoto, diroptout, dirnote, dirupdated";

      void ReadRecord(std::shared_ptr<DALRecordset> recordset, DirectoryRecord &record)
      {
         record.accountId = recordset->GetInt64Value("diraccountid");
         record.title = recordset->GetStringValue("dirtitle");
         record.department = recordset->GetStringValue("dirdepartment");
         record.phone = recordset->GetStringValue("dirphone");
         record.mobile = recordset->GetStringValue("dirmobile");
         record.office = recordset->GetStringValue("diroffice");
         record.photoType = recordset->GetStringValue("dirphototype");
         record.photo = recordset->GetStringValue("dirphoto");
         record.optOut = recordset->GetLongValue("diroptout") != 0;
         record.note = recordset->GetStringValue("dirnote");
         record.updated = recordset->GetStringValue("dirupdated");
         record.stored = true;
      }

      // The base64 of the column without the white space a store or a client
      // may have folded into it.
      AnsiString CleanBase64(const String &text)
      {
         AnsiString source;
         Unicode::WideToMultiByte(text, source);
         AnsiString clean;
         for (int i = 0; i < source.GetLength(); i++)
         {
            const char c = source.GetAt(i);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
               clean += c;
         }
         return clean;
      }

      AnsiString Utf8Of(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // The base64 of a column as the image it is: decoded, and its media type
      // read from the bytes rather than trusted from the column beside it. The
      // column was written by a request, and a browser told image/png about a
      // file that is not one is the sniffing problem the nosniff header exists
      // for.
      bool DecodeStoredPhoto(const String &stored, AnsiString &bytes, AnsiString &contentType)
      {
         bytes = "";
         contentType = "";

         if (stored.IsEmpty())
            return false;

         AnsiString clean = CleanBase64(stored);
         if (clean.IsEmpty())
            return false;

         bytes = Base64::Decode(clean.c_str(), (int) clean.GetLength());
         if (bytes.IsEmpty())
            return false;

         AnsiString sniffed;
         String problem;
         if (!DirectoryStore::ReadPhoto(bytes, sniffed, problem))
         {
            bytes = "";
            return false;
         }

         contentType = sniffed;
         return true;
      }

      AnsiString StableUidFromSeed(const AnsiString &seed)
      {
         unsigned char digest[SHA256_DIGEST_LENGTH] = {};
         SHA256(reinterpret_cast<const unsigned char *>(seed.c_str()), static_cast<size_t>(seed.GetLength()), digest);
         digest[6] = static_cast<unsigned char>((digest[6] & 0x0f) | 0x80);
         digest[8] = static_cast<unsigned char>((digest[8] & 0x3f) | 0x80);

         static const char *hex = "0123456789abcdef";
         AnsiString result;
         for (int i = 0; i < 16; i++)
         {
            if (i == 4 || i == 6 || i == 8 || i == 10)
               result += "-";
            result += hex[digest[i] >> 4];
            result += hex[digest[i] & 0x0f];
         }
         return result;
      }
   }

   bool
   DirectoryStore::Enabled()
   {
      return Configuration::Instance()->GetDirectoryEnabled();
   }

   bool
   DirectoryStore::UserEditable()
   {
      return Configuration::Instance()->GetDirectoryUserEditable();
   }

   String
   DirectoryStore::NameOf(const String &firstName, const String &lastName)
   {
      String first = firstName;
      String last = lastName;
      first.TrimLeft();
      first.TrimRight();
      last.TrimLeft();
      last.TrimRight();

      if (first.IsEmpty())
         return last;
      if (last.IsEmpty())
         return first;

      return first + _T(" ") + last;
   }

   bool
   DirectoryStore::Get(__int64 accountId, DirectoryRecord &record)
   {
      record = DirectoryRecord();
      record.accountId = accountId;

      SQLCommand command(AnsiString("select ") + DirectoryColumns + " from hm_directory where diraccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return true;

      ReadRecord(recordset, record);
      return true;
   }

   bool
   DirectoryStore::Save(const DirectoryRecord &record)
   {
      if (record.accountId <= 0)
         return false;

      DirectoryRecord current;
      if (!Get(record.accountId, current))
         return false;

      if (Write_(record, current.stored))
         return true;

      // Nothing holds a lock between the read above and the write, and there
      // are four REST worker threads and COM beside them: a person's first
      // save from the webmail and an administrator's first save of the same
      // account can both see no row and both insert one. The second insert is
      // refused by the unique index on diraccountid, which would answer 500 a
      // request that was perfectly valid - so an insert that fails is tried
      // once more as the update it has become. (It is the last writer that
      // wins either way; this is about not failing, not about merging.)
      if (current.stored)
         return false;

      DirectoryRecord again;
      if (!Get(record.accountId, again) || !again.stored)
         return false;

      return Write_(record, true);
   }

   bool
   DirectoryStore::Write_(const DirectoryRecord &record, bool update)
   {
      const String updated = Time::GetCurrentDateTime();

      SQLStatement statement;
      statement.SetTable("hm_directory");

      if (update)
      {
         AnsiString where;
         where.Format("diraccountid = %I64d", record.accountId);
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetWhereClause(String(where));
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("dirid");
         statement.AddColumnInt64("diraccountid", record.accountId);
      }

      statement.AddColumn("dirtitle", record.title);
      statement.AddColumn("dirdepartment", record.department);
      statement.AddColumn("dirphone", record.phone);
      statement.AddColumn("dirmobile", record.mobile);
      statement.AddColumn("diroffice", record.office);
      statement.AddColumn("dirphototype", record.photoType);
      statement.AddColumn("dirphoto", record.photo);
      statement.AddColumnInt64("diroptout", record.optOut ? 1 : 0);
      statement.AddColumn("dirnote", record.note);
      statement.AddColumnDate("dirupdated", Time::GetDateFromSystemDate(updated));

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   DirectoryStore::Photo(__int64 accountId, AnsiString &bytes, AnsiString &contentType)
   {
      bytes = "";
      contentType = "";

      DirectoryRecord record;
      if (!Get(accountId, record) || !record.stored)
         return false;

      return DecodeStoredPhoto(record.photo, bytes, contentType);
   }

   bool
   DirectoryStore::ListedPhoto(__int64 domainId, __int64 accountId, AnsiString &bytes, AnsiString &contentType)
   {
      bytes = "";
      contentType = "";

      if (domainId <= 0 || accountId <= 0)
         return false;

      // One query rather than a listing of the whole domain. The route that
      // answers this is asked once per card the page draws, so deciding
      // whether one id is listed by listing everybody made drawing a page of
      // N people cost N listings of N people - work any signed-in mailbox of
      // the domain could ask for by the thousand. The three conditions are
      // the listing's own: this domain, an account that can sign in, and one
      // that has not opted out.
      SQLCommand command(
         "select d.dirphoto from hm_directory d "
         "inner join hm_accounts a on a.accountid = d.diraccountid "
         "where d.diraccountid = @ACCOUNTID and a.accountdomainid = @DOMAINID and "
         "a.accountactive = 1 and d.diroptout = 0");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      return DecodeStoredPhoto(recordset->GetStringValue("dirphoto"), bytes, contentType);
   }

   bool
   DirectoryStore::ReadPhoto(const AnsiString &bytes, AnsiString &contentType, String &problem)
   {
      contentType = "";
      problem = _T("");

      if (bytes.IsEmpty())
      {
         problem = _T("the photo is empty");
         return false;
      }

      if (bytes.GetLength() > MaximumPhotoBytes)
      {
         problem.Format(_T("the photo is %d bytes; a directory photo is at most %d"), bytes.GetLength(), MaximumPhotoBytes);
         return false;
      }

      const unsigned char *data = reinterpret_cast<const unsigned char *>(bytes.c_str());
      const int length = bytes.GetLength();

      if (length > 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
      {
         contentType = "image/jpeg";
         return true;
      }

      if (length > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
          data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
      {
         contentType = "image/png";
         return true;
      }

      if (length > 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
          (data[4] == '7' || data[4] == '9') && data[5] == 'a')
      {
         contentType = "image/gif";
         return true;
      }

      problem = _T("a directory photo is a JPEG, a PNG or a GIF; these bytes are none of the three");
      return false;
   }

   bool
   DirectoryStore::List(__int64 domainId, std::vector<DirectoryEntry> &entries)
   {
      entries.clear();

      if (domainId <= 0)
         return true;

      // The accounts. An account that is not active is not in the directory -
      // it cannot sign in and mail to it is refused, so listing it would be
      // offering a colleague an address that bounces. An account that has
      // opted out is not in it either; the left join is what lets an account
      // with no row be listed at all.
      {
         SQLCommand command(
            "select a.accountid, a.accountaddress, a.accountpersonfirstname, a.accountpersonlastname, "
            "d.dirtitle, d.dirdepartment, d.dirphone, d.dirmobile, d.diroffice, d.dirphototype, "
            "d.diroptout, d.dirupdated "
            "from hm_accounts a left join hm_directory d on d.diraccountid = a.accountid "
            "where a.accountdomainid = @DOMAINID and a.accountactive = 1");
         command.AddParameter("@DOMAINID", domainId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            DirectoryEntry entry;
            entry.id = recordset->GetInt64Value("accountid");
            entry.address = recordset->GetStringValue("accountaddress");
            entry.address.ToLower();
            entry.name = NameOf(recordset->GetStringValue("accountpersonfirstname"),
                                recordset->GetStringValue("accountpersonlastname"));

            DirectoryRecord &record = entry.record;
            record.accountId = entry.id;
            record.title = recordset->GetStringValue("dirtitle");
            record.department = recordset->GetStringValue("dirdepartment");
            record.phone = recordset->GetStringValue("dirphone");
            record.mobile = recordset->GetStringValue("dirmobile");
            record.office = recordset->GetStringValue("diroffice");
            record.photoType = recordset->GetStringValue("dirphototype");
            record.optOut = recordset->GetLongValue("diroptout") != 0;
            record.updated = recordset->GetStringValue("dirupdated");
            record.stored = !record.updated.IsEmpty();

            entry.hasPhoto = !record.photoType.IsEmpty();

            recordset->MoveNext();

            if (record.optOut || entry.address.IsEmpty())
               continue;

            entries.push_back(entry);
         }
      }

      // The lists. A list has no person behind it and no row of its own: it is
      // a name and an address, and it is in the directory because "who do I
      // write to about invoices" is the question a directory answers.
      {
         SQLCommand command(
            "select distributionlistid, distributionlistaddress from hm_distributionlists "
            "where distributionlistdomainid = @DOMAINID and distributionlistenabled = 1");
         command.AddParameter("@DOMAINID", domainId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            DirectoryEntry entry;
            entry.list = true;
            entry.id = recordset->GetInt64Value("distributionlistid");
            entry.address = recordset->GetStringValue("distributionlistaddress");
            entry.address.ToLower();

            recordset->MoveNext();

            if (entry.address.IsEmpty())
               continue;

            entries.push_back(entry);
         }
      }

      // No photo is read here, whoever is asking. A listing is two queries
      // whatever the domain holds; reading each photo with it was one query
      // and up to 137 KB of base64 per person, on every poll of a ctag and
      // every card a phone fetched. What needs one photo reads that one
      // (Photo, ListedPhoto), and the CardDAV collection fills a card's PHOTO
      // when a client actually asks for that card's body.
      std::sort(entries.begin(), entries.end(), [] (const DirectoryEntry &left, const DirectoryEntry &right) -> bool
      {
         String leftKey = left.name.IsEmpty() ? left.address : left.name;
         String rightKey = right.name.IsEmpty() ? right.address : right.name;
         leftKey.ToLower();
         rightKey.ToLower();
         if (leftKey != rightKey)
            return leftKey < rightKey;
         return left.address < right.address;
      });

      return true;
   }

   AnsiString
   DirectoryStore::StableUid(const AnsiString &addressUtf8, bool list)
   {
      return StableUidFromSeed(AnsiString(list ? "hMailServer.directory.list:" : "hMailServer.directory.account:") + addressUtf8);
   }

   AnsiString
   DirectoryStore::GenerateCard(const AnsiString &uid, const DirectoryEntry &entry)
   {
      AnsiString address;
      AnsiString name;
      Unicode::WideToMultiByte(entry.address, address);
      Unicode::WideToMultiByte(entry.name, name);

      AnsiString family;
      AnsiString given;
      VCard::SplitName(name, family, given);

      const AnsiString formattedName = VCard::Escape(name.IsEmpty() ? address : name);

      AnsiString card;
      card += "BEGIN:VCARD\r\n";
      card += "VERSION:3.0\r\n";
      card += "PRODID:-//hMailServer//Directory//EN\r\n";
      card += VCard::Fold("UID:" + uid) + "\r\n";
      card += VCard::Fold("FN:" + formattedName) + "\r\n";

      if (entry.list)
      {
         // A list is not a person. It is written as an organisation card, which
         // is what a client shows without a given and a family name, and its
         // N is empty rather than a guess at somebody's surname.
         card += "N:;;;;\r\n";
         card += VCard::Fold("ORG:" + formattedName) + "\r\n";
         card += "X-ADDRESSBOOKSERVER-KIND:org\r\n";
      }
      else
      {
         card += VCard::Fold("N:" + VCard::Escape(family) + ";" + VCard::Escape(given) + ";;;") + "\r\n";

         const DirectoryRecord &record = entry.record;
         if (!record.title.IsEmpty())
            card += VCard::Fold("TITLE:" + VCard::Escape(Utf8Of(record.title))) + "\r\n";
         if (!record.department.IsEmpty())
            card += VCard::Fold("ORG:;" + VCard::Escape(Utf8Of(record.department))) + "\r\n";
         if (!record.phone.IsEmpty())
            card += VCard::Fold("TEL;TYPE=WORK,VOICE:" + VCard::Escape(Utf8Of(record.phone))) + "\r\n";
         if (!record.mobile.IsEmpty())
            card += VCard::Fold("TEL;TYPE=CELL:" + VCard::Escape(Utf8Of(record.mobile))) + "\r\n";
         if (!record.office.IsEmpty())
            card += VCard::Fold("ADR;TYPE=WORK:;;" + VCard::Escape(Utf8Of(record.office)) + ";;;;") + "\r\n";
      }

      card += VCard::Fold("EMAIL;TYPE=INTERNET,PREF:" + VCard::Escape(address)) + "\r\n";

      if (!entry.list && !entry.record.photo.IsEmpty() && !entry.record.photoType.IsEmpty())
      {
         AnsiString clean = CleanBase64(entry.record.photo);
         AnsiString kind = entry.record.photoType;
         if (kind.StartsWith("image/"))
            kind = kind.Mid(6);
         kind.ToUpper();
         if (!clean.IsEmpty())
            card += VCard::Fold("PHOTO;ENCODING=b;TYPE=" + kind + ":" + clean) + "\r\n";
      }

      card += "END:VCARD\r\n";
      return card;
   }
}
