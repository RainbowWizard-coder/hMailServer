// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See ReputationWatch.h.

#include "StdAfx.h"

#include "ReputationWatch.h"

#include "AlertManager.h"

#include "../Application/ObjectCache.h"
#include "../BO/Rule.h"
#include "../BO/RuleAction.h"
#include "../BO/RuleActions.h"
#include "../BO/Rules.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../TCPIP/DNSRecord.h"
#include "../TCPIP/DNSResolverWinApi.h"
#include "../TCPIP/IPAddress.h"

#include "../../SMTP/BLCheck.h"
#include "../../SMTP/SMTPConfiguration.h"

#include <algorithm>
#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const char *ReputationWatch::DefaultLists =
      "ipv4:zen.spamhaus.org, ipv4:bl.spamcop.net, ipv4:psbl.surriel.com, ipv4:bl.mailspike.net";

   namespace
   {
      // The two record types, by the numbers RFC 1035 assigns them - which are
      // what DNSResolverWinApi::Query takes on both platforms, and what windns.h
      // spells DNS_TYPE_A and DNS_TYPE_TEXT. Written out rather than included,
      // because the Windows header is not there on the POSIX build.
      const int DnsTypeA = 1;
      const int DnsTypeTxt = 16;

      // One check at a time. The daily task and a check asked for over REST or COM
      // write the same rows, and a check makes lookups that take seconds.
      boost::recursive_mutex &Mutex_()
      {
         static boost::recursive_mutex mutex;
         return mutex;
      }

      // When this process last ran a check. The rows carry the same moment, but a
      // check that found nothing to look up writes none, and without this the
      // watch would decide it was due again at the next wake.
      __int64 last_run_ = 0;

      // The characters a list, an address and an answer are made of. Anything a
      // DNS server sends that is not printable ASCII is a byte this server has no
      // business putting into a database column, a JSON document and a page.
      AnsiString Printable(const AnsiString &value)
      {
         AnsiString result;
         result.reserve(value.GetLength());

         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) value[i];

            result += (character >= 0x20 && character <= 0x7E) ? (char) character : ' ';
         }

         result.Trim();
         return result;
      }

      // What a pair the check ran out of time for is reported as. Never "not
      // listed": nothing was asked about it.
      AnsiString OutOfTime()
      {
         return AnsiString(Formatter::Format("The check reached its limit of {0} seconds before this lookup, so nothing was asked. A resolver that is not answering is the usual reason; DNSQueryTimeout bounds each query.",
                                             (int) ReputationWatch::MaximumCheckSeconds));
      }

      AnsiString Shorten(const AnsiString &value, int maximum)
      {
         if (value.GetLength() <= maximum)
            return value;

         if (maximum <= 3)
            return value.Mid(0, maximum);

         return value.Mid(0, maximum - 3) + "...";
      }

      // The entries of a setting: separated by a comma, a semicolon or any
      // whitespace, so that a list typed on one line and a list typed on several
      // are the same list.
      std::vector<AnsiString> Tokens(const AnsiString &text)
      {
         std::vector<AnsiString> tokens;
         AnsiString current;

         for (int i = 0; i < text.GetLength(); i++)
         {
            const char character = text[i];

            if (character == ',' || character == ';' || character == ' ' ||
                character == '\t' || character == '\r' || character == '\n')
            {
               if (!current.IsEmpty())
                  tokens.push_back(current);

               current = "";
               continue;
            }

            current += character;
         }

         if (!current.IsEmpty())
            tokens.push_back(current);

         return tokens;
      }

      AnsiString JsonEscape(const AnsiString &value)
      {
         AnsiString result;
         result.reserve(value.GetLength() + 8);

         for (int i = 0; i < value.GetLength(); i++)
         {
            const char character = value[i];

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
               if ((unsigned char) character >= 0x20)
                  result += character;
               break;
            }
         }

         return result;
      }

      AnsiString JsonString(const AnsiString &value)
      {
         return "\"" + JsonEscape(value) + "\"";
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString TimeText(__int64 seconds)
      {
         if (seconds <= 0)
            return "";

         time_t value = (time_t) seconds;

         tm utc = {};
         gmtime_s(&utc, &value);

         AnsiString text;
         text.Format("%04d-%02d-%02d %02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);

         return text;
      }

      bool IsNumber(const AnsiString &value, int &number)
      {
         if (value.IsEmpty() || value.GetLength() > 3)
            return false;

         number = 0;

         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value[i] < '0' || value[i] > '9')
               return false;

            number = number * 10 + (value[i] - '0');
         }

         return number <= 255;
      }

      std::vector<AnsiString> SplitOn(const AnsiString &value, char separator)
      {
         std::vector<AnsiString> parts;
         AnsiString current;

         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value[i] == separator)
            {
               parts.push_back(current);
               current = "";
               continue;
            }

            current += value[i];
         }

         parts.push_back(current);
         return parts;
      }

      // A zone name a query can be built under: labels of letters, digits,
      // hyphens and underscores. Short enough that the longest name built from it
      // - an IPv6 address is 63 characters of nibbles - stays inside the 253 a
      // name may have.
      bool IsZone(const AnsiString &zone)
      {
         if (zone.GetLength() < 3 || zone.GetLength() > 180)
            return false;

         std::vector<AnsiString> labels = SplitOn(zone, '.');
         if (labels.size() < 2)
            return false;

         for (size_t i = 0; i < labels.size(); i++)
         {
            const AnsiString &label = labels[i];

            if (label.IsEmpty() || label.GetLength() > 63)
               return false;

            if (label[0] == '-' || label[label.GetLength() - 1] == '-')
               return false;

            for (int c = 0; c < label.GetLength(); c++)
            {
               const char character = label[c];

               const bool allowed = (character >= 'a' && character <= 'z') ||
                                    (character >= '0' && character <= '9') ||
                                    character == '-' || character == '_';

               if (!allowed)
                  return false;
            }
         }

         return true;
      }

      // One listing code: 127.0.0.2, 127.0.0.4-7 or 127.0.0.*, in any octet.
      bool IsCode(const AnsiString &code)
      {
         std::vector<AnsiString> parts = SplitOn(code, '.');
         if (parts.size() != 4)
            return false;

         for (size_t i = 0; i < parts.size(); i++)
         {
            const AnsiString &part = parts[i];

            if (part == "*")
               continue;

            int value = 0;
            const int hyphen = part.Find("-");

            if (hyphen > 0)
            {
               int low = 0;
               int high = 0;

               if (!IsNumber(part.Mid(0, hyphen), low) || !IsNumber(part.Mid(hyphen + 1), high) || low > high)
                  return false;

               continue;
            }

            if (!IsNumber(part, value))
               return false;
         }

         return true;
      }

      bool MatchesOneCode(const AnsiString &answer, const AnsiString &code)
      {
         std::vector<AnsiString> left = SplitOn(answer, '.');
         std::vector<AnsiString> right = SplitOn(code, '.');

         if (left.size() != 4 || right.size() != 4)
            return false;

         for (size_t i = 0; i < 4; i++)
         {
            if (right[i] == "*")
               continue;

            int value = 0;
            if (!IsNumber(left[i], value))
               return false;

            const int hyphen = right[i].Find("-");

            if (hyphen > 0)
            {
               int low = 0;
               int high = 0;

               if (!IsNumber(right[i].Mid(0, hyphen), low) || !IsNumber(right[i].Mid(hyphen + 1), high))
                  return false;

               if (value < low || value > high)
                  return false;

               continue;
            }

            int wanted = 0;
            if (!IsNumber(right[i], wanted) || wanted != value)
               return false;
         }

         return true;
      }

      // 127.0.0.0/8, which is where every block list answers.
      bool IsLoopbackAnswer(const AnsiString &answer)
      {
         std::vector<AnsiString> parts = SplitOn(answer, '.');
         if (parts.size() != 4)
            return false;

         int first = 0;
         if (!IsNumber(parts[0], first) || first != 127)
            return false;

         for (size_t i = 1; i < parts.size(); i++)
         {
            int value = 0;
            if (!IsNumber(parts[i], value))
               return false;
         }

         return true;
      }

      // 127.255.255.0/24, which is where the lists answer with an error rather
      // than a verdict.
      bool IsErrorAnswer(const AnsiString &answer)
      {
         if (!IsLoopbackAnswer(answer))
            return false;

         std::vector<AnsiString> parts = SplitOn(answer, '.');

         int second = 0;
         int third = 0;

         return IsNumber(parts[1], second) && second == 255 && IsNumber(parts[2], third) && third == 255;
      }

      AnsiString Join(const std::vector<AnsiString> &values, const char *separator)
      {
         AnsiString result;

         for (size_t i = 0; i < values.size(); i++)
         {
            if (i > 0)
               result += separator;

            result += values[i];
         }

         return result;
      }
   }

   // --------------------------------------------------------------------------
   // The settings
   // --------------------------------------------------------------------------

   bool
   ReputationWatch::ParseLists(const String &text, std::vector<List> &lists, String &refusal)
   {
      lists.clear();
      refusal = _T("");

      const std::vector<AnsiString> tokens = Tokens(AnsiString(text));

      for (size_t i = 0; i < tokens.size(); i++)
      {
         AnsiString entry = tokens[i];
         entry.MakeLower();

         List list;

         if (entry.StartsWith("ipv4:"))
         {
            list.family = FamilyIPv4;
            entry = entry.Mid(5);
         }
         else if (entry.StartsWith("ipv6:"))
         {
            list.family = FamilyIPv6;
            entry = entry.Mid(5);
         }

         const int equals = entry.Find("=");

         if (equals >= 0)
         {
            list.codes = entry.Mid(equals + 1);
            entry = entry.Mid(0, equals);
         }

         while (entry.GetLength() > 0 && entry[entry.GetLength() - 1] == '.')
            entry = entry.Mid(0, entry.GetLength() - 1);

         list.zone = entry;

         if (!IsZone(list.zone))
         {
            if (refusal.IsEmpty())
            {
               refusal = Formatter::Format("\"{0}\" is not a list: one is written as a DNS zone such as bl.spamcop.net, "
                                           "with ipv4: or ipv6: in front of it when the list covers only that family, and "
                                           "=127.0.0.2|127.0.0.4-7 after it when only some answers mean listed.",
                                           String(Shorten(tokens[i], 60)));
            }

            continue;
         }

         if (!list.codes.IsEmpty())
         {
            bool ok = true;

            const std::vector<AnsiString> codes = SplitOn(list.codes, '|');

            for (size_t c = 0; c < codes.size(); c++)
            {
               if (!IsCode(codes[c]))
                  ok = false;
            }

            if (!ok)
            {
               if (refusal.IsEmpty())
               {
                  refusal = Formatter::Format("\"{0}\" does not name the answers that mean listed: they are addresses in "
                                              "127.0.0.0/8 separated by |, each of them exact (127.0.0.2), a range in the "
                                              "last octet (127.0.0.4-7) or a wildcard (127.0.0.*).",
                                              String(Shorten(tokens[i], 60)));
               }

               continue;
            }
         }

         bool duplicate = false;

         for (size_t existing = 0; existing < lists.size(); existing++)
         {
            if (lists[existing].zone == list.zone)
               duplicate = true;
         }

         if (duplicate)
            continue;

         if ((int) lists.size() >= MaximumLists)
         {
            if (refusal.IsEmpty())
            {
               refusal = Formatter::Format("More than {0} lists are named; the ones after the {0}th are not checked. Each "
                                           "list costs one query per address per day, plus two for the list itself.",
                                           MaximumLists);
            }

            break;
         }

         lists.push_back(list);
      }

      return refusal.IsEmpty();
   }

   bool
   ReputationWatch::ParseAddresses(const String &text, std::vector<AnsiString> &addresses, String &refusal)
   {
      addresses.clear();
      refusal = _T("");

      const std::vector<AnsiString> tokens = Tokens(AnsiString(text));

      for (size_t i = 0; i < tokens.size(); i++)
      {
         IPAddress parsed;

         // false: an address that does not parse is reported here in a sentence
         // the administrator reads, not in the debug log.
         if (!parsed.TryParse(tokens[i], false))
         {
            if (refusal.IsEmpty())
            {
               refusal = Formatter::Format("\"{0}\" is not an IP address. ReputationWatchAddresses holds the addresses "
                                           "other servers see this one as - the public address in front of it when it is "
                                           "behind NAT - and nothing else; a host name is not one of them, because the "
                                           "address a name resolves to is not necessarily the address mail leaves from.",
                                           String(Shorten(tokens[i], 60)));
            }

            continue;
         }

         const AnsiString normalised = parsed.ToString();

         bool duplicate = false;

         for (size_t existing = 0; existing < addresses.size(); existing++)
         {
            if (addresses[existing] == normalised)
               duplicate = true;
         }

         if (duplicate)
            continue;

         if ((int) addresses.size() >= MaximumAddresses)
         {
            if (refusal.IsEmpty())
            {
               refusal = Formatter::Format("More than {0} addresses are named; the ones after the {0}th are not checked.",
                                           MaximumAddresses);
            }

            break;
         }

         addresses.push_back(normalised);
      }

      return refusal.IsEmpty();
   }

   String
   ReputationWatch::ValidateLists(const String &text)
   {
      std::vector<List> lists;
      String refusal;

      ParseLists(text, lists, refusal);

      return refusal;
   }

   String
   ReputationWatch::ValidateAddresses(const String &text)
   {
      std::vector<AnsiString> addresses;
      String refusal;

      ParseAddresses(text, addresses, refusal);

      return refusal;
   }

   // --------------------------------------------------------------------------
   // The addresses
   // --------------------------------------------------------------------------

   AnsiString
   ReputationWatch::NotCheckedReason(const AnsiString &address)
   {
      IPAddress parsed;
      if (!parsed.TryParse(address, false))
         return "invalid";

      if (parsed.GetType() == IPAddress::IPV4)
      {
         std::vector<AnsiString> parts = SplitOn(parsed.ToString(), '.');
         if (parts.size() != 4)
            return "invalid";

         int first = 0;
         int second = 0;

         if (!IsNumber(parts[0], first) || !IsNumber(parts[1], second))
            return "invalid";

         if (first == 0)
            return "unspecified";
         if (first == 127)
            return "loopback";
         if (first == 10)
            return "private";
         if (first == 172 && second >= 16 && second <= 31)
            return "private";
         if (first == 192 && second == 168)
            return "private";
         if (first == 169 && second == 254)
            return "link_local";
         if (first == 100 && second >= 64 && second <= 127)
            return "shared";
         if (first >= 224 && first <= 239)
            return "multicast";
         if (first >= 240)
            return "reserved";

         return "";
      }

      // The long form is the sixteen bytes as eight groups of four hexadecimal
      // digits, so the leading bits each rule is about can be read off it.
      const AnsiString expanded = parsed.ToLongString();
      std::vector<AnsiString> groups = SplitOn(expanded, ':');

      if (groups.size() != 8)
         return "invalid";

      unsigned int values[8] = {};

      for (size_t i = 0; i < groups.size(); i++)
      {
         unsigned int value = 0;

         for (int c = 0; c < groups[i].GetLength(); c++)
         {
            const char character = groups[i][c];
            unsigned int digit = 0;

            if (character >= '0' && character <= '9')
               digit = (unsigned int) (character - '0');
            else if (character >= 'a' && character <= 'f')
               digit = (unsigned int) (character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
               digit = (unsigned int) (character - 'A' + 10);
            else
               return "invalid";

            value = value * 16 + digit;
         }

         values[i] = value;
      }

      bool allZero = true;
      for (size_t i = 0; i < 8; i++)
      {
         if (values[i] != 0)
            allZero = false;
      }

      if (allZero)
         return "unspecified";

      if (values[0] == 0 && values[1] == 0 && values[2] == 0 && values[3] == 0 &&
          values[4] == 0 && values[5] == 0 && values[6] == 0 && values[7] == 1)
         return "loopback";

      // ::ffff:a.b.c.d - an IPv4 address wearing an IPv6 spelling. The lists
      // answer for the IPv4 address, so that is what belongs in the setting.
      if (values[0] == 0 && values[1] == 0 && values[2] == 0 && values[3] == 0 &&
          values[4] == 0 && values[5] == 0xffff)
         return "mapped";

      if ((values[0] & 0xffc0) == 0xfe80)
         return "link_local";

      if ((values[0] & 0xfe00) == 0xfc00)
         return "private";

      if ((values[0] & 0xff00) == 0xff00)
         return "multicast";

      return "";
   }

   AnsiString
   ReputationWatch::NotCheckedSentence(const AnsiString &reason)
   {
      if (reason == "private")
         return "A private address. It is not what another server sees, and asking a block list about it would tell the list what this network looks like inside.";
      if (reason == "loopback")
         return "A loopback address. It never leaves this machine.";
      if (reason == "link_local")
         return "A link-local address. It never leaves this network segment.";
      if (reason == "shared")
         return "An address in the carrier-grade NAT range (100.64.0.0/10). Mail leaves through the carrier's public address, which is what belongs in ReputationWatchAddresses.";
      if (reason == "unspecified")
         return "The unspecified address. Binding it means \"any address\", so it says nothing about what this server is seen as.";
      if (reason == "multicast")
         return "A multicast address. No mail is ever sent from one.";
      if (reason == "reserved")
         return "A reserved address. No mail is ever sent from one.";
      if (reason == "mapped")
         return "An IPv4 address written as IPv6. Name the IPv4 address itself; that is what the lists answer for.";
      if (reason == "too_many")
      {
         return AnsiString(Formatter::Format("More outbound addresses were found than the watch checks ({0}), and this one is past that. Take one out of ReputationWatchAddresses, or out of a rule that binds it, so that the ones that matter are checked.",
                                             (int) MaximumAddresses));
      }
      if (reason == "invalid")
         return "Not an IP address.";

      return "";
   }

   void
   ReputationWatch::CollectAddresses(std::vector<Address> &addresses, std::vector<Skipped> &skipped)
   {
      addresses.clear();
      skipped.clear();

      std::vector<AnsiString> raw;
      std::vector<AnsiString> sources;

      // The three places an outbound address comes from, in the order the report
      // shows them.
      const String bind = Configuration::Instance()->GetSMTPConfiguration()->GetSMTPDeliveryBindToIP();

      if (!bind.IsEmpty())
      {
         raw.push_back(AnsiString(bind));
         sources.push_back("bind");
      }

      std::shared_ptr<Rules> rules = ObjectCache::Instance()->GetGlobalRules();

      if (rules)
      {
         std::vector<std::shared_ptr<Rule> > ruleList = rules->GetSnapshot();

         for (size_t r = 0; r < ruleList.size(); r++)
         {
            std::shared_ptr<Rule> rule = ruleList[r];

            if (!rule || !rule->GetActive())
               continue;

            std::shared_ptr<RuleActions> actions = rule->GetActions();
            if (!actions)
               continue;

            std::vector<std::shared_ptr<RuleAction> > actionList = actions->GetSnapshot();

            for (size_t a = 0; a < actionList.size(); a++)
            {
               std::shared_ptr<RuleAction> action = actionList[a];

               if (!action || action->GetType() != RuleAction::BindToAddress)
                  continue;

               const AnsiString value = AnsiString(action->GetValue());

               if (!value.IsEmpty())
               {
                  raw.push_back(value);
                  sources.push_back("rule");
               }
            }
         }
      }

      const std::vector<AnsiString> named = Tokens(AnsiString(Configuration::Instance()->GetReputationWatchAddresses()));

      for (size_t i = 0; i < named.size(); i++)
      {
         raw.push_back(named[i]);
         sources.push_back("setting");
      }

      for (size_t i = 0; i < raw.size(); i++)
      {
         AnsiString text = raw[i];
         text.Trim();

         if (text.IsEmpty())
            continue;

         IPAddress parsed;

         if (!parsed.TryParse(text, false))
         {
            Skipped entry;
            entry.address = Shorten(Printable(text), 64);
            entry.source = sources[i];
            entry.reason = "invalid";
            skipped.push_back(entry);
            continue;
         }

         const AnsiString normalised = parsed.ToString();
         const AnsiString reason = NotCheckedReason(normalised);

         if (!reason.IsEmpty())
         {
            bool already = false;

            for (size_t s = 0; s < skipped.size(); s++)
            {
               if (skipped[s].address == normalised)
                  already = true;
            }

            if (!already)
            {
               Skipped entry;
               entry.address = normalised;
               entry.source = sources[i];
               entry.reason = reason;
               skipped.push_back(entry);
            }

            continue;
         }

         bool found = false;

         for (size_t a = 0; a < addresses.size(); a++)
         {
            if (addresses[a].address != normalised)
               continue;

            found = true;

            // The same address bound and named is one address from two places,
            // and the report says both rather than the first.
            if (addresses[a].sources.Find(sources[i]) < 0)
               addresses[a].sources += "," + sources[i];
         }

         if (found)
            continue;

         if ((int) addresses.size() >= MaximumAddresses)
         {
            // Named and not watched, which the report has to say. An address
            // nobody is checking and nobody is told about is the same failure as
            // one reported clean. The cap is the sum of the three sources, so a
            // server with many rules that bind an address reaches it without
            // anybody naming thirty-two addresses anywhere.
            bool already = false;

            for (size_t s = 0; s < skipped.size(); s++)
            {
               if (skipped[s].address == normalised)
                  already = true;
            }

            if (!already)
            {
               Skipped entry;
               entry.address = normalised;
               entry.source = sources[i];
               entry.reason = "too_many";
               skipped.push_back(entry);
            }

            continue;
         }

         Address address;
         address.address = normalised;
         address.family = parsed.GetType() == IPAddress::IPV6 ? FamilyIPv6 : FamilyIPv4;
         address.sources = sources[i];

         addresses.push_back(address);
      }
   }

   // --------------------------------------------------------------------------
   // The lookups
   // --------------------------------------------------------------------------

   bool
   ReputationWatch::Lookup_(const AnsiString &name, int resourceType, std::vector<AnsiString> &values, int &status, int &lookups)
   {
      values.clear();
      status = 0;
      lookups++;

      DNSResolverWinApi resolver;
      std::vector<DNSRecord> records;

      // The server's own resolver, so a configured DNSServer is honoured and the
      // answers are the ones delivery would see.
      const bool answered = resolver.Query(String(name), resourceType, records, status);

      for (size_t i = 0; i < records.size(); i++)
         values.push_back(Printable(records[i].GetValue()));

      return answered;
   }

   AnsiString
   ReputationWatch::FailureSentence_(int status)
   {
      // 258 is WAIT_TIMEOUT, which is what the Windows resolver reports when the
      // query outlived its timeout; 9002 is what both platforms report when no
      // resolver answered or the answer was a server failure.
      if (status == 258 || status == 1460)
         return "The lookup timed out.";
      if (status == 9002)
         return "No resolver answered, or the answer was a server failure.";
      if (status == 9005)
         return "The resolver refused the query.";
      if (status == 9502)
         return "The resolver's answer could not be read.";

      AnsiString sentence;
      sentence.Format("The lookup failed (status %d).", status);
      return sentence;
   }

   AnsiString
   ReputationWatch::RefusalSentence_(const AnsiString &code)
   {
      if (code == "127.255.255.252")
         return "The list refused the query and says the zone name is mistyped (127.255.255.252).";
      if (code == "127.255.255.254")
         return "The list refuses queries that reach it through a public or open resolver (127.255.255.254). Point DNSServer at a resolver of this server's own, or put the list's key-based zone in ReputationWatchLists.";
      if (code == "127.255.255.255")
         return "The list refuses this resolver for sending it too many queries (127.255.255.255).";

      return "The list refused the query with the error code " + code + ".";
   }

   int
   ReputationWatch::ClassifyAnswer(const std::vector<AnsiString> &answers, const AnsiString &codes, AnsiString &code)
   {
      code = "";

      if (answers.empty())
         return AnswerNone;

      std::vector<AnsiString> errors;
      std::vector<AnsiString> outside;
      std::vector<AnsiString> listed;
      std::vector<AnsiString> other;

      for (size_t i = 0; i < answers.size(); i++)
      {
         const AnsiString &answer = answers[i];

         if (IsErrorAnswer(answer))
            errors.push_back(answer);
         else if (!IsLoopbackAnswer(answer))
            outside.push_back(answer);
         else if (MatchesCodes(answer, codes))
            listed.push_back(answer);
         else
            other.push_back(answer);
      }

      // A refusal outranks everything: it is not an answer about the address at
      // all. An answer from outside 127.0.0.0/8 comes next, because a resolver
      // that answers every name with an address of its own would otherwise have
      // its answer read as a verdict.
      if (!errors.empty())
      {
         code = Join(errors, ", ");
         return AnswerRefused;
      }

      if (!outside.empty())
      {
         code = Join(outside, ", ");
         return AnswerUnexpected;
      }

      if (!listed.empty())
      {
         code = Join(listed, ", ");
         return AnswerListed;
      }

      code = Join(other, ", ");
      return AnswerNotListed;
   }

   bool
   ReputationWatch::MatchesCodes(const AnsiString &answer, const AnsiString &codes)
   {
      if (!IsLoopbackAnswer(answer) || IsErrorAnswer(answer))
         return false;

      if (codes.IsEmpty())
         return true;

      const std::vector<AnsiString> patterns = SplitOn(codes, '|');

      for (size_t i = 0; i < patterns.size(); i++)
      {
         if (MatchesOneCode(answer, patterns[i]))
            return true;
      }

      return false;
   }

   AnsiString
   ReputationWatch::TextReason_(const AnsiString &name, int &lookups)
   {
      std::vector<AnsiString> texts;
      int status = 0;

      if (!Lookup_(name, DnsTypeTxt, texts, status, lookups))
         return "";

      return Shorten(Printable(Join(texts, " ")), 250);
   }

   void
   ReputationWatch::ProbeList_(const List &list, Result &verdict, int &lookups)
   {
      verdict.status = StatusNotListed;
      verdict.code = "";
      verdict.reason = "";

      // RFC 5782 section 5: every list MUST list 127.0.0.2. A list that does not
      // is not answering this resolver, whatever the reason, and its silence about
      // a real address means nothing.
      const AnsiString listedName = "2.0.0.127." + list.zone;

      std::vector<AnsiString> answers;
      int status = 0;

      if (!Lookup_(listedName, DnsTypeA, answers, status, lookups))
      {
         verdict.status = StatusLookupFailed;
         verdict.reason = "The list's test entry 127.0.0.2 could not be looked up. " + FailureSentence_(status);
         return;
      }

      AnsiString code;
      int answer = ClassifyAnswer(answers, "", code);

      if (answer == AnswerRefused)
      {
         verdict.status = StatusRefused;
         verdict.code = code;

         AnsiString reason = RefusalSentence_(code);
         const AnsiString text = TextReason_(listedName, lookups);

         if (!text.IsEmpty())
            reason += " The list says: " + text;

         verdict.reason = reason;
         return;
      }

      if (answer == AnswerUnexpected)
      {
         verdict.status = StatusUnexpectedAnswer;
         verdict.code = code;
         verdict.reason = "The list's test entry 127.0.0.2 answered with an address outside 127.0.0.0/8, which no block list answers with. A resolver that answers names it cannot find with an address of its own does this.";
         return;
      }

      if (answer == AnswerNone)
      {
         verdict.status = StatusListNotAnswering;
         verdict.reason = "The list does not list its own test entry 127.0.0.2, which RFC 5782 requires of every list. The zone may have been retired, or this resolver may not be getting its answers - either way, \"not listed\" from it would not mean anything.";
         return;
      }

      // And it MUST NOT list 127.0.0.1. A zone that does is answering for
      // everything, which is what a retired list's name does once somebody else
      // owns it.
      const AnsiString unlistedName = "1.0.0.127." + list.zone;

      if (!Lookup_(unlistedName, DnsTypeA, answers, status, lookups))
      {
         verdict.status = StatusLookupFailed;
         verdict.reason = "The list's test entry 127.0.0.1 could not be looked up. " + FailureSentence_(status);
         return;
      }

      answer = ClassifyAnswer(answers, list.codes, code);

      if (answer == AnswerRefused)
      {
         verdict.status = StatusRefused;
         verdict.code = code;
         verdict.reason = RefusalSentence_(code);
         return;
      }

      if (answer == AnswerListed)
      {
         verdict.status = StatusListListsEverything;
         verdict.code = code;
         verdict.reason = "The list lists 127.0.0.1, which RFC 5782 forbids: it is answering for every address, so a listing from it says nothing about this server.";
         return;
      }
   }

   void
   ReputationWatch::CheckAddress_(const Address &address, const List &list, Result &result, int &lookups)
   {
      const String reversed = BLCheck::GetRevertedIP(String(address.address));

      if (reversed.IsEmpty())
      {
         result.status = StatusLookupFailed;
         result.reason = "The address could not be turned into a name to look up.";
         return;
      }

      const AnsiString name = AnsiString(reversed) + "." + list.zone;

      std::vector<AnsiString> answers;
      int status = 0;

      if (!Lookup_(name, DnsTypeA, answers, status, lookups))
      {
         result.status = StatusLookupFailed;
         result.reason = FailureSentence_(status);
         return;
      }

      AnsiString code;
      const int answer = ClassifyAnswer(answers, list.codes, code);

      result.code = code;

      switch (answer)
      {
      case AnswerListed:
         {
            result.status = StatusListed;
            result.reason = TextReason_(name, lookups);
         }
         break;

      case AnswerRefused:
         {
            result.status = StatusRefused;

            AnsiString reason = RefusalSentence_(code);
            const AnsiString text = TextReason_(name, lookups);

            if (!text.IsEmpty())
               reason += " The list says: " + text;

            result.reason = reason;
         }
         break;

      case AnswerUnexpected:
         {
            result.status = StatusUnexpectedAnswer;
            result.reason = "The list answered with an address outside 127.0.0.0/8, which no block list answers with.";
         }
         break;

      case AnswerNotListed:
         {
            // In 127.0.0.0/8, but not one of the codes this list was told mean
            // listed - a welcome list's 127.0.0.1, for instance.
            result.status = StatusNotListed;
            result.reason = "The list answered " + code + ", which is not one of the answers configured as a listing for it (" + list.codes + ").";
         }
         break;

      default:
         {
            result.status = StatusNotListed;
            result.reason = "";
         }
         break;
      }
   }

   // --------------------------------------------------------------------------
   // The rows
   // --------------------------------------------------------------------------

   AnsiString
   ReputationWatch::Key_(const AnsiString &address, const AnsiString &zone)
   {
      return address + " " + zone;
   }

   bool
   ReputationWatch::ReadRows_(std::vector<Result> &rows)
   {
      rows.clear();

      SQLCommand command(_T("select * from hm_reputationchecks order by rcid asc"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Result row;

         row.id = recordset->GetInt64Value("rcid");
         row.address = AnsiString(recordset->GetStringValue("rcaddress"));
         row.sources = AnsiString(recordset->GetStringValue("rcsources"));
         row.zone = AnsiString(recordset->GetStringValue("rczone"));
         row.status = recordset->GetLongValue("rcstatus");
         row.code = AnsiString(recordset->GetStringValue("rccode"));
         row.reason = AnsiString(recordset->GetStringValue("rcreason"));
         row.checked = recordset->GetInt64Value("rcchecked");
         row.listed_since = recordset->GetInt64Value("rclistedsince");
         row.confirmed = recordset->GetInt64Value("rcconfirmed");

         rows.push_back(row);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   ReputationWatch::WriteRows_(const std::vector<Result> &results, const std::vector<Result> &previous)
   {
      bool stored = true;

      for (size_t i = 0; i < results.size(); i++)
      {
         const Result &result = results[i];

         String errorMessage;

         if (result.id > 0)
         {
            // By id, and never by address and zone: SQL Server Compact will not
            // compare a long nvarchar parameter with a column.
            SQLCommand command(_T("update hm_reputationchecks set rcsources = @SOURCES, rcstatus = @STATUS, rccode = @CODE, ")
                               _T("rcreason = @REASON, rcchecked = @CHECKED, rclistedsince = @SINCE, rcconfirmed = @CONFIRMED ")
                               _T("where rcid = @ID"));

            command.AddParameter("@SOURCES", String(Shorten(result.sources, 64)));
            command.AddParameter("@STATUS", result.status);
            command.AddParameter("@CODE", String(Shorten(result.code, 255)));
            command.AddParameter("@REASON", String(Shorten(result.reason, 255)));
            command.AddParameter("@CHECKED", result.checked);
            command.AddParameter("@SINCE", result.listed_since);
            command.AddParameter("@CONFIRMED", result.confirmed);
            command.AddParameter("@ID", result.id);

            if (!Application::Instance()->GetDBManager()->Execute(command, nullptr, 0, errorMessage))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6900, "ReputationWatch::WriteRows_",
                  "A reputation check's result could not be stored: " + errorMessage);
               stored = false;
            }

            continue;
         }

         SQLStatement statement(SQLStatement::STInsert, _T("hm_reputationchecks"));
         statement.AddColumn(_T("rcaddress"), String(Shorten(result.address, 64)), 64);
         statement.AddColumn(_T("rcsources"), String(Shorten(result.sources, 64)), 64);
         statement.AddColumn(_T("rczone"), String(Shorten(result.zone, 255)), 255);
         statement.AddColumn(_T("rcstatus"), (long) result.status);
         statement.AddColumn(_T("rccode"), String(Shorten(result.code, 255)), 255);
         statement.AddColumn(_T("rcreason"), String(Shorten(result.reason, 255)), 255);
         statement.AddColumnInt64(_T("rcchecked"), result.checked);
         statement.AddColumnInt64(_T("rclistedsince"), result.listed_since);
         statement.AddColumnInt64(_T("rcconfirmed"), result.confirmed);
         statement.SetIdentityColumn(_T("rcid"));

         if (!Application::Instance()->GetDBManager()->Execute(statement.GetCommand(), nullptr, 0, errorMessage))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6900, "ReputationWatch::WriteRows_",
               "A reputation check's result could not be stored: " + errorMessage);
            stored = false;
         }
      }

      // What the last check covered and this one does not: an address taken out
      // of the setting, a list taken out of it.
      for (size_t i = 0; i < previous.size(); i++)
      {
         bool kept = false;

         for (size_t r = 0; r < results.size(); r++)
         {
            if (results[r].id == previous[i].id)
               kept = true;
         }

         if (kept)
            continue;

         SQLCommand command(_T("delete from hm_reputationchecks where rcid = @ID"));
         command.AddParameter("@ID", previous[i].id);

         String errorMessage;

         if (!Application::Instance()->GetDBManager()->Execute(command, nullptr, 0, errorMessage))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6900, "ReputationWatch::WriteRows_",
               "A reputation check's result could not be removed: " + errorMessage);
            stored = false;
         }
      }

      return stored;
   }

   __int64
   ReputationWatch::NewestCheck_()
   {
      // max() over no rows is one row holding NULL, which is "never checked" -
      // and two statements are not needed here, because nothing reads the row
      // that holds it.
      SQLCommand command(_T("select max(rcchecked) as newest from hm_reputationchecks"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF() || recordset->GetIsNull("newest"))
         return last_run_;

      const __int64 newest = recordset->GetInt64Value("newest");

      return newest > last_run_ ? newest : last_run_;
   }

   bool
   ReputationWatch::IsListedForAlert_(const Result &result, __int64 now)
   {
      if (result.status == ReputationWatch::StatusListed)
         return true;

      // An answer that says nothing does not delist an address. The row keeps the
      // moment the last definite answer found it listed, and that is what the
      // alert reads until a check gets another one.
      if (!IsUnknown_(result) || result.listed_since <= 0)
         return false;

      // But a list that has stopped answering altogether would otherwise assert
      // that listing for ever - and a retired zone answers nothing for ever. After
      // a week with no definite answer the pair is unknown rather than listed:
      // hmctl still exits 1 for it, the Deck still draws it as a warning, and the
      // alert can clear. Measured from the last DEFINITE answer, not from the
      // start of the listing, so an address listed for a year and confirmed this
      // morning is not aged out the moment the list goes quiet.
      const __int64 confirmed = result.confirmed > 0 ? result.confirmed : result.listed_since;

      // A clock that moved backwards must not age a listing out at once.
      if (now <= confirmed)
         return true;

      return now - confirmed < UnconfirmedListingSeconds;
   }

   bool
   ReputationWatch::IsUnknown_(const Result &result)
   {
      switch (result.status)
      {
      case ReputationWatch::StatusRefused:
      case ReputationWatch::StatusLookupFailed:
      case ReputationWatch::StatusUnexpectedAnswer:
      case ReputationWatch::StatusListNotAnswering:
      case ReputationWatch::StatusListListsEverything:
         return true;
      default:
         return false;
      }
   }

   const char *
   ReputationWatch::StatusWord(int status)
   {
      switch (status)
      {
      case StatusListed:               return "listed";
      case StatusRefused:              return "refused";
      case StatusLookupFailed:         return "lookup_failed";
      case StatusUnexpectedAnswer:     return "unexpected_answer";
      case StatusListNotAnswering:     return "list_not_answering";
      case StatusListListsEverything:  return "list_lists_everything";
      case StatusNotApplicable:        return "not_applicable";
      case StatusNotChecked:           return "not_checked";
      default:                         return "not_listed";
      }
   }

   // --------------------------------------------------------------------------
   // The check
   // --------------------------------------------------------------------------

   ReputationWatch::RunSummary
   ReputationWatch::Run(__int64 now)
   {
      boost::lock_guard<boost::recursive_mutex> guard(Mutex_());

      return RunLocked_(now);
   }

   bool
   ReputationWatch::TryRun(__int64 now, RunSummary &summary)
   {
      boost::unique_lock<boost::recursive_mutex> guard(Mutex_(), boost::defer_lock);

      guard.try_lock();

      if (!guard.owns_lock())
         return false;

      summary = RunLocked_(now);
      return true;
   }

   bool
   ReputationWatch::RunIfDue(__int64 now)
   {
      // Both under the lock the check holds. A check asked for over REST or COM
      // between the question and the answer would otherwise cause a second full
      // check within the minute, and the day's queries to every configured list
      // would be made twice - which is the one cost this whole design is careful
      // about.
      boost::lock_guard<boost::recursive_mutex> guard(Mutex_());

      if (!IsDue(now))
         return false;

      RunLocked_(now);

      return true;
   }

   ReputationWatch::RunSummary
   ReputationWatch::RunLocked_(__int64 now)
   {
      RunSummary summary;
      summary.as_of = now;

      // The budget is measured from here rather than from now, which is the
      // caller's clock and may be a moment old - or, for a task that was queued
      // behind a long one, a good deal older than that.
      const __int64 started = (__int64) time(nullptr);

      std::vector<Address> addresses;
      std::vector<Skipped> skipped;
      CollectAddresses(addresses, skipped);

      std::vector<List> lists;
      String listRefusal;
      ParseLists(Configuration::Instance()->GetReputationWatchLists(), lists, listRefusal);

      if (!listRefusal.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("Reputation watch: ReputationWatchLists holds an entry that cannot be used, and the others are checked. {0}",
                                           listRefusal));
      }

      summary.addresses = (int) addresses.size();
      summary.lists = (int) lists.size();

      std::vector<Result> previous;

      if (!ReadRows_(previous))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6901, "ReputationWatch::Run",
            "The reputation watch could not read what the last check found, so this one has been abandoned rather than written over it.");

         summary.stored = false;
         return summary;
      }

      std::vector<Result> results;

      for (size_t l = 0; l < lists.size(); l++)
      {
         const List &list = lists[l];

         std::vector<size_t> applicable;

         for (size_t a = 0; a < addresses.size(); a++)
         {
            if (list.family != FamilyAny && list.family != addresses[a].family)
            {
               Result result;
               result.address = addresses[a].address;
               result.sources = addresses[a].sources;
               result.zone = list.zone;
               result.status = StatusNotApplicable;
               result.reason = list.family == FamilyIPv4
                  ? "The list covers IPv4 addresses only, so it has nothing to say about this one."
                  : "The list covers IPv6 addresses only, so it has nothing to say about this one.";

               results.push_back(result);
               continue;
            }

            applicable.push_back(a);
         }

         if (applicable.empty())
            continue;

         Result verdict;

         if ((__int64) time(nullptr) - started >= MaximumCheckSeconds)
         {
            // Out of time before this list was asked anything. Its addresses take
            // the verdict below, which is a failed lookup: the rows say nothing
            // was asked, and the next check asks again.
            summary.complete = false;
            verdict.status = StatusLookupFailed;
            verdict.reason = OutOfTime();
         }
         else
         {
            // The list itself first: two queries that decide whether anything it
            // says about a real address can be believed.
            ProbeList_(list, verdict, summary.lookups);
         }

         for (size_t i = 0; i < applicable.size(); i++)
         {
            const Address &address = addresses[applicable[i]];

            Result result;
            result.address = address.address;
            result.sources = address.sources;
            result.zone = list.zone;

            if (verdict.status != StatusNotListed)
            {
               result.status = verdict.status;
               result.code = verdict.code;
               result.reason = verdict.reason;
            }
            else if ((__int64) time(nullptr) - started >= MaximumCheckSeconds)
            {
               summary.complete = false;
               result.status = StatusLookupFailed;
               result.reason = OutOfTime();
            }
            else
            {
               CheckAddress_(address, list, result, summary.lookups);
            }

            results.push_back(result);
         }
      }

      for (size_t i = 0; i < results.size(); i++)
      {
         Result &result = results[i];
         result.checked = now;

         __int64 before = 0;
         __int64 beforeConfirmed = 0;

         for (size_t p = 0; p < previous.size(); p++)
         {
            if (Key_(previous[p].address, previous[p].zone) != Key_(result.address, result.zone))
               continue;

            result.id = previous[p].id;
            before = previous[p].listed_since;
            beforeConfirmed = previous[p].confirmed;
         }

         if (result.status == StatusListed)
            result.listed_since = before > 0 ? before : now;
         else if (IsUnknown_(result))
            result.listed_since = before;
         else
            result.listed_since = 0;

         // Listed, not listed, and "the other family of addresses" are answers; a
         // refusal, a timeout and a list that is not answering are not, and carry
         // the last answer's moment forward untouched.
         const bool definite = result.status == StatusListed || result.status == StatusNotListed ||
                               result.status == StatusNotApplicable;

         result.confirmed = definite ? now : beforeConfirmed;

         if (IsListedForAlert_(result, now))
            summary.listed++;
         else if (IsUnknown_(result))
            summary.unknown++;
      }

      summary.stored = WriteRows_(results, previous);

      last_run_ = now;

      LOG_APPLICATION(Formatter::Format("Reputation watch: {0} address(es) against {1} list(s) in {2} lookup(s): {3} listed, {4} unanswered.",
                                        summary.addresses, summary.lists, summary.lookups, summary.listed, summary.unknown));

      // The alert is raised from the rows this has just written, so that a check
      // on demand tells the administrator at once rather than within the minute.
      AlertManager::Instance()->EvaluateReputationWatch();

      return summary;
   }

   bool
   ReputationWatch::IsDue(__int64 now)
   {
      if (!Configuration::Instance()->GetReputationWatchEnabled())
         return false;

      const __int64 newest = NewestCheck_();

      if (newest <= 0)
         return true;

      // A clock moved backwards must not put the next check a day away.
      if (newest > now)
         return true;

      return now - newest >= SecondsBetweenChecks;
   }

   // --------------------------------------------------------------------------
   // What it all reads as
   // --------------------------------------------------------------------------

   AnsiString
   ReputationWatch::ReportJson(__int64 now)
   {
      std::vector<Address> addresses;
      std::vector<Skipped> skipped;
      CollectAddresses(addresses, skipped);

      std::vector<List> lists;
      String listRefusal;
      ParseLists(Configuration::Instance()->GetReputationWatchLists(), lists, listRefusal);

      std::vector<Result> rows;
      const bool readable = ReadRows_(rows);

      int listed = 0;
      int notListed = 0;
      int unknown = 0;
      int notApplicable = 0;
      int notChecked = 0;

      AnsiString json = "{";
      json += "\"enabled\":";
      json += Configuration::Instance()->GetReputationWatchEnabled() ? "true" : "false";
      json += ",\"lists\":[";

      for (size_t l = 0; l < lists.size(); l++)
      {
         if (l > 0)
            json += ",";

         json += "{\"zone\":" + JsonString(lists[l].zone);
         json += ",\"family\":" + JsonString(lists[l].family == FamilyIPv4 ? "ipv4" : (lists[l].family == FamilyIPv6 ? "ipv6" : "any"));
         json += ",\"codes\":" + JsonString(lists[l].codes);
         json += "}";
      }

      json += "],\"lists_problem\":" + JsonString(AnsiString(listRefusal));
      json += ",\"addresses\":[";

      for (size_t a = 0; a < addresses.size(); a++)
      {
         if (a > 0)
            json += ",";

         const Address &address = addresses[a];

         json += "{\"address\":" + JsonString(address.address);
         json += ",\"family\":" + JsonString(address.family == FamilyIPv6 ? "ipv6" : "ipv4");
         json += ",\"sources\":[";

         const std::vector<AnsiString> sources = SplitOn(address.sources, ',');

         for (size_t s = 0; s < sources.size(); s++)
         {
            if (s > 0)
               json += ",";

            json += JsonString(sources[s]);
         }

         json += "],\"results\":[";

         bool anyListed = false;
         bool anyUnknown = false;
         bool anyDefinite = false;
         __int64 checked = 0;

         for (size_t l = 0; l < lists.size(); l++)
         {
            Result result;
            result.address = address.address;
            result.zone = lists[l].zone;
            result.status = StatusNotChecked;

            bool found = false;

            for (size_t r = 0; r < rows.size(); r++)
            {
               if (Key_(rows[r].address, rows[r].zone) != Key_(address.address, lists[l].zone))
                  continue;

               result = rows[r];
               found = true;
            }

            if (!found && lists[l].family != FamilyAny && lists[l].family != address.family)
            {
               result.status = StatusNotApplicable;
               result.reason = "The list covers the other family of addresses, so it has nothing to say about this one.";
            }

            if (l > 0)
               json += ",";

            json += "{\"list\":" + JsonString(lists[l].zone);
            json += ",\"status\":" + JsonString(StatusWord(result.status));

            // Three values, not two: false is "this list says it is not listed",
            // and null is "this list did not say", which is never the same thing.
            if (result.status == StatusListed)
               json += ",\"listed\":true";
            else if (result.status == StatusNotListed)
               json += ",\"listed\":false";
            else
               json += ",\"listed\":null";

            json += ",\"code\":" + JsonString(result.code);
            json += ",\"reason\":" + JsonString(result.reason);
            json += ",\"checked\":" + Number(result.checked);
            json += ",\"confirmed\":" + Number(result.confirmed);
            json += ",\"listed_since\":" + Number(result.listed_since);
            json += "}";

            if (result.checked > checked)
               checked = result.checked;

            switch (result.status)
            {
            case StatusListed:
               listed++;
               anyListed = true;
               anyDefinite = true;
               break;
            case StatusNotListed:
               notListed++;
               anyDefinite = true;
               break;
            case StatusNotApplicable:
               notApplicable++;
               break;
            case StatusNotChecked:
               notChecked++;
               break;
            default:
               unknown++;
               anyUnknown = true;
               if (IsListedForAlert_(result, now))
                  anyListed = true;
               break;
            }
         }

         json += "],\"checked\":" + Number(checked);

         if (anyListed)
            json += ",\"listed\":true";
         else if (anyDefinite && !anyUnknown)
            json += ",\"listed\":false";
         else
            json += ",\"listed\":null";

         json += "}";
      }

      json += "],\"skipped\":[";

      for (size_t s = 0; s < skipped.size(); s++)
      {
         if (s > 0)
            json += ",";

         json += "{\"address\":" + JsonString(skipped[s].address);
         json += ",\"source\":" + JsonString(skipped[s].source);
         json += ",\"reason\":" + JsonString(skipped[s].reason);
         json += ",\"detail\":" + JsonString(NotCheckedSentence(skipped[s].reason));
         json += "}";
      }

      const __int64 newest = readable ? NewestCheck_() : 0;

      json += "],\"last_checked\":" + Number(newest);
      json += ",\"next_check\":" + Number(Configuration::Instance()->GetReputationWatchEnabled() && newest > 0 ? newest + SecondsBetweenChecks : 0);
      json += ",\"now\":" + Number(now);
      json += ",\"counts\":{\"addresses\":" + Number((__int64) addresses.size());
      json += ",\"lists\":" + Number((__int64) lists.size());
      json += ",\"listed\":" + Number(listed);
      json += ",\"not_listed\":" + Number(notListed);
      json += ",\"unknown\":" + Number(unknown);
      json += ",\"not_applicable\":" + Number(notApplicable);
      json += ",\"not_checked\":" + Number(notChecked);
      json += ",\"skipped\":" + Number((__int64) skipped.size());
      json += "}}";

      return json;
   }

   AnsiString
   ReputationWatch::RunJson(const RunSummary &summary, __int64 now)
   {
      AnsiString report = ReportJson(now);

      AnsiString run = "\"run\":{\"as_of\":" + Number(summary.as_of);
      run += ",\"addresses\":" + Number(summary.addresses);
      run += ",\"lists\":" + Number(summary.lists);
      run += ",\"lookups\":" + Number(summary.lookups);
      run += ",\"listed\":" + Number(summary.listed);
      run += ",\"unknown\":" + Number(summary.unknown);
      run += ",\"stored\":";
      run += summary.stored ? "true" : "false";
      run += ",\"complete\":";
      run += summary.complete ? "true" : "false";
      run += "},";

      // After the opening brace of the report, so that the run is the first thing
      // a reader sees and the report is unchanged otherwise.
      if (report.GetLength() > 1 && report[0] == '{')
         return "{" + run + report.Mid(1);

      return report;
   }

   AnsiString
   ReputationWatch::ClearSentence_(int definite, int notApplicable, int unknown, int aged, int notChecked, int pairs)
   {
      // The sentence an administrator is mailed when the alert stops. It is the
      // one place where "nothing was found" has to be told apart from "nothing
      // answered", because those two arrive as the same silence and mean opposite
      // things - and a mail saying nothing is listed is read as an all-clear.
      if (pairs <= 0)
         return "The reputation watch has no address or no list to check, so nothing is being looked up and nothing is known about this server's reputation.";

      if (unknown == 0 && notChecked == 0)
      {
         if (definite == 0)
         {
            return "No block list this server asks covers the family of any of its outbound addresses, so none of them has "
                   "been looked up. Nothing is listed because nothing was asked.";
         }

         AnsiString sentence = "No outbound address of this server is on any of the block lists it checks.";

         if (notApplicable > 0)
         {
            sentence += " (" + Number(notApplicable) + " of the " + Number(pairs) + " pairs are an address of the family the "
                        "list does not cover, and were not looked up.)";
         }

         return sentence;
      }

      // What follows has to fit in 250 characters: AlertManager shortens a summary
      // to that, and this is the sentence an administrator reads as a result. So
      // the misleading case goes first and the arithmetic second, and the reasons
      // - which list refused, which never answered - stay in the report.
      const int unanswered = unknown + notChecked;
      const AnsiString counted = Number(unanswered) + " of the " + Number(pairs) + " checks came back with no answer";

      if (aged > 0)
      {
         return "A listing found earlier is no longer counted as one: nothing has confirmed it for a week, and that is "
                "not a delisting. " + counted + " - this is not an all-clear. See GET /api/v1/reputation.";
      }

      return "No block list that answered says an outbound address of this server is listed, but " + counted +
             ". This is not an all-clear: see GET /api/v1/reputation.";
   }

   bool
   ReputationWatch::CollectListed(String &summary, String &detail, bool &known, String &clearSummary)
   {
      summary = _T("");
      detail = _T("");
      clearSummary = _T("");
      known = false;

      std::vector<Result> rows;

      if (!ReadRows_(rows))
         return false;

      known = true;

      // Nothing stored is nothing listed, and what follows reads the settings and
      // walks every active global rule's actions. The alert pass runs this once a
      // minute on every server, so the answer on a server that checks nothing
      // costs one select and stops here.
      if (rows.empty())
      {
         clearSummary = _T("Nothing is stored for the reputation watch: no check has an answer for any outbound address of this server.");
         return false;
      }

      const __int64 now = (__int64) time(nullptr);

      std::vector<Address> addresses;
      std::vector<Skipped> skipped;
      CollectAddresses(addresses, skipped);

      std::vector<List> lists;
      String refusal;
      ParseLists(Configuration::Instance()->GetReputationWatchLists(), lists, refusal);

      std::vector<Result> listed;

      // Why nothing was found, which is not one answer but four. See the sentence
      // built below: a clear that says "nothing is on any list" when three lists
      // have refused every query for a week is the same lie as a clean report.
      int definite = 0;
      int notApplicable = 0;
      int unknown = 0;
      int aged = 0;
      int covered = 0;

      for (size_t r = 0; r < rows.size(); r++)
      {
         bool watched = false;

         for (size_t a = 0; a < addresses.size(); a++)
         {
            if (addresses[a].address == rows[r].address)
               watched = true;
         }

         bool configured = false;

         for (size_t l = 0; l < lists.size(); l++)
         {
            if (lists[l].zone == rows[r].zone)
               configured = true;
         }

         // A row about an address the watch no longer checks, or a list no longer
         // named, is stale rather than news: the next check removes it.
         if (!watched || !configured)
            continue;

         covered++;

         if (IsListedForAlert_(rows[r], now))
         {
            listed.push_back(rows[r]);
            continue;
         }

         if (rows[r].status == StatusNotListed)
            definite++;
         else if (rows[r].status == StatusNotApplicable)
            notApplicable++;
         else if (IsUnknown_(rows[r]))
         {
            unknown++;

            // A listing this row still carries, which only stopped counting
            // because a week went by with nothing confirming it. The pair is
            // unknown, and saying "not listed" about it would be the opposite of
            // what is known.
            if (rows[r].listed_since > 0)
               aged++;
         }
      }

      if (listed.empty())
      {
         const int pairs = (int) (lists.size() * addresses.size());
         const int notChecked = pairs > covered ? pairs - covered : 0;

         clearSummary = String(ClearSentence_(definite, notApplicable, unknown, aged, notChecked, pairs));
         return false;
      }

      std::vector<AnsiString> order;

      for (size_t i = 0; i < listed.size(); i++)
      {
         bool seen = false;

         for (size_t o = 0; o < order.size(); o++)
         {
            if (order[o] == listed[i].address)
               seen = true;
         }

         if (!seen)
            order.push_back(listed[i].address);
      }

      std::sort(order.begin(), order.end());

      AnsiString sentence;
      AnsiString lines;

      for (size_t o = 0; o < order.size(); o++)
      {
         std::vector<AnsiString> zones;

         for (size_t i = 0; i < listed.size(); i++)
         {
            if (listed[i].address == order[o])
               zones.push_back(listed[i].zone);
         }

         std::sort(zones.begin(), zones.end());

         if (o > 0)
            sentence += "; ";

         sentence += order[o] + " on " + Join(zones, " and ");

         for (size_t i = 0; i < listed.size(); i++)
         {
            if (listed[i].address != order[o])
               continue;

            lines += order[o] + " on " + listed[i].zone + ": ";
            lines += listed[i].status == StatusListed ? "listed" : "listed at the last check that got an answer";

            if (!listed[i].code.IsEmpty())
               lines += ", answer " + listed[i].code;

            if (listed[i].listed_since > 0)
               lines += ", since " + TimeText(listed[i].listed_since);

            lines += ".";

            if (!listed[i].reason.IsEmpty())
               lines += " " + listed[i].reason;

            lines += "\r\n";
         }
      }

      const bool one = order.size() == 1 && listed.size() == 1;

      summary = String(one
         ? "An outbound address of this server is on a DNS block list: " + sentence + "."
         : "Outbound addresses of this server are on DNS block lists: " + sentence + ".");

      AnsiString body = lines;
      body += "\r\nMail from a listed address is refused or filed as junk by every server that uses the list, and the first "
              "sign of it is usually somebody saying a message never arrived. Read the reason above - a list's own page "
              "says why it listed an address and how to ask for removal - stop whatever caused it, and ask the list to "
              "remove the address. The watch clears this alert at the first check that finds the address no longer listed; "
              "a check that cannot reach a list does not clear it. Every answer is at GET /api/v1/reputation and on the "
              "Control Deck's Outbound reputation page.";

      detail = String(body);

      return true;
   }

   // --------------------------------------------------------------------------
   // The rules above, tested in process
   // --------------------------------------------------------------------------

   namespace
   {
      void Require(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }
   }

   void
   ReputationWatchTester::Test()
   {
      typedef ReputationWatch Watch;

      // ---- the lists
      {
         std::vector<Watch::List> lists;
         String refusal;

         Require(Watch::ParseLists(String(Watch::DefaultLists), lists, refusal), "The shipped list of lists was refused.");
         Require(lists.size() == 4, "The shipped list of lists is not four lists.");
         Require(lists[0].zone == "zen.spamhaus.org" && lists[0].family == Watch::FamilyIPv4, "The first shipped list is not zen.spamhaus.org for IPv4 only.");
         Require(lists[1].family == Watch::FamilyIPv4, "bl.spamcop.net is not marked IPv4-only.");

         // Every shipped list is IPv4-only, and that is not an oversight: none of
         // the four answers an IPv6 query on its free public zone, and a list that
         // does not answer looks exactly like one that says "not listed". A
         // default that read as clean for an IPv6 address no list had covered is
         // the one failure this whole file exists to prevent.
         for (size_t i = 0; i < lists.size(); i++)
            Require(lists[i].family == Watch::FamilyIPv4, "A shipped list is not marked IPv4-only.");

         lists.clear();
         Require(Watch::ParseLists(_T("ipv6:bl.example.test=127.0.0.2|127.0.0.4-7"), lists, refusal), "A family, a zone and codes together were refused.");
         Require(lists.size() == 1 && lists[0].family == Watch::FamilyIPv6 && lists[0].codes == "127.0.0.2|127.0.0.4-7", "The codes or the family were not read.");

         lists.clear();
         Require(!Watch::ParseLists(_T("not a zone!"), lists, refusal), "\"not a zone!\" was accepted as a list.");
         Require(!refusal.IsEmpty(), "A refused list produced no sentence.");

         lists.clear();
         Require(!Watch::ParseLists(_T("bl.example.test=127.0.0"), lists, refusal), "A code with three octets was accepted.");

         lists.clear();
         // The entries after one that cannot be read are still read.
         Watch::ParseLists(_T("not a zone!, bl.example.test"), lists, refusal);
         Require(lists.size() == 1 && lists[0].zone == "bl.example.test", "An unreadable entry took the entry after it with it.");

         lists.clear();
         Watch::ParseLists(_T("bl.example.test bl.example.test"), lists, refusal);
         Require(lists.size() == 1, "The same zone named twice is two lists.");
      }

      // ---- the addresses
      {
         std::vector<AnsiString> addresses;
         String refusal;

         Require(Watch::ParseAddresses(_T("192.0.2.25, 2001:db8::25"), addresses, refusal), "Two good addresses were refused.");
         Require(addresses.size() == 2, "Two addresses did not parse as two.");

         addresses.clear();
         Require(!Watch::ParseAddresses(_T("mail.example.com"), addresses, refusal), "A host name was accepted as an address.");
      }

      // ---- which addresses are worth asking about
      {
         Require(Watch::NotCheckedReason("192.0.2.25").IsEmpty(), "A public address was skipped.");
         Require(Watch::NotCheckedReason("2001:db8::25").IsEmpty(), "A public IPv6 address was skipped.");
         Require(Watch::NotCheckedReason("10.0.0.1") == "private", "10.0.0.1 is not private.");
         Require(Watch::NotCheckedReason("172.16.0.1") == "private", "172.16.0.1 is not private.");
         Require(Watch::NotCheckedReason("172.32.0.1").IsEmpty(), "172.32.0.1 is not in 172.16/12 and was skipped anyway.");
         Require(Watch::NotCheckedReason("192.168.1.1") == "private", "192.168.1.1 is not private.");
         Require(Watch::NotCheckedReason("127.0.0.1") == "loopback", "127.0.0.1 is not loopback.");
         Require(Watch::NotCheckedReason("169.254.1.1") == "link_local", "169.254.1.1 is not link-local.");
         Require(Watch::NotCheckedReason("100.64.0.1") == "shared", "100.64.0.1 is not the shared range.");
         Require(Watch::NotCheckedReason("0.0.0.0") == "unspecified", "0.0.0.0 is not unspecified.");
         Require(Watch::NotCheckedReason("::1") == "loopback", "::1 is not loopback.");
         Require(Watch::NotCheckedReason("fe80::1") == "link_local", "fe80::1 is not link-local.");
         Require(Watch::NotCheckedReason("fd00::1") == "private", "fd00::1 is not a unique local address.");
         Require(Watch::NotCheckedReason("::ffff:192.0.2.25") == "mapped", "An IPv4-mapped address was not recognised.");
         Require(!Watch::NotCheckedSentence("private").IsEmpty(), "A skipped address has no sentence.");
      }

      // ---- the codes
      {
         Require(Watch::MatchesCodes("127.0.0.2", ""), "With no codes, 127.0.0.2 is not a listing.");
         Require(Watch::MatchesCodes("127.0.0.11", ""), "With no codes, 127.0.0.11 is not a listing.");
         Require(!Watch::MatchesCodes("127.255.255.254", ""), "An error code was read as a listing.");
         Require(!Watch::MatchesCodes("203.0.113.1", ""), "An answer outside 127.0.0.0/8 was read as a listing.");
         Require(Watch::MatchesCodes("127.0.0.5", "127.0.0.4-7"), "A range did not match.");
         Require(!Watch::MatchesCodes("127.0.0.8", "127.0.0.4-7"), "A range matched outside itself.");
         Require(Watch::MatchesCodes("127.0.0.2", "127.0.0.2|127.0.0.4-7"), "The first of two codes did not match.");
         Require(!Watch::MatchesCodes("127.0.0.1", "127.0.0.2"), "A welcome list's 127.0.0.1 was read as a block.");
         Require(Watch::MatchesCodes("127.0.0.9", "127.0.0.*"), "A wildcard did not match.");
      }

      // ---- what an answer adds up to
      {
         AnsiString code;
         std::vector<AnsiString> answers;

         Require(Watch::ClassifyAnswer(answers, "", code) == Watch::AnswerNone, "No answer is not AnswerNone.");

         answers.push_back("127.0.0.2");
         Require(Watch::ClassifyAnswer(answers, "", code) == Watch::AnswerListed && code == "127.0.0.2", "127.0.0.2 is not a listing.");

         answers.push_back("127.255.255.254");
         Require(Watch::ClassifyAnswer(answers, "", code) == Watch::AnswerRefused && code == "127.255.255.254",
                 "A refusal beside a listing did not outrank it.");

         answers.clear();
         answers.push_back("127.0.0.2");
         answers.push_back("203.0.113.1");
         Require(Watch::ClassifyAnswer(answers, "", code) == Watch::AnswerUnexpected,
                 "An answer from outside 127.0.0.0/8 beside a listing did not outrank it.");

         answers.clear();
         answers.push_back("127.0.0.1");
         Require(Watch::ClassifyAnswer(answers, "127.0.0.2", code) == Watch::AnswerNotListed && code == "127.0.0.1",
                 "An answer that is not one of the list's codes was read as a listing.");
      }
   }
}
