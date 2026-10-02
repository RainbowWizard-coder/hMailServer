// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OpenPgpRing.h"
#include "OpenPgpKey.h"
#include "OpenPgpKeyStore.h"
#include "HttpsClient.h"

#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../AntiSpam/DMARC/DMARC.h"

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <chrono>
#include <map>

#include <ctime>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *RingColumns = "pgrid, pgraccountid, pgrkind, pgraddress, pgrname, pgrfingerprint, pgrkeyid, "
                                "pgralgorithm, pgrpublic, pgrsecret, pgrcreated, pgrexpires, pgrrevoked, "
                                "pgrsource, pgrprefer, pgrfirstseen, pgrlastseen, "
                                "pgrverified, pgrverifiedat, pgrchangedfrom, pgrchangedat, pgraccepted";

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String Wide(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Escape(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      void ReadRow(std::shared_ptr<DALRecordset> recordset, OpenPgpRing::Row &row)
      {
         row.id = recordset->GetInt64Value("pgrid");
         row.kind = (int) recordset->GetLongValue("pgrkind");
         row.address = recordset->GetStringValue("pgraddress");
         row.name = recordset->GetStringValue("pgrname");
         row.fingerprint = AnsiString(recordset->GetStringValue("pgrfingerprint"));
         row.key_id = AnsiString(recordset->GetStringValue("pgrkeyid"));
         row.algorithm = recordset->GetStringValue("pgralgorithm");
         row.public_key = AnsiString(recordset->GetStringValue("pgrpublic"));
         row.secret_key = AnsiString(recordset->GetStringValue("pgrsecret"));
         row.created = recordset->GetInt64Value("pgrcreated");
         row.expires = recordset->GetInt64Value("pgrexpires");
         row.revoked = recordset->GetLongValue("pgrrevoked") != 0;
         row.source = recordset->GetStringValue("pgrsource");
         row.prefer = (int) recordset->GetLongValue("pgrprefer");
         row.first_seen = recordset->GetInt64Value("pgrfirstseen");
         row.last_seen = recordset->GetInt64Value("pgrlastseen");
         row.verified = recordset->GetLongValue("pgrverified") != 0;
         row.verified_at = recordset->GetInt64Value("pgrverifiedat");
         row.changed_from = AnsiString(recordset->GetStringValue("pgrchangedfrom"));
         row.changed_at = recordset->GetInt64Value("pgrchangedat");
         row.accepted = recordset->GetLongValue("pgraccepted") != 0;
      }

      // The key an address was holding, when a correspondent's row is about to
      // be filed under an address that already has one with another
      // fingerprint: a row of that address whose fingerprint is not this one,
      // preferring a SETTLED key - one that replaced nothing, or whose
      // replacement the person accepted - and, among those, the newest. A
      // pending key counts only when no settled one is left, so that removing
      // the old key does not make the next arrival look like the first: a
      // change is a change against whatever was held.
      //
      // BOTH KINDS of row count, and the account's OWN key above all. A
      // correspondent's key filed under an address the account holds its own
      // key for is a change of THAT key: the page encrypts every message to
      // the sender's own key so that the copy in Sent can be read, and a
      // stranger's key filed under the account's address in silence would have
      // every message the person sends sealed to the stranger as well.
      bool PreviousKeyFor(__int64 accountId, const String &address, const AnsiString &fingerprint, AnsiString &previous)
      {
         SQLCommand command(String(_T("select ")) + String(RingColumns) +
                            _T(" from hm_pgpring where pgraccountid = @ACCOUNTID and pgraddress = @ADDRESS order by pgrfirstseen desc, pgrid desc"));
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@ADDRESS", address);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         AnsiString pending;
         while (!recordset->IsEOF())
         {
            OpenPgpRing::Row row;
            ReadRow(recordset, row);
            recordset->MoveNext();
            if (row.fingerprint == fingerprint)
               continue;

            if (row.changed_from.IsEmpty() || row.accepted)
            {
               previous = row.fingerprint;
               return true;
            }
            if (pending.IsEmpty())
               pending = row.fingerprint;
         }

         if (pending.IsEmpty())
            return false;

         previous = pending;
         return true;
      }

      String LocalPartOf(const String &address)
      {
         int at = address.Find(_T("@"));
         return at > 0 ? address.Mid(0, at) : String();
      }

      String DomainOf(const String &address)
      {
         int at = address.Find(_T("@"));
         return at >= 0 ? address.Mid(at + 1) : String();
      }

      // What a directory or a keyserver answered, read as a key. Both forms are
      // accepted: a Web Key Directory serves the binary, a keyserver serves the
      // armour, and nothing is answered to the page that did not parse here.
      bool KeyFromAnswer(const AnsiString &body, AnsiString &base64, String &error)
      {
         if (body.IsEmpty())
         {
            error = _T("the answer was empty");
            return false;
         }

         const size_t maximum = (size_t) Configuration::Instance()->GetOpenPGPKeyMaximumBytes();
         OpenPgpKey::Parsed parsed;
         if (body.Find("-----BEGIN PGP") >= 0)
            parsed = OpenPgpKey::ParseText(body, maximum);
         else
            parsed = OpenPgpKey::Parse((const unsigned char *) body.c_str(), (size_t) body.GetLength(), maximum);

         if (!parsed.ok)
         {
            error = Wide(parsed.error);
            return false;
         }

         if (parsed.bytes.empty())
         {
            error = _T("the answer held no key material");
            return false;
         }

         base64 = Base64::Encode((const char *) &parsed.bytes[0], (int) parsed.bytes.size());
         return true;
      }

      // One GET, capped and deadlined. A status that is not 200 is not an error
      // worth a sentence of its own - a directory that has no key answers 404 by
      // design - so it is reported as a status and the caller moves on.
      //
      // publicOnly is the difference between the two legs, and it is the whole
      // of the difference. A Web Key Directory host comes from an address a
      // person typed, so it goes through FetchFromDirectory, which resolves the
      // name and refuses every address inside this network before a connection
      // is made, and asks HttpProxy, when one is set, for a tunnel to an
      // address that passed: otherwise anyone with a mailbox here could aim
      // this server at whatever only this server can reach. The keyserver is a URL an
      // ADMINISTRATOR set in hm_settings, so it goes through Request, which
      // verifies https against the machine's roots and accepts plain http only
      // to a loopback address - which is what lets a keyserver on this machine,
      // and the regression fixture's own listener, be reached at all.
      // timeout is what is LEFT of the lookup's one deadline, not a fresh
      // allowance per leg: three legs at the full setting each would hold a
      // REST worker three times longer than the listener gives a request.
      bool Fetch(const AnsiString &url, bool publicOnly, int timeout, AnsiString &body, int &status, String &error)
      {
         const size_t cap = (size_t) Configuration::Instance()->GetOpenPGPKeyMaximumBytes() + 4096;
         if (publicOnly)
            return OpenPgpRing::FetchFromDirectory(url, timeout, cap, body, status, error);

         HttpsClient::Response response;
         std::vector<AnsiString> headers;
         headers.push_back("Accept: application/octet-stream, application/pgp-keys, */*");
         if (!HttpsClient::Request("GET", url, headers, "", "", response, error, timeout, cap))
         {
            status = 0;
            return false;
         }

         status = response.status_code;
         body = response.body;
         return response.status_code == 200;
      }
   }

   bool
   OpenPgpRing::FetchFromDirectory(const AnsiString &url, int timeout, size_t maxBytes, AnsiString &body, int &status, String &error)
   {
      HttpsClient::Response response;
      std::vector<AnsiString> headers;
      headers.push_back("Accept: application/octet-stream, application/pgp-keys, */*");
      if (!HttpsClient::RequestPublicThroughProxy("GET", url, headers, "", "", response, error, timeout, maxBytes))
      {
         status = 0;
         return false;
      }

      status = response.status_code;
      body = response.body;
      return response.status_code == 200;
   }

   bool
   OpenPgpRing::IsFingerprint(const AnsiString &text)
   {
      if (text.GetLength() != 40 && text.GetLength() != 64)
         return false;

      for (int i = 0; i < text.GetLength(); i++)
      {
         const char c = text[i];
         if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
      }

      return true;
   }

   bool
   OpenPgpRing::IsSource(const String &text)
   {
      return text == _T("generated") || text == _T("imported") || text == _T("autocrypt") ||
             text == _T("wkd") || text == _T("keyserver");
   }

   bool
   OpenPgpRing::List(__int64 accountId, int kind, std::vector<Row> &rows)
   {
      rows.clear();

      SQLCommand command(String(_T("select ")) + String(RingColumns) + _T(" from hm_pgpring where pgraccountid = @ACCOUNTID and pgrkind = @KIND order by pgrid asc"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@KIND", (int) kind);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         // Said out loud, because the alternative is a webmail whose key page
         // is empty and an account that concludes its keys are gone.
         String text;
         text.Format(_T("The OpenPGP keys of account %I64d could not be read."), accountId);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7041, "OpenPgpRing::List", text);
         return false;
      }

      while (!recordset->IsEOF())
      {
         Row row;
         ReadRow(recordset, row);
         rows.push_back(row);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   OpenPgpRing::Find(__int64 accountId, int kind, const AnsiString &fingerprint, Row &row)
   {
      if (!IsFingerprint(fingerprint))
         return false;

      SQLCommand command(String(_T("select ")) + String(RingColumns) + _T(" from hm_pgpring where pgraccountid = @ACCOUNTID and pgrkind = @KIND and pgrfingerprint = @FINGERPRINT"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@KIND", (int) kind);
      command.AddParameter("@FINGERPRINT", String(fingerprint));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadRow(recordset, row);
      return true;
   }

   int
   OpenPgpRing::Count(__int64 accountId, int kind)
   {
      SQLCommand command("select count(*) as c from hm_pgpring where pgraccountid = @ACCOUNTID and pgrkind = @KIND");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@KIND", (int) kind);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return -1;

      return (int) recordset->GetLongValue("c");
   }

   bool
   OpenPgpRing::Save(__int64 accountId, Row &row, bool &replaced)
   {
      Row existing;
      replaced = Find(accountId, row.kind, row.fingerprint, existing);

      // The trust columns are the ring's to keep and never the request's: a
      // row that comes back keeps its mark and its change as they were, and a
      // new row has none. So is the ADDRESS, like first_seen: a key that comes
      // back with another address in the request stays filed where it was,
      // because a row that moved would carry its mark and its history to an
      // address they were never about, and would leave the first address
      // with nothing. A person who wants a key under another address removes
      // it and imports it again, and the ring records that as what it is.
      // Then, for a correspondent's key filed under an address for the first
      // time, the ring asks whether that address already holds a different
      // key - a correspondent's, or the account's own - and if it does this
      // one is a CHANGE of it: not to be encrypted to, and its signatures not
      // shown as good, until the person says so. The same key seen again is
      // not a change, whatever brought it back.
      if (replaced)
      {
         row.address = existing.address;
         row.verified = existing.verified;
         row.verified_at = existing.verified_at;
         row.changed_from = existing.changed_from;
         row.changed_at = existing.changed_at;
         row.accepted = existing.accepted;
      }
      else
      {
         row.verified = false;
         row.verified_at = 0;
         row.changed_from.Empty();
         row.changed_at = 0;
         row.accepted = false;
      }

      if (row.kind == KindCorrespondent && !replaced)
      {
         AnsiString previous;
         if (PreviousKeyFor(accountId, row.address, row.fingerprint, previous))
         {
            row.changed_from = previous;
            row.changed_at = Now();
            row.accepted = false;
         }
      }

      SQLStatement statement;
      statement.SetTable("hm_pgpring");
      if (replaced)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetWhereClause(String(_T("pgrid = ")) + String(Int64Text(existing.id)));
         // First seen is when it was first seen, whatever brought it back.
         row.first_seen = existing.first_seen;
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("pgrid");
         statement.AddColumnInt64("pgraccountid", accountId);
         statement.AddColumn("pgrkind", (long) row.kind);
         if (row.first_seen == 0)
            row.first_seen = Now();
         statement.AddColumnInt64("pgrfirstseen", row.first_seen);
         statement.AddColumn("pgrfingerprint", String(row.fingerprint));
      }

      row.last_seen = Now();
      statement.AddColumn("pgraddress", row.address);
      statement.AddColumn("pgrname", row.name);
      statement.AddColumn("pgrkeyid", String(row.key_id));
      statement.AddColumn("pgralgorithm", row.algorithm);
      statement.AddColumn("pgrpublic", String(row.public_key));
      statement.AddColumn("pgrsecret", String(row.secret_key));
      statement.AddColumnInt64("pgrcreated", row.created);
      statement.AddColumnInt64("pgrexpires", row.expires);
      statement.AddColumn("pgrrevoked", (long) (row.revoked ? 1 : 0));
      statement.AddColumn("pgrsource", row.source);
      statement.AddColumn("pgrprefer", (long) row.prefer);
      statement.AddColumnInt64("pgrlastseen", row.last_seen);
      statement.AddColumn("pgrverified", (long) (row.verified ? 1 : 0));
      statement.AddColumnInt64("pgrverifiedat", row.verified_at);
      statement.AddColumn("pgrchangedfrom", String(row.changed_from));
      statement.AddColumnInt64("pgrchangedat", row.changed_at);
      statement.AddColumn("pgraccepted", (long) (row.accepted ? 1 : 0));

      __int64 id = existing.id;
      const bool written = replaced
         ? Application::Instance()->GetDBManager()->Execute(statement)
         : (Application::Instance()->GetDBManager()->Execute(statement, &id) && id != 0);
      if (!written)
      {
         // An own key that is not stored is a key the account has in one
         // browser tab and nowhere else, so this is worth a line in the log
         // rather than only a 500 that whoever pressed the button sees.
         String text;
         text.Format(_T("The OpenPGP key %s of account %I64d could not be stored."), Wide(row.fingerprint).c_str(), accountId);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7040, "OpenPgpRing::Save", text);
         return false;
      }

      row.id = id;
      return true;
   }

   bool
   OpenPgpRing::Remove(__int64 accountId, int kind, const AnsiString &fingerprint)
   {
      Row row;
      if (!Find(accountId, kind, fingerprint, row))
         return false;

      SQLCommand command("delete from hm_pgpring where pgrid = @ID");
      command.AddParameter("@ID", row.id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   OpenPgpRing::FindById(__int64 accountId, __int64 id, Row &row)
   {
      if (id <= 0)
         return false;

      SQLCommand command(String(_T("select ")) + String(RingColumns) + _T(" from hm_pgpring where pgrid = @ID and pgraccountid = @ACCOUNTID"));
      command.AddParameter("@ID", id);
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadRow(recordset, row);
      return true;
   }

   bool
   OpenPgpRing::SetVerified(__int64 accountId, __int64 id, bool verified)
   {
      Row row;
      if (!FindById(accountId, id, row))
         return false;

      // The account id is in the WHERE as well as in the lookup above: the
      // row was found under this account a moment ago, and the write says so
      // again rather than trusting the number alone.
      SQLCommand command(verified
         ? "update hm_pgpring set pgrverified = 1, pgrverifiedat = @AT, pgraccepted = @ACCEPTED where pgrid = @ID and pgraccountid = @ACCOUNTID"
         : "update hm_pgpring set pgrverified = 0, pgrverifiedat = 0 where pgrid = @ID and pgraccountid = @ACCOUNTID");
      if (verified)
      {
         command.AddParameter("@AT", Now());
         // A fingerprint read back to its owner settles the question a
         // change asks; a row that replaced nothing keeps what it had.
         command.AddParameter("@ACCEPTED", (int) (row.changed_from.IsEmpty() ? (row.accepted ? 1 : 0) : 1));
      }
      command.AddParameter("@ID", id);
      command.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   OpenPgpRing::SetAccepted(__int64 accountId, __int64 id, bool accepted)
   {
      SQLCommand command("update hm_pgpring set pgraccepted = @ACCEPTED where pgrid = @ID and pgraccountid = @ACCOUNTID");
      command.AddParameter("@ACCEPTED", (int) (accepted ? 1 : 0));
      command.AddParameter("@ID", id);
      command.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   AnsiString
   OpenPgpRing::RowJson(const Row &row, bool secretKeyIncluded)
   {
      AnsiString json;
      json.Format("{\"id\":%I64d,\"address\":\"%hs\",\"name\":\"%hs\",\"fingerprint\":\"%hs\",\"key_id\":\"%hs\","
                  "\"algorithm\":\"%hs\",\"public_key\":\"%hs\",\"created\":%I64d,\"expires\":%I64d,\"revoked\":%hs,"
                  "\"source\":\"%hs\",\"prefer_encrypt\":\"%hs\",\"first_seen\":%I64d,\"last_seen\":%I64d",
                  row.id,
                  Escape(Utf8(row.address)).c_str(),
                  Escape(Utf8(row.name)).c_str(),
                  row.fingerprint.c_str(),
                  row.key_id.c_str(),
                  Escape(Utf8(row.algorithm)).c_str(),
                  row.public_key.c_str(),
                  row.created,
                  row.expires,
                  row.revoked ? "true" : "false",
                  Escape(Utf8(row.source)).c_str(),
                  row.prefer == 1 ? "mutual" : "nopreference",
                  row.first_seen,
                  row.last_seen);

      // What the account itself has said about the key, and what the ring
      // noticed about it: the mark on every row, the change only on a row
      // that replaced another, so that "absent" means "replaced nothing".
      AnsiString trust;
      trust.Format(",\"verified\":%hs,\"verified_at\":%I64d", row.verified ? "true" : "false", row.verified_at);
      json += trust;
      if (!row.changed_from.IsEmpty())
      {
         AnsiString change;
         change.Format(",\"changed_from\":\"%hs\",\"changed_at\":%I64d,\"accepted\":%hs",
                       Escape(row.changed_from).c_str(), row.changed_at, row.accepted ? "true" : "false");
         json += change;
      }

      // The secret key goes back to the page that stored it and nowhere else.
      // It is encrypted under a passphrase this server has never had, so what
      // travels here is what the database holds: opaque to both ends of the
      // wire, and readable only in the browser that knows the passphrase.
      if (row.kind == KindOwn && secretKeyIncluded)
         json += ",\"secret_key\":\"" + Escape(row.secret_key) + "\"";
      else if (row.kind == KindOwn)
         json += ",\"secret_key_withheld\":true";

      json += "}";
      return json;
   }

   bool
   OpenPgpRing::DescribeFrom(const AnsiString &publicKeyBase64, Row &row, AnsiString &problem)
   {
      AnsiString decoded = Base64::Decode(publicKeyBase64.c_str(), publicKeyBase64.GetLength());
      if (decoded.IsEmpty())
      {
         problem = "the public key is not base64 of an OpenPGP key";
         return false;
      }

      OpenPgpKey::Parsed parsed = OpenPgpKey::Parse((const unsigned char *) decoded.c_str(), (size_t) decoded.GetLength(),
                                                    (size_t) Configuration::Instance()->GetOpenPGPKeyMaximumBytes());
      if (!parsed.ok)
      {
         problem = parsed.error;
         return false;
      }

      row.fingerprint = parsed.fingerprint;
      row.key_id = parsed.key_id;
      row.created = parsed.created;
      row.expires = parsed.expires;
      row.algorithm = Wide(parsed.algorithm_name);
      if (parsed.bits > 0)
      {
         AnsiString bits;
         bits.Format(" %d", parsed.bits);
         row.algorithm += Wide(bits);
      }

      // The address and the name the key itself carries, when the caller gave
      // none: a listing that named an address the key does not is a listing
      // that would have a person encrypt to the wrong key.
      const AnsiString keyAddress = OpenPgpKey::AddressOfUserId(parsed.primary_user_id);
      if (row.address.IsEmpty() && !keyAddress.IsEmpty())
         row.address = Wide(keyAddress);
      if (row.name.IsEmpty() && !parsed.primary_user_id.IsEmpty())
      {
         AnsiString userId = parsed.primary_user_id;
         const int bracket = userId.Find("<");
         AnsiString name = bracket > 0 ? userId.Mid(0, bracket) : userId;
         name.Trim();
         row.name = Wide(name);
      }

      return true;
   }

   namespace
   {
      // Organizational domains whose key directory did not answer, and when.
      // Keyed by the Public Suffix List alone (OrganizationOf), never by the DNS.
      const time_t UnreachableSeconds = 60 * 60;
      const time_t SlowSeconds = 3;
      const size_t UnreachableKept = 2000;
      boost::mutex unreachableLock;
      std::map<String, time_t> unreachableDirectories;

      // The registrable domain by the Public Suffix List, with no DNS query. Not
      // DMARC::GetOrganizationalDomain, which under DmarcTreeWalkEnabled (on by
      // default) walks the DNS and answers a name that publishes no DMARC record
      // as its own organization: a tarpit domain need only publish none for
      // every subdomain of it to be held apart, and the hold would never cover
      // the next one.
      String OrganizationOf(const String &domain)
      {
         String lowered = domain;
         lowered.Trim();
         lowered.MakeLower();
         while (lowered.EndsWith(_T(".")))
            lowered = lowered.Mid(0, lowered.GetLength() - 1);
         if (lowered.IsEmpty())
            return String();
         return DMARC::GetOrganizationalDomainByPublicSuffixList(lowered);
      }
   }

   void
   OpenPgpRing::NoteDirectoryUnreachable(const String &domain)
   {
      const String organizational = OrganizationOf(domain);
      if (organizational.IsEmpty())
         return;

      boost::lock_guard<boost::mutex> guard(unreachableLock);
      if (unreachableDirectories.size() >= UnreachableKept)
         unreachableDirectories.clear();
      unreachableDirectories[organizational] = time(nullptr);
   }

   bool
   OpenPgpRing::DirectoryUnreachable(const String &domain)
   {
      const String organizational = OrganizationOf(domain);
      if (organizational.IsEmpty())
         return false;

      boost::lock_guard<boost::mutex> guard(unreachableLock);
      std::map<String, time_t>::const_iterator held = unreachableDirectories.find(organizational);
      return held != unreachableDirectories.end() && time(nullptr) - held->second < UnreachableSeconds;
   }

   void
   OpenPgpRing::ForgetUnreachable()
   {
      boost::lock_guard<boost::mutex> guard(unreachableLock);
      unreachableDirectories.clear();
   }

   bool
   OpenPgpRing::MayHaveWebKeyDirectory(const String &domain)
   {
      String name = domain;
      name.Trim();
      while (name.EndsWith(_T(".")))
         name = name.Mid(0, name.GetLength() - 1);

      if (name.IsEmpty() || name.StartsWith(_T("[")))
         return false;

      const int lastDot = name.ReverseFind('.');
      if (lastDot <= 0 || lastDot == name.GetLength() - 1)
         return false;

      const wchar_t first = name.GetAt(lastDot + 1);
      return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z');
   }

   OpenPgpRing::Found
   OpenPgpRing::Lookup(const String &address)
   {
      Found found;

      // The whole route - both directory legs and the keyserver - shares ONE
      // deadline, the setting's seconds from now. Each leg is given what is
      // left, and a leg that finds nothing left is not attempted: the caller
      // names the host these fetches go to, and four requests from one mailbox
      // at three full timeouts each would stall every REST worker at once.
      const time_t deadline = time(nullptr) + Configuration::Instance()->GetOpenPGPLookupTimeout();
      auto remaining = [&]() -> int { const time_t left = deadline - time(nullptr); return left > 0 ? (int) left : 0; };

      String localPart = LocalPartOf(address);
      String domain = DomainOf(address);
      if (localPart.IsEmpty() || domain.IsEmpty())
      {
         found.error = _T("that is not an e-mail address");
         return found;
      }

      if (Configuration::Instance()->GetOpenPGPLookupWkd() && !MayHaveWebKeyDirectory(domain))
      {
         // An address literal, a name of one label, or an IPv4 address written
         // as a name (MayHaveWebKeyDirectory): no directory can be there, and
         // asking would connect to whatever address the caller chose. The
         // keyserver, if it is on, is still asked.
         found.error = _T("that address is not at a domain name, so there is no key directory to ask");
      }
      else if (Configuration::Instance()->GetOpenPGPLookupWkd() && DirectoryUnreachable(domain))
      {
         // Held (NoteDirectoryUnreachable): the directory is not asked, and the
         // keyserver, if it is on, still is.
         found.error = _T("asking the key directory of that domain's organization took three seconds or more within the last hour, so it is not asked again yet");
      }
      else if (Configuration::Instance()->GetOpenPGPLookupWkd())
      {
         // A directory that held this worker SlowSeconds or more is held against
         // the organization, whatever it answered - silence, a 404, or a key.
         // Holding only silence left the wait to whoever chose it: a directory
         // that answers 404 after nine seconds, for a new local part at the
         // same domain every time, costs a worker as much as one that never
         // answers, and needs no subdomain at all. A quick answer of any kind -
         // a name with no such host, a port closed, a 404 - is not held.
         const std::chrono::steady_clock::time_point directoryAsked = std::chrono::steady_clock::now();
         auto holdIfSlow = [&]()
         {
            if (std::chrono::steady_clock::now() - directoryAsked >= std::chrono::seconds(SlowSeconds))
               NoteDirectoryUnreachable(domain);
         };
         const AnsiString urls[2] =
         {
            OpenPgpKeyStore::DirectoryUrl(address, true),
            OpenPgpKeyStore::DirectoryUrl(address, false)
         };
         const wchar_t *names[2] = { L"wkd-advanced", L"wkd-direct" };

         for (int i = 0; i < 2; i++)
         {
            if (urls[i].IsEmpty())
               continue;

            if (remaining() <= 0)
            {
               // The directory's first leg took the whole deadline.
               holdIfSlow();
               found.error = _T("the lookup ran out of time");
               return found;
            }
            AnsiString body;
            int status = 0;
            String error;
            const bool fetched = Fetch(urls[i], true, remaining(), body, status, error);
            if (!fetched)
            {
               found.status = status;
               // A directory that has no key answers 404, and a domain that has
               // no directory answers whatever its web server answers. Neither
               // is worth reporting as a failure; the LAST thing that went
               // wrong is kept, so that a lookup which found nothing can still
               // say what it met.
               if (!error.IsEmpty())
                  found.error = error;
               else if (status > 0)
                  found.error.Format(_T("%s answered %d"), names[i], status);
               continue;
            }

            String problem;
            AnsiString base64;
            if (!KeyFromAnswer(body, base64, problem))
            {
               found.error.Format(_T("%s answered something that is not a key: %s"), names[i], problem.c_str());
               continue;
            }

            found.key_base64 = base64;
            found.source = names[i];
            found.url = Wide(urls[i]);
            found.status = status;
            // Answered with the key, which the person gets; a slow answer is
            // held all the same, or serving a key slowly would be the way round.
            holdIfSlow();
            return found;
         }

         holdIfSlow();
      }

      if (Configuration::Instance()->GetOpenPGPLookupKeyserver())
      {
         String base = Configuration::Instance()->GetOpenPGPKeyserverUrl();
         base.Trim();
         while (base.EndsWith(_T("/")))
            base = base.Mid(0, base.GetLength() - 1);

         String lowered = base;
         lowered.MakeLower();
         if (base.IsEmpty() || (!lowered.StartsWith(_T("https://")) && !lowered.StartsWith(_T("http://"))))
         {
            found.error = _T("the keyserver address is not set, or is not an http address");
            return found;
         }

         const AnsiString url = Utf8(base) + "/vks/v1/by-email/" + HttpsClient::FormEncode(Utf8(address));
         if (remaining() <= 0)
         {
            found.error = _T("the lookup ran out of time");
            return found;
         }
         AnsiString body;
         int status = 0;
         String error;
         if (!Fetch(url, false, remaining(), body, status, error))
         {
            found.status = status;
            if (!error.IsEmpty())
               found.error = error;
            else
               found.error.Format(_T("the keyserver answered %d"), status);
            return found;
         }

         String problem;
         AnsiString base64;
         if (!KeyFromAnswer(body, base64, problem))
         {
            found.error = String(_T("the keyserver answered something that is not a key: ")) + problem;
            return found;
         }

         found.key_base64 = base64;
         found.source = _T("keyserver");
         found.url = Wide(url);
         found.status = status;
         return found;
      }

      if (found.error.IsEmpty())
      {
         found.error = Configuration::Instance()->GetOpenPGPLookupWkd()
            ? _T("no key was found for that address")
            : _T("looking a key up is switched off on this server");
      }

      return found;
   }
}
