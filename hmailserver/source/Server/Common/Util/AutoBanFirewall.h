// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <set>
#include <vector>

namespace HM
{
   class IPAddress;

   // What an auto-ban does below the server.
   //
   // An auto-ban is a SecurityRange row that expires. The listeners consult it at
   // accept time, so a banned address is still accepted by the operating system,
   // handed to this process and dropped by it - a socket, a thread's attention and
   // a log line per attempt, and nothing else on the machine knows. This class
   // makes the ban visible beneath the server, under three [Settings] keys:
   //
   //   AutoBanFirewall=1    Windows: an inbound block rule per banned address in
   //                        Windows Defender Firewall, TCP only and scoped to the
   //                        ports this server listens on, named
   //                        "hMailServer auto-ban <address>" in a rule group of
   //                        their own so they can be told from everything else.
   //                        Linux: the hook the package installs at
   //                        /usr/lib/hmailserver/autoban-hook (below the build's
   //                        own prefix elsewhere: PackagedHook), which keeps an
   //                        nftables set with a timeout, or iptables where there
   //                        is no nft. The service needs CAP_NET_ADMIN for that,
   //                        which the unit does not grant by default; the hook
   //                        says so, precisely, when it cannot act.
   //   AutoBanCommand=...   Any program, run as
   //                           <command> ban <address> <minutes> <ports>
   //                           <command> unban <address>
   //                        for a cloud firewall, a router, a SIEM - whatever is
   //                        not the operating system's own. Both platforms, and
   //                        in addition to the above.
   //   AutoBanNeverBan=...  Addresses and CIDR blocks that are never auto-banned,
   //                        however many logons they fail. Once a ban can reach
   //                        the firewall, the administrator's own address being
   //                        banned stops being an inconvenience and becomes a
   //                        lockout from the whole machine's mail ports.
   //
   // Beside AutoBanNeverBan, the never-ban list by reverse name (a table,
   // hm_neverbannames; NeverBanNameList.h) spares a provider named by the
   // reverse names of its hosts. While it has rows, what Banned and its three
   // siblings do below the range - the log line, the alert count and the
   // reconciliation - waits for the address's reverse name to be looked up off
   // the protocol thread, and does not happen at all for an address it spares,
   // whose range is deleted instead; and a reconciliation run meanwhile, by
   // another ban or the expiry pass, leaves the waiting address's range out
   // (NeverBanNameList::IsPending).
   //
   // A ban AutoBanWidener has widened to its reverse-name neighbours (GitLab work
   // item #55; AutoBanWidenByReverseName, off by default) is one range of IPv4
   // addresses inside a /24. It is mirrored as one Windows rule named for the
   // run, "hMailServer auto-ban 203.0.113.10-203.0.113.22", whose
   // RemoteAddresses is that range; AutoBanCommand and the packaged hook are
   // told "ban" for each address of it, one at a time, as they always were.
   //
   // It is a reconciliation, not an event stream. The set of ranges is the truth;
   // Synchronise() reads it and makes the firewall match, so it does not matter
   // whether a ban was created by a logon failure, removed by expiry, deleted by
   // an administrator, or was already there when the service started. It runs at
   // start-up, whenever a ban is created, whenever a range is deleted, and once a
   // minute from RemoveExpiredRecords, which is what un-bans an expired address.
   // Because of that last one, a reconciliation that is deferred or dropped costs
   // at most a minute of the firewall lagging the ranges - which is what lets a
   // spam trap's ban hand the work to the asynchronous queue rather than do it
   // inside an SMTP command.
   class AutoBanFirewall
   {
   public:
      // The address failed enough logons to be banned and the range is saved.
      static void Banned(const IPAddress &address, int minutes, const String &username, int failures);

      // The same, for an address banned because it addressed a spam trap. A line
      // of its own because the reason is different and the packaged fail2ban
      // filter matches on it.
      static void BannedForSpamTrap(const IPAddress &address, int minutes, const String &trapAddress);

      // And for an address banned because its null sender named more than one
      // recipient (discussion #283). Its own line, for the same two readers.
      static void BannedForNullSender(const IPAddress &address, int minutes);

      // And for an address banned because it sent AUTH to an SMTP listener whose
      // authentication is Off or Honeypot (GitLab work item #54). Its own line,
      // carrying the listener's port, the mechanism named and the listener's mode
      // - never the argument - for the fail2ban filter and a person reading it.
      static void BannedForAuthOnDeliveryPort(const IPAddress &address, int minutes, int port,
                                              const String &mechanism, const String &mode);

      // Make the firewall match the auto-ban ranges. Any thread, any time; the
      // reason is for the log.
      static void Synchronise(const String &why);

      // The same, at a start (Application::StartServers, before its first
      // listener: the service's start, a reinitialise, the COM API's Start - and
      // never a command that starts the server short of its listeners, which
      // may run beside a running server and would take apart what that server
      // keeps below it): what the firewall holds of an earlier run is learned before
      // the active bans are announced, so that a ban that ended while the
      // service was stopped, or an un-ban still owed when it stopped, is lifted
      // and not left below the server for good. Windows: the rules of the
      // group are enumerated again, and every one with no ban behind it is
      // removed. Linux: the packaged hook is told "reset", which removes
      // everything it made, whether AutoBanFirewall is on or off, wherever it
      // is installed. AutoBanCommand, whose contract has no reset: told
      // "unban" for every address its ledger (a file in the temp folder) says
      // it was told "ban" for and that is not banned now.
      static void SynchroniseAtStart();

      // What one hook holds, as far as this process knows; defined in the .cpp.
      struct HookState;

#ifndef HM_PLATFORM_POSIX
      // Every rule of the group removed from Windows Defender Firewall, for
      // hMailServer.exe /Unregister - what the uninstaller runs - since a rule
      // is kept across restarts of Windows and nothing would remove it once the
      // server is gone: it would block its address on those ports for good,
      // whatever listens on them next. How many were removed, or -1 with the
      // error when the firewall could not be read or a rule not removed.
      static int RemoveAllWindowsRules(String &error);
#endif

      // True if AutoBanNeverBan lists the address, alone or inside a block.
      static bool IsNeverBanned(const IPAddress &address);

      // "port,port,..." - every TCP port the server listens on, which is what a
      // block rule and the hook are scoped to. Empty when there are none.
      static String ListeningPorts();

      // The rule name as Windows shows it, and the group all of them share.
      static String RuleName(const String &address);
      static const wchar_t *RuleGroup();

      // Where the Linux hook lives: where the build installed it, its
      // CMAKE_INSTALL_PREFIX's lib/hmailserver/autoban-hook - /usr/lib/... in
      // every package, the Nix store's in the flake's (CMakeLists.txt).
      static const wchar_t *PackagedHook();

   private:

      // Synchronise, handed to the asynchronous work queue. For a caller that
      // must not wait: a spam trap's ban is placed inside RCPT TO, and the
      // refusal that follows has to look exactly like the refusal an address
      // this server does not have gets - including how long it took. Falls back
      // to doing the work here when there is no queue; nothing is lost in any
      // case, because RemoveExpiredRecords reconciles once a minute regardless.
      static void SynchroniseLater_(const String &why);

      // IsNeverBanned's answer, under the never-ban list's lock; rejected is
      // given the entries a new value of the setting dropped, which IsNeverBanned
      // reports after the lock is let go.
      static bool IsNeverBanned_(const IPAddress &address, String &rejected);

      struct Ban
      {
         // The Windows rule's address: the banned address, or for a ban
         // AutoBanWidener widened, its run as "lower-upper".
         String address;

         // The addresses a hook is told about, one by one: the banned address,
         // or every address of the run.
         std::vector<String> members;

         int minutesLeft = 0;
      };

      // The auto-bans in force, or false when the IP ranges could not be read -
      // which is not "none": a pass that took it for none would lift every ban
      // below the server, and at a start tell AutoBanCommand to un-ban all its
      // ledger holds.
      static bool ActiveBans_(std::vector<Ban> &bans);
      static void ReconcileWindowsRules_(bool enabled, const std::set<String> &desired, const String &ports);
      // Every configured hook (commands: AutoBanCommand, the packaged hook) made
      // to match the bans; customCommand is AutoBanCommand, or empty, whose
      // ledger is read when starting and written when its account changed.
      static void ReconcileHooks_(const std::vector<String> &commands, const String &customCommand, const std::vector<Ban> &bans, const String &ports, bool starting);
      static void ReconcileHook_(const String &command, HookState &state, const std::vector<Ban> &bans, const std::set<String> &desired, const String &ports);
      // The packaged hook's reset, when one is owed: at a start, and when
      // AutoBanFirewall goes off (configured says whether it is on).
      static void ResetPackagedHook_(bool configured);
      static void LoadLedger_(const String &customCommand, const std::set<String> &desired);
      static void SaveLedger_(const String &customCommand);
      // timedOut says the program was stopped for running past the external
      // process timeout, rather than failing in any other way.
      static bool RunHook_(const String &command, const String &verb, const String &address, int minutes, const String &ports, String &failure, bool &timedOut);
   };
}
