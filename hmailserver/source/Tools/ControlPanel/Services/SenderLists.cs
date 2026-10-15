// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>One of the lists a sender is let through or stopped by, as the spam filtering overview explains it.</summary>
   public sealed class SenderListGuideRow
   {
      internal SenderListGuideRow(string page, string when, string matches, string does)
      {
         Page = page;
         When = when;
         Matches = matches;
         Does = does;
      }

      /// <summary>The navigation key of the list's page; its title is the list's name.</summary>
      public string Page { get; }

      /// <summary>When in a message's arrival the server reads the list (a catalogue key).</summary>
      public string When { get; }

      /// <summary>What an entry is matched against (a catalogue key).</summary>
      public string Matches { get; }

      /// <summary>What a match does to the message (a catalogue key).</summary>
      public string Does { get; }
   }

   /// <summary>
   /// "Which list do I need?": the eight lists that let a sender through or stop one,
   /// in the order a message meets them, each with when it is read, what it is
   /// matched against and what a match does. Three let a sender through, three stop
   /// one and two can do either, and each page explained at most one neighbour
   /// (review finding F269). What each says is the server's: the spam exemptions and
   /// the allowed and blocked domains are read at MAIL FROM (SMTPConnection::
   /// InitializeSpamProtectionType_, and the domain policy right after it), the
   /// blocked senders and the DNS block lists with the checks before the body
   /// (SpamTestRunner, the blocked senders first), the greylisting exemptions and
   /// the bypass list at RCPT TO (SpamProtection's greylisting, the bypass after
   /// DATA for an entry that needs DKIM or DMARC), SURBL after the body, and the
   /// allow and block list at delivery, recipient by recipient (LocalDelivery).
   /// </summary>
   public static class SenderListGuide
   {
      public static IReadOnlyList<SenderListGuideRow> Rows { get; } = new[]
      {
         new SenderListGuideRow("spamwhitelist",
            N("At MAIL FROM, before any spam check"),
            N("The connecting IP address and the envelope sender, with wildcards"),
            N("Skips every spam check: the message is never scored.")),
         new SenderListGuideRow("domainpolicylist",
            N("At MAIL FROM, after the spam exemptions"),
            N("The envelope sender's domain or the HELO name, exactly or with its subdomains"),
            N("Allowed: no greylisting and no refusal by score, though the checks still run. Blocked: refused at MAIL FROM.")),
         new SenderListGuideRow("blockedsenders",
            N("With the checks before the body, first among them"),
            N("The envelope sender: one address, or a domain with its subdomains"),
            N("Adds the entry's score; the default of 100 refuses the message.")),
         new SenderListGuideRow("dnsbl",
            N("With the checks before the body"),
            N("The connecting IP address, asked of each active list's DNS zone"),
            N("Adds the list's score.")),
         new SenderListGuideRow("greylistwhitelist",
            N("At RCPT TO, before greylisting"),
            N("The connecting IP address, with wildcards"),
            N("Never greylisted.")),
         new SenderListGuideRow("greylistbypass",
            N("At RCPT TO, or after DATA for an entry that needs DKIM or DMARC"),
            N("The envelope sender's domain or the HELO name, with an SPF, DKIM or DMARC pass"),
            N("Let past greylisting.")),
         new SenderListGuideRow("surbl",
            N("After the body has arrived"),
            N("The host names of the links in the message"),
            N("Adds the server's score.")),
         new SenderListGuideRow("senderlist",
            N("At delivery, recipient by recipient, after the account's own list and its domain's"),
            N("The envelope sender or the From address: one address, or a domain with its subdomains"),
            N("Allowed: the spam verdict is skipped. Blocked: held in the quarantine where that is on, otherwise marked as spam."))
      };
   }

   /// <summary>What a list says about the sender asked about.</summary>
   public enum SenderCheckOutcome
   {
      /// <summary>No entry matches.</summary>
      NoMatch,

      /// <summary>An entry matches, on everything asked about.</summary>
      Matches,

      /// <summary>An entry matches what was asked about and holds a condition that was not - an IP range for an address asked alone, an authentication result.</summary>
      MatchesIf,

      /// <summary>The list keys on something not asked about, or is asked of the network.</summary>
      NotChecked,

      /// <summary>The list could not be read from the server.</summary>
      NotRead,

      /// <summary>
      /// An entry matches and adds its score, and the score cannot refuse the message:
      /// the allowed and blocked domains allow the sender's domain, which lifts every
      /// refusal by score (<see cref="SenderCheck.ApplyPrecedence"/>).
      /// </summary>
      AddsToScore,

      /// <summary>
      /// The server does not consult the list for this sender, because a list it meets
      /// first has decided: a spam exemption that covers it, or its domain allowed or
      /// blocked (<see cref="SenderCheck.ApplyPrecedence"/>).
      /// </summary>
      NotReached
   }

   /// <summary>One list's answer: the outcome, the entry that decided it, and the sentence that says it.</summary>
   public sealed class SenderCheckResult
   {
      internal SenderCheckResult(string page, SenderCheckOutcome outcome, string text, bool blocks = false)
      {
         Page = page;
         Outcome = outcome;
         Text = text;
         Blocks = blocks;
      }

      /// <summary>The navigation key of the list's page.</summary>
      public string Page { get; }

      public SenderCheckOutcome Outcome { get; }

      /// <summary>What the list says, in the reader's language.</summary>
      public string Text { get; }

      /// <summary>True when the entry that matched stops the sender rather than letting it through.</summary>
      public bool Blocks { get; }
   }

   /// <summary>
   /// "Check a sender": what each of the server-wide lists says about a sender
   /// address and a connecting IP address, by the rules the server matches them by
   /// (review finding F269 - there was no "is this sender on any list?"). Each
   /// method is one list and mirrors its server code: the spam exemptions
   /// (WhiteListCache: the IP within the range and the address matching the
   /// entry's wildcard, an empty one or * matching every address), the blocked
   /// senders (SenderPattern: an entry with an @ is one exact address, one without
   /// a domain and its subdomains at a label boundary; the highest score wins), the
   /// allow and block list (the same patterns, an expired entry skipped, the most
   /// specific entry wins and a block wins a tie), the greylisting exemptions (the
   /// connecting address against the entry's * and ? wildcards), the greylisting
   /// bypass list (GreyListingBypassEntry.Covers on the sender's domain, which
   /// then needs the authentication results the entry requires) and the allowed and
   /// blocked domains (DomainPolicyEntry.Covers: exact, or a .suffix covering the
   /// name and every name under it; an exact entry beats a suffix, a longer pattern
   /// a shorter one, and a block a tie). The HELO name is not asked, so entries
   /// matched against it alone are not checked, and the DNS block lists and SURBL
   /// are asked of the network when mail arrives, not here. Each list is asked on its
   /// own; <see cref="ApplyPrecedence"/> then makes the answers the server's, where a
   /// list it reads first keeps it from reading a later one - and where the connecting
   /// address's IP range keeps it from reading any (<see cref="ConnectingRange"/>;
   /// release review finding CP-6). A sender that signs in meets none of the lists but
   /// the allow and block list, which the card says, since the check cannot know it.
   /// </summary>
   public static class SenderCheck
   {
      /// <summary>The navigation key of the IP ranges page, whose answer stands before the lists' when an address is given.</summary>
      public const string RangesPage = "ipranges"; // no-loc: a page key

      /// <summary>A list that could not be read: the reason last, as a failure's sentence ends.</summary>
      public static SenderCheckResult NotRead(string page, string why)
         => new SenderCheckResult(page, SenderCheckOutcome.NotRead, F("Could not read the list: {0}", why));

      /// <summary>
      /// The answers as the server acts on them: each list was asked on its own, and the
      /// server reads them in order, where a list it meets first can decide that a later
      /// one is never consulted - which the answers asked one by one did not say, so a
      /// sender a spam exemption covers read "Lets it through" on its exemption and
      /// "Stops it" on its blocked domain and on the blocked senders below it, while the
      /// server accepts its mail with no check at all. In the server's order:
      /// <list type="number">
      /// <item>A spam exemption that covers the sender makes the session one the spam
      /// protection skips (SMTPConnection::InitializeSpamProtectionType_ sets SPNone):
      /// the allowed and blocked domains are not consulted (CheckDomainPolicy_ returns at
      /// once), and no spam test - the blocked senders, the DNS block lists, SURBL - nor
      /// greylisting runs (GetDoSpamProtection_ is false). The allow and block list still
      /// applies, at delivery.</item>
      /// <item>A blocked domain is refused at MAIL FROM (CheckDomainPolicy_), before any
      /// later list is read.</item>
      /// <item>An allowed domain is never greylisted (the RCPT TO gate reads
      /// DomainPolicyWhite_) and no score refuses or quarantines it (DoSpamProtection_),
      /// though the tests still run: a blocked sender's score still counts, towards
      /// marking it as spam.</item>
      /// </list>
      /// Before all of them, the connecting address's IP range, where one was given
      /// (<see cref="ConnectingRange"/>): a connection no range contains is refused before
      /// any list is read, and a range whose anti-spam is off makes the session one the
      /// spam protection skips, as a covering exemption does - the range is asked first
      /// (InitializeSpamProtectionType_), so not even the exemptions are read.
      /// <paramref name="inGuideOrder"/> holds one answer a list, in
      /// <see cref="SenderListGuide.Rows"/>' order, after the range's when there is one.
      /// </summary>
      public static IReadOnlyList<SenderCheckResult> ApplyPrecedence(IReadOnlyList<SenderCheckResult> inGuideOrder)
      {
         if (inGuideOrder == null)
            return Array.Empty<SenderCheckResult>();

         var results = inGuideOrder.ToList();

         SenderCheckResult range = Answer_(results, RangesPage);
         if (range?.Outcome == SenderCheckOutcome.Matches)
         {
            string unread = range.Blocks
               ? L("Not reached: the server refuses the connection before the sender is given.")
               : L("Not consulted: the connecting address's IP range has anti-spam off, so the server runs no spam check and does not greylist the session.");
            for (int at = 0; at < results.Count; at++)
            {
               string page = results[at].Page;
               if (page == RangesPage || (!range.Blocks && page == "senderlist")) // no-loc: page keys
                  continue;
               results[at] = new SenderCheckResult(page, SenderCheckOutcome.NotReached, unread);
            }
            return results;
         }

         SenderCheckResult exemption = Answer_(results, "spamwhitelist"); // no-loc: page keys
         if (exemption?.Outcome == SenderCheckOutcome.Matches)
         {
            string skipped = L("Not consulted: the spam exemption covers this sender, so the server checks neither its domain nor its score, and does not greylist it.");
            for (int at = 0; at < results.Count; at++)
            {
               if (SkippedForAnExemption_.Contains(results[at].Page))
                  results[at] = new SenderCheckResult(results[at].Page, SenderCheckOutcome.NotReached, skipped);
            }
            return results;
         }

         int domainAt = results.FindIndex(result => result.Page == "domainpolicylist"); // no-loc
         SenderCheckResult domain = domainAt < 0 ? null : results[domainAt];
         if (domain?.Outcome != SenderCheckOutcome.Matches)
            return results;

         if (domain.Blocks)
         {
            string refused = L("Not reached: the message is refused at MAIL FROM.");
            for (int at = domainAt + 1; at < results.Count; at++)
               results[at] = new SenderCheckResult(results[at].Page, SenderCheckOutcome.NotReached, refused);
            return results;
         }

         string neverGreylisted = L("Not consulted: the allowed domain is never greylisted.");
         for (int at = 0; at < results.Count; at++)
         {
            SenderCheckResult result = results[at];
            if (result.Page == "greylistwhitelist" || result.Page == "greylistbypass") // no-loc
               results[at] = new SenderCheckResult(result.Page, SenderCheckOutcome.NotReached, neverGreylisted);
            else if (result.Page == "blockedsenders" && result.Outcome == SenderCheckOutcome.Matches && result.Blocks) // no-loc
               results[at] = new SenderCheckResult(result.Page, SenderCheckOutcome.AddsToScore,
                  Sentences(result.Text, L("The allowed domain means the score cannot refuse or quarantine the message, though it can still mark it as spam.")));
         }

         return results;
      }

      /// <summary>
      /// The line above the rows, and what a screen reader hears when a check is done:
      /// which list decides what happens to the sender, at that list's level, with its
      /// sentence - "The list that decides is Allowed and blocked domains. The entry
      /// mass-mail.example blocks this domain: its mail is refused at MAIL FROM." Of the
      /// answers <see cref="ApplyPrecedence"/> gave, in the server's order: a spam
      /// exemption that covers the sender, unless the allow and block list then blocks it
      /// at delivery; an exemption on a condition not asked about, which decides if the
      /// condition holds; a blocked domain; a blocked sender, whose score refuses the
      /// message; the allow and block list, whose word overrules the spam verdict at
      /// delivery; and an allowed domain. With none of them, the spam checks and their
      /// score decide. A list that could not be read leaves the answer open, and says so.
      /// </summary>
      public static (StatusLevel Level, string Text) Summary(IReadOnlyList<SenderCheckResult> answers)
      {
         answers ??= Array.Empty<SenderCheckResult>();
         SenderCheckResult range = Answer_(answers, RangesPage);
         SenderCheckResult exemption = Answer_(answers, "spamwhitelist"); // no-loc: page keys
         SenderCheckResult domain = Answer_(answers, "domainpolicylist");
         SenderCheckResult blocked = Answer_(answers, "blockedsenders");
         SenderCheckResult list = Answer_(answers, "senderlist");

         static bool Matches(SenderCheckResult answer) => answer?.Outcome == SenderCheckOutcome.Matches;

         SenderCheckResult deciding = null;
         bool conditional = false;
         if (Matches(range))
            deciding = !range.Blocks && Matches(list) && list.Blocks ? list : range;
         else if (Matches(exemption))
            deciding = Matches(list) && list.Blocks ? list : exemption;
         else if (exemption?.Outcome == SenderCheckOutcome.MatchesIf)
            (deciding, conditional) = (exemption, true);
         else if (Matches(domain) && domain.Blocks)
            deciding = domain;
         else if (Matches(blocked) && blocked.Blocks)
            deciding = blocked;
         else if (Matches(list))
            deciding = list;
         else if (Matches(domain))
            deciding = domain;

         StatusLevel level;
         string text;
         if (deciding == null)
         {
            level = StatusLevel.Information;
            text = L("No list decides for this sender: the spam checks and their score do.");
         }
         else if (deciding.Page == RangesPage)
         {
            level = Pill(deciding).Level;
            text = F("The IP ranges decide. {0}", deciding.Text);
         }
         else
         {
            level = Pill(deciding).Level;
            string title = L(NavigationMap.TitleOf(deciding.Page));
            text = conditional
               ? F("The list that decides is {0}, if its condition holds. {1}", title, deciding.Text)
               : F("The list that decides is {0}. {1}", title, deciding.Text);
         }

         if (answers.Any(answer => answer.Outcome == SenderCheckOutcome.NotRead))
         {
            text = Sentences(text, L("A list could not be read, so the answer may change once it can be."));
            if (level < StatusLevel.Warning)
               level = StatusLevel.Warning;
         }

         return (level, text);
      }

      // The lists a spam exemption keeps the server from reading: every one but itself
      // and the allow and block list, which delivery reads.
      private static readonly HashSet<string> SkippedForAnExemption_ = new HashSet<string>(StringComparer.Ordinal)
      {
         "domainpolicylist", "blockedsenders", "dnsbl", "greylistwhitelist", "greylistbypass", "surbl" // no-loc: page keys
      };

      private static SenderCheckResult Answer_(IReadOnlyList<SenderCheckResult> results, string page)
         => results.FirstOrDefault(result => result.Page == page);

      /// <summary>
      /// The pill a list's answer is drawn with: its level and its word. A match that
      /// stops the sender is a warning, one that lets it through good, one on a
      /// condition information, a list that could not be read critical, and the rest -
      /// no match, not checked, not reached - say nothing by their colour.
      /// </summary>
      public static (StatusLevel Level, string Word) Pill(SenderCheckResult result)
      {
         switch (result?.Outcome)
         {
            case SenderCheckOutcome.Matches:
               return result.Blocks ? (StatusLevel.Warning, L("Stops it")) : (StatusLevel.Good, L("Lets it through"));
            case SenderCheckOutcome.AddsToScore:
               return (StatusLevel.Warning, L("Adds to the score"));
            case SenderCheckOutcome.MatchesIf:
               return (StatusLevel.Information, L("Depends"));
            case SenderCheckOutcome.NotRead:
               return (StatusLevel.Critical, L("Not read"));
            case SenderCheckOutcome.NotChecked:
               return (StatusLevel.Normal, L("Not checked"));
            case SenderCheckOutcome.NotReached:
               return (StatusLevel.Normal, L("Not reached"));
            default:
               return (StatusLevel.Normal, L("No match"));
         }
      }

      /// <summary>The DNS block lists and SURBL, which are asked of the network as mail arrives.</summary>
      public static SenderCheckResult AskedOfTheNetwork(string page)
         => new SenderCheckResult(page, SenderCheckOutcome.NotChecked,
            page == "surbl"
               ? L("Not checked here: SURBL is asked about the links in a message once its body has arrived.")
               : L("Not checked here: the DNS block lists are asked about the connecting address over DNS as mail arrives."));

      // ---- the IP range of the connecting address ---------------------------------

      /// <summary>One IP range: its name, its two ends, its priority and whether its anti-spam is on.</summary>
      public sealed record IpRange(string Name, string LowerIp, string UpperIp, int Priority, bool AntiSpam);

      /// <summary>
      /// The IP range a connection from <paramref name="ip"/> is given, as the server gives
      /// it (PersistentSecurityRange::ReadMatchingIP): of the ranges of the address's own
      /// family that contain it, the one of the highest priority, and of two that share it
      /// the first in <paramref name="ranges"/>' order - the IP ranges page's - where the
      /// server's database decides. Null for no address, one that is not an address, or
      /// one that no range contains.
      /// </summary>
      public static IpRange RangeOf(IEnumerable<IpRange> ranges, string ip)
      {
         System.Net.IPAddress connecting = ParseIp_(ip);
         if (connecting == null)
            return null;

         IpRange best = null;
         foreach (IpRange range in ranges ?? Array.Empty<IpRange>())
         {
            System.Net.IPAddress lower = ParseIp_(range.LowerIp), upper = ParseIp_(range.UpperIp);
            if (lower == null || upper == null || !WithinRange_(connecting, lower, upper))
               continue;
            if (best == null || range.Priority > best.Priority)
               best = range;
         }

         return best;
      }

      /// <summary>
      /// What the connecting address's IP range does to the sender before any list is
      /// read, or null where it does nothing to the answers - no address given, or a range
      /// that runs the spam checks. A connection that no range contains is refused at
      /// connect (SessionManager, TCPServer), before a greeting; a range whose anti-spam is
      /// off sets the session's spam protection to none (SMTPConnection::
      /// InitializeSpamProtectionType_, before the exemptions are asked), so that no spam
      /// test, no greylisting and no domain policy runs (GetDoSpamProtection_,
      /// CheckDomainPolicy_), and only the allow and block list applies, at delivery. Check
      /// a sender read none of it: a blocked domain read "refused at MAIL FROM" for mail
      /// from an office range with anti-spam off, which the server accepts with no check
      /// at all (release review finding CP-6).
      /// </summary>
      public static SenderCheckResult ConnectingRange(IEnumerable<IpRange> ranges, string ip)
      {
         System.Net.IPAddress connecting = ParseIp_(ip);
         if (connecting == null)
            return null;

         IpRange range = RangeOf(ranges, ip);
         if (range == null)
            return new SenderCheckResult(RangesPage, SenderCheckOutcome.Matches,
               F("No IP range contains {0}, so the server refuses a connection from it before the sender is given.", connecting.ToString()), blocks: true);

         if (range.AntiSpam)
            return null;

         return new SenderCheckResult(RangesPage, SenderCheckOutcome.Matches,
            F("The address is in the IP range {0}, whose anti-spam is off: the server runs no spam check and does not greylist a session from it, and of the lists below only the allow and block list applies, at delivery.", (range.Name ?? "").Trim()));
      }

      // ---- the spam exemptions --------------------------------------------------

      /// <summary>One spam exemption: an IP range and an address wildcard (empty or * for every address).</summary>
      public sealed record SpamExemption(string LowerIp, string UpperIp, string Address, string Description);

      public static SenderCheckResult SpamExemptions(IEnumerable<SpamExemption> entries, string address, string ip)
      {
         const string page = "spamwhitelist";
         address = Clean_(address);
         System.Net.IPAddress connecting = ParseIp_(ip);
         SpamExemption conditional = null;

         foreach (SpamExemption entry in entries ?? Array.Empty<SpamExemption>())
         {
            System.Net.IPAddress lower = ParseIp_(entry.LowerIp), upper = ParseIp_(entry.UpperIp);
            if (lower == null || upper == null)
               continue;

            string wildcard = (entry.Address ?? "").Trim();
            bool everyAddress = wildcard.Length == 0 || wildcard == "*";
            bool everyIp = CoversEveryAddress_(lower, upper);

            bool? ipMatches = connecting == null ? (everyIp ? true : (bool?)null) : WithinRange_(connecting, lower, upper);
            bool? addressMatches = everyAddress ? true : address.Length == 0 ? (bool?)null : WildcardMatch_(wildcard.ToLowerInvariant(), address);

            if (ipMatches == false || addressMatches == false)
               continue;
            if (ipMatches == true && addressMatches == true)
               return new SenderCheckResult(page, SenderCheckOutcome.Matches,
                  F("The exemption {0} covers this sender, so it skips every spam check.", Describe_(entry)));

            conditional ??= entry;
         }

         if (conditional != null)
         {
            return new SenderCheckResult(page, SenderCheckOutcome.MatchesIf, connecting == null
               ? F("The exemption {0} covers this sender only when it connects from {1} - {2}.", Describe_(conditional), conditional.LowerIp, conditional.UpperIp)
               : F("The exemption {0} covers this sender only when its address matches {1}.", Describe_(conditional), conditional.Address));
         }

         return new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No exemption covers this sender."));
      }

      private static string Describe_(SpamExemption entry)
      {
         string address = string.IsNullOrWhiteSpace(entry.Address) ? "*" : entry.Address.Trim();
         string range = entry.LowerIp == entry.UpperIp ? entry.LowerIp : entry.LowerIp + " - " + entry.UpperIp;
         return string.IsNullOrWhiteSpace(entry.Description)
            ? F("{0} from {1}", address, range)
            : F("\"{0}\" ({1} from {2})", entry.Description.Trim(), address, range);
      }

      // ---- the blocked senders --------------------------------------------------

      /// <summary>One blocked sender: an address or a domain, and the score a match adds.</summary>
      public sealed record BlockedSender(string Address, int Score);

      public static SenderCheckResult BlockedSenders(IEnumerable<BlockedSender> entries, string address)
      {
         const string page = "blockedsenders";
         address = Clean_(address);
         if (address.Length == 0)
            return new SenderCheckResult(page, SenderCheckOutcome.NotChecked, L("Not checked: this list matches the sender's address, and none was given."));

         BlockedSender best = null;
         foreach (BlockedSender entry in entries ?? Array.Empty<BlockedSender>())
         {
            if (PatternMatches(entry.Address, address) && (best == null || entry.Score > best.Score))
               best = entry;
         }

         return best == null
            ? new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No blocked sender matches this address."))
            : new SenderCheckResult(page, SenderCheckOutcome.Matches,
               F("The entry {0} matches, adding {1} to the score.", best.Address.Trim(), best.Score), blocks: true);
      }

      // ---- the allow and block list (the server's) ------------------------------

      /// <summary>One entry of the server's allow and block list.</summary>
      public sealed record SenderListEntry(string Value, bool Blocked, bool Expires, DateTime ExpiresAt, string Reason);

      public static SenderCheckResult SenderList(IEnumerable<SenderListEntry> entries, string address, DateTime now)
      {
         const string page = "senderlist";
         address = Clean_(address);
         if (address.Length == 0)
            return new SenderCheckResult(page, SenderCheckOutcome.NotChecked, L("Not checked: this list matches the sender's address, and none was given."));

         SenderListEntry best = null;
         int bestSpecificity = -1;
         foreach (SenderListEntry entry in entries ?? Array.Empty<SenderListEntry>())
         {
            if (entry.Expires && entry.ExpiresAt <= now)
               continue;
            if (!PatternMatches(entry.Value, address))
               continue;

            int specificity = Specificity(entry.Value);
            if (specificity > bestSpecificity || (specificity == bestSpecificity && entry.Blocked && !best.Blocked))
            {
               best = entry;
               bestSpecificity = specificity;
            }
         }

         if (best == null)
            return new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No entry matches this address, so the list leaves it to the spam checks."));

         return best.Blocked
            ? new SenderCheckResult(page, SenderCheckOutcome.Matches,
               F("The entry {0} blocks this sender for every recipient whose own list and domain's list say nothing.", best.Value.Trim()), blocks: true)
            : new SenderCheckResult(page, SenderCheckOutcome.Matches,
               F("The entry {0} allows this sender for every recipient whose own list and domain's list say nothing.", best.Value.Trim()));
      }

      // ---- the greylisting exemptions -------------------------------------------

      public static SenderCheckResult GreylistingExemptions(IEnumerable<string> addresses, string ip)
      {
         const string page = "greylistwhitelist";
         ip = (ip ?? "").Trim();
         if (ip.Length == 0)
            return new SenderCheckResult(page, SenderCheckOutcome.NotChecked, L("Not checked: this list matches the connecting IP address, and none was given."));

         System.Net.IPAddress parsed = ParseIp_(ip);
         string connecting = parsed != null ? parsed.ToString() : ip;
         foreach (string pattern in addresses ?? Array.Empty<string>())
         {
            string entry = (pattern ?? "").Trim();
            if (entry.Length > 0 && WildcardMatch_(entry.ToLowerInvariant(), connecting.ToLowerInvariant()))
               return new SenderCheckResult(page, SenderCheckOutcome.Matches,
                  F("The exemption {0} covers this address, so it is never greylisted.", entry));
         }

         return new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No exemption covers this address."));
      }

      // ---- the greylisting bypass list ------------------------------------------

      /// <summary>One bypass entry: a domain or a .suffix, what it is matched against (GreyListingBypassEntry's 1 envelope, 2 HELO, 3 both) and what it requires.</summary>
      public sealed record BypassEntry(string Pattern, int MatchOn, bool RequireSpf, bool RequireDkim, bool RequireDmarc);

      public const int MatchEnvelope = 1;
      public const int MatchHelo = 2;
      public const int MatchBoth = 3;

      public static SenderCheckResult GreylistingBypass(IEnumerable<BypassEntry> entries, string address)
      {
         const string page = "greylistbypass";
         string domain = DomainOf_(Clean_(address));
         if (domain.Length == 0)
            return new SenderCheckResult(page, SenderCheckOutcome.NotChecked, L("Not checked: this list matches the sender's domain or the HELO name, and no address was given."));

         foreach (BypassEntry entry in entries ?? Array.Empty<BypassEntry>())
         {
            if (entry.MatchOn == MatchHelo || !BypassCovers(entry.Pattern, domain))
               continue;

            var needs = new List<string>();
            if (entry.RequireSpf)
               needs.Add("SPF"); // no-loc: the name of the check
            if (entry.RequireDkim)
               needs.Add("DKIM"); // no-loc
            if (entry.RequireDmarc)
               needs.Add("DMARC"); // no-loc

            return needs.Count == 0
               ? new SenderCheckResult(page, SenderCheckOutcome.Matches, F("The entry {0} lets this sender past greylisting.", entry.Pattern.Trim()))
               : new SenderCheckResult(page, SenderCheckOutcome.MatchesIf,
                  F("The entry {0} lets this sender past greylisting when the message passes what it requires: {1}.", entry.Pattern.Trim(), string.Join(", ", needs)));
         }

         return new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No entry covers this sender's domain; entries matched against the HELO name are not checked here."));
      }

      /// <summary>GreyListingBypassEntry::Covers: exact, or a .suffix covering the name and every name under it at a label boundary.</summary>
      public static bool BypassCovers(string pattern, string name)
      {
         string entry = (pattern ?? "").Trim().ToLowerInvariant();
         string candidate = (name ?? "").Trim().ToLowerInvariant();
         if (entry.Length == 0 || candidate.Length == 0)
            return false;

         if (entry[0] != '.')
            return candidate == entry;

         return candidate == entry.Substring(1) || (candidate.Length > entry.Length && candidate.EndsWith(entry, StringComparison.Ordinal));
      }

      // ---- the allowed and blocked domains ---------------------------------------

      /// <summary>
      /// The allowed and blocked domains, from the document the server keeps
      /// ({"items":[{"pattern":..,"match_on":"envelope|helo|both","action":"white|black"}]}).
      /// </summary>
      public static SenderCheckResult DomainPolicy(string document, string address)
      {
         const string page = "domainpolicylist";
         string domain = NormaliseName_(DomainOf_(Clean_(address)));
         if (domain.Length == 0)
            return new SenderCheckResult(page, SenderCheckOutcome.NotChecked, L("Not checked: this list matches the sender's domain or the HELO name, and no address was given."));

         (string Pattern, bool Black)? best = null;
         foreach ((string pattern, string matchOn, bool black) in DomainPolicyRows(document))
         {
            if (matchOn == "helo" || !DomainPolicyCovers(pattern, domain)) // no-loc: the server's word
               continue;
            if (best == null || Beats_(pattern, black, best.Value.Pattern, best.Value.Black))
               best = (pattern, black);
         }

         if (best == null)
            return new SenderCheckResult(page, SenderCheckOutcome.NoMatch, L("No entry covers this sender's domain; entries matched against the HELO name are not checked here."));

         return best.Value.Black
            ? new SenderCheckResult(page, SenderCheckOutcome.Matches, F("The entry {0} blocks this domain: its mail is refused at MAIL FROM.", best.Value.Pattern), blocks: true)
            : new SenderCheckResult(page, SenderCheckOutcome.Matches, F("The entry {0} allows this domain: its mail is not greylisted or refused by score.", best.Value.Pattern));
      }

      /// <summary>The rows of the server's domain policy document; none for a document that cannot be read.</summary>
      public static IReadOnlyList<(string Pattern, string MatchOn, bool Black)> DomainPolicyRows(string document)
      {
         var rows = new List<(string, string, bool)>();
         if (string.IsNullOrWhiteSpace(document))
            return rows;

         try
         {
            using JsonDocument json = JsonDocument.Parse(document);
            if (json.RootElement.ValueKind != JsonValueKind.Object || !json.RootElement.TryGetProperty("items", out JsonElement items) || items.ValueKind != JsonValueKind.Array)
               return rows;

            foreach (JsonElement item in items.EnumerateArray())
            {
               string pattern = NormalisePattern_(Text_(item, "pattern"));
               if (pattern.Length == 0)
                  continue;
               rows.Add((pattern, Text_(item, "match_on").Trim().ToLowerInvariant(), string.Equals(Text_(item, "action").Trim(), "black", StringComparison.OrdinalIgnoreCase)));
            }
         }
         catch (JsonException)
         {
            // A document that cannot be read is a list of nothing here; its own page says why.
         }

         return rows;
      }

      /// <summary>DomainPolicyEntry::Covers: exact, or a .suffix covering the name and every name under it.</summary>
      public static bool DomainPolicyCovers(string pattern, string normalisedName)
      {
         if (string.IsNullOrEmpty(pattern) || string.IsNullOrEmpty(normalisedName))
            return false;
         if (!(pattern.Length > 1 && pattern[0] == '.'))
            return normalisedName == pattern;
         return normalisedName == pattern.Substring(1) || (normalisedName.Length > pattern.Length && normalisedName.EndsWith(pattern, StringComparison.Ordinal));
      }

      // DomainPolicyList's Beats: an exact entry beats a suffix, a longer pattern a shorter one, and a block a tie.
      private static bool Beats_(string candidate, bool candidateBlack, string held, bool heldBlack)
      {
         bool candidateSuffix = candidate.Length > 1 && candidate[0] == '.';
         bool heldSuffix = held.Length > 1 && held[0] == '.';
         if (candidateSuffix != heldSuffix)
            return !candidateSuffix;
         if (candidate.Length != held.Length)
            return candidate.Length > held.Length;
         return candidateBlack && !heldBlack;
      }

      // ---- the patterns the blocked senders and the allow and block list share ---

      /// <summary>
      /// SenderPattern::Matches: an entry with an @ matches that one address
      /// exactly; one without matches the domain and its subdomains at a label
      /// boundary; a leading @ is dropped; a URL entry matches no address; and
      /// the null sender matches nothing. Case does not matter.
      /// </summary>
      public static bool PatternMatches(string pattern, string address)
      {
         string from = Clean_(address);
         if (from.Length == 0)
            return false;

         string entry = Canonical_(pattern);
         if (entry.Length == 0 || entry.EndsWith("/", StringComparison.Ordinal))
            return false;

         if (entry.Contains('@'))
            return from == entry;

         int at = from.LastIndexOf('@');
         if (at < 0)
            return false;

         string domain = from.Substring(at + 1);
         return domain == entry || (domain.Length > entry.Length + 1 && domain.EndsWith("." + entry, StringComparison.Ordinal));
      }

      /// <summary>SenderPattern::Specificity: an address 1000, a domain one more than its labels.</summary>
      public static int Specificity(string pattern)
      {
         string entry = Canonical_(pattern);
         if (entry.Length == 0)
            return 0;
         if (entry.Contains('@'))
            return 1000;
         return 2 + entry.Count(c => c == '.');
      }

      // A URL entry ends in "/" once canonical, as the server's does, so that it matches no address.
      private static string Canonical_(string pattern)
      {
         string entry = (pattern ?? "").Trim().ToLowerInvariant();
         if (entry.StartsWith("@", StringComparison.Ordinal))
            entry = entry.Substring(1);
         if (entry.StartsWith("http://", StringComparison.Ordinal) || entry.StartsWith("https://", StringComparison.Ordinal) || entry.Contains('/'))
            return entry.TrimEnd('/') + "/";
         return entry;
      }

      // ---- helpers ----------------------------------------------------------------

      /// <summary>StringParser::WildcardMatch: * any run of characters, ? exactly one, the rest literally, the whole string.</summary>
      internal static bool WildcardMatch_(string pattern, string value)
      {
         int p = 0, v = 0, star = -1, mark = 0;
         while (v < value.Length)
         {
            if (p < pattern.Length && (pattern[p] == '?' || pattern[p] == value[v]))
            {
               p++;
               v++;
            }
            else if (p < pattern.Length && pattern[p] == '*')
            {
               star = p++;
               mark = v;
            }
            else if (star >= 0)
            {
               p = star + 1;
               v = ++mark;
            }
            else
            {
               return false;
            }
         }

         while (p < pattern.Length && pattern[p] == '*')
            p++;
         return p == pattern.Length;
      }

      private static string Clean_(string address) => (address ?? "").Trim().ToLowerInvariant();

      private static string DomainOf_(string address)
      {
         int at = address.LastIndexOf('@');
         return at < 0 ? address : address.Substring(at + 1);
      }

      // DomainPolicyList::NormaliseName: trimmed, lower case, a trailing dot dropped, an address literal none.
      private static string NormaliseName_(string name)
      {
         string value = (name ?? "").Trim().ToLowerInvariant();
         if (value.StartsWith("[", StringComparison.Ordinal))
            return "";
         return value.TrimEnd('.');
      }

      // DomainPolicyEntry::SetPattern: trimmed, lower case, a trailing dot dropped (a lone dot kept).
      private static string NormalisePattern_(string pattern)
      {
         string value = (pattern ?? "").Trim().ToLowerInvariant();
         while (value.Length > 1 && value.EndsWith(".", StringComparison.Ordinal))
            value = value.Substring(0, value.Length - 1);
         return value;
      }

      private static string Text_(JsonElement item, string name)
         => item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

      private static System.Net.IPAddress ParseIp_(string text)
      {
         text = (text ?? "").Trim();
         return text.Length > 0 && System.Net.IPAddress.TryParse(text, out System.Net.IPAddress parsed) ? parsed : null;
      }

      // IPAddress::WithinRange, for two addresses of the address's own family.
      private static bool WithinRange_(System.Net.IPAddress address, System.Net.IPAddress lower, System.Net.IPAddress upper)
      {
         if (address.AddressFamily != lower.AddressFamily || address.AddressFamily != upper.AddressFamily)
            return false;
         return Compare_(address, lower) >= 0 && Compare_(address, upper) <= 0;
      }

      private static bool CoversEveryAddress_(System.Net.IPAddress lower, System.Net.IPAddress upper)
      {
         byte[] low = lower.GetAddressBytes(), high = upper.GetAddressBytes();
         return low.Length == high.Length && low.All(b => b == 0) && high.All(b => b == 0xFF);
      }

      private static int Compare_(System.Net.IPAddress left, System.Net.IPAddress right)
      {
         byte[] a = left.GetAddressBytes(), b = right.GetAddressBytes();
         for (int i = 0; i < a.Length && i < b.Length; i++)
         {
            if (a[i] != b[i])
               return a[i].CompareTo(b[i]);
         }
         return a.Length.CompareTo(b.Length);
      }
   }
}
