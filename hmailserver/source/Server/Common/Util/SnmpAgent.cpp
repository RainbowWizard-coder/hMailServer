// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The SNMP agent's listener, its settings and the objects it serves. See
// SnmpAgent.h, and SnmpEngine.h for what one datagram gets back.

#include "StdAfx.h"

#include "SnmpAgent.h"
#include "SnmpUsm.h"
#include "ServerStatus.h"
#include "CountryBlocking.h"
#include "Crypt.h"
#include "Unicode.h"

#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../BO/Message.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Persistence/PersistentMessage.h"
#include "../TCPIP/AcceptLoop.h"
#include "../TCPIP/IPAddress.h"
#include "../TCPIP/SocketConstants.h"

#include <openssl/rand.h>

#include <chrono>
#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // One entry of SnmpAllowedAddresses: an address, and the length of its
   // prefix, or -1 for the address alone.
   struct SnmpAllowedRange
   {
      SnmpAllowedRange() :
         prefix(-1)
      {
      }

      IPAddress address;
      int prefix;
   };

   struct SnmpAgent::UdpListener
   {
      UdpListener() :
         socket(io),
         consecutive_errors(0)
      {
         // One octet more than the engine decodes, so that a datagram too large
         // to be answered arrives too large to be decoded rather than cut to fit.
         buffer.resize(SnmpEngine::MaxMessageSize + 1);
      }

      boost::asio::io_context io;
      boost::asio::ip::udp::socket socket;
      boost::asio::ip::udp::endpoint sender;
      std::vector<unsigned char> buffer;
      std::vector<SnmpAllowedRange> allowed;
      unsigned int consecutive_errors;
   };

   namespace
   {
      // SO_EXCLUSIVEADDRUSE on the agent's socket before it is bound, on Windows
      // (AcceptLoop::ClaimPortExclusively): no other socket then takes the
      // requests sent to one address of the agent's port, nor does the agent start
      // on the wildcard while another holds one address of it (measured on Windows
      // 11 for UDP too: without the option a second socket on 127.0.0.1 took all
      // 20 datagrams sent there). A datagram socket leaves nothing behind it, so a
      // restart binds again whatever was sent before. No effect on POSIX.
      void ClaimExclusively_(boost::asio::ip::udp::socket &udpSocket)
      {
         AcceptLoop::ClaimPortExclusively(udpSocket.native_handle());
      }

      // How often the refresher looks, and how old the delivery queue's figures
      // may grow before a request asks for them again: MetricsServer's numbers.
      const ULONGLONG RefresherTickMilliseconds = 1000;
      const ULONGLONG DeliveryQueueRefreshMilliseconds = 10000;

      // The most entries SnmpAllowedAddresses may hold, and characters.
      const size_t MaxAllowedEntries = 64;
      const int MaxAllowedLength = 1000;

      // RFC 3411's enterprise form of an engine ID: the top bit set over the
      // enterprise number - 32473, RFC 5612's number for documentation, until
      // this project has one of its own - then format 5, octets.
      const unsigned char EngineIdPrefix[] = { 0x80, 0x00, 0x7E, 0xD9, 0x05 };

      // Twelve octets of SHA-256 after the prefix: seventeen in all, inside the
      // 5 to 32 RFC 3411 allows.
      const size_t EngineIdDigestOctets = 12;

      const size_t EngineSeedOctets = 16;

      // A boots row is named SnmpBoots and the hex of this many octets of the
      // engine ID after its prefix: 9 + 20 = 29 characters, and
      // hm_settings.settingname holds 30.
      const size_t EngineBootsIdOctets = 10;

      // The one boots row of eb7df1d99, the row's first commit, shared by every
      // engine on a database. Never released, but a bench that ran that commit
      // with the agent on holds it. Nothing reads or writes it now; it is only
      // named, so that a backup and the changed-from-stock list leave it out as
      // they leave out the rows that replaced it.
      const TCHAR *const LegacyEngineBootsName = _T("SnmpEngineBoots");

      // The groups of the mail MIB: one per protocol the server runs, with the
      // index it always has, so that a group keeps its row when another is off.
      struct MtaGroup
      {
         unsigned int index;
         const char *name;
         unsigned int port;
         int sessionType;
         const char *description;
      };

      const MtaGroup MtaGroups[] =
      {
         { 1, "SMTP", 25, STSMTP, "SMTP: mail received from other servers and from clients, and delivered from the delivery queue, which is this group's store." },
         { 2, "POP3", 110, STPOP3, "POP3: mailboxes read by clients." },
         { 3, "IMAP", 143, STIMAP, "IMAP: mailboxes read by clients." }
      };

      std::string ToUtf8(const String &value)
      {
         if (value.IsEmpty())
            return std::string();

         AnsiString converted;
         if (!Unicode::WideToMultiByte(value, converted))
            return std::string();

         return std::string(converted.c_str(), (size_t) converted.GetLength());
      }

      String FromAscii(const std::string &value)
      {
         return String(AnsiString(value.c_str()));
      }

      std::string Trimmed(const std::string &value)
      {
         const size_t first = value.find_first_not_of(" \t\r\n");
         if (first == std::string::npos)
            return std::string();

         const size_t last = value.find_last_not_of(" \t\r\n");
         return value.substr(first, last - first + 1);
      }

      // "192.0.2.10", "10.0.0.0/8", "2001:db8::/32". False for anything else.
      bool ParseRange(const std::string &entry, SnmpAllowedRange &range)
      {
         std::string addressPart = entry;
         int prefix = -1;

         const size_t slash = entry.find('/');
         if (slash != std::string::npos)
         {
            addressPart = entry.substr(0, slash);
            const std::string prefixPart = entry.substr(slash + 1);

            if (prefixPart.empty() || prefixPart.size() > 3)
               return false;

            prefix = 0;
            for (size_t i = 0; i < prefixPart.size(); i++)
            {
               if (prefixPart[i] < '0' || prefixPart[i] > '9')
                  return false;

               prefix = prefix * 10 + (prefixPart[i] - '0');
            }
         }

         if (addressPart.empty())
            return false;

         IPAddress address;
         if (!address.TryParse(AnsiString(addressPart.c_str()), false))
            return false;

         if (address.GetType() == IPAddress::IPV4 && prefix > 32)
            return false;

         if (address.GetType() == IPAddress::IPV6 && prefix > 128)
            return false;

         range.address = address;
         range.prefix = prefix;
         return true;
      }

      // Splits the list at commas, semicolons and white space. False, with the
      // entry that would not parse, for a list that holds one.
      bool ParseAllowedList(const std::string &list, std::vector<SnmpAllowedRange> &ranges, std::string &refused)
      {
         ranges.clear();
         refused.clear();

         std::string entry;

         for (size_t i = 0; i <= list.size(); i++)
         {
            const char c = i < list.size() ? list[i] : ',';

            if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
               if (!entry.empty())
               {
                  SnmpAllowedRange range;
                  if (!ParseRange(entry, range))
                  {
                     refused = entry;
                     return false;
                  }

                  ranges.push_back(range);
                  entry.clear();
               }

               continue;
            }

            entry += c;
         }

         return true;
      }

      // The same comparison TrustedProxyList makes: the families never match
      // across, and a prefix compares the leading bits.
      bool RangeMatches(const SnmpAllowedRange &range, const IPAddress &address)
      {
         if (range.address.GetType() != address.GetType())
            return false;

         if (range.prefix < 0)
         {
            return range.address.GetAddress1() == address.GetAddress1() &&
                   range.address.GetAddress2() == address.GetAddress2();
         }

         if (address.GetType() == IPAddress::IPV4)
         {
            const unsigned __int64 mask = range.prefix == 0 ? 0 : (0xFFFFFFFFULL << (32 - range.prefix)) & 0xFFFFFFFFULL;
            return (range.address.GetAddress1() & mask) == (address.GetAddress1() & mask);
         }

         const int highBits = range.prefix > 64 ? 64 : range.prefix;
         const int lowBits = range.prefix > 64 ? range.prefix - 64 : 0;

         const unsigned __int64 highMask = highBits == 0 ? 0 : (highBits == 64 ? 0xFFFFFFFFFFFFFFFFULL : (0xFFFFFFFFFFFFFFFFULL << (64 - highBits)));
         const unsigned __int64 lowMask = lowBits == 0 ? 0 : (lowBits == 64 ? 0xFFFFFFFFFFFFFFFFULL : (0xFFFFFFFFFFFFFFFFULL << (64 - lowBits)));

         return (range.address.GetAddress1() & highMask) == (address.GetAddress1() & highMask) &&
                (range.address.GetAddress2() & lowMask) == (address.GetAddress2() & lowMask);
      }

      // hm_messages.messagecreatetime ("YYYY-MM-DD HH:MM:SS", local time) as a
      // Unix time; 0 for anything else. MetricsServer reads the same column the
      // same way for hmailserver_delivery_queue_oldest_message_age_seconds.
      __int64 LocalTimeStampToUnixTime(const String &timestamp)
      {
         if (timestamp.GetLength() < 19)
            return 0;

         tm parsed = {};
         parsed.tm_year = _ttoi(timestamp.Mid(0, 4).c_str()) - 1900;
         parsed.tm_mon = _ttoi(timestamp.Mid(5, 2).c_str()) - 1;
         parsed.tm_mday = _ttoi(timestamp.Mid(8, 2).c_str());
         parsed.tm_hour = _ttoi(timestamp.Mid(11, 2).c_str());
         parsed.tm_min = _ttoi(timestamp.Mid(14, 2).c_str());
         parsed.tm_sec = _ttoi(timestamp.Mid(17, 2).c_str());
         parsed.tm_isdst = -1;

         if (parsed.tm_year < 70)
            return 0;

         const time_t converted = mktime(&parsed);
         if (converted == (time_t) -1)
            return 0;

         return (__int64) converted;
      }

      // One row of hm_settings as the database holds it now - not as this process
      // loaded it, which is out of date the moment another node writes. False
      // when the database did not answer; found says whether the row is there.
      bool ReadRecordRow(const String &name, bool &found, String &value)
      {
         found = false;
         value.Empty();

         std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
         if (!database)
            return false;

         SQLCommand command("select settingstring from hm_settings where settingname = @SETTINGNAME");
         command.AddParameter("@SETTINGNAME", name);

         std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);
         if (!recordset)
            return false;

         if (!recordset->IsEOF())
         {
            found = true;
            value = recordset->GetStringValue("settingstring");
         }

         return true;
      }

      void AddScalar(std::vector<SnmpBinding> &bindings, const SnmpOid &base, unsigned int arc, const SnmpValue &value)
      {
         SnmpOid name = base;
         name.push_back(arc);
         name.push_back(0);
         bindings.push_back(SnmpBinding(name, 1, value));
      }

      void AddColumn(std::vector<SnmpBinding> &bindings, const SnmpOid &entry, unsigned int column,
                     const SnmpOid &instance, const SnmpValue &value)
      {
         SnmpOid name = entry;
         name.push_back(column);
         name.insert(name.end(), instance.begin(), instance.end());
         bindings.push_back(SnmpBinding(name, instance.size(), value));
      }

      // A TimeInterval (SNMPv2-TC): hundredths of a second, 0 to 2^31-1.
      SnmpValue TimeInterval(unsigned long long hundredths)
      {
         return SnmpValue::MakeInteger(hundredths > 2147483647ULL ? 2147483647LL : (long long) hundredths);
      }

      bool IsVisibleAscii(const String &value, bool spaceAllowed)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            const wchar_t c = value.GetAt(i);

            if (c == ' ' && spaceAllowed)
               continue;

            if (c <= 0x20 || c >= 0x7F)
               return false;
         }

         return true;
      }
   }

   SnmpAgent::SnmpAgent() :
      running_(false),
      start_tick_(0),
      group_smtp_(false),
      group_pop3_(false),
      group_imap_(false),
      refresher_stop_(true),
      queue_refresh_requested_(false),
      queue_refresh_tick_(0),
      queue_depth_(0),
      queue_oldest_age_seconds_(0)
   {
   }

   SnmpAgent::~SnmpAgent()
   {
      Stop();
   }

   const TCHAR *
   SnmpAgent::EngineSeedName()
   {
      return _T("SnmpEngineSeed");
   }

   const TCHAR *
   SnmpAgent::EngineBootsPrefix()
   {
      return _T("SnmpBoots");
   }

   String
   SnmpAgent::EngineBootsName(const SnmpBytes &engineId)
   {
      // The engine ID's own octets after its five-octet prefix, ten of them: the
      // prefix and twenty hex digits are 29 characters, and hm_settings.settingname
      // holds 30.
      SnmpBytes own;
      for (size_t i = sizeof(EngineIdPrefix); i < engineId.size() && own.size() < EngineBootsIdOctets; i++)
         own.push_back(engineId[i]);

      String name = EngineBootsPrefix();
      name += FromAscii(SnmpBer::ToHex(own));
      return name;
   }

   bool
   SnmpAgent::IsEngineRecord(const String &name)
   {
      if (name.CompareNoCase(EngineSeedName()) == 0 ||
          name.CompareNoCase(LegacyEngineBootsName) == 0)
         return true;

      // SnmpBoots and exactly the twenty hex digits EngineBootsName writes, so
      // that no setting that merely begins the same way is taken for one.
      const String prefix = EngineBootsPrefix();
      const int hexDigits = (int) (2 * EngineBootsIdOctets);

      if (name.GetLength() != prefix.GetLength() + hexDigits ||
          name.Left(prefix.GetLength()).CompareNoCase(prefix.c_str()) != 0)
         return false;

      for (int i = prefix.GetLength(); i < name.GetLength(); i++)
      {
         const wchar_t c = name.GetAt(i);
         const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         if (!hex)
            return false;
      }

      return true;
   }

   SnmpOid
   SnmpAgent::EnterpriseOid()
   {
      return SnmpBer::Oid("1.3.6.1.4.1.32473.1");
   }

   String
   SnmpAgent::BindAddressProblem(const String &value)
   {
      String address = value;
      address.Trim();

      if (address.IsEmpty())
         return String();

      const AnsiString narrow = address;

      IPAddress parsed;
      if (address.GetLength() > 64 || !parsed.TryParse(narrow, false))
         return String(_T("SnmpBindAddress must be empty, which is every address, or one IPv4 or IPv6 address of this machine, such as 127.0.0.1."));

      return String();
   }

   String
   SnmpAgent::AllowedAddressesProblem(const String &value)
   {
      if (value.GetLength() > MaxAllowedLength)
         return String(_T("SnmpAllowedAddresses is longer than 1000 characters."));

      std::vector<SnmpAllowedRange> ranges;
      std::string refused;

      if (!ParseAllowedList(ToUtf8(value), ranges, refused))
      {
         return Formatter::Format(_T("SnmpAllowedAddresses: {0} is not an IP address or a range in CIDR form, such as 192.0.2.10 or 10.0.0.0/8."),
            FromAscii(refused));
      }

      if (ranges.size() > MaxAllowedEntries)
         return String(_T("SnmpAllowedAddresses holds more than 64 addresses and ranges; a range in CIDR form covers many."));

      return String();
   }

   String
   SnmpAgent::UserProblem(const String &value)
   {
      if (value.IsEmpty())
         return String();

      if (value.GetLength() > 32 || !IsVisibleAscii(value, false))
         return String(_T("SnmpV3User is at most 32 visible ASCII characters, with no space (RFC 3414 usmUserName)."));

      return String();
   }

   String
   SnmpAgent::PasswordProblem(const String &value, const String &settingName)
   {
      if (value.IsEmpty())
         return String();

      // RFC 3414 section 11.2 will not turn fewer than eight characters into a
      // key, and a manager will refuse them too. ASCII, because a password is
      // turned into a key octet by octet and two ends that encode a character
      // differently hold two different keys.
      if (value.GetLength() < (int) SnmpUsm::MinimumPasswordLength || value.GetLength() > 64 || !IsVisibleAscii(value, true))
         return settingName + _T(" must be 8 to 64 ASCII characters (RFC 3414 section 11.2 asks for at least 8).");

      if (Crypt::Instance()->ProtectSecret(value).IsEmpty())
         return settingName + _T(" could not be stored protected; the error log says why.");

      return String();
   }

   String
   SnmpAgent::CommunityProblem(const String &value)
   {
      if (value.IsEmpty())
         return String();

      if (value.GetLength() > 64 || !IsVisibleAscii(value, false))
         return String(_T("SnmpV2cCommunity is at most 64 visible ASCII characters, with no space."));

      if (Crypt::Instance()->ProtectSecret(value).IsEmpty())
         return String(_T("SnmpV2cCommunity could not be stored protected; the error log says why."));

      return String();
   }

   String
   SnmpAgent::TextProblem(const String &value, const String &settingName)
   {
      if (value.GetLength() > 255)
         return settingName + _T(" is at most 255 characters (a DisplayString).");

      // A DisplayString (RFC 2579) is NVT ASCII: printable characters, one line.
      // Anything else would be octets a manager reads as something other than
      // what was typed, and a character outside ASCII more than one of them.
      for (int i = 0; i < value.GetLength(); i++)
      {
         const wchar_t c = value.GetAt(i);
         if (c < 0x20 || c > 0x7E)
            return settingName + _T(" is one line of printable ASCII - a DisplayString (RFC 2579) holds nothing else.");
      }

      return String();
   }

   bool
   SnmpAgent::PrepareEngine_(SnmpEngineSettings &settings)
   {
      Configuration *configuration = Configuration::Instance();

      // The installation's seed, made the first time and kept.
      SnmpBytes seed;
      if (!ReadOrMakeSeed_(seed))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7421, "SnmpAgent::Start",
            "The SNMP agent could not read or make its engine's seed (the SnmpEngineSeed row of hm_settings), so it is not answering. A value in that row that is not a seed is refused rather than replaced, since a new seed is a new engine; empty the row to make one.");
         return false;
      }

      // The engine ID: the prefix, and twelve octets of SHA-256 over the seed and
      // this machine's name, so that two nodes sharing one database differ.
      std::string machine;
      try
      {
         machine = boost::asio::ip::host_name();
      }
      catch (...)
      {
         machine.clear();
      }

      SnmpBytes material = seed;
      material.insert(material.end(), machine.begin(), machine.end());

      SnmpBytes digest;
      if (!SnmpUsm::Sha256(&material[0], material.size(), digest) || digest.size() < EngineIdDigestOctets)
         return false;

      settings.engineId.assign(EngineIdPrefix, EngineIdPrefix + sizeof(EngineIdPrefix));
      settings.engineId.insert(settings.engineId.end(), digest.begin(), digest.begin() + (std::ptrdiff_t) EngineIdDigestOctets);

      // snmpEngineBoots: one more than last time, kept for this engine and
      // written before the socket opens. A boots value this engine has used
      // before would let a message recorded then be accepted now, so an agent
      // that cannot record it stays off.
      long long boots = 0;
      if (!RecordBoot_(settings.engineId, boots))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7421, "SnmpAgent::Start",
            Formatter::Format(_T("The SNMP agent could not read, or found damaged, or could not write its boot count (the {0} row of hm_settings), so it is not answering: a boot count used twice would let a message recorded in one run be accepted in the next. A row that holds anything but a count is refused rather than read as none. Emptying the SnmpEngineSeed row makes a new engine, with a boot count of its own; emptying this row alone would start the same engine's count again and is not the way."),
               EngineBootsName(settings.engineId)));
         return false;
      }

      settings.engineBoots = boots;

      if (boots >= SnmpEngine::MaxEngineBoots)
      {
         LOG_APPLICATION("SNMP: snmpEngineBoots has reached 2147483647, where RFC 3414 latches it, so no SNMPv3 request will be answered. Empty the SnmpEngineSeed row of hm_settings to make a new engine, with a boot count of its own.");
      }

      // The one SNMPv3 user, and its keys localised to this engine.
      String user = configuration->GetSnmpV3User();
      user.Trim();

      const String authPassword = configuration->GetSnmpV3AuthPassword();
      const String privPassword = configuration->GetSnmpV3PrivPassword();

      if (!user.IsEmpty())
      {
         if (authPassword.IsEmpty() || privPassword.IsEmpty())
         {
            LOG_APPLICATION("SNMP: SnmpV3User is set but SnmpV3AuthPassword and SnmpV3PrivPassword are not both set, so no SNMPv3 request will be answered. This agent answers SNMPv3 at authPriv only.");
         }
         else if (!UserProblem(user).IsEmpty() ||
                  authPassword.GetLength() < (int) SnmpUsm::MinimumPasswordLength ||
                  privPassword.GetLength() < (int) SnmpUsm::MinimumPasswordLength)
         {
            LOG_APPLICATION("SNMP: SnmpV3User, SnmpV3AuthPassword or SnmpV3PrivPassword is not one this agent can use (a user name of at most 32 visible characters, passwords of at least 8), so no SNMPv3 request will be answered.");
         }
         else
         {
            std::string authText = ToUtf8(authPassword);
            std::string privText = ToUtf8(privPassword);

            SnmpBytes privFull;

            const bool keyed = SnmpUsm::LocalisedKey(authText, settings.engineId, settings.authKey) &&
                               SnmpUsm::LocalisedKey(privText, settings.engineId, privFull) &&
                               privFull.size() >= SnmpUsm::PrivKeyLength;

            if (keyed)
            {
               settings.privKey.assign(privFull.begin(), privFull.begin() + (std::ptrdiff_t) SnmpUsm::PrivKeyLength);
               settings.user = ToUtf8(user);
            }
            else
            {
               SnmpUsm::Scrub(settings.authKey);
               LOG_APPLICATION("SNMP: the SNMPv3 keys could not be made from the passwords, so no SNMPv3 request will be answered.");
            }

            SnmpUsm::Scrub(authText);
            SnmpUsm::Scrub(privText);
            SnmpUsm::Scrub(privFull);
         }
      }

      // SNMPv2c: off unless asked for, and then only with a community.
      settings.v2cEnabled = configuration->GetSnmpV2cEnabled();
      settings.community = ToUtf8(configuration->GetSnmpV2cCommunity());

      if (settings.v2cEnabled && settings.community.empty())
      {
         LOG_APPLICATION("SNMP: SnmpV2cEnabled is on but SnmpV2cCommunity is empty, so no SNMPv2c request will be answered.");
      }

      // The system group.
      Application *application = Application::Instance();
      settings.description = "hMailServer " + ToUtf8(application->GetVersionNumber()) + " (" + ToUtf8(application->GetVersionArchitecture()) + ")";

      // sysObjectID: 1.3.6.1.4.1.32473.1.1, the arc and .1 - the statistics are .2.
      settings.objectId = EnterpriseOid();
      settings.objectId.push_back(1);

      settings.contact = ToUtf8(configuration->GetSnmpContact());
      settings.location = ToUtf8(configuration->GetSnmpLocation());

      settings.name = ToUtf8(configuration->GetHostName());
      if (settings.name.empty())
         settings.name = machine;

      return true;
   }

   bool
   SnmpAgent::ReadOrMakeSeed_(SnmpBytes &seed)
   {
      seed.clear();

      bool found = false;
      String stored;
      if (!ReadRecordRow(EngineSeedName(), found, stored))
         return false;

      if (!stored.IsEmpty())
         return SnmpBer::FromHex(ToUtf8(stored), seed) && seed.size() == EngineSeedOctets;

      SnmpBytes made(EngineSeedOctets, 0);
      if (RAND_bytes(&made[0], (int) made.size()) != 1)
         return false;

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database)
         return false;

      const String madeText = FromAscii(SnmpBer::ToHex(made));

      // Written only where there is none yet. Of two nodes starting together
      // for the first time, the first to write wins and both read its seed back
      // below: the update fills the row only while it is still empty, and an
      // insert that finds the row already made by the other node (settingname
      // is unique) is let fail quietly rather than reported.
      if (found)
      {
         SQLCommand update("update hm_settings set settingstring = @SETTINGSTRING where settingname = @SETTINGNAME and settingstring = ''");
         update.AddParameter("@SETTINGSTRING", madeText);
         update.AddParameter("@SETTINGNAME", String(EngineSeedName()));

         if (!database->Execute(update))
            return false;
      }
      else
      {
         SQLCommand insert("insert into hm_settings (settingname, settingstring, settinginteger) values (@SETTINGNAME, @SETTINGSTRING, 0)");
         insert.AddParameter("@SETTINGNAME", String(EngineSeedName()));
         insert.AddParameter("@SETTINGSTRING", madeText);

         database->Execute(insert, 0, DALConnection::DALErrorInSQL);
      }

      if (!ReadRecordRow(EngineSeedName(), found, stored) || !found)
         return false;

      return SnmpBer::FromHex(ToUtf8(stored), seed) && seed.size() == EngineSeedOctets;
   }

   bool
   SnmpAgent::RecordBoot_(const SnmpBytes &engineId, long long &boots)
   {
      boots = 0;

      const String name = EngineBootsName(engineId);

      bool found = false;
      String stored;
      if (!ReadRecordRow(name, found, stored))
         return false;

      // An empty row is read as a missing one, and this engine starts at 1: no
      // write of this agent's leaves one empty, so it is a row somebody emptied,
      // and Snmp.md says what that costs. A value that is not a count is
      // damaged, and refused rather than read as none: starting again from 1
      // would reuse every count this engine has had.
      const std::string text = Trimmed(ToUtf8(stored));
      long long previous = 0;

      if (text.size() > 10)
         return false;

      for (size_t i = 0; i < text.size(); i++)
      {
         if (text[i] < '0' || text[i] > '9')
            return false;

         previous = previous * 10 + (text[i] - '0');
      }

      if (previous > SnmpEngine::MaxEngineBoots)
         return false;

      boots = previous < SnmpEngine::MaxEngineBoots ? previous + 1 : SnmpEngine::MaxEngineBoots;

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database)
         return false;

      const String written = FromAscii(std::to_string(boots));

      if (found)
      {
         SQLCommand update("update hm_settings set settingstring = @SETTINGSTRING where settingname = @SETTINGNAME");
         update.AddParameter("@SETTINGSTRING", written);
         update.AddParameter("@SETTINGNAME", name);

         if (!database->Execute(update))
            return false;
      }
      else
      {
         SQLCommand insert("insert into hm_settings (settingname, settingstring, settinginteger) values (@SETTINGNAME, @SETTINGSTRING, 0)");
         insert.AddParameter("@SETTINGNAME", name);
         insert.AddParameter("@SETTINGSTRING", written);

         if (!database->Execute(insert))
            return false;
      }

      // Believed only once the database holds it.
      String check;
      if (!ReadRecordRow(name, found, check) || !found)
         return false;

      return Trimmed(ToUtf8(check)) == std::to_string(boots);
   }

   bool
   SnmpAgent::OpenSocket_(const String &bindAddress, int port)
   {
      boost::asio::ip::udp::socket &udpSocket = transport_->socket;
      boost::system::error_code error;

      const std::string bind = Trimmed(ToUtf8(bindAddress));
      const unsigned short portNumber = (unsigned short) port;

      if (bind.empty())
      {
         // Every address: :: with IPV6_V6ONLY cleared, which is both families on
         // one socket, and 0.0.0.0 on a machine with no IPv6.
         udpSocket.open(boost::asio::ip::udp::v6(), error);

         if (!error)
            udpSocket.set_option(boost::asio::ip::v6_only(false), error);

         if (!error)
            ClaimExclusively_(udpSocket);

         if (!error)
            udpSocket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), portNumber), error);

         if (error)
         {
            boost::system::error_code ignored;
            udpSocket.close(ignored);

            error.clear();
            udpSocket.open(boost::asio::ip::udp::v4(), error);

            if (!error)
               ClaimExclusively_(udpSocket);

            if (!error)
               udpSocket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), portNumber), error);
         }
      }
      else
      {
         // Read as IPAddress reads an address on every platform.
         boost::asio::ip::address address;
         if (!IPAddress::Parse(AnsiString(bind.c_str()), address))
            error = boost::asio::error::invalid_argument;

         if (!error)
         {
            const boost::asio::ip::udp::endpoint endpoint(address, portNumber);
            udpSocket.open(endpoint.protocol(), error);

            if (!error)
               ClaimExclusively_(udpSocket);

            if (!error)
               udpSocket.bind(endpoint, error);
         }
      }

      if (error)
      {
         boost::system::error_code ignored;
         udpSocket.close(ignored);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7420, "SnmpAgent::Start",
            Formatter::Format(_T("The SNMP agent could not listen on UDP port {0} of {1}, so it is not answering. Another program may hold the port; on Linux a port below 1024 needs CAP_NET_BIND_SERVICE, which the packaged unit grants."),
               port, bind.empty() ? String(_T("every address")) : FromAscii(bind)),
            error);
         return false;
      }

      // A reply the socket's buffer cannot take is dropped rather than waited
      // for, as one the network lost would be.
      boost::system::error_code ignored;
      udpSocket.non_blocking(true, ignored);

      return true;
   }

   bool
   SnmpAgent::Start()
   {
      if (running_)
         return true;

      Configuration *configuration = Configuration::Instance();

      const int port = configuration->GetSnmpPort();
      const String bindAddress = configuration->GetSnmpBindAddress();
      const std::string allowedText = ToUtf8(configuration->GetSnmpAllowedAddresses());

      group_smtp_ = configuration->GetUseSMTP();
      group_pop3_ = configuration->GetUsePOP3();
      group_imap_ = configuration->GetUseIMAP();

      SnmpEngineSettings settings;
      if (!PrepareEngine_(settings))
         return false;

      transport_.reset(new UdpListener());

      std::string refused;
      if (!ParseAllowedList(allowedText, transport_->allowed, refused))
      {
         // Every surface refuses such a list when it is saved; one written by hand
         // into the database answers nobody rather than guessing.
         transport_->allowed.clear();
         LOG_APPLICATION(Formatter::Format(_T("SNMP: SnmpAllowedAddresses holds {0}, which is not an address or a range, so nobody is answered until the list is corrected."), FromAscii(refused)));
      }

      if (!OpenSocket_(bindAddress, port))
      {
         transport_.reset();
         SnmpUsm::Scrub(settings.authKey);
         SnmpUsm::Scrub(settings.privKey);
         SnmpUsm::Scrub(settings.community);
         return false;
      }

      const std::string engineIdText = SnmpBer::ToHex(settings.engineId);
      const std::string userText = settings.user;
      const bool v2c = settings.v2cEnabled && !settings.community.empty();
      const long long boots = settings.engineBoots;

      engine_.reset(new SnmpEngine(settings, this));

      SnmpUsm::Scrub(settings.authKey);
      SnmpUsm::Scrub(settings.privKey);
      SnmpUsm::Scrub(settings.community);

      start_tick_ = GetTickCount64();

      {
         std::lock_guard<std::mutex> guard(cache_mutex_);
         refresher_stop_ = false;
         queue_refresh_requested_ = true;
         queue_refresh_tick_ = 0;
      }

      running_ = true;

      worker_ = std::thread(&SnmpAgent::Run_, this);
      refresher_ = std::thread(&SnmpAgent::Run_Refresher_, this);

      // What is answered, in one line, and nothing secret in it.
      String bindText = bindAddress;
      bindText.Trim();

      String message = _T("SNMP: answering on UDP port ");
      message += FromAscii(std::to_string(port));
      message += _T(" of ");
      message += bindText.IsEmpty() ? String(_T("every address")) : bindText;
      message += _T(", engine ID ");
      message += FromAscii(engineIdText);
      message += _T(", boots ");
      message += FromAscii(std::to_string(boots));
      message += _T(". ");

      if (userText.empty())
         message += _T("No SNMPv3 user. ");
      else
      {
         message += _T("SNMPv3 user ");
         message += FromAscii(userText);
         message += _T(" at authPriv (HMAC-SHA-256, AES-128). ");
      }

      message += v2c ? _T("SNMPv2c on. ") : _T("SNMPv2c off. ");
      message += FromAscii(std::to_string(transport_->allowed.size()));
      message += _T(" address(es) or range(s) allowed; every other address is dropped unanswered.");

      LOG_APPLICATION(message);

      if (transport_->allowed.empty())
      {
         LOG_APPLICATION("SNMP: SnmpAllowedAddresses is empty, so every request is dropped unanswered until a manager's address is named there.");
      }

      return true;
   }

   void
   SnmpAgent::Stop()
   {
      if (!running_)
         return;

      running_ = false;

      // io_context::stop is the one call on it that is safe from another thread,
      // and it ends run() without waiting for a datagram.
      if (transport_)
         transport_->io.stop();

      {
         std::lock_guard<std::mutex> guard(cache_mutex_);
         refresher_stop_ = true;
      }

      refresher_wakeup_.notify_all();

      if (worker_.joinable())
         worker_.join();

      // The refresher may be inside a query, which nothing can cancel; the join
      // waits for it as MetricsServer's does, bounded by the database's own
      // timeout.
      if (refresher_.joinable())
         refresher_.join();

      if (transport_)
      {
         boost::system::error_code ignored;
         transport_->socket.close(ignored);
      }

      engine_.reset();
      transport_.reset();
   }

   void
   SnmpAgent::Run_()
   {
      // The top of a thread: an exception that left it would end the process.
      try
      {
         Receive_();
         transport_->io.run();
      }
      catch (...)
      {
         try
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7422, "SnmpAgent::Run_",
               "An exception escaped the SNMP agent's thread, which has stopped answering. A Reinitialize, or a restart of the service, starts it again.");
         }
         catch (...)
         {
         }
      }
   }

   void
   SnmpAgent::Receive_()
   {
      UdpListener *transport = transport_.get();

      transport->socket.async_receive_from(boost::asio::buffer(&transport->buffer[0], transport->buffer.size()), transport->sender,
         [this, transport](const boost::system::error_code &error, std::size_t bytes)
         {
            if (error == boost::asio::error::operation_aborted || !running_)
               return;

            if (error)
            {
               // A datagram longer than the buffer (Windows says so; POSIX fills
               // the buffer, and the engine refuses the length) is a message that
               // did not decode. Anything else - an ICMP port unreachable that
               // Windows reports on the next receive - is passed over, with a
               // pause if it keeps happening, so that a fault cannot spin a core.
               if (error == boost::asio::error::message_size && IsAllowed_())
                  engine_->CountOversizedDatagram();

               if (++transport->consecutive_errors > 16)
                  std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            else
            {
               transport->consecutive_errors = 0;
               Answer_(bytes);
            }

            Receive_();
         });
   }

   bool
   SnmpAgent::IsAllowed_() const
   {
      // Asked before a byte is decoded. An IPv4 manager reaching a dual-stack
      // socket arrives as ::ffff:a.b.c.d, and is judged as the address it is.
      const IPAddress peer = CountryBlocking::Unmapped(IPAddress(transport_->sender.address()));

      for (const SnmpAllowedRange &range : transport_->allowed)
      {
         if (RangeMatches(range, peer))
            return true;
      }

      return false;
   }

   void
   SnmpAgent::Answer_(size_t length)
   {
      // Not on the list: dropped here, before the engine, so that it moves no
      // counter, is never answered and costs nothing but this comparison. Nothing
      // is logged, here or for any datagram, so the port cannot fill the log.
      if (!IsAllowed_())
         return;

      engine_->SetElapsedMilliseconds(GetTickCount64() - start_tick_);

      SnmpBytes response;
      bool answer = false;

      try
      {
         answer = engine_->Process(&transport_->buffer[0], length, response);
      }
      catch (...)
      {
         answer = false;
      }

      if (!answer || response.empty())
         return;

      boost::system::error_code ignored;
      transport_->socket.send_to(boost::asio::buffer(&response[0], response.size()), transport_->sender, 0, ignored);
   }

   void
   SnmpAgent::Collect(std::vector<SnmpBinding> &bindings)
   {
      ServerStatus *status = ServerStatus::Instance();

      long long queueDepth = 0;
      long long oldestAge = 0;

      {
         std::lock_guard<std::mutex> guard(cache_mutex_);
         queueDepth = queue_depth_;
         oldestAge = queue_oldest_age_seconds_;
         queue_refresh_requested_ = true;
      }

      if (queueDepth < 0)
         queueDepth = 0;

      if (oldestAge < 0)
         oldestAge = 0;

      const unsigned long long uptimeHundredths = (unsigned long long) ((GetTickCount64() - start_tick_) / 10);
      const unsigned long long delivered = (unsigned long long) (unsigned int) status->GetNumberOfMessagesDelivered();

      // ---- MTA-MIB (RFC 2789): mtaTable, its one row, applIndex 1.
      const SnmpOid mtaEntry = SnmpBer::Oid("1.3.6.1.2.1.28.1.1");
      SnmpOid applIndex;
      applIndex.push_back(1);

      AddColumn(bindings, mtaEntry, 2, applIndex, SnmpValue::MakeGauge32((unsigned long long) queueDepth));
      AddColumn(bindings, mtaEntry, 3, applIndex, SnmpValue::MakeCounter32(delivered));

      // ---- mtaGroupTable: one row per protocol the server runs.
      const SnmpOid groupEntry = SnmpBer::Oid("1.3.6.1.2.1.28.2.1");
      const SnmpOid tcpProtocols = SnmpBer::Oid("1.3.6.1.2.1.27.4");

      for (const MtaGroup &group : MtaGroups)
      {
         const bool runs = group.index == 1 ? group_smtp_ : group.index == 2 ? group_pop3_ : group_imap_;
         if (!runs)
            continue;

         SnmpOid instance;
         instance.push_back(1);
         instance.push_back(group.index);

         if (group.index == 1)
         {
            AddColumn(bindings, groupEntry, 4, instance, SnmpValue::MakeGauge32((unsigned long long) queueDepth));
            AddColumn(bindings, groupEntry, 5, instance, SnmpValue::MakeCounter32(delivered));
            AddColumn(bindings, groupEntry, 12, instance, TimeInterval((unsigned long long) oldestAge * 100ULL));
         }

         int sessions = status->GetNumberOfSessions(group.sessionType);
         if (sessions < 0)
            sessions = 0;

         AddColumn(bindings, groupEntry, 13, instance, SnmpValue::MakeGauge32((unsigned long long) sessions));

         SnmpOid protocol = tcpProtocols;
         protocol.push_back(group.port);

         AddColumn(bindings, groupEntry, 24, instance, SnmpValue::MakeOid(protocol));
         AddColumn(bindings, groupEntry, 25, instance, SnmpValue::MakeText(group.name));
         AddColumn(bindings, groupEntry, 28, instance, SnmpValue::MakeText(group.description));
         AddColumn(bindings, groupEntry, 30, instance, TimeInterval(uptimeHundredths));
         AddColumn(bindings, groupEntry, 31, instance, SnmpValue::MakeInteger(0));
      }

      // ---- The enterprise arc: the other counters /metrics serves, by the
      // names docs/Snmp.md gives them. Not registered: see EnterpriseOid.
      SnmpOid statistics = EnterpriseOid();
      statistics.push_back(2);

      int missingFiles = status->GetMessageStoreMissingFiles();
      if (missingFiles < 0)
         missingFiles = 0;

      AddScalar(bindings, statistics, 1, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfProcessedMessages()));
      AddScalar(bindings, statistics, 2, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfMessagesDeferred()));
      AddScalar(bindings, statistics, 3, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfMessagesBounced()));
      AddScalar(bindings, statistics, 4, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfDetectedSpamMessages()));
      AddScalar(bindings, statistics, 5, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfRemovedViruses()));
      AddScalar(bindings, statistics, 6, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfAuthenticationsSucceeded()));
      AddScalar(bindings, statistics, 7, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfAuthenticationFailures()));
      AddScalar(bindings, statistics, 8, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfTlsHandshakesCompleted()));
      AddScalar(bindings, statistics, 9, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfTlsHandshakeFailures()));
      AddScalar(bindings, statistics, 10, SnmpValue::MakeCounter32(status->GetNumberOfSpamTrapHits()));
      AddScalar(bindings, statistics, 11, SnmpValue::MakeCounter32((unsigned int) status->GetNumberOfCountryRefusals()));
      AddScalar(bindings, statistics, 12, SnmpValue::MakeCounter32(status->GetDatabaseSlowQueriesCount()));
      AddScalar(bindings, statistics, 13, SnmpValue::MakeGauge32((unsigned long long) missingFiles));
      AddScalar(bindings, statistics, 14, SnmpValue::MakeInteger(status->GetState()));
      AddScalar(bindings, statistics, 15, SnmpValue::MakeGauge32((unsigned long long) queueDepth));
      AddScalar(bindings, statistics, 16, SnmpValue::MakeGauge32((unsigned long long) oldestAge));
   }

   void
   SnmpAgent::Run_Refresher_()
   {
      ULONGLONG failureSaidAt = 0; // when a failed refresh was last said

      for (;;)
      {
         try
         {
            RefreshDeliveryQueue_();
         }
         catch (...)
         {
            // The previous figures stay; a database that cannot answer is
            // reported by whatever else asked it, and anything else here - once
            // an hour at most, since a refresh is tried every ten seconds while a
            // manager polls.
            const ULONGLONG now = GetTickCount64();
            if (failureSaidAt == 0 || now - failureSaidAt >= 3600000)
            {
               failureSaidAt = now;
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7422, "SnmpAgent::Run_Refresher_",
                  _T("An exception escaped the SNMP agent's refresh of the delivery queue's two figures, which stay as they were until a refresh succeeds; the agent goes on answering. Said once an hour at most."));
            }
         }

         std::unique_lock<std::mutex> lock(cache_mutex_);

         if (refresher_stop_)
            return;

         const bool stopping = refresher_wakeup_.wait_for(lock, std::chrono::milliseconds((long long) RefresherTickMilliseconds),
            [this] { return refresher_stop_; });

         if (stopping)
            return;
      }
   }

   void
   SnmpAgent::RefreshDeliveryQueue_()
   {
      {
         std::lock_guard<std::mutex> guard(cache_mutex_);

         // Nothing has asked since the last refresh: a server with the agent on
         // and no manager polling it pays for no query.
         if (!queue_refresh_requested_)
            return;

         const ULONGLONG now = GetTickCount64();
         if (queue_refresh_tick_ != 0 && now - queue_refresh_tick_ < DeliveryQueueRefreshMilliseconds)
            return;

         queue_refresh_tick_ = now;
         queue_refresh_requested_ = false;
      }

      std::shared_ptr<DatabaseConnectionManager> database = Application::Instance()->GetDBManager();
      if (!database || !database->GetIsConnected())
         return;

      // The two figures /metrics serves as hmailserver_delivery_queue_messages
      // and hmailserver_delivery_queue_oldest_message_age_seconds, read the way
      // MetricsServer reads them, with no lock held.
      const long long depth = (long long) PersistentMessage::GetDeliveryQueueCount();
      long long oldestAge = 0;

      SQLCommand command("select min(messagecreatetime) as oldest from hm_messages where messagetype = @MESSAGETYPE");
      command.AddParameter("@MESSAGETYPE", Message::Delivering);

      std::shared_ptr<DALRecordset> recordset = database->OpenRecordset(command);

      if (recordset && !recordset->IsEOF())
      {
         const __int64 oldest = LocalTimeStampToUnixTime(recordset->GetStringValue("oldest"));

         if (oldest > 0)
         {
            const __int64 age = (__int64) time(nullptr) - oldest;
            oldestAge = age > 0 ? (long long) age : 0;
         }
      }

      std::lock_guard<std::mutex> guard(cache_mutex_);
      queue_depth_ = depth;
      queue_oldest_age_seconds_ = oldestAge;
   }
}
