// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See SignInStore.h.

#include "StdAfx.h"

#include "SignInStore.h"

#include "AuditTrail.h"
#include "RestApiServer.h"
#include "Time.h"
#include "Unicode.h"
#include "Hashing/HashCreator.h"

#include "../Application/Application.h"
#include "../BO/Account.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../TCPIP/TCPConnection.h"

#include <atomic>
#include <climits>
#include <ctime>
#include <map>
#include <mutex>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How stale a client's "last seen" is allowed to get before the row is
      // written again. See the header: this is what keeps a polling client from
      // turning a column nobody watches in real time into the busiest write on
      // the server.
      const ULONGLONG write_interval_milliseconds = 60ULL * 1000;

      // The write throttle. Bounded rather than pruned entry by entry: the
      // entries are worth a minute each, so emptying the whole map when it grows
      // past this costs at most one extra write per client and needs no sweep.
      const size_t maximum_throttle_entries = 4096;

      std::mutex throttle_mutex;
      std::map<String, ULONGLONG> throttle;

      // The live connections, by account and device. A weak pointer, so this
      // never keeps a connection alive and never decides when one ends.
      struct LiveConnection
      {
         __int64 account_id = 0;
         String device;
         std::weak_ptr<TCPConnection> connection;
      };

      std::mutex live_mutex;
      std::vector<LiveConnection> live_connections;

      // Every entry whose connection has gone. Called under live_mutex by each
      // of the three operations, so the vector is swept by use rather than by a
      // timer.
      void SweepLive_()
      {
         live_connections.erase(
            std::remove_if(live_connections.begin(), live_connections.end(),
               [](const LiveConnection &entry)
               {
                  return entry.connection.expired();
               }),
            live_connections.end());
      }

      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString result;
         result.reserve(value.GetLength() + 8);

         for (int i = 0; i < value.GetLength(); i++)
         {
            char character = value[i];

            switch (character)
            {
            case '\"':
               result += "\\\"";
               break;
            case '\\':
               result += "\\\\";
               break;
            case '\r':
               result += "\\r";
               break;
            case '\n':
               result += "\\n";
               break;
            case '\t':
               result += "\\t";
               break;
            default:
               if (static_cast<unsigned char>(character) >= 0x20)
                  result += character;
               break;
            }
         }

         return result;
      }

      AnsiString Utf8Of(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // A user agent as it is stored: one line, printable, and short enough for
      // the column. A client chooses this string, so it is bounded here rather
      // than trusted - and a control character in it would otherwise reach a log
      // line and a page.
      String CleanAgent_(const String &agent)
      {
         String result;
         result.reserve(agent.GetLength());

         for (int index = 0; index < agent.GetLength() && result.GetLength() < 200; index++)
         {
            wchar_t character = agent.SafeGetAt((unsigned int) index);

            if (character < 0x20 || character == 0x7f)
               character = ' ';

            result += character;
         }

         result.Trim();

         return result;
      }
   }

   String
   SignInStore::WriteKey_(__int64 accountId, const String &device)
   {
      return Formatter::Format("{0}:{1}", accountId, device);
   }

   bool
   SignInStore::RecentlyWritten_(__int64 accountId, const String &device)
   {
      std::lock_guard<std::mutex> guard(throttle_mutex);

      auto found = throttle.find(WriteKey_(accountId, device));
      if (found == throttle.end())
         return false;

      return GetTickCount64() - found->second < write_interval_milliseconds;
   }

   void
   SignInStore::MarkWritten_(__int64 accountId, const String &device)
   {
      std::lock_guard<std::mutex> guard(throttle_mutex);

      if (throttle.size() >= maximum_throttle_entries)
         throttle.clear();

      throttle[WriteKey_(accountId, device)] = GetTickCount64();
   }

   void
   SignInStore::ForgetWritten_(__int64 accountId, const String &device)
   {
      std::lock_guard<std::mutex> guard(throttle_mutex);

      throttle.erase(WriteKey_(accountId, device));
   }

   String
   SignInStore::DeviceKey(int protocol, const String &address, const String &agent)
   {
      // The separator is a character neither an address nor a user agent may
      // hold after CleanAgent_, so two different clients cannot be given one key
      // by putting the separator in a field.
      String material;
      material.Format(_T("%d\n%s\n%s"), protocol, address.c_str(), CleanAgent_(agent).c_str());

      AnsiString utf8;
      Unicode::WideToMultiByte(material, utf8);

      HashCreator hasher(HashCreator::SHA256);
      AnsiString hash = hasher.GenerateHashNoSalt(utf8, HashCreator::hex);

      // Half of a SHA-256, lower case. Sixteen bytes is far beyond what is needed
      // to tell one person's clients apart and short enough to read in a URL.
      return String(hash.Mid(0, 32));
   }

   String
   SignInStore::Record(std::shared_ptr<const Account> account, int protocol, const IPAddress &address,
                       const String &agent, int credential, const String &credentialName)
   {
      return RecordFrom(account, protocol, String(address.ToString()), agent, credential, credentialName);
   }

   String
   SignInStore::RecordFrom(std::shared_ptr<const Account> account, int protocol, const String &address,
                           const String &agent, int credential, const String &credentialName)
   {
      if (!account || account->GetID() == 0)
         return String();

      const __int64 accountId = account->GetID();
      const String cleanAgent = CleanAgent_(agent);
      const String device = DeviceKey(protocol, address, cleanAgent);

      if (RecentlyWritten_(accountId, device))
         return device;

      try
      {
         SQLCommand existing("select siid, sicount from hm_signins where siaccountid = @ACCOUNTID and sidevice = @DEVICE");
         existing.AddParameter("@ACCOUNTID", accountId);
         existing.AddParameter("@DEVICE", device);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(existing);

         const String now = Time::GetCurrentDateTime();

         __int64 id = 0;
         int count = 0;

         if (recordset && !recordset->IsEOF())
         {
            id = recordset->GetInt64Value("siid");
            count = (int) recordset->GetLongValue("sicount");
         }

         SQLStatement statement;
         statement.SetTable("hm_signins");

         statement.AddColumnInt64("siaccountid", accountId);
         statement.AddColumn("sidevice", device);
         statement.AddColumn("siprotocol", (long) protocol);
         statement.AddColumn("siaddress", address, 64);
         statement.AddColumn("siagent", cleanAgent, 255);
         statement.AddColumn("sicredential", (long) credential);
         statement.AddColumn("sicredentialname", credentialName, 64);
         statement.AddColumnDate("silastseen", Time::GetDateFromSystemDate(now));
         // sicount is an int column: a count at INT_MAX stays there rather than
         // wrapping to a negative one.
         const int seen = count < INT_MAX ? count + 1 : INT_MAX;
         statement.AddColumn("sicount", (long) seen);

         // A client that signs in again is no longer revoked. A revocation ends
         // the sessions a device holds; it is not a ban, and the row must not go
         // on saying "revoked" about a client that is plainly back - see the
         // header, and docs/SessionsAndDevices.md, which says the same thing to
         // the person reading the list.
         statement.AddColumn("sirevoked", 0L);
         statement.AddColumnNULL("sirevokedtime");

         if (id == 0)
         {
            statement.AddColumnDate("sifirstseen", Time::GetDateFromSystemDate(now));
            statement.SetStatementType(SQLStatement::STInsert);
            statement.SetIdentityColumn("siid");
         }
         else
         {
            statement.SetStatementType(SQLStatement::STUpdate);
            statement.SetWhereClause(Formatter::Format("siid = {0}", id));
         }

         __int64 written = 0;

         if (!Application::Instance()->GetDBManager()->Execute(statement, id == 0 ? &written : 0))
         {
            // Reported, not fatal. A mail client must never be refused because
            // the record of it could not be kept.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6990, "SignInStore::RecordFrom",
               "A sign-in could not be recorded, so this client will not appear in the account's list of devices. "
               "Everything else about the sign-in succeeded.");

            return String();
         }

         if (id == 0)
            PruneAccount_(accountId);

         MarkWritten_(accountId, device);

         return device;
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("SignInStore::RecordFrom: a stop of the servers interrupted the recording of a sign-in.");
         throw;
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6991, "SignInStore::RecordFrom",
            "An unexpected error occurred while recording a sign-in. The sign-in itself was not affected.");

         return String();
      }
   }

   void
   SignInStore::PruneAccount_(__int64 accountId)
   {
      // The cap, applied when a row has just been added. Two statements rather
      // than one: SQL Server Compact refuses a scalar subquery compared with =,
      // and a DELETE with an IN over a TOP is not portable across the four
      // backends either - so the ids to remove are read first and deleted by id.
      try
      {
         SQLCommand count("select count(*) as sitotal from hm_signins where siaccountid = @ACCOUNTID");
         count.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(count);

         if (!counted || counted->IsEOF())
            return;

         const int total = (int) counted->GetLongValue("sitotal");
         if (total <= MaxDevicesPerAccount)
            return;

         SQLCommand oldest("select siid from hm_signins where siaccountid = @ACCOUNTID order by silastseen asc");
         oldest.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(oldest);

         if (!recordset)
            return;

         int toRemove = total - MaxDevicesPerAccount;

         std::vector<__int64> ids;
         while (!recordset->IsEOF() && (int) ids.size() < toRemove)
         {
            ids.push_back(recordset->GetInt64Value("siid"));
            recordset->MoveNext();
         }

         for (__int64 id : ids)
         {
            SQLCommand remove("delete from hm_signins where siid = @ID");
            remove.AddParameter("@ID", id);

            Application::Instance()->GetDBManager()->Execute(remove);
         }
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("SignInStore::PruneAccount_: a stop of the servers interrupted the keeping of an account's list of devices to its limit.");
         throw;
      }
      catch (...)
      {
         // A cap that could not be applied leaves a longer list, which is a
         // cosmetic fault and not one worth failing a sign-in over. Said, as
         // RecordFrom's own catch says what stops it, but once an hour at most:
         // every new device of every account comes here.
         static std::atomic<long long> lastReport(0);
         long long last = lastReport.load();
         const long long now = (long long) std::time(nullptr);
         if ((last == 0 || now < last || now - last >= 3600) && lastReport.compare_exchange_strong(last, now))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6991, "SignInStore::PruneAccount_",
               _T("An unexpected error occurred while keeping an account's list of devices to its limit; the list may be longer than the limit until the next new device. The sign-in itself was not affected. Said once an hour at most."));
         }
      }
   }

   bool
   SignInStore::ReadRow_(std::shared_ptr<DALRecordset> recordset, SignInRecord &out_record)
   {
      if (!recordset || recordset->IsEOF())
         return false;

      out_record.id = recordset->GetInt64Value("siid");
      out_record.account_id = recordset->GetInt64Value("siaccountid");
      out_record.device = recordset->GetStringValue("sidevice");
      out_record.protocol = (int) recordset->GetLongValue("siprotocol");
      out_record.address = recordset->GetStringValue("siaddress");
      out_record.agent = recordset->GetStringValue("siagent");
      out_record.credential = (int) recordset->GetLongValue("sicredential");
      out_record.credential_name = recordset->GetStringValue("sicredentialname");
      out_record.first_seen = recordset->GetStringValue("sifirstseen");
      out_record.last_seen = recordset->GetStringValue("silastseen");
      out_record.count = (int) recordset->GetLongValue("sicount");
      out_record.revoked = recordset->GetLongValue("sirevoked") != 0;
      out_record.revoked_time = recordset->GetStringValue("sirevokedtime");

      return true;
   }

   std::vector<SignInRecord>
   SignInStore::List(__int64 accountId)
   {
      std::vector<SignInRecord> result;

      if (accountId == 0)
         return result;

      SQLCommand command("select * from hm_signins where siaccountid = @ACCOUNTID order by silastseen desc");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return result;

      while (!recordset->IsEOF() && (int) result.size() <= MaxDevicesPerAccount)
      {
         SignInRecord record;

         if (ReadRow_(recordset, record))
            result.push_back(record);

         recordset->MoveNext();
      }

      return result;
   }

   bool
   SignInStore::Get(__int64 accountId, const String &device, SignInRecord &out_record)
   {
      if (accountId == 0 || device.IsEmpty())
         return false;

      SQLCommand command("select * from hm_signins where siaccountid = @ACCOUNTID and sidevice = @DEVICE");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@DEVICE", device);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      return ReadRow_(recordset, out_record);
   }

   void
   SignInStore::Attach(__int64 accountId, const String &device, std::shared_ptr<TCPConnection> connection)
   {
      if (accountId == 0 || device.IsEmpty() || !connection)
         return;

      std::lock_guard<std::mutex> guard(live_mutex);

      SweepLive_();

      LiveConnection entry;
      entry.account_id = accountId;
      entry.device = device;
      entry.connection = connection;

      live_connections.push_back(entry);
   }

   int
   SignInStore::DropConnections(__int64 accountId, const String &device)
   {
      std::vector<std::shared_ptr<TCPConnection>> doomed;

      {
         std::lock_guard<std::mutex> guard(live_mutex);

         SweepLive_();

         for (auto iterator = live_connections.begin(); iterator != live_connections.end();)
         {
            if (iterator->account_id != accountId || iterator->device != device)
            {
               ++iterator;
               continue;
            }

            std::shared_ptr<TCPConnection> connection = iterator->connection.lock();
            if (connection)
               doomed.push_back(connection);

            iterator = live_connections.erase(iterator);
         }
      }

      // Outside the lock: EnqueueDisconnect dispatches onto the connection's own
      // strand, and nothing here should hold a registry lock while another
      // thread's completion handler runs.
      for (std::shared_ptr<TCPConnection> connection : doomed)
         connection->EnqueueDisconnect();

      return (int) doomed.size();
   }

   int
   SignInStore::DropConnectionsForAccount(__int64 accountId)
   {
      std::vector<std::shared_ptr<TCPConnection>> doomed;

      {
         std::lock_guard<std::mutex> guard(live_mutex);

         SweepLive_();

         for (auto iterator = live_connections.begin(); iterator != live_connections.end();)
         {
            if (iterator->account_id != accountId)
            {
               ++iterator;
               continue;
            }

            std::shared_ptr<TCPConnection> connection = iterator->connection.lock();
            if (connection)
               doomed.push_back(connection);

            iterator = live_connections.erase(iterator);
         }
      }

      for (std::shared_ptr<TCPConnection> connection : doomed)
         connection->EnqueueDisconnect();

      return (int) doomed.size();
   }

   bool
   SignInStore::DisableAppPassword_(__int64 accountId, const String &name)
   {
      if (name.IsEmpty())
         return false;

      SQLCommand command("update hm_apppasswords set apactive = 0 where apaccountid = @ACCOUNTID and apname = @NAME and apactive <> 0");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NAME", name);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   SignInStore::Revoke(__int64 accountId, const String &device, bool revokeCredential,
                       int &out_connectionsDropped, int &out_sessionsEnded, bool &out_credentialDisabled)
   {
      out_connectionsDropped = 0;
      out_sessionsEnded = 0;
      out_credentialDisabled = false;

      SignInRecord record;
      if (!Get(accountId, device, record))
         return false;

      const String now = Time::GetCurrentDateTime();

      SQLStatement statement;
      statement.SetTable("hm_signins");
      statement.SetStatementType(SQLStatement::STUpdate);
      statement.SetWhereClause(Formatter::Format("siid = {0}", record.id));
      statement.AddColumn("sirevoked", 1L);
      statement.AddColumnDate("sirevokedtime", Time::GetDateFromSystemDate(now));

      Application::Instance()->GetDBManager()->Execute(statement);

      // The throttle entry goes with it, so that the next sign-in from this
      // client writes its row rather than being skipped as recent - which is how
      // a revoked entry would otherwise go on reading "revoked" about a client
      // that had plainly come back.
      ForgetWritten_(accountId, device);

      out_sessionsEnded = RestApiServer::EndSessionsForDevice(accountId, device);
      out_connectionsDropped = DropConnections(accountId, device);

      if (revokeCredential && record.credential == CredentialAppPassword)
         out_credentialDisabled = DisableAppPassword_(accountId, record.credential_name);

      // The one place a revoke is recorded, whichever interface asked - the
      // account holder's own page, an administrator over REST, or COM. The row
      // it writes in hm_signins is not an audited table (see
      // build/check-audit-choke-point.py: a sign-in would be recorded on every
      // authentication), and the sessions and connections it ends leave no row at
      // all, so without this the strongest thing a person can do about a client
      // they do not recognise would be the one thing the trail did not hold.
      String detail;
      detail.Format(_T("%s from %s; %d session(s) ended, %d connection(s) dropped%s"),
         String(ProtocolName(record.protocol)).c_str(),
         record.address.c_str(),
         out_sessionsEnded,
         out_connectionsDropped,
         out_credentialDisabled ? _T(", the app password it used deactivated") : _T(""));

      AuditTrail::Instance()->RecordEvent(_T("device"), record.device, _T("revoked"), detail);

      return true;
   }

   bool
   SignInStore::Forget(__int64 accountId, const String &device, int &out_connectionsDropped, int &out_sessionsEnded)
   {
      out_connectionsDropped = 0;
      out_sessionsEnded = 0;

      SignInRecord record;
      if (!Get(accountId, device, record))
         return false;

      out_sessionsEnded = RestApiServer::EndSessionsForDevice(accountId, device);
      out_connectionsDropped = DropConnections(accountId, device);

      ForgetWritten_(accountId, device);

      SQLCommand remove("delete from hm_signins where siid = @ID");
      remove.AddParameter("@ID", record.id);

      return Application::Instance()->GetDBManager()->Execute(remove);
   }

   AnsiString
   SignInStore::ProtocolName(int protocol)
   {
      switch (protocol)
      {
      case ProtocolSmtp:
         return "smtp";
      case ProtocolPop3:
         return "pop3";
      case ProtocolImap:
         return "imap";
      case ProtocolWeb:
         return "web";
      case ProtocolDav:
         return "dav";
      case ProtocolManageSieve:
         return "managesieve";
      case ProtocolAutodiscover:
         return "autodiscover";
      default:
         return "unknown";
      }
   }

   AnsiString
   SignInStore::CredentialName(int credential)
   {
      switch (credential)
      {
      case CredentialAppPassword:
         return "app_password";
      case CredentialToken:
         return "token";
      case CredentialCertificate:
         return "certificate";
      case CredentialSingleSignOn:
         return "single_sign_on";
      case CredentialPasskey:
         return "passkey";
      default:
         return "password";
      }
   }

   AnsiString
   SignInStore::ToJson(const SignInRecord &record, const AnsiString &extra)
   {
      AnsiString json;

      json.Format("{\"id\":\"%hs\",\"protocol\":\"%hs\",\"address\":\"%hs\",\"agent\":\"%hs\",\"credential\":\"%hs\","
                  "\"credential_name\":\"%hs\",\"first_seen\":\"%hs\",\"last_seen\":\"%hs\",\"sign_ins\":%d,"
                  "\"revoked\":%hs,\"revoked_at\":\"%hs\"}",
         JsonEscape(Utf8Of(record.device)).c_str(),
         ProtocolName(record.protocol).c_str(),
         JsonEscape(Utf8Of(record.address)).c_str(),
         JsonEscape(Utf8Of(record.agent)).c_str(),
         CredentialName(record.credential).c_str(),
         JsonEscape(Utf8Of(record.credential_name)).c_str(),
         JsonEscape(Utf8Of(record.first_seen)).c_str(),
         JsonEscape(Utf8Of(record.last_seen)).c_str(),
         record.count,
         record.revoked ? "true" : "false",
         JsonEscape(Utf8Of(record.revoked_time)).c_str());

      if (!extra.IsEmpty())
      {
         json = json.Mid(0, json.GetLength() - 1);
         json += ",";
         json += extra;
         json += "}";
      }

      return json;
   }

   AnsiString
   SignInStore::ToJson(const std::vector<SignInRecord> &records)
   {
      AnsiString json = "[";

      for (size_t index = 0; index < records.size(); index++)
      {
         if (index > 0)
            json += ",";

         json += ToJson(records[index]);
      }

      json += "]";

      return json;
   }
}
