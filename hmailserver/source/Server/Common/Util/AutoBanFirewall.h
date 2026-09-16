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
   //                        /usr/lib/hmailserver/autoban-hook, which keeps an
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

      // Make the firewall match the auto-ban ranges. Any thread, any time; the
      // reason is for the log.
      static void Synchronise(const String &why);

      // True if AutoBanNeverBan lists the address, alone or inside a block.
      static bool IsNeverBanned(const IPAddress &address);

      // "port,port,..." - every TCP port the server listens on, which is what a
      // block rule and the hook are scoped to. Empty when there are none.
      static String ListeningPorts();

      // The rule name as Windows shows it, and the group all of them share.
      static String RuleName(const String &address);
      static const wchar_t *RuleGroup();

      // Where the packaged Linux hook lives.
      static const wchar_t *PackagedHook();

   private:

      // Synchronise, handed to the asynchronous work queue. For a caller that
      // must not wait: a spam trap's ban is placed inside RCPT TO, and the
      // refusal that follows has to look exactly like the refusal an address
      // this server does not have gets - including how long it took. Falls back
      // to doing the work here when there is no queue; nothing is lost in any
      // case, because RemoveExpiredRecords reconciles once a minute regardless.
      static void SynchroniseLater_(const String &why);

      struct Ban
      {
         String address;
         int minutesLeft;
      };

      static std::vector<Ban> ActiveBans_();
      static void ReconcileWindowsRules_(bool enabled, const std::set<String> &desired, const String &ports);
      static void ReconcileHooks_(const std::vector<String> &commands, const std::vector<Ban> &bans, const String &ports);
      static bool RunHook_(const String &command, const String &verb, const String &address, int minutes, const String &ports, String &failure);
   };
}
