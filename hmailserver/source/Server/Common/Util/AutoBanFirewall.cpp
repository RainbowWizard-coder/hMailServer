// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "AutoBanFirewall.h"

#include "AlertManager.h"
#include "AutoBanWidener.h"
#include "NeverBanNameList.h"
#include "../Application/Application.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"

#include "FileUtilities.h"
#include "ProcessLauncher.h"
#include "ServiceAccount.h"
#include "Unicode.h"
#include "VariantDateTime.h"
#include "Hashing/HashCreator.h"
#include "Parsing/StringParser.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Configuration.h"
#include "../BO/SecurityRanges.h"
#include "../BO/SecurityRange.h"
#include "../BO/TCPIPPorts.h"
#include "../BO/TCPIPPort.h"
#include "../TCPIP/IPAddress.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef HM_PLATFORM_POSIX
#include <netfw.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // What one hook holds, as far as this process knows.
   struct AutoBanFirewall::HookState
   {
      // The addresses it has been told "ban" for and not since "unban". At a
      // start (SynchroniseAtStart) this is forgotten, as a new process has it:
      // every active ban is announced again, and the hook is required to be
      // idempotent; the packaged one is. What it held before the start is
      // learned otherwise - the packaged hook is reset, and AutoBanCommand's
      // account is read back from its ledger (below).
      std::set<String> hooked;

      // Un-bans owed: addresses whose un-ban it was not run for to an answer -
      // skipped in a pass in which it had already run past the external process
      // timeout, or stopped for it itself. Each stays in hooked, so a later pass
      // asks again, until it has been run for it. The packaged hook's nftables
      // set lets a ban expire below the server on its own; with iptables, or a
      // command of the administrator's, nothing else ever lifts it. A pass whose
      // un-bans run past the timeout puts the next retry of what is owed further
      // off - the next pass, then a minute, doubling to an hour - so that a hook
      // which hangs for good costs one timeout an hour under the lock every ban
      // takes, not one a minute.
      std::set<String> owed;
      std::chrono::steady_clock::time_point retryAt;
      int retrySeconds = 0;
   };

   namespace
   {
      // One reconciliation at a time. A ban is created on a protocol thread, the
      // expiry pass runs on the scheduler's, an administrator's delete arrives on
      // the COM thread and start-up on the main one; two of them interleaving would
      // add a rule the other is about to remove.
      std::mutex g_mutex;

      // Windows: the addresses that have a rule of ours, learned once by
      // enumerating the firewall for the rule group and kept current afterwards, so
      // the once-a-minute pass costs nothing when nothing changed.
      std::set<String> g_windowsRules;
      bool g_windowsRulesKnown = false;
      bool g_windowsUnavailableReported = false;
      // That the service's account may not write firewall rules, said once; and
      // that rules a run as LocalSystem left are there, which it cannot remove,
      // said once too, though every start enumerates them again.
      bool g_windowsAccountReported = false;
      bool g_windowsLeftoversReported = false;

      // Hooks - the packaged one and AutoBanCommand - each by its command line,
      // with its own account of what it has been told (HookState, above), so
      // that a hook switched on, off or replaced while the service runs starts
      // from, or leaves behind, its own account and not the other's.
      std::map<String, AutoBanFirewall::HookState> g_hooks;
      bool g_hookFailureReported = false;
      const int UnbanRetryLongestSeconds = 3600;

      // Set by SynchroniseAtStart, taken by the reconciliation that follows: the
      // pass that learns what the hooks held before this start.
      bool g_startPending = false;

#ifdef HM_PLATFORM_POSIX
      // The packaged hook's "reset" - which removes everything it made, its
      // nftables table or its iptables chains - is owed at a start, before the
      // active bans are announced to it again, so that a ban that ended while
      // the service was stopped, or an un-ban still owed when it stopped, is not
      // left in the firewall for good (the iptables variant has no timeout);
      // and when AutoBanFirewall is switched off, so that nothing it made
      // outlives the switch. Run whether the setting is on or off, wherever the
      // hook is installed - as Windows removes its rules when the setting went
      // off while the service was stopped. A reset that fails is asked again a
      // minute later, then at doubling waits up to an hour.
      bool g_resetOwed = false;
      std::chrono::steady_clock::time_point g_resetRetryAt;
      int g_resetRetrySeconds = 0;
      bool g_resetFailureReported = false;
#endif

      // AutoBanCommand's ledger: what it was told "ban" for and not since
      // "unban", kept in a file in the temp folder (LedgerPath) so that a start
      // can tell it "unban" for every address it was left holding - a ban that
      // ended while the service was stopped, an un-ban still owed when it
      // stopped. Its contract has no "reset" for this, and the packaged hook's
      // reset is no help to a command of the administrator's. The command is
      // recorded by its SHA-256, not its text, since a command line can carry a
      // credential; a ledger written for another command is reported, never
      // run. Read at a start (g_ledgerKnown), written when the account changes.
      bool g_ledgerKnown = false;
      String g_ledgerCommand;
      std::set<String> g_ledgerWritten;
      bool g_ledgerFailureReported = false;

      const char *LedgerHeader = "hMailServer auto-ban command ledger 1";

      // The never-ban list, parsed from the setting's text and re-parsed only when
      // that text changes, so a list of a few hundred blocks costs nothing per
      // logon failure.
      String g_neverBanSource;
      bool g_neverBanParsed = false;
      std::vector<std::pair<boost::asio::ip::address, int>> g_neverBanBlocks;

      const wchar_t *RangeNamePrefix = L"Auto-ban: ";
      const wchar_t *RuleNamePrefix = L"hMailServer auto-ban ";

      // A reconciliation asked for by a thread that must not wait for it. See
      // SynchroniseLater below for the one caller and why it is the only one.
      class SynchroniseTask : public Task
      {
      public:
         SynchroniseTask(const String &why) : Task("AutoBanFirewall::Synchronise"), why_(why) { }

         virtual void DoWork() { AutoBanFirewall::Synchronise(why_); }

      private:
         String why_;
      };

      std::string Trim(const std::string &s)
      {
         size_t b = s.find_first_not_of(" \t\r\n");
         if (b == std::string::npos)
            return "";
         size_t e = s.find_last_not_of(" \t\r\n");
         return s.substr(b, e - b + 1);
      }

      bool Within(const boost::asio::ip::address &address, const boost::asio::ip::address &block, int prefix)
      {
         if (address.is_v4() != block.is_v4())
            return false;

         if (address.is_v4())
         {
            if (prefix >= 32)
               return address.to_v4() == block.to_v4();
            unsigned long mask = prefix <= 0 ? 0UL : (0xFFFFFFFFUL << (32 - prefix)) & 0xFFFFFFFFUL;
            return (address.to_v4().to_uint() & mask) == (block.to_v4().to_uint() & mask);
         }

         boost::asio::ip::address_v6::bytes_type a = address.to_v6().to_bytes();
         boost::asio::ip::address_v6::bytes_type b = block.to_v6().to_bytes();
         int bits = std::min(std::max(prefix, 0), 128);
         for (int i = 0; i < 16 && bits > 0; i++, bits -= 8)
         {
            unsigned char mask = bits >= 8 ? 0xFF : (unsigned char) (0xFF << (8 - bits));
            if ((a[i] & mask) != (b[i] & mask))
               return false;
         }
         return true;
      }

      // "1.2.3.4", "1.2.3.0/24", "2001:db8::/32": one entry of AutoBanNeverBan.
      bool ParseBlock(const std::string &entry, boost::asio::ip::address &address, int &prefix)
      {
         std::string text = Trim(entry);
         if (text.empty())
            return false;

         prefix = -1;
         size_t slash = text.find('/');
         if (slash != std::string::npos)
         {
            std::string bits = Trim(text.substr(slash + 1));
            text = Trim(text.substr(0, slash));
            if (bits.empty() || bits.find_first_not_of("0123456789") != std::string::npos)
               return false;
            prefix = atoi(bits.c_str());
         }

         boost::system::error_code ec;
         address = boost::asio::ip::make_address(text, ec);
         if (ec)
            return false;

         int widest = address.is_v4() ? 32 : 128;
         if (prefix < 0)
            prefix = widest;
         return prefix <= widest;
      }

      String LedgerPath()
      {
         return FileUtilities::Combine(IniFileSettings::Instance()->GetTempDirectory(), _T("autoban-command.ledger"));
      }

      // What a ledger records a command by: its SHA-256, in hex.
      String CommandDigest(const String &command)
      {
         HashCreator hasher(HashCreator::SHA256);
         return String(hasher.GenerateHashNoSalt(Unicode::ToUtf8(command), HashCreator::hex));
      }

      // An address as a hook is told it, or empty for a line that is not one:
      // a ledger line goes into a command line, so it is read back to the
      // character as an address and nothing else.
      String CanonicalAddress(const String &text)
      {
         AnsiString narrow;
         if (!Unicode::ToUtf8Exact(text, narrow))
            return String();

         boost::system::error_code ec;
         boost::asio::ip::address address = boost::asio::ip::make_address(std::string(narrow.c_str()), ec);
         if (ec)
            return String();

         return String(address.to_string().c_str());
      }

      // Up to twenty addresses, for a sentence, and how many more there were.
      String SomeAddresses(const std::set<String> &addresses)
      {
         String text;
         int written = 0;
         for (const String &address : addresses)
         {
            if (written == 20)
            {
               text += _T(" and ") + StringParser::IntToString((int) addresses.size() - written) + _T(" more");
               break;
            }
            if (written > 0)
               text += _T(", ");
            text += address;
            written++;
         }
         return text;
      }

      // A command the administrator replaced or cleared, or the one an earlier
      // run's ledger names, which is not the one configured, was left holding
      // these addresses. It is not run again - a command taken out of the
      // configuration may have been taken out because it misbehaves - so what
      // it blocked is named, for the administrator to lift where it put them.
      void ReportLeftBehind(const std::set<String> &addresses, const String &which)
      {
         if (addresses.empty())
            return;

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6523, "AutoBanFirewall::Synchronise",
            which + _T(" was due to ban ") + StringParser::IntToString((int) addresses.size()) +
            _T(" address(es) - told to, or passed over after it failed or ran past the external process timeout - and was not told to un-ban them: ") + SomeAddresses(addresses) +
            _T(". This server does not run a command it is no longer configured with, so whatever that command blocked of these stays blocked until it is lifted where the command put it."));
      }
   }

   String
   AutoBanFirewall::RuleName(const String &address)
   {
      return String(RuleNamePrefix) + address;
   }

   const wchar_t *
   AutoBanFirewall::RuleGroup()
   {
      return L"hMailServer auto-ban";
   }

   const wchar_t *
   AutoBanFirewall::PackagedHook()
   {
      // Where the build installs it, which CMakeLists.txt says: the packages'
      // /usr/lib/hmailserver/autoban-hook, or the same below another prefix (the
      // Nix store's, /usr/local). The Windows project, which has no hook, does
      // not define it.
#ifdef HM_AUTOBAN_HOOK
      return L"" HM_AUTOBAN_HOOK;
#else
      return L"/usr/lib/hmailserver/autoban-hook";
#endif
   }

   String
   AutoBanFirewall::ListeningPorts()
   {
      std::set<int> ports;

      std::shared_ptr<TCPIPPorts> tcpipPorts = Configuration::Instance()->GetTCPIPPorts();
      if (tcpipPorts)
      {
         // owned by this call: GetTCPIPPorts() loads a new collection for every caller
         for (std::shared_ptr<TCPIPPort> port : tcpipPorts->GetVector())
            ports.insert(port->GetPortNumber());
      }

      String result;
      for (int port : ports)
      {
         if (!result.IsEmpty())
            result += _T(",");
         result += StringParser::IntToString(port);
      }
      return result;
   }

   bool
   AutoBanFirewall::IsNeverBanned(const IPAddress &address)
   {
      std::lock_guard<std::mutex> guard(g_mutex);

      String source = IniFileSettings::Instance()->GetAutoBanNeverBan();
      if (!g_neverBanParsed || source != g_neverBanSource)
      {
         g_neverBanBlocks.clear();
         g_neverBanSource = source;
         g_neverBanParsed = true;

         // Comma, semicolon or whitespace between entries - whichever the
         // administrator reached for. Addresses are ASCII, so narrowing is exact.
         std::vector<std::string> entries;
         std::string current;
         for (wchar_t c : std::wstring(source.c_str()))
         {
            if (c == L',' || c == L';' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
            {
               if (!current.empty())
                  entries.push_back(current);
               current.clear();
            }
            else
               current += (char) c;
         }
         if (!current.empty())
            entries.push_back(current);

         String rejected;
         for (const std::string &entry : entries)
         {
            std::string text = Trim(entry);
            if (text.empty())
               continue;

            boost::asio::ip::address block;
            int prefix = 0;
            if (ParseBlock(text, block, prefix))
               g_neverBanBlocks.push_back(std::make_pair(block, prefix));
            else
            {
               if (!rejected.IsEmpty())
                  rejected += _T(", ");
               rejected += String(text.c_str());
            }
         }

         // A list that cannot be read is worse than no list, because the
         // administrator believes their address is protected. Reported once per
         // value of the setting, naming every entry that was dropped.
         if (!rejected.IsEmpty())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6524, "AutoBanFirewall::IsNeverBanned",
               "AutoBanNeverBan contains entries that are not an address or a CIDR block and were ignored: " + rejected +
               ". Addresses in those entries are NOT protected from auto-ban.");
         }
      }

      if (g_neverBanBlocks.empty())
         return false;

      boost::asio::ip::address asioAddress = address.GetAddress();
      for (const std::pair<boost::asio::ip::address, int> &block : g_neverBanBlocks)
      {
         if (Within(asioAddress, block.first, block.second))
            return true;
      }
      return false;
   }

   bool
   AutoBanFirewall::ActiveBans_(std::vector<Ban> &bans)
   {
      bans.clear();

      // A read that failed leaves the collection empty, and until 6.3.6 that was
      // taken for no bans at all: every hook was told "unban" for every address,
      // every Windows rule was removed, and a minute later all of it was put back
      // - a database that faltered once lifted every ban below the server.
      std::shared_ptr<SecurityRanges> ranges = std::shared_ptr<SecurityRanges>(new SecurityRanges());
      if (!ranges->Refresh())
         return false;

      DateTime now = DateTime::GetCurrentTime();

      // owned by this call: ranges is a local collection loaded above
      for (std::shared_ptr<SecurityRange> range : ranges->GetVector())
      {
         // An auto-ban is a single address, expiring, named by the code that made
         // it - or, once AutoBanWidener has widened it, a run of IPv4 addresses
         // inside one /24. Nothing else on the IP ranges page is ours to mirror: a
         // permanent block an administrator wrote belongs in the firewall only if
         // they put it there themselves, and a range of any other shape is not one
         // the auto-ban writes.
         if (!range->GetExpires())
            continue;
         if (!range->GetName().StartsWith(RangeNamePrefix))
            continue;

         Ban ban;
         if (!AutoBanWidener::MirrorShape(range->GetLowerIP(), range->GetUpperIP(), ban.address, ban.members))
            continue;

         DateTimeSpan left = range->GetExpiresTime() - now;
         double seconds = left.GetNumberOfSeconds();
         if (seconds <= 0)
            continue;

         ban.minutesLeft = (int) ((seconds + 59) / 60);
         bans.push_back(ban);
      }

      return true;
   }

   void
   AutoBanFirewall::Banned(const IPAddress &address, int minutes, const String &username, int failures)
   {
      const String text = String(address.ToString());

      // Everything a ban does below its range, run now - or, while the never-ban
      // list by reverse name has rows and nothing is known yet about this
      // address, once its reverse name has been looked up on the name lookup
      // queue, and not at all if that spares it (NeverBanNameList::VerifyBan).
      // An empty list runs it here, exactly as before.
      NeverBanNameList::VerifyBan(address, [address, text, minutes, username, failures]()
      {
         // The one line fail2ban and a person reading the log are both after: what
         // was banned, for how long, and why. Written whether or not anything below
         // the server is told, because the ban itself has happened.
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" is banned for ") + StringParser::IntToString(minutes) +
            _T(" minutes after ") + StringParser::IntToString(failures) + _T(" failed logons; last user name ") + username);

         // Counted, not alerted: one ban is ordinary and a hundred in an hour is
         // not, and only the second is worth telling anybody about. AlertTask asks
         // for the count once a minute and compares it with the rule's threshold.
         AlertManager::Instance()->NoteAutoBan();

         Synchronise(_T("ban"));

         // Widened to its reverse-name neighbours, in the background, when
         // AutoBanWidenByReverseName is on (AutoBanWidener.h): queued here, and
         // never looked up here.
         AutoBanWidener::Banned(address);
      });
   }

   void
   AutoBanFirewall::BannedForSpamTrap(const IPAddress &address, int minutes, const String &trapAddress)
   {
      const String text = String(address.ToString());

      // Run now, or after the never-ban list by reverse name has had its answer,
      // as Banned above says. Its lookup is never made on this thread.
      NeverBanNameList::VerifyBan(address, [address, text, minutes, trapAddress]()
      {
         // The same shape as the line above, with the reason a trap gives. Written
         // whether or not anything below the server is told, because the ban itself
         // has happened - and it is what the fail2ban filter counts.
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" is banned for ") + StringParser::IntToString(minutes) +
            _T(" minutes after addressing the spam trap ") + trapAddress);

         // Counted with the logon bans: a hundred bans in an hour is worth telling
         // somebody about however they were earned.
         AlertManager::Instance()->NoteAutoBan();

         // NOT on this thread. This one is inside RCPT TO, and the reply that
         // follows is supposed to be indistinguishable from the one an address
         // this server has never had gets. Synchronise takes a global lock, reads
         // every security range and may run a firewall API or spawn the packaged
         // hook - hundreds of milliseconds that an unknown address does not spend,
         // and a difference a harvester can time. The ban itself is already saved
         // and already refuses the address at accept; this only mirrors it below
         // the server, where a fraction of a second later is the same thing.
         SynchroniseLater_(_T("spam trap"));

         // Widened in the background, as Banned above says; only queued here, so
         // the RCPT TO reply waits for nothing.
         AutoBanWidener::Banned(address);
      });
   }

   void
   AutoBanFirewall::BannedForNullSender(const IPAddress &address, int minutes)
   {
      const String text = String(address.ToString());

      // Run now, or after the never-ban list by reverse name has had its answer,
      // as Banned above says.
      NeverBanNameList::VerifyBan(address, [address, text, minutes]()
      {
         // The same shape again, with this reason, for the fail2ban filter and for a
         // person reading the log.
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" is banned for ") + StringParser::IntToString(minutes) +
            _T(" minutes after a null sender named more than one recipient"));

         AlertManager::Instance()->NoteAutoBan();

         // Placed inside RCPT TO, as a trap's ban is, and for the same reason not
         // done on this thread: the refusal is answered at once and the firewall
         // catches up a fraction of a second later, or within the minute in any case.
         SynchroniseLater_(_T("null sender"));

         // Widened in the background, as Banned above says.
         AutoBanWidener::Banned(address);
      });
   }

   void
   AutoBanFirewall::BannedForAuthOnDeliveryPort(const IPAddress &address, int minutes, int port,
                                                const String &mechanism, const String &mode)
   {
      const String text = String(address.ToString());

      // Run now, or after the never-ban list by reverse name has had its answer,
      // as Banned above says.
      NeverBanNameList::VerifyBan(address, [address, text, minutes, port, mechanism, mode]()
      {
         // The one line the ban writes: the same shape as the three above, so the
         // fail2ban filter matches it as it matches them, with what was asked of
         // which listener. The mechanism is the word the caller made safe to log;
         // what followed it - a user name, a password - is never written.
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" is banned for ") + StringParser::IntToString(minutes) +
            _T(" minutes after AUTH on a delivery port (port ") + StringParser::IntToString(port) +
            _T(", mechanism ") + mechanism + _T(", listener ") + mode + _T(")"));

         AlertManager::Instance()->NoteAutoBan();

         // Placed inside the AUTH command, whose 502 is answered at once; the
         // firewall catches up a fraction of a second later, or within the minute.
         SynchroniseLater_(_T("AUTH on a delivery port"));

         // Widened in the background, as Banned above says.
         AutoBanWidener::Banned(address);
      });
   }

   void
   AutoBanFirewall::SynchroniseLater_(const String &why)
   {
      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();

      if (queue)
      {
         queue->AddTask(std::shared_ptr<Task>(new SynchroniseTask(why)));
         return;
      }

      // Before the queue exists or after it has stopped. Doing it here is the
      // safe answer - and the once-a-minute pass from RemoveExpiredRecords would
      // pick it up in any case, which is why nothing is lost either way.
      Synchronise(why);
   }

   void
   AutoBanFirewall::SynchroniseAtStart()
   {
      {
         std::lock_guard<std::mutex> guard(g_mutex);

         // What this process knew of the firewall is forgotten, as a new process
         // has it - a reinitialise is a start too, and a restore of a backup
         // brings other ranges with it - and learned again by the pass below:
         // Windows' rules by enumerating them, the packaged hook by its reset,
         // AutoBanCommand by its ledger.
         g_startPending = true;
         g_hooks.clear();
         g_ledgerKnown = false;
#ifndef HM_PLATFORM_POSIX
         g_windowsRules.clear();
         g_windowsRulesKnown = false;
#else
         g_resetOwed = true;
         g_resetRetrySeconds = 0;
         g_resetRetryAt = std::chrono::steady_clock::time_point();
#endif
      }

      Synchronise(_T("start-up"));
   }

   void
   AutoBanFirewall::Synchronise(const String &why)
   {
      std::lock_guard<std::mutex> guard(g_mutex);

      IniFileSettings *ini = IniFileSettings::Instance();
      bool firewall = ini->GetAutoBanFirewallEnabled();
      String command = ini->GetAutoBanCommand();

      // The bans first. When they cannot be read nothing below the server is
      // changed - no rule removed, no hook reset or told "unban" - and a start's
      // work is left to the next pass, at the latest the expiry pass a minute on.
      std::vector<Ban> bans;
      if (!ActiveBans_(bans))
      {
         LOG_DEBUG(_T("AutoBanFirewall::Synchronise - ") + why + _T(": the IP ranges could not be read; nothing below the server is changed in this pass."));
         return;
      }

      const bool starting = g_startPending;
      g_startPending = false;

#ifdef HM_PLATFORM_POSIX
      // After the bans are read and before they are announced: the hook holds
      // nothing after it (ResetPackagedHook_ forgets its account), so every ban
      // in force is told to it again below. At a start no listener of this
      // process is open yet (Application::StartServers), so no address the reset
      // lifts can reach one before it is told again; a reset that failed then and
      // is asked again by a later pass runs with the listeners open, and for that
      // moment the server refuses those addresses itself, at accept. A command
      // that starts the server short of its listeners never gets here at its
      // start, so it never resets what a server running beside it keeps.
      ResetPackagedHook_(firewall);
#endif

      // One Windows Defender Firewall rule per ban, named for its address or, for
      // a widened ban, for its run ("lower-upper").
      std::set<String> desired;
      for (const Ban &ban : bans)
         desired.insert(ban.address);

      String ports = ListeningPorts();

      LOG_DEBUG(_T("AutoBanFirewall::Synchronise - ") + why + _T(": ") + StringParser::IntToString((int) bans.size()) +
         _T(" active auto-ban(s); firewall ") + String(firewall ? _T("on") : _T("off")) +
         (command.IsEmpty() ? String(_T("; no command")) : String(_T("; command configured"))));

#ifndef HM_PLATFORM_POSIX
      ReconcileWindowsRules_(firewall, desired, ports);
#endif

      std::vector<String> commands;
      if (!command.IsEmpty())
         commands.push_back(command);
#ifdef HM_PLATFORM_POSIX
      if (firewall)
         commands.push_back(String(PackagedHook()));
#endif
      ReconcileHooks_(commands, command, bans, ports, starting);
   }

#ifdef HM_PLATFORM_POSIX
   void
   AutoBanFirewall::ResetPackagedHook_(bool configured)
   {
      const String hook(PackagedHook());

      // Switched off since the last pass: what it made goes with the switch.
      if (!configured && g_hooks.find(hook) != g_hooks.end())
      {
         g_hooks.erase(hook);
         g_resetOwed = true;
         g_resetRetrySeconds = 0;
         g_resetRetryAt = std::chrono::steady_clock::time_point();
      }

      if (!g_resetOwed)
         return;

      // Off, and not installed: nothing of it can be in the firewall, and a stock
      // build run from its tree has no hook to ask.
      if (!configured && !FileUtilities::Exists(hook))
      {
         g_resetOwed = false;
         return;
      }

      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      if (now < g_resetRetryAt)
         return;

      String failure;
      bool timedOut = false;
      if (RunHook_(hook, _T("reset"), String(), 0, String(), failure, timedOut))
      {
         g_resetOwed = false;
         g_resetRetrySeconds = 0;

         // It holds nothing now: every ban in force is told to it again, below.
         g_hooks.erase(hook);
         LOG_DEBUG(_T("AutoBanFirewall - the packaged hook was reset: what it blocked before is gone, and every ban in force is told to it again."));
         return;
      }

      g_resetRetrySeconds = g_resetRetrySeconds == 0 ? 60 : std::min(g_resetRetrySeconds * 2, UnbanRetryLongestSeconds);
      g_resetRetryAt = now + std::chrono::seconds(g_resetRetrySeconds);

      if (!g_resetFailureReported)
      {
         g_resetFailureReported = true;
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6523, "AutoBanFirewall::Synchronise",
            "The auto-ban hook could not be run to reset what it blocked before: " + failure +
            ". An address whose ban ended while the service was stopped, or while AutoBanFirewall was off, may stay blocked below the server until it can; it is asked again in " +
            StringParser::IntToString(g_resetRetrySeconds / 60) + " minute(s), and `" + hook + " reset` run as root does the same. Reported once; later failures are in the debug log.");
      }
      else
         LOG_DEBUG(_T("AutoBanFirewall - the packaged hook's reset failed: ") + failure);
   }
#endif

   void
   AutoBanFirewall::ReconcileHooks_(const std::vector<String> &commands, const String &customCommand, const std::vector<Ban> &bans, const String &ports, bool starting)
   {
      // A hook is told one address at a time, as it always was - a widened ban is
      // each address of its run - so that a program written for "ban <address>"
      // is never handed a range it cannot read.
      std::set<String> desired;
      for (const Ban &ban : bans)
         desired.insert(ban.members.begin(), ban.members.end());

      // A command this process told about bans that is no longer configured:
      // AutoBanCommand replaced or cleared. (The packaged hook switched off was
      // reset, and forgotten, before the bans were read.)
      for (std::map<String, HookState>::iterator hook = g_hooks.begin(); hook != g_hooks.end();)
      {
         if (std::find(commands.begin(), commands.end(), hook->first) != commands.end())
         {
            ++hook;
            continue;
         }

         ReportLeftBehind(hook->second.hooked, _T("AutoBanCommand, since replaced or cleared,"));
         hook = g_hooks.erase(hook);
      }

      // A start: what AutoBanCommand was told and not taken back, before it.
      if (starting)
         LoadLedger_(customCommand, desired);

      for (const String &command : commands)
         ReconcileHook_(command, g_hooks[command], bans, desired, ports);

      SaveLedger_(customCommand);
   }

   void
   AutoBanFirewall::ReconcileHook_(const String &command, HookState &hook, const std::vector<Ban> &bans, const std::set<String> &desired, const String &ports)
   {
      // A command that ran past the external process timeout once in this pass
      // is not run again in it. A widened ban is up to 65 addresses told one at
      // a time, under the lock every ban's announcement takes, and a hook that
      // hangs would otherwise hold it for 65 timeouts rather than one. A ban it
      // was not run for is treated as a failure of it: not asked again, as a
      // failed run never was. An un-ban it was not run for is owed, and asked
      // for again by a later pass (HookState::owed above): skipped here, it
      // would otherwise stay blocked below the server for good.
      bool hung = false;
      int skipped = 0;

      // A ban made again since its un-ban was owed is one the hook is right to
      // hold: nothing is owed for it any more.
      for (std::set<String>::iterator owed = hook.owed.begin(); owed != hook.owed.end();)
      {
         if (desired.find(*owed) != desired.end())
            owed = hook.owed.erase(owed);
         else
            ++owed;
      }

      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      const bool retryOwed = now >= hook.retryAt;
      bool unbanTimedOut = false;
      int deferred = 0;

      // Un-ban first: an address that has expired should stop being blocked
      // before a new one starts, and a hook that is failing altogether should
      // not be asked to add before it has been asked to remove.
      std::vector<String> gone;
      for (const String &address : hook.hooked)
      {
         if (desired.find(address) == desired.end())
            gone.push_back(address);
      }

      for (const String &address : gone)
      {
         // Owed from an earlier pass, and the wait after a hook that ran past
         // the timeout is not over: left for a later pass.
         if (!retryOwed && hook.owed.find(address) != hook.owed.end())
         {
            deferred++;
            continue;
         }

         // Not run for it, or stopped for the timeout: the un-ban is owed, the
         // address stays, and a later pass asks again (the hook is idempotent).
         // A run that answered with an exit code has said it could not; it is
         // reported, and not asked every minute for the same answer.
         bool owed = false;
         if (hung)
         {
            skipped++;
            owed = true;
         }
         else
         {
            String failure;
            bool timedOut = false;
            if (!RunHook_(command, _T("unban"), address, 0, ports, failure, timedOut))
            {
               if (timedOut)
               {
                  hung = true;
                  unbanTimedOut = true;
                  owed = true;
               }
               if (!g_hookFailureReported)
               {
                  g_hookFailureReported = true;
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6523, "AutoBanFirewall::Synchronise",
                     "The auto-ban hook could not be run to un-ban " + address + ": " + failure +
                     ". Whatever the hook blocks stays blocked until it can run. Reported once; later failures are in the debug log.");
               }
               else
                  LOG_DEBUG(_T("AutoBanFirewall - unban hook failed for ") + address + _T(": ") + failure);
            }
         }

         if (owed)
         {
            hook.owed.insert(address);
            continue;
         }

         hook.owed.erase(address);
         hook.hooked.erase(address);
      }

      if (unbanTimedOut)
      {
         hook.retryAt = now + std::chrono::seconds(hook.retrySeconds);
         hook.retrySeconds = hook.retrySeconds == 0 ? 60 : std::min(hook.retrySeconds * 2, UnbanRetryLongestSeconds);
      }
      else if (hook.owed.empty())
      {
         hook.retrySeconds = 0;
      }

      if (!hook.owed.empty())
      {
         LOG_DEBUG(_T("AutoBanFirewall - ") + StringParser::IntToString((int) hook.owed.size()) +
                   _T(" un-ban(s) owed to the hook, asked for again by a later pass") +
                   (deferred > 0 ? _T(" (") + StringParser::IntToString(deferred) + _T(" waiting after a timeout)") : String(_T(""))) + _T("."));
      }

      for (const Ban &ban : bans)
      {
         for (const String &member : ban.members)
         {
            if (hook.hooked.find(member) != hook.hooked.end())
               continue;

            if (hung)
            {
               skipped++;
            }
            else
            {
               String failure;
               bool timedOut = false;
               if (!RunHook_(command, _T("ban"), member, ban.minutesLeft, ports, failure, timedOut))
               {
                  if (timedOut)
                     hung = true;

                  if (!g_hookFailureReported)
                  {
                     g_hookFailureReported = true;
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6523, "AutoBanFirewall::Synchronise",
                        "The auto-ban hook could not be run to ban " + member + ": " + failure +
                        ". The address is still refused by the server itself; it is not blocked below it. Reported once; later failures are in the debug log.");
                  }
                  else
                     LOG_DEBUG(_T("AutoBanFirewall - ban hook failed for ") + member + _T(": ") + failure);
               }
            }
            hook.hooked.insert(member);
         }
      }

      if (skipped > 0)
      {
         LOG_DEBUG(_T("AutoBanFirewall - ") + StringParser::IntToString(skipped) +
                   _T(" hook run(s) skipped in this pass: the command ran past the external process timeout once already."));
      }
   }

   void
   AutoBanFirewall::LoadLedger_(const String &customCommand, const std::set<String> &desired)
   {
      g_ledgerKnown = true;
      g_ledgerCommand.clear();
      g_ledgerWritten.clear();

      const String path = LedgerPath();
      if (!FileUtilities::Exists(path))
         return;

      String text = FileUtilities::ReadCompleteTextFile(path);
      text.Replace(_T("\r"), _T(""));
      std::vector<String> lines = StringParser::SplitString(text, _T("\n"));

      // A file that is not a ledger of this shape is not one to act on, and is
      // written over by the next account (or removed with no command).
      if (lines.size() < 2 || lines[0] != String(LedgerHeader) || !lines[1].StartsWith(_T("command ")))
      {
         LOG_DEBUG(_T("AutoBanFirewall - ") + path + _T(" is not an auto-ban command ledger; it is not acted on."));
         g_ledgerCommand = _T("?");
         return;
      }

      g_ledgerCommand = lines[1].Mid(8);
      int unreadable = 0;
      for (size_t i = 2; i < lines.size(); i++)
      {
         if (lines[i].IsEmpty())
            continue;

         const String address = CanonicalAddress(lines[i]);
         if (address.IsEmpty() || address != lines[i])
         {
            unreadable++;
            continue;
         }
         g_ledgerWritten.insert(address);
      }

      if (unreadable > 0)
         LOG_DEBUG(_T("AutoBanFirewall - ") + StringParser::IntToString(unreadable) + _T(" line(s) of ") + path + _T(" are not an address and are passed over."));

      if (g_ledgerWritten.empty())
         return;

      if (!customCommand.IsEmpty() && g_ledgerCommand == CommandDigest(customCommand))
      {
         // Its own: an address it holds that is not banned now is told "unban"
         // by the pass that follows. One that is still banned is announced again
         // with every other, as at every start.
         HookState &hook = g_hooks[customCommand];
         int stale = 0;
         for (const String &address : g_ledgerWritten)
         {
            if (desired.find(address) == desired.end())
            {
               hook.hooked.insert(address);
               stale++;
            }
         }

         if (stale > 0)
            LOG_APPLICATION(_T("Auto-ban: AutoBanCommand was left holding ") + StringParser::IntToString(stale) +
                            _T(" address(es) whose ban ended while the service was stopped, or whose un-ban was still owed when it stopped; it is told to un-ban them now."));
         return;
      }

      ReportLeftBehind(g_ledgerWritten, _T("The AutoBanCommand of an earlier run, which is not the one configured now,"));
   }

   void
   AutoBanFirewall::SaveLedger_(const String &customCommand)
   {
      // Not read since the start: nothing is known of what the file holds, and
      // it is not written over.
      if (!g_ledgerKnown)
         return;

      const String path = LedgerPath();

      if (customCommand.IsEmpty())
      {
         if (g_ledgerCommand.IsEmpty() && g_ledgerWritten.empty())
            return;

         // Its addresses were named as it went (ReportLeftBehind). A file that
         // cannot be removed is said so by DeleteFile, and not asked again: the
         // next start reads it and names them once more.
         if (FileUtilities::Exists(path))
            FileUtilities::DeleteFile(path);
         g_ledgerCommand.clear();
         g_ledgerWritten.clear();
         return;
      }

      const String digest = CommandDigest(customCommand);
      const std::set<String> &hooked = g_hooks[customCommand].hooked;
      if (digest == g_ledgerCommand && hooked == g_ledgerWritten)
         return;

      AnsiString content = LedgerHeader;
      content += "\n";
      content += "command ";
      content += Unicode::ToUtf8(digest);
      content += "\n";
      for (const String &address : hooked)
      {
         content += Unicode::ToUtf8(address);
         content += "\n";
      }

      if (FileUtilities::WriteToFileAtomically(path, content))
      {
         g_ledgerCommand = digest;
         g_ledgerWritten = hooked;
         return;
      }

      if (!g_ledgerFailureReported)
      {
         g_ledgerFailureReported = true;
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6523, "AutoBanFirewall::Synchronise",
            "What AutoBanCommand has been told to ban could not be written to " + path +
            ". The command is told everything as before while the service runs, but an un-ban owed when the service stops, or a ban that ends while it is stopped, is not asked of it after the next start. Reported once.");
      }
   }

   bool
   AutoBanFirewall::RunHook_(const String &command, const String &verb, const String &address, int minutes, const String &ports, String &failure, bool &timedOut)
   {
      timedOut = false;

      String commandLine = command + _T(" ") + verb;
      if (!address.IsEmpty())
         commandLine += _T(" ") + address;
      if (verb == _T("ban"))
         commandLine += _T(" ") + StringParser::IntToString(minutes) + _T(" ") + (ports.IsEmpty() ? String(_T("-")) : ports);

      LOG_DEBUG(_T("AutoBanFirewall - running: ") + commandLine);

      ProcessLauncher launcher(commandLine);
      launcher.SetErrorLogTimeout(30000);

      unsigned int exitCode = 0;
      ProcessLauncher::FailureReason reason = ProcessLauncher::FailureReason::None;
      if (!launcher.Launch(exitCode, reason))
      {
         timedOut = reason == ProcessLauncher::FailureReason::TimedOut;
         failure = reason == ProcessLauncher::FailureReason::TimedOut
            ? _T("it ran for more than the external process timeout and was stopped")
            : _T("it could not be started (check the path, and that the service account may execute it)");
         return false;
      }

      if (exitCode != 0)
      {
         failure = _T("it exited with code ") + StringParser::IntToString((int) exitCode) + _T(" (command line: ") + commandLine + _T(")");
         return false;
      }

      return true;
   }

#ifndef HM_PLATFORM_POSIX

   namespace
   {
      // COM on whatever thread we are on. A thread that already initialised as
      // STA answers RPC_E_CHANGED_MODE and keeps its apartment; the calls below
      // work from either, and only an initialisation that succeeded is undone.
      struct ComScope
      {
         HRESULT hr;
         ComScope() : hr(CoInitializeEx(NULL, COINIT_MULTITHREADED)) {}
         ~ComScope() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
      };

      String Describe(HRESULT hr)
      {
         String text;
         text.Format(_T("0x%08X"), (unsigned int) hr);
         return text;
      }

      bool OpenRules(CComPtr<INetFwRules> &rules, String &error)
      {
         CComPtr<INetFwPolicy2> policy;
         HRESULT hr = policy.CoCreateInstance(__uuidof(NetFwPolicy2), NULL, CLSCTX_INPROC_SERVER);
         if (FAILED(hr) || !policy)
         {
            error = _T("Windows Defender Firewall's policy object could not be created (") + Describe(hr) + _T("). Is the Windows Defender Firewall service (mpssvc) running?");
            return false;
         }

         hr = policy->get_Rules(&rules);
         if (FAILED(hr) || !rules)
         {
            error = _T("the firewall's rule collection could not be opened (") + Describe(hr) + _T(")");
            return false;
         }
         return true;
      }

      // Every rule in our group, by the address in its name. Once, at first use.
      bool EnumerateOurRules(INetFwRules *rules, std::set<String> &addresses, String &error)
      {
         CComPtr<IUnknown> unknown;
         HRESULT hr = rules->get__NewEnum(&unknown);
         if (FAILED(hr) || !unknown)
         {
            error = _T("the firewall's rules could not be enumerated (") + Describe(hr) + _T(")");
            return false;
         }

         CComPtr<IEnumVARIANT> enumerator;
         hr = unknown->QueryInterface(IID_IEnumVARIANT, (void **) &enumerator);
         if (FAILED(hr) || !enumerator)
         {
            error = _T("the firewall's rule enumerator is not available (") + Describe(hr) + _T(")");
            return false;
         }

         String prefix(RuleNamePrefix);
         CComVariant item;
         ULONG fetched = 0;
         while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1)
         {
            if (item.vt == VT_DISPATCH && item.pdispVal)
            {
               CComPtr<INetFwRule> rule;
               if (SUCCEEDED(item.pdispVal->QueryInterface(__uuidof(INetFwRule), (void **) &rule)) && rule)
               {
                  CComBSTR grouping;
                  if (SUCCEEDED(rule->get_Grouping(&grouping)) && grouping && wcscmp(grouping, AutoBanFirewall::RuleGroup()) == 0)
                  {
                     CComBSTR name;
                     if (SUCCEEDED(rule->get_Name(&name)) && name)
                     {
                        String ruleName((const wchar_t *) name);
                        if (ruleName.StartsWith(prefix.c_str()))
                           addresses.insert(ruleName.Mid(prefix.GetLength()));
                     }
                  }
               }
            }
            item.Clear();
         }
         return true;
      }

      bool AddRule(INetFwRules *rules, const String &address, const String &ports, String &error)
      {
         CComPtr<INetFwRule> rule;
         HRESULT hr = rule.CoCreateInstance(__uuidof(NetFwRule), NULL, CLSCTX_INPROC_SERVER);
         if (FAILED(hr) || !rule)
         {
            error = _T("a firewall rule object could not be created (") + Describe(hr) + _T(")");
            return false;
         }

         String name = AutoBanFirewall::RuleName(address);
         rule->put_Name(CComBSTR(name.c_str()));
         // A widened ban's rule is named for its run, "lower-upper", which is also
         // how RemoteAddresses takes a range.
         const bool run = address.Find(_T("-")) >= 0;
         rule->put_Description(CComBSTR(run
            ? L"Created by hMailServer auto-ban for a ban widened to the neighbouring addresses whose reverse names share its domain. hMailServer removes it when the ban expires or when the matching IP range is deleted; delete the IP range rather than this rule."
            : L"Created by hMailServer auto-ban after repeated failed logons from this address. hMailServer removes it when the ban expires or when the matching IP range is deleted; delete the IP range rather than this rule."));
         rule->put_Grouping(CComBSTR(AutoBanFirewall::RuleGroup()));
         rule->put_Direction(NET_FW_RULE_DIR_IN);
         rule->put_Action(NET_FW_ACTION_BLOCK);
         rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP);
         if (!ports.IsEmpty())
            rule->put_LocalPorts(CComBSTR(ports.c_str()));
         rule->put_RemoteAddresses(CComBSTR(address.c_str()));
         rule->put_Profiles(NET_FW_PROFILE2_ALL);
         rule->put_Enabled(VARIANT_TRUE);

         hr = rules->Add(rule);
         if (FAILED(hr))
         {
            error = _T("the rule '") + name + _T("' could not be added (") + Describe(hr) + _T("). The service account needs to be an administrator or LocalSystem to write firewall rules.");
            return false;
         }
         return true;
      }

      bool RemoveRule(INetFwRules *rules, const String &address, String &error)
      {
         String name = AutoBanFirewall::RuleName(address);
         HRESULT hr = rules->Remove(CComBSTR(name.c_str()));
         if (FAILED(hr))
         {
            error = _T("the rule '") + name + _T("' could not be removed (") + Describe(hr) + _T(")");
            return false;
         }
         return true;
      }
   }

   void
   AutoBanFirewall::ReconcileWindowsRules_(bool enabled, const std::set<String> &desired, const String &ports)
   {
      // Nothing to do and nothing ever done: the common case on a server that has
      // never had the setting on, and it must not touch COM or the firewall.
      if (!enabled && g_windowsRulesKnown && g_windowsRules.empty())
         return;

      // Writing a firewall rule takes an administrator, and no narrower right
      // exists to grant: LocalSystem is one, and the virtual account a new
      // installation runs as is not (docs/ServiceAccount.md). Refused, said
      // once, and not attempted - a failed Add per ban and a failed Remove per
      // minute would say the same thing hundreds of times. What a service
      // running as LocalSystem left behind is still found, and named, so that it
      // is not a lockout nobody knows about.
      if (!ServiceAccount::ProcessIsAdministrator())
      {
         if (enabled && !g_windowsAccountReported)
         {
            g_windowsAccountReported = true;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7460, "AutoBanFirewall::Synchronise",
               "AutoBanFirewall is on, but this service runs as " + ServiceAccount::ProcessAccountName() +
               ", which cannot write Windows Defender Firewall rules - that takes an administrator. Banned addresses are refused by the server itself and are NOT blocked in the firewall. Run the service as LocalSystem, or reach a firewall through AutoBanCommand. Reported once.");
         }

         if (!g_windowsRulesKnown)
         {
            ComScope readScope;
            CComPtr<INetFwRules> readRules;
            String readError;
            if (OpenRules(readRules, readError) && EnumerateOurRules(readRules, g_windowsRules, readError))
            {
               g_windowsRulesKnown = true;
               if (!g_windowsRules.empty() && !g_windowsLeftoversReported)
               {
                  g_windowsLeftoversReported = true;
                  ErrorManager::Instance()->ReportError(ErrorManager::High, 7461, "AutoBanFirewall::Synchronise",
                     "Windows Defender Firewall holds " + StringParser::IntToString((int) g_windowsRules.size()) +
                     " auto-ban rule(s) of this server's, left by a run as LocalSystem, and this service runs as " + ServiceAccount::ProcessAccountName() +
                     ", which cannot remove them: each blocks its address until an administrator removes it (Get-NetFirewallRule -Group \"" + String(RuleGroup()) +
                     "\" | Remove-NetFirewallRule). Reported once.");
               }
            }
            else
            {
               // Unreadable: nothing is known, and nothing is asked again.
               g_windowsRulesKnown = true;
            }
         }
         return;
      }

      ComScope com;

      CComPtr<INetFwRules> rules;
      String error;
      if (!OpenRules(rules, error))
      {
         if (enabled && !g_windowsUnavailableReported)
         {
            g_windowsUnavailableReported = true;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6525, "AutoBanFirewall::Synchronise",
               "AutoBanFirewall is on but " + error + ". Banned addresses are refused by the server itself and are NOT blocked in the firewall. Reported once.");
         }
         return;
      }

      // The first pass learns what a previous run of the service left behind, so
      // that a rule for a ban that expired while the service was stopped - or every
      // rule, if the setting was turned off in between - is removed rather than
      // left to block an address for ever.
      if (!g_windowsRulesKnown)
      {
         if (!EnumerateOurRules(rules, g_windowsRules, error))
         {
            if (enabled && !g_windowsUnavailableReported)
            {
               g_windowsUnavailableReported = true;
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6525, "AutoBanFirewall::Synchronise",
                  "AutoBanFirewall is on but " + error + ". Reported once.");
            }
            return;
         }
         g_windowsRulesKnown = true;
         if (!g_windowsRules.empty())
            LOG_DEBUG(_T("AutoBanFirewall - ") + StringParser::IntToString((int) g_windowsRules.size()) + _T(" rule(s) of ours found in Windows Defender Firewall"));
      }

      const std::set<String> &target = enabled ? desired : std::set<String>();

      std::vector<String> toRemove;
      for (const String &address : g_windowsRules)
      {
         if (target.find(address) == target.end())
            toRemove.push_back(address);
      }

      for (const String &address : toRemove)
      {
         if (RemoveRule(rules, address, error))
         {
            g_windowsRules.erase(address);
            LOG_APPLICATION(_T("Auto-ban: the Windows Defender Firewall rule for ") + address + _T(" was removed"));
         }
         else
         {
            // Left in the set so that it is tried again next minute; a rule that
            // cannot be removed is a lockout that outlives the ban, which is the
            // one outcome this class exists to prevent, so it is reported each time.
            ErrorManager::Instance()->ReportError(ErrorManager::High, 6522, "AutoBanFirewall::Synchronise",
               "The auto-ban for " + address + " has ended but " + error + ". The address stays blocked in the firewall until this succeeds; remove the rule by hand if it does not.");
         }
      }

      for (const String &address : target)
      {
         if (g_windowsRules.find(address) != g_windowsRules.end())
            continue;

         if (AddRule(rules, address, ports, error))
         {
            g_windowsRules.insert(address);
            LOG_APPLICATION(_T("Auto-ban: ") + address + _T(" is blocked in Windows Defender Firewall on TCP port(s) ") + (ports.IsEmpty() ? String(_T("all")) : ports));
         }
         else
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6521, "AutoBanFirewall::Synchronise",
               "The auto-ban for " + address + " is in force in the server but " + error + " The address is refused by the server itself and is NOT blocked in the firewall.");
         }
      }
   }

   int
   AutoBanFirewall::RemoveAllWindowsRules(String &error)
   {
      std::lock_guard<std::mutex> guard(g_mutex);

      ComScope com;

      CComPtr<INetFwRules> rules;
      std::set<String> addresses;
      if (!OpenRules(rules, error) || !EnumerateOurRules(rules, addresses, error))
         return -1;

      int left = 0;
      for (const String &address : addresses)
      {
         String removeError;
         if (RemoveRule(rules, address, removeError))
            g_windowsRules.erase(address);
         else
         {
            left++;
            error = removeError;
         }
      }

      return left == 0 ? (int) addresses.size() : -1;
   }

#endif
}
