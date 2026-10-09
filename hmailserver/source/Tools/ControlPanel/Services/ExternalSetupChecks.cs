// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>Ordered so that aggregating an item is Max over its findings:
   /// the last member is the worst.</summary>
   public enum SetupItemState
   {
      NotNeeded,
      Done,
      CannotTell,
      ActionNeeded
   }

   /// <summary>One per-domain / per-port / per-certificate line inside an item.</summary>
   public sealed class SetupFinding
   {
      public SetupItemState State;
      public string Text;
   }

   /// <summary>
   /// One external prerequisite: what it is for, its aggregated state, the
   /// concrete thing to do about it, and the navigation key of the page that
   /// owns its settings.
   /// </summary>
   public sealed class SetupItem
   {
      public string Title;
      public SetupItemState State;

      /// <summary>One sentence: what the feature is for.</summary>
      public string Purpose;

      /// <summary>The concrete thing to do, and where to do it.</summary>
      public string Action;

      /// <summary>Navigation key of the page that owns the settings, or null.</summary>
      public string Page;

      public readonly List<SetupFinding> Findings = new();

      public void Add(SetupItemState state, string text) => Findings.Add(new SetupFinding { State = state, Text = text });

      /// <summary>The item is as bad as its worst finding.</summary>
      public void AggregateFromFindings()
      {
         if (Findings.Count > 0)
            State = Findings.Max(f => f.State);
      }
   }

   /// <summary>
   /// The computation behind "what does this server still need done OUTSIDE it":
   /// eleven prerequisite checks (DNS records, key and CA files, trusted lists,
   /// identity-provider key material) read from COM, hMailServer.INI and the
   /// local disk, each yielding one of four honest states.
   ///
   /// This lived inside <c>Views/ExternalSetupView</c> until the dashboard needed
   /// the same answer for its "needs attention" card. It is one class used by
   /// both, rather than two copies, because two copies of a checklist drift and
   /// then disagree in front of the administrator - which is worse than no
   /// checklist. The Setup checklist page renders every item; the dashboard
   /// renders the aggregate and the items that need action.
   ///
   /// Usage, in three steps that mirror the threading rules of the data:
   ///
   ///   1. <see cref="Run"/> on the UI thread - it reads apartment-bound COM
   ///      objects the way every page does, plus INI and local files, and
   ///      schedules a DNS TXT probe for each record it wants to verify.
   ///   2. <see cref="ResolveDnsLookups"/> on a background thread - each lookup
   ///      can block for the resolver's full timeout, several seconds when the
   ///      record is missing, which is the common case right after publishing.
   ///      It touches no COM object: everything the probes need was copied into
   ///      plain strings during the build.
   ///   3. <see cref="ApplyDnsResults"/> back on the UI thread - it rewrites the
   ///      "checking..." findings into verdicts and re-aggregates the items the
   ///      caller is rendering.
   ///
   /// Honesty rule, learned the hard way from fifteen documented overclaims in
   /// this project: nothing here asserts a status it did not actually determine.
   /// Where the answer is knowable from COM, the local filesystem or a TXT
   /// lookup it is checked; where it is not, the item says "Cannot tell" and
   /// exactly what to check instead.
   /// </summary>
   public sealed class ExternalSetupChecks
   {
      private ExternalSetupChecks()
      {
      }

      /// <summary>
      /// Reads the configuration and builds the checklist. Call on the UI
      /// thread: the COM objects it reads are apartment-bound there, and the
      /// synchronous part is the same cost every settings page pays on open.
      /// </summary>
      public static ExternalSetupChecks Run()
      {
         var checks = new ExternalSetupChecks();
         checks.items_ = checks.BuildItems();
         return checks;
      }

      /// <summary>The checklist, one item per external prerequisite.</summary>
      public IReadOnlyList<SetupItem> Items => items_;

      /// <summary>How many COM/INI reads failed while building; the items are
      /// incomplete when this is non-zero.</summary>
      public int FailedReads => failedReads_;

      /// <summary>The first read failure, described for the administrator, or null.</summary>
      public string FirstError => firstError_;

      /// <summary>
      /// True when a read failed because the link to the service had gone. The
      /// checklist then says so at Critical, as the shell's pill does, rather than
      /// as a Warning about values the server could not give.
      /// </summary>
      public bool LinkLost => linkLost_;

      /// <summary>
      /// How many DNS TXT lookups this run scheduled. Zero means the items are
      /// already final; otherwise the findings those lookups belong to say
      /// "checking..." until <see cref="ApplyDnsResults"/> rewrites them.
      /// </summary>
      public int DnsLookupCount => probes_.Count;

      /// <summary>
      /// Performs the scheduled DNS lookups, blocking until all have answered
      /// or timed out. Call on a background thread; it reads only the plain
      /// strings captured at build time, never a COM object.
      /// </summary>
      public void ResolveDnsLookups()
      {
         Parallel.ForEach(probes_, probe => probe.Result = DnsTxtLookup.Query(probe.Host));
      }

      /// <summary>
      /// Rewrites the "checking..." findings from the completed lookups and
      /// re-aggregates every item's state. Call on the UI thread, after
      /// <see cref="ResolveDnsLookups"/>, because it mutates the findings the
      /// caller is rendering.
      /// </summary>
      public void ApplyDnsResults()
      {
         foreach (DnsProbe probe in probes_)
            ApplyProbe(probe);

         foreach (SetupItem item in items_)
            item.AggregateFromFindings();
      }

      /// <summary>The state as the word the administrator reads. The word is
      /// always in the rendered text - never only a colour or a shape.</summary>
      public static string StateWord(SetupItemState state)
      {
         switch (state)
         {
            case SetupItemState.Done: return L("Done");
            case SetupItemState.NotNeeded: return L("Not needed");
            case SetupItemState.ActionNeeded: return L("Action needed");
            default: return L("Cannot tell");
         }
      }

      /// <summary>
      /// Reuses the application-wide status vocabulary (colour token + distinct
      /// shape) so the checklist's badges mean the same thing they mean everywhere
      /// else. The words, though, are the checklist's own four states.
      /// </summary>
      public static StatusLevel LevelFor(SetupItemState state)
      {
         switch (state)
         {
            case SetupItemState.Done: return StatusLevel.Good;
            case SetupItemState.NotNeeded: return StatusLevel.Normal;
            case SetupItemState.ActionNeeded: return StatusLevel.Warning;
            default: return StatusLevel.Information;
         }
      }

      // ---- reading the configuration -------------------------------------------

      private int failedReads_;
      private string firstError_;

      // Whether a read failed because the link to the service had gone: one event,
      // shown at one level, Critical, wherever it is shown.
      private bool linkLost_;

      /// <summary>The items of this run, kept so the DNS pass can update them in place.</summary>
      private List<SetupItem> items_ = new();

      /// <summary>DNS lookups scheduled during the current build; run after rendering.</summary>
      private readonly List<DnsProbe> probes_ = new();

      /// <summary>
      /// File and INI checks are only meaningful on the machine the server runs
      /// on: every path this page inspects is a path the SERVER will open, and
      /// IniFeatureStore locates hMailServer.INI through the local registry and
      /// service table. Connected to a remote host, checking this machine's disk
      /// would answer a question nobody asked - so those checks honestly degrade
      /// to "Cannot tell" instead.
      /// </summary>
      private static bool IsLocalServer() => ServerSession.IsLocalSession;

      private T Read<T>(Func<T> read)
      {
         try
         {
            return read();
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
            return default;
         }
      }

      /// <summary>What this page needs to know about one hosted domain.</summary>
      private sealed class DomainDkim
      {
         // Every string starts empty rather than null. These are filled from an
         // object initialiser whose right-hand sides are dynamic COM reads, which the
         // compiler cannot see as assignments - hence CS0649 on the last of them - and
         // more usefully, a domain row that fails to read part-way would otherwise
         // leave a null here for code that goes straight to .Length.
         public string Name = "";
         public bool Active;
         public bool SignEnabled;
         public string Selector = "";
         public string KeyFile = "";
         public string SecondarySelector = "";
         public string SecondaryKeyFile = "";
      }

      private List<DomainDkim> ReadDomains()
      {
         var result = new List<DomainDkim>();
         dynamic domains = null;
         try
         {
            domains = ServerSession.Current.Application.Domains;
            int count = (int)domains.Count;
            for (int i = 0; i < count; i++)
            {
               dynamic domain = domains.Item[i];
               try
               {
                  result.Add(new DomainDkim
                  {
                     Name = ((string)domain.Name ?? "").Trim(),
                     Active = (bool)domain.Active,
                     SignEnabled = (bool)domain.DKIMSignEnabled,
                     Selector = ((string)domain.DKIMSelector ?? "").Trim(),
                     KeyFile = ((string)domain.DKIMPrivateKeyFile ?? "").Trim(),
                     SecondarySelector = ((string)domain.DKIMSecondarySelector ?? "").Trim(),
                     SecondaryKeyFile = ((string)domain.DKIMSecondaryPrivateKeyFile ?? "").Trim()
                  });
               }
               finally
               {
                  ServerSession.Release((object)domain);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
         }
         finally
         {
            ServerSession.Release((object)domains);
         }
         return result;
      }

      /// <summary>
      /// True when a PEM private key file is passphrase-protected. These are the
      /// two markers OpenSSL's own loader keys off: the PKCS#8 "ENCRYPTED PRIVATE
      /// KEY" block, and the PKCS#1 "Proc-Type: 4,ENCRYPTED" header. Reading only
      /// the head of the file: both markers sit before the key material.
      /// </summary>
      private static bool LooksLikeEncryptedPem(string path)
      {
         using var reader = new StreamReader(path);
         var buffer = new char[4096];
         int read = reader.Read(buffer, 0, buffer.Length);
         string head = new string(buffer, 0, Math.Max(read, 0));
         return head.Contains("ENCRYPTED PRIVATE KEY") || head.Contains("Proc-Type: 4,ENCRYPTED");
      }

      // ---- DNS probes -----------------------------------------------------------

      private enum ProbeKind
      {
         DkimPrimary,
         DkimSecondary,
         MtaSts,
         Spf,
         Dmarc
      }

      /// <summary>
      /// One DNS TXT lookup scheduled while the items were built, applied to its
      /// finding when the background pass completes. The probes run off the UI
      /// thread because DnsTxtLookup.Query blocks for the resolver's own timeout
      /// - several seconds when the record is missing, which is the common case
      /// right after publishing, and the one this page exists for.
      /// </summary>
      private sealed class DnsProbe
      {
         public ProbeKind Kind;
         public string Host;
         public string Label;

         /// <summary>Expected DKIM record ("v=DKIM1; p=..."), derived from the
         /// domain's private key file when it is readable, or null when only the
         /// record's presence can be judged.</summary>
         public string Expected;

         public SetupFinding Target;
         public DnsTxtLookup.LookupResult Result;
      }

      /// <summary>
      /// The DKIM record this domain's key SHOULD publish, derived from the
      /// private key file - the p= tag is the SubjectPublicKeyInfo of the key
      /// pair, so it can be computed without any secret leaving this process.
      /// Null when the file is unreadable or encrypted, in which case the DNS
      /// check degrades to presence-only rather than guessing at the value.
      /// </summary>
      private static string TryDeriveDkimExpectedRecord(string keyFile)
      {
         try
         {
            string pem = File.ReadAllText(keyFile);
            using var rsa = RSA.Create();
            rsa.ImportFromPem(pem);
            return "v=DKIM1; p=" + Convert.ToBase64String(rsa.ExportSubjectPublicKeyInfo()); // no-loc
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return null;
         }
      }

      /// <summary>RFC 8461: a policy-discovery record begins "v=STSv1".</summary>
      private static bool HasStsMarker(IEnumerable<string> records)
         => records.Any(r => (r ?? "").Replace(" ", "").StartsWith("v=STSv1", StringComparison.OrdinalIgnoreCase));

      /// <summary>
      /// Rewrites a probe's finding from "checking..." into the four-way verdict
      /// DnsTxtLookup keeps apart on purpose: found-and-right, found-but-wrong
      /// (waiting will not help), not found (publish, or keep waiting), and
      /// lookup-failed (which says nothing about the record at all).
      /// </summary>
      private static void ApplyProbe(DnsProbe probe)
      {
         SetupFinding finding = probe.Target;
         DnsTxtLookup.LookupResult result = probe.Result;

         if (result == null || result.Status == DnsTxtLookup.LookupStatus.Failed)
         {
            finding.State = SetupItemState.CannotTell;
            finding.Text = F("{0}: the DNS lookup for {1} failed - {2} This says nothing about whether the record exists; check this machine's DNS resolver and press Refresh.", probe.Label, probe.Host, (result != null ? result.Error : L("no result.")));
            return;
         }

         bool matches = probe.Expected != null && result.Records.Any(r => DnsTxtLookup.Matches(r, probe.Expected));

         switch (probe.Kind)
         {
            case ProbeKind.DkimPrimary:
               if (result.Status == DnsTxtLookup.LookupStatus.NoRecord)
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: no TXT record was found at {1}, so receivers cannot verify this domain's signatures and may treat its mail as forged. Publish the record at your DNS host - or, if you just did, wait for propagation and press Refresh.", probe.Label, probe.Host);
               }
               else if (probe.Expected == null)
               {
                  finding.State = SetupItemState.CannotTell;
                  finding.Text = F("{0}: a TXT record exists at {1}, but whether it carries the right public key could not be verified - the private key file was not readable from this panel.", probe.Label, probe.Host);
               }
               else if (matches)
               {
                  finding.State = SetupItemState.Done;
                  finding.Text = F("{0}: the TXT record at {1} is published and carries this key's public key.", probe.Label, probe.Host);
               }
               else
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: what is published at {1} does not carry this key's public key, so signatures fail verification. Fix the record's value - waiting for propagation will not help.", probe.Label, probe.Host);
               }
               return;

            case ProbeKind.DkimSecondary:
               if (result.Status == DnsTxtLookup.LookupStatus.NoRecord)
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: the secondary key is staged but no TXT record exists at {1} yet. Publish it and let it propagate BEFORE promoting the secondary key, or mail will sign with a key the world cannot look up.", probe.Label, probe.Host);
               }
               else if (probe.Expected == null)
               {
                  finding.State = SetupItemState.CannotTell;
                  finding.Text = F("{0}: a TXT record exists at {1}, but it could not be compared with the staged secondary key from this panel. Confirm the value before promoting.", probe.Label, probe.Host);
               }
               else if (matches)
               {
                  finding.State = SetupItemState.Done;
                  finding.Text = F("{0}: the rotation record at {1} is published and matches the staged secondary key. Promote whenever it has propagated everywhere.", probe.Label, probe.Host);
               }
               else
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: what is published at {1} does not match the staged secondary key. Fix the value before promoting - waiting will not help.", probe.Label, probe.Host);
               }
               return;

            case ProbeKind.Spf:
            case ProbeKind.Dmarc:
               {
                  // Presence of the record the receiving server would find: a TXT at
                  // the domain beginning v=spf1, a TXT at _dmarc.<domain> beginning
                  // v=DMARC1. TXT records that exist without either are "no record"
                  // for the purpose - the receiver ignores them too.
                  string kind = probe.Kind == ProbeKind.Spf ? "SPF" : "DMARC"; // no-loc
                  string marker = probe.Kind == ProbeKind.Spf ? "v=spf1" : "v=DMARC1"; // no-loc
                  bool published = result.Status == DnsTxtLookup.LookupStatus.Found &&
                     result.Records.Any(r => (r ?? "").Trim().StartsWith(marker, StringComparison.OrdinalIgnoreCase));
                  finding.State = published ? SetupItemState.Done : SetupItemState.ActionNeeded;
                  finding.Text = published
                     ? F("{0}: the {1} record at {2} is published.", probe.Label, kind, probe.Host)
                     : F("{0}: no {1} record at {2}.", probe.Label, kind, probe.Host);
                  return;
               }

            default: // MtaSts
               if (result.Status == DnsTxtLookup.LookupStatus.NoRecord)
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: no TXT record at {1}, so sending servers never discover the policy and the hosting does nothing for this domain. Publish it (v=STSv1; id=...) - or, if you just did, wait for propagation and press Refresh.", probe.Label, probe.Host);
               }
               else if (HasStsMarker(result.Records))
               {
                  finding.State = SetupItemState.Done;
                  finding.Text = F("{0}: the {1} TXT record is published. Not checkable from here: that mta-sts.{2} resolves to this server, and that the certificate covers that name.", probe.Label, probe.Host, probe.Label);
               }
               else
               {
                  finding.State = SetupItemState.ActionNeeded;
                  finding.Text = F("{0}: nothing published at {1} begins with v=STSv1, so senders will not recognise a policy there. Fix the record's value.", probe.Label, probe.Host);
               }
               return;
         }
      }

      // ---- the checklist itself -------------------------------------------------

      private List<SetupItem> BuildItems()
      {
         bool local = IsLocalServer();

         // The INI store finds hMailServer.INI through this machine's registry
         // and service table, so it is only consulted when this machine IS the
         // server; otherwise it would happily describe some other install.
         IniFeatureStore ini = local ? new IniFeatureStore() : null;
         bool iniReadable = ini != null && ini.IsAvailable;
         string iniExcuse = local
            ? L("hMailServer.INI was not found on this machine, so its settings cannot be read from here.")
            : F("the Control Panel is connected to '{0}'; hMailServer.INI and files on disk can only be inspected on the server machine itself.", (ServerSession.Current != null ? ServerSession.Current.Host : "?"));

         List<DomainDkim> domains = ReadDomains();

         var items = new List<SetupItem>
         {
            DkimItem(domains, local),
            ArcFilteringItem(),
            ArcSealingItem(domains, ini, iniReadable, iniExcuse),
            MtaStsItem(domains, ini, iniReadable, iniExcuse),
            TlsRptItem(ini, iniReadable, iniExcuse),
            DaneItem(ini, iniReadable),
            EncryptedKeysItem(local, iniExcuse),
            AcmeItem(ini, iniReadable, iniExcuse),
            ClientCertificatesItem(local, iniExcuse),
            OAuth2Item(ini, iniReadable, iniExcuse, local),
            AutoconfigItem(ini, iniReadable, iniExcuse),
            SandboxItem(),
            ServiceAccountItem(ini, iniReadable, iniExcuse, local)
         };

         // The first hour comes first: a server with no domain has no use for a
         // DANE record.
         List<SetupItem> firstHour = FirstHourItems();

         // The host name belongs with the first hour, after the domain and the
         // account: it is what every server this one delivers to is told, and the
         // SPF and PTR records that follow are written for it.
         firstHour.Insert(firstHour.FindLastIndex(item => item.Page == "domains") + 1, HostNameItem());
         items.InsertRange(0, firstHour);

         return items;
      }

      /// <summary>
      /// The host name this server announces in HELO/EHLO (Settings.HostName), which
      /// the server's own first-hour list does not judge: empty, the server announces
      /// its Windows computer name, which no receiver can look up; set, it should be
      /// the full DNS name its public address's PTR record names
      /// (Services/HostNameVerdict).
      /// </summary>
      private SetupItem HostNameItem()
      {
         var item = new SetupItem
         {
            Title = L("The host name - the name this server gives other servers"),
            Purpose = L("Every server this one delivers to is told its host name and compares it with DNS: the name's address, and the PTR record of the address the mail came from. A name that does not match counts against the mail."),
            Action = L("On the Protocols page's SMTP tab, set Host name to this server's full DNS name, such as mail.example.com. Then ask whoever owns the server's public address for a PTR record naming it; the DNS records page shows what to ask for."),
            Page = "protocols"
         };

         int failuresBefore = failedReads_;
         string hostName = Read(() =>
         {
            dynamic settings = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               return (string)settings.HostName ?? "";
            }
            finally
            {
               ServerSession.Release((object)settings);
            }
         });

         if (failedReads_ != failuresBefore)
         {
            item.Add(SetupItemState.CannotTell, L("The host name could not be read from the server."));
         }
         else
         {
            string name = (hostName ?? "").Trim();
            switch (HostNameVerdict.Judge(name))
            {
               case HostNameVerdict.Kind.Empty:
                  item.Add(SetupItemState.ActionNeeded,
                     L("No host name is set, so this server announces its Windows computer name, which no DNS on the Internet knows."));
                  break;
               case HostNameVerdict.Kind.NotADnsName:
                  item.Add(SetupItemState.ActionNeeded,
                     F("The host name {0} is not a full DNS name, such as mail.example.com, so no DNS record can agree with it.", name));
                  break;
               case HostNameVerdict.Kind.PrivateName:
                  item.Add(SetupItemState.ActionNeeded,
                     F("The host name {0} ends in a name no public DNS answers for, so a receiver on the Internet cannot look it up.", name));
                  break;
               default:
                  item.Add(SetupItemState.Done,
                     F("This server announces itself as {0}. Whether the PTR record of its public address names it, and the name's own address record points back, cannot be told from here: the DNS records page says what to ask for.", name));
                  break;
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      // ---- the first hour, judged by the server ----------------------------------

      /// <summary>
      /// The first-hour items the server judges itself (Common/Util/SetupChecklist.h):
      /// a setting profile on a young server, a domain, an account, TLS on the submission port, passwords only over TLS, no password on port 25, SPF, DMARC, a backup
      /// destination, a second factor on the administrator password and the stored-secrets key made and exported. Read over
      /// COM as Utilities.GetSetupChecklist without DNS lookups - the SPF and DMARC
      /// records are looked up here, off the window thread, by the same probes the
      /// DKIM item uses - and rendered from the finding codes, so that the sentence
      /// is this panel's in every language while the judgement is the server's. The
      /// server's DKIM item is left out: the DKIM item below verifies the record
      /// against the key file, which the server's presence check does not. So is
      /// its service_account item, which FirstHourItem does not know: the service
      /// account item below reads the service manager and hMailServer.INI and says
      /// what was asked for beside what the service runs as.
      /// </summary>
      private List<SetupItem> FirstHourItems()
      {
         var items = new List<SetupItem>();

         string json = Read(() => (string)ServerSession.Current.Application.Utilities.GetSetupChecklist(false));
         if (json == null)
         {
            SetupItem unread = FirstHourItem("domain");
            unread.Add(SetupItemState.CannotTell, F("The server's checklist could not be read: {0}", firstError_ ?? ""));
            unread.AggregateFromFindings();
            items.Add(unread);
            return items;
         }

         try
         {
            using JsonDocument document = JsonDocument.Parse(json);
            foreach (JsonElement element in document.RootElement.GetProperty("items").EnumerateArray())
            {
               string key = element.GetProperty("key").GetString() ?? "";
               SetupItem item = key == "dkim" ? null : FirstHourItem(key);
               if (item == null)
                  continue;

               foreach (JsonElement finding in element.GetProperty("findings").EnumerateArray())
                  AddFirstHourFinding(item, key, finding);

               item.AggregateFromFindings();
               items.Add(item);
            }
         }
         catch (Exception ex) when (ex is JsonException || ex is KeyNotFoundException || ex is InvalidOperationException)
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
         }

         return items;
      }

      /// <summary>What each first-hour item is for and where it is done, or null for a key this panel does not know.</summary>
      private static SetupItem FirstHourItem(string key)
      {
         switch (key)
         {
            // The first item on a young server (docs/Profiles.md): the profile that
            // sets the settings the rest of the first hour is spent finding.
            case "profile":
               return new SetupItem
               {
                  Title = L("A profile for this server"),
                  Purpose = L("A profile sets the thirty or so settings one kind of server needs - which, to what and why - shows the difference from this server before anything changes, and keeps what it replaces so that it can be put back."),
                  Action = L("On the Setting profiles page, compare the four with this server and apply the one that fits; what it replaces is kept, so that it can be put back."),
                  Page = "profiles"
               };
            case "domain":
               return new SetupItem
               {
                  Title = L("A domain - the server needs one before it can receive mail"),
                  Purpose = L("Mail is addressed to a domain, so the server needs one before it can receive anything."),
                  Action = L("Add the domain on the Domains page. Its name is the part after the @ in every address it will hold."),
                  Page = "domains"
               };
            case "account":
               return new SetupItem
               {
                  Title = L("An account - a mailbox in a domain"),
                  Purpose = L("An account is a mailbox in a domain: the address people write to, and what a client signs in as."),
                  Action = L("Add an account under the domain on the Domains page: an address, and the password a mail client signs in with."),
                  Page = "domains"
               };
            case "submission":
               return new SetupItem
               {
                  Title = L("TLS on the submission port - the port mail clients send through"),
                  Purpose = L("Mail clients send through the submission port, and without TLS their passwords cross the network in the clear."),
                  Action = L("On the TCP/IP ports page, give the submission port (587) STARTTLS with a certificate from the SSL certificates page, or add port 465 with implicit TLS."),
                  Page = "ports"
               };
            // docs/SecureDefaults.md: a new installation starts with this done; an
            // upgraded server is told here. The Control Deck's checklist offers the
            // one-click fix the finding carries; here the page is one click away.
            case "sign_in_tls":
               return new SetupItem
               {
                  Title = L("Passwords only over TLS - from any address but this computer's"),
                  Purpose = L("A mail client signs in with a password, and from any address but this computer's the password should cross the network only inside TLS."),
                  Action = L("Where a port named below has no TLS, give it a certificate and STARTTLS on the TCP/IP ports page first; then, on the IP ranges page, turn on Require TLS for auth for each range named below. This computer's own clients are not affected."),
                  Page = "ipranges"
               };
            // GitLab work item #54 (docs/SmtpAuthOnDeliveryPorts.md): a new
            // installation's port 25 is a honeypot already; an upgraded server's is
            // On until the administrator changes it, which is what arms the ban - so
            // the step is theirs, on the page, and the finding carries no fix.
            case "delivery_port_auth":
               return new SetupItem
               {
                  Title = L("No password on port 25 - the port other servers deliver to"),
                  Purpose = L("Port 25 is where other servers deliver mail, and no mail client should sign in there: an AUTH on it is a password being guessed."),
                  Action = L("On the TCP/IP ports page, open the SMTP port 25 and set its SMTP authentication to Honeypot or Off, then reinitialise. Mail clients that still sign in on 25 are refused and banned from then on, so move them to 587 first."),
                  Page = "ports"
               };
            case "spf":
               return new SetupItem
               {
                  Title = L("An SPF record - which servers may send this domain's mail"),
                  Purpose = L("SPF tells receivers which servers may send this domain's mail; without it the mail is more likely to be refused or filed as spam."),
                  Action = L("At your DNS host, publish a TXT record at the domain, such as v=spf1 mx -all, naming the servers that send its mail. The DNS records page shows the record to publish."),
                  Page = "dnsrecords"
               };
            case "dmarc":
               return new SetupItem
               {
                  Title = L("A DMARC record - what receivers do with mail that fails SPF and DKIM"),
                  Purpose = L("DMARC tells receivers what to do with mail that fails SPF and DKIM, and where to report it."),
                  Action = L("At your DNS host, publish a TXT record named _dmarc.<domain>, starting with v=DMARC1; p=none while you read the reports, then p=quarantine or p=reject. The DNS records page shows the record to publish."),
                  Page = "dnsrecords"
               };
            case "backup":
               return new SetupItem
               {
                  Title = L("A backup destination - until one is set there is no backup"),
                  Purpose = L("The backup writes the database and the message store to a folder; until one is set there is no backup."),
                  Action = L("On the Backup page, set the folder the backup is written to, on a disk other than the one the message store is on."),
                  Page = "backup"
               };
            case "second_factor":
               return new SetupItem
               {
                  Title = L("A second factor for the administrator - a one-time code beside the password"),
                  Purpose = L("The administrator password opens everything; a one-time code beside it keeps a leaked password from being enough."),
                  Action = L("On the Administrative access page's Two-factor tab, enrol an authenticator app for the administrator password."),
                  Page = "adminaccess"
               };
            // The key the database's second factors and [Settings] secrets are sealed
            // under (docs/SecretsAtRest.md): made and exported on the server itself, by
            // an elevated verb, so there is no page of this panel's to send anybody to.
            case "stored_secrets_key":
               return new SetupItem
               {
                  Title = L("The stored-secrets key - what the database's secrets are sealed under"),
                  Purpose = L("The key an account's second factor and the [Settings] secrets are sealed under in the database. It exists on the server alone, so a copy has to be exported and kept with the database backups."),
                  Action = L("On the server, from an elevated prompt in the Bin folder: hMailServer.exe --create-stored-secrets-key makes the key, and hMailServer.exe --export-stored-secrets-key FILE exports it. Keep the file with the database backups, stored apart from them."),
                  Page = null
               };
            default:
               return null;
         }
      }

      /// <summary>
      /// The finding codes the server writes (SetupChecklist.cpp says what each
      /// means), each with this panel's sentence for it. A code that is not here is
      /// shown in the server's own words, so a newer server is not silenced by an
      /// older panel.
      /// </summary>
      private static readonly Dictionary<string, string> FirstHourTexts = new()
      {
         ["profile_none"] = N("No profile has been applied. A profile sets about thirty settings for the kind of server this is - a small office, behind a filtering gateway, an outbound relay, a high-volume sender - and shows every change before it is made."),
         ["profile_in_service"] = N("This server holds {0} accounts, so it is in service: a profile is offered to a new server, and any of the four can still be compared with this one."),
         ["profile_applied"] = N("The {0} profile was applied on {1}."),
         ["profile_kept"] = N("The {0} profile was applied on {1}; its way back was discarded and its settings kept."),
         ["profile_reverted"] = N("The {0} profile was applied and then reverted on {1}; the settings are the ones it replaced."),
         ["profile_declined"] = N("The settings this version ships with were chosen on {0}, and no profile applied; any of the four can still be compared with this server."),
         ["profile_failed"] = N("The {0} profile could not be applied in full on {1}: some of its settings were not written. Put back what it wrote, or keep it, and apply it again."),
         ["domain_none"] = N("No domain has been added yet."),
         ["delivery_port_auth_on"] = N("Port {0} still accepts AUTH: it is where other servers deliver mail, and a password tried there is a guess. Set its SMTP authentication to Honeypot or Off, and sign mail clients in on 587."),
         ["delivery_port_auth_honeypot"] = N("Port {0} takes no password: EHLO offers AUTH there as bait, and an address that sends one is refused and banned."),
         ["delivery_port_auth_off"] = N("Port {0} takes no password: EHLO does not offer AUTH there, and an address that sends one anyway is refused and banned."),
         ["delivery_port_auth_no_ban"] = N("Port {0} takes no password, but AutoBanMinutes is 0, so an address that sends AUTH there is refused and not banned."),
         ["delivery_port_auth_disabled"] = N("Port {0} does not offer AUTH: DisableAUTHList names it, and an AUTH sent there is refused without a ban."),
         ["delivery_port_none"] = N("No SMTP listener is on port 25, so no delivery port takes a password."),
         ["domain_none_active"] = N("{0} domains, none of them active."),
         ["domain_count"] = N("Active domains: {0} of {1}."),
         ["sample_domain"] = N("{0} is the sample domain and is not counted here; remove it when you are done with it."),
         ["account_none"] = N("No account has been added in any domain."),
         ["account_count"] = N("Accounts: {0}."),
         ["submission_none"] = N("No port takes message submission, so mail clients cannot send through this server."),
         ["submission_plain"] = N("Port {0} takes submission without TLS, so passwords and mail would cross the network in the clear."),
         ["submission_no_certificate"] = N("Port {0} is set to TLS but has no certificate, so TLS cannot start."),
         ["submission_tls"] = N("Port {0} offers TLS with the certificate {1}."),
         ["submission_self_signed"] = N("Port {0} offers TLS with {1}, the self-signed certificate this installation made: mail clients warn about it until it is replaced with one a certificate authority issued, which ACME can fetch from Let's Encrypt."),
         ["sign_in_plain"] = N("The IP range {0} takes a password without TLS on port {1}, so a client there that signs in before STARTTLS sends it across the network in the clear."),
         ["sign_in_no_tls"] = N("The IP range {0} takes a password without TLS, and port {1} has no TLS a client could start: give those ports a certificate and STARTTLS first, then require TLS for sign-in from the range."),
         ["sign_in_tls_required"] = N("The IP range {0} takes a password only over TLS."),
         ["sign_in_tls_ports"] = N("Every port the IP range {0} may use starts TLS before it takes a password."),
         ["sign_in_local_only"] = N("Only this computer may connect, so no password crosses a network."),
         ["no_active_domain"] = N("No active domain, so there is nothing to publish yet."),
         ["record_published"] = N("{0}: the {1} record at {2} is published."),
         ["record_missing"] = N("{0}: no {1} record at {2}."),
         ["record_not_looked_up"] = N("{0}: the {1} record at {2} was not looked up."),
         ["lookup_failed"] = N("{0}: the lookup for {2} failed, which says nothing about the {1} record."),
         ["domains_not_looked_up"] = N("{0} more active domains were not looked up."),
         ["backup_none"] = N("No backup destination is set."),
         ["backup_folder_missing"] = N("The backup destination {0} does not exist on the server."),
         ["backup_set"] = N("Backups go to {0}."),
         ["second_factor_on"] = N("The administrator's password is backed by a one-time code."),
         ["second_factor_off"] = N("The administrator signs in with a password alone."),
         ["stored_secrets_off"] = N("ProtectStoredSecretsWithDPAPI is 0, so an account's second factor and the [Settings] secrets are stored as written, and no stored-secrets key is needed."),
         ["stored_secrets_key_none"] = N("No stored-secrets key has been made, so an account's second factor and the [Settings] secrets are stored as written. Make one on the server with {0}, then export it."),
         ["stored_secrets_key_not_held"] = N("The database's second factors and [Settings] secrets are sealed under the stored-secrets key {0}, which this server does not hold, so they do not open here. Import it on the server from one that holds it: {1}."),
         ["stored_secrets_key_not_exported"] = N("The stored-secrets key {0} has never been exported. Export it on the server with {1} and keep the file with the database backups: a database restored without it opens no second factor and no [Settings] secret."),
         ["stored_secrets_key_exported"] = N("The stored-secrets key {0} was exported on {1}; keep that file with the database backups."),
         ["stored_secrets_key_unreadable"] = N("The database's record of its stored-secrets key could not be read, so whether this server holds the database's key cannot be told.")
      };

      private void AddFirstHourFinding(SetupItem item, string key, JsonElement finding)
      {
         string code = finding.TryGetProperty("code", out JsonElement codeElement) ? codeElement.GetString() ?? "" : "";
         var args = new List<string>();
         if (finding.TryGetProperty("args", out JsonElement argsElement) && argsElement.ValueKind == JsonValueKind.Array)
         {
            foreach (JsonElement arg in argsElement.EnumerateArray())
               args.Add(arg.GetString() ?? "");
         }

         // The server was asked not to look the records up, so that this panel's
         // window thread is not held for the resolver's timeout; the lookup is
         // scheduled here and lands when the background pass completes.
         if (code == "record_not_looked_up" && args.Count >= 3 && (key == "spf" || key == "dmarc"))
         {
            var pending = new SetupFinding
            {
               State = SetupItemState.CannotTell,
               Text = F("{0}: checking DNS for the {1} record at {2}…", args[0], args[1], args[2])
            };
            item.Findings.Add(pending);
            probes_.Add(new DnsProbe
            {
               Kind = key == "spf" ? ProbeKind.Spf : ProbeKind.Dmarc,
               Host = args[2],
               Label = args[0],
               Target = pending
            });
            return;
         }

         SetupItemState state = FirstHourState(finding.TryGetProperty("state", out JsonElement stateElement) ? stateElement.GetString() : null);
         object[] formatArgs = args.Cast<object>().ToArray();
         string text = FirstHourCount_(code, args, formatArgs)
            ?? (FirstHourTexts.TryGetValue(code, out string english)
               ? F(english, formatArgs)
               : (finding.TryGetProperty("text", out JsonElement textElement) ? textElement.GetString() : null) ?? code);
         item.Add(state, text);
      }

      /// <summary>
      /// A first-hour text that counts what its first argument counts, in the form
      /// the language in force has for that number (Loc.Plural); null for any other
      /// code, or a first argument that is not a number. The table above keeps the
      /// server's sentence for these codes as well, as the server words them.
      /// </summary>
      private static string FirstHourCount_(string code, List<string> args, object[] formatArgs)
      {
         if (args.Count == 0 || !long.TryParse(args[0], System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out long count))
            return null;

         return code switch
         {
            "profile_in_service" => PluralF(count,
               "This server holds {0} account, so it is in service: a profile is offered to a new server, and any of the four can still be compared with this one.",
               "This server holds {0} accounts, so it is in service: a profile is offered to a new server, and any of the four can still be compared with this one.",
               formatArgs),
            "domain_none_active" => PluralF(count, "{0} domain, and it is not active.", "{0} domains, none of them active.", formatArgs),
            "domains_not_looked_up" => PluralF(count, "{0} more active domain was not looked up.", "{0} more active domains were not looked up.", formatArgs),
            _ => null
         };
      }

      private static SetupItemState FirstHourState(string word)
      {
         switch (word)
         {
            case "done": return SetupItemState.Done;
            case "action_needed": return SetupItemState.ActionNeeded;
            case "not_needed": return SetupItemState.NotNeeded;
            default: return SetupItemState.CannotTell;
         }
      }

      /// <summary>
      /// The Windows account the service logs on as.
      ///
      /// It belongs on this checklist because it is decided outside hMailServer:
      /// the Service Control Manager holds it, /Register sets it from its command
      /// line alone (docs/ServiceAccount.md) - never from ServiceAccountName, which
      /// the service and hMailServer administrators can write - and the account
      /// needs file rights that are granted in Windows, by Grant access.
      ///
      /// The state is read from the Service Control Manager rather than inferred,
      /// so this reports what the service IS running as. A service account
      /// password an earlier version stored in hMailServer.INI is raised: it is a
      /// Windows credential in plain text that nothing reads any more.
      /// </summary>
      private SetupItem ServiceAccountItem(IniFeatureStore ini, bool iniReadable, string iniExcuse, bool local)
      {
         var item = new SetupItem
         {
            Title = L("Windows service account - a least-privilege account needs granting outside hMailServer"),
            Purpose = L("Which Windows account the hMailServer service logs on as. A new installation uses NT SERVICE\\hMailServer, a virtual account of its own that needs no password and reaches only the folders it is granted; an installation upgraded from an earlier version keeps LocalSystem, the most privileged account on the machine, until it is moved - and under LocalSystem a flaw reachable through SMTP, IMAP or POP3 would have full control of the computer."),
            Action = L("On the Server limits & expert settings page, card Windows service account: Move to NT SERVICE\\hMailServer grants the service its folders - asking first about any folder or key file the configuration names outside them - and moves it; then restart the service. Running the installer again offers the same choice. An external database with Windows authentication needs a login for the account as well."),
            Page = "hardening"
         };

         if (!local)
         {
            item.Add(SetupItemState.CannotTell, F("The service account cannot be read: {0}", iniExcuse));
            item.AggregateFromFindings();
            return item;
         }

         WindowsServiceInfo service = WindowsServiceInfo.Query();

         if (!service.Exists)
         {
            item.Add(SetupItemState.CannotTell,
               service.Error != null
                  ? F("The Service Control Manager could not be queried, so the account the service runs as is unknown: {0}", service.Error)
                  : L("Windows does not report an hMailServer service on this machine, so there is no account to show or move yet."));
            item.AggregateFromFindings();
            return item;
         }

         bool runningAsLocalSystem = string.Equals(
            WindowsServiceInfo.Canonical(service.StartName, Environment.MachineName), "localsystem",
            StringComparison.OrdinalIgnoreCase);

         if (runningAsLocalSystem)
         {
            item.Add(SetupItemState.ActionNeeded,
               F("The service is running as {0}, the most privileged account on the machine: a flaw reachable through SMTP, IMAP or POP3 would have full control of the computer. NT SERVICE\\hMailServer, the account a new installation uses, contains that.", WindowsServiceInfo.DescribeAccount(service.StartName)));
         }
         else
         {
            item.Add(SetupItemState.Done,
               F("The service is running as {0}, which is not LocalSystem, so it is already contained.", service.StartName));
         }

         if (iniReadable && ini.Read("ServiceAccountPassword", "").Trim().Length > 0)
         {
            // ActionNeeded, not CannotTell: a fact the panel just established by
            // reading the value's length. Only emptiness is tested; the value is
            // never read into the UI.
            item.Add(SetupItemState.ActionNeeded,
               L("A service account password from an earlier version is stored in hMailServer.INI in plain text, and nothing reads it any more: the service is registered from the installer or the buttons on the Windows service account card, and Windows keeps its own copy of any password. Delete the ServiceAccountPassword line from hMailServer.INI with an elevated editor, and the setting with DELETE /api/v1/settings/ini/ServiceAccountPassword, so that the database's copy goes too."));
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// DKIM signing. The server can hold the key and sign every message, but a
      /// signature is only worth anything once the matching public key is
      /// published in the domain's DNS zone - and DNS is the one place the
      /// server has no write access to. Verified against DKIMSigner::Sign: with
      /// signing enabled but no selector or key file, the server reports error
      /// 5305 and the mail simply goes out unsigned.
      /// </summary>
      private SetupItem DkimItem(List<DomainDkim> domains, bool local)
      {
         var item = new SetupItem
         {
            Title = L("DKIM signing - publish each domain's public key in DNS"),
            Purpose = L("Signs outgoing mail so receivers can verify it really came from your domain; without the DNS record every signature fails verification."),
            Action = L("At your DNS host, publish a TXT record named <selector>._domainkey.<domain> containing the public key (v=DKIM1; k=rsa; p=...). This page looks the record up through the Windows resolver, and where the key file is readable it also checks that the published key is the RIGHT key - a wrong value cannot be fixed by waiting."),
            Page = "domains"
         };

         var signing = domains.Where(d => d.SignEnabled).ToList();

         if (domains.Count == 0)
         {
            item.Add(SetupItemState.NotNeeded, L("No domains are configured, so there is nothing to sign and no record to publish."));
         }
         else if (signing.Count == 0)
         {
            item.Add(SetupItemState.NotNeeded, L("No domain has DKIM signing switched on, so no DNS record is required."));
         }
         else
         {
            foreach (DomainDkim domain in signing)
            {
               string label = domain.Active ? domain.Name : F("{0} (domain is inactive)", domain.Name);

               if (domain.Selector.Length == 0 || domain.KeyFile.Length == 0)
               {
                  // Verified: DKIMSigner reports error 5305 and returns without
                  // signing, so mail leaves unsigned while the checkbox says on.
                  item.Add(SetupItemState.ActionNeeded,
                     domain.Selector.Length == 0
                        ? F("{0}: signing is on but the selector is not set, so every message goes out unsigned (server error 5305). Set both on the domain, then publish the DNS record.", label)
                        : F("{0}: signing is on but the private key file is not set, so every message goes out unsigned (server error 5305). Set both on the domain, then publish the DNS record.", label));
               }
               else if (local && !File.Exists(domain.KeyFile))
               {
                  item.Add(SetupItemState.ActionNeeded,
                     F("{0}: the key file {1} does not exist on this machine, so signing fails and mail goes out unsigned. Restore the file or generate a new key, then publish the matching DNS record.", label, domain.KeyFile));
               }
               else
               {
                  string fileNote = local ? L("The key file exists on disk.") : L("The key file could not be checked from this machine.");
                  string host = domain.Selector + "._domainkey." + domain.Name;

                  var finding = new SetupFinding
                  {
                     State = SetupItemState.CannotTell,
                     Text = F("{0}: selector '{1}' and key file are set. {2} Checking DNS for the TXT record {3}…", label, domain.Selector, fileNote, host)
                  };
                  item.Findings.Add(finding);

                  probes_.Add(new DnsProbe
                  {
                     Kind = ProbeKind.DkimPrimary,
                     Host = host,
                     Label = label,
                     Expected = local ? TryDeriveDkimExpectedRecord(domain.KeyFile) : null,
                     Target = finding
                  });
               }

               if (domain.SecondarySelector.Length > 0)
               {
                  // Verified in DKIMSigner: only the primary pair ever signs; the
                  // secondary exists purely to stage a rotation, and promoting it
                  // before its record propagates makes every receiver fail the mail.
                  string host = domain.SecondarySelector + "._domainkey." + domain.Name;

                  var finding = new SetupFinding
                  {
                     State = SetupItemState.CannotTell,
                     Text = F("{0}: a secondary key is staged for rotation. Checking DNS for the TXT record {1}…", label, host)
                  };
                  item.Findings.Add(finding);

                  probes_.Add(new DnsProbe
                  {
                     Kind = ProbeKind.DkimSecondary,
                     Host = host,
                     Label = label,
                     Expected = local && domain.SecondaryKeyFile.Length > 0 && File.Exists(domain.SecondaryKeyFile)
                        ? TryDeriveDkimExpectedRecord(domain.SecondaryKeyFile)
                        : null,
                     Target = finding
                  });
               }
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// ARC inbound filtering. This is the clearest instance of the whole
      /// problem: SpamTestArc refuses to act unless the trusted-sealer list has
      /// entries, because a forwarder's seal is only worth anything from a
      /// forwarder the administrator trusts (RFC 8617 7.1 - anyone can fabricate
      /// a chain that validates). Naming who you trust is a judgement only the
      /// administrator can make, which is why the server cannot default it.
      ///
      /// Both values ARE readable now, through AntiSpam.ArcFilteringEnabled and
      /// AntiSpam.ArcTrustedSealers. They were not when this page was first
      /// written - they lived only in hm_settings with no COM property - and this
      /// item reported "cannot tell" plus a SELECT for the administrator to run.
      /// The COM accessors landed straight afterwards, so it reads them, because a
      /// page whose whole purpose is to say what is really configured must not
      /// keep claiming it cannot see something it can.
      /// </summary>
      private SetupItem ArcFilteringItem()
      {
         var item = new SetupItem
         {
            Title = L("ARC inbound filtering - name the forwarders you trust"),
            Purpose = L("Rescues legitimate forwarded mail from DMARC-failure penalties, but only for forwarders on your trusted-sealer list - with the list empty the feature does nothing at all, even when enabled."),
            Action = L("On the Anti-spam settings page, list the sealing domains you trust (the mailing-list and forwarding services your users rely on) and turn on the ARC offset. Naming who you trust is a judgement only you can make, which is why there is no default."),
            Page = "antispam"
         };

         int arcFailuresBefore = failedReads_;

         bool arcEnabled = Read(() =>
         {
            dynamic settings = null;
            dynamic antiSpam = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               antiSpam = settings.AntiSpam;
               return (bool)antiSpam.ArcFilteringEnabled;
            }
            finally
            {
               ServerSession.Release((object)antiSpam);
               ServerSession.Release((object)settings);
            }
         });

         string arcSealers = Read(() =>
         {
            dynamic settings = null;
            dynamic antiSpam = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               antiSpam = settings.AntiSpam;
               return (string)antiSpam.ArcTrustedSealers ?? "";
            }
            finally
            {
               ServerSession.Release((object)antiSpam);
               ServerSession.Release((object)settings);
            }
         });

         if (failedReads_ != arcFailuresBefore)
         {
            item.Add(SetupItemState.CannotTell,
               L("The ARC filtering settings could not be read from the server, so this page cannot say whether the feature is configured."));
         }
         else if (!arcEnabled)
         {
            item.Add(SetupItemState.NotNeeded,
               L("ARC filtering is off, so forwarded mail that fails DMARC is scored as it arrives. That is the default and it is a reasonable choice; turn it on only once you have forwarders whose seals you are willing to trust."));
         }
         else if (string.IsNullOrWhiteSpace(arcSealers))
         {
            // The exact shape this whole page exists to catch: switched on, looks
            // configured, does nothing whatsoever.
            item.Add(SetupItemState.ActionNeeded,
               L("ARC filtering is ENABLED but the trusted-sealer list is EMPTY, so it does nothing at all. That is deliberate, not a bug: anyone can fabricate a whole ARC chain and seal it with a key they publish in their own DNS, and it will validate perfectly - so a passing chain proves nothing unless you already trust the sealer. The list is not an option of the feature, it is the feature."));
         }
         else
         {
            item.Add(SetupItemState.Done,
               F("ARC filtering is enabled and trusts: {0}. Seals from any other domain are ignored, and matching is exact - a subdomain or a lookalike of a trusted name is not trusted.", arcSealers.Trim()));
         }

         // The one part that IS readable from here: ARC's score offset is defined
         // as minus the DMARC failure score, so a zero score makes ARC filtering
         // a no-op regardless of the sealer list. Verified in SpamTestArc::RunTest.
         //
         // Guarded on the read actually succeeding: Read returns default(int)=0
         // on a COM failure, and asserting "your score is 0" off a failed read
         // would be exactly the kind of overclaim this page exists to prevent.
         int failuresBefore = failedReads_;
         int dmarcFailureScore = Read(() =>
         {
            dynamic settings = null;
            dynamic antiSpam = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               antiSpam = settings.AntiSpam;
               return (int)antiSpam.DMARCFailureScore;
            }
            finally
            {
               ServerSession.Release((object)antiSpam);
               ServerSession.Release((object)settings);
            }
         });

         if (failedReads_ == failuresBefore && dmarcFailureScore <= 0)
         {
            item.Add(SetupItemState.ActionNeeded,
               F("The DMARC failure score is currently {0}. ARC works by offsetting that score, so at zero ARC filtering can do nothing even with trusted sealers listed. Set a positive DMARC failure score on the Anti-spam settings page first.", dmarcFailureScore));
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// ARC sealing of forwarded mail (ArcSealingEnabled in hMailServer.INI,
      /// default off). Verified in DKIMSigner::GetHostedDomainIdentity_: the seal
      /// borrows a hosted domain's primary DKIM selector and key, and with no
      /// domain fully configured the sealing path silently produces nothing.
      /// The external half is the same DKIM TXT record the signing item covers,
      /// because the next hop verifies the seal against that record.
      /// </summary>
      private SetupItem ArcSealingItem(List<DomainDkim> domains, IniFeatureStore ini, bool iniReadable, string iniExcuse)
      {
         var item = new SetupItem
         {
            Title = L("ARC sealing of forwarded mail - needs a domain DKIM key"),
            Purpose = L("Seals mail this server forwards so the next hop can still trust the original authentication results; the seal is made with a hosted domain's DKIM key."),
            Action = L("Give at least one active domain a DKIM selector and key file, and publish that domain's DKIM TXT record - the receiving server verifies the seal against the same record DKIM signing uses."),
            Page = "security"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("ArcSealingEnabled could not be read: {0}", iniExcuse));
         }
         else if (!ini.ReadBool("ArcSealingEnabled", false))
         {
            item.Add(SetupItemState.NotNeeded, L("ARC sealing is off (ArcSealingEnabled=0, the default), so nothing external is required."));
         }
         else
         {
            bool anySealingIdentity = domains.Any(d => d.Active && d.SignEnabled && d.Selector.Length > 0 && d.KeyFile.Length > 0);
            if (anySealingIdentity)
            {
               item.Add(SetupItemState.Done,
                  L("ARC sealing is on and at least one active domain has a DKIM selector and key to seal with. The DNS side is covered by the DKIM item above - the seal verifies against the same TXT record."));
            }
            else
            {
               item.Add(SetupItemState.ActionNeeded,
                  L("ARC sealing is on (ArcSealingEnabled=1) but no active domain has DKIM signing with a selector and key file configured, so forwarded mail is silently not sealed at all."));
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// MTA-STS policy hosting. Verified against WebServicesServer: hosting is
      /// enabled by default (MtaStsHostingEnabled=1) while both listener ports
      /// default to 0, so a stock install advertises the feature and serves
      /// nothing - the startup log now says so, and this item says it where an
      /// administrator will actually look. RFC 8461 3.3 requires the policy be
      /// fetched over HTTPS at https://mta-sts.&lt;domain&gt;/.well-known/mta-sts.txt,
      /// and the discovery record _mta-sts.&lt;domain&gt; lives in DNS, which the
      /// server cannot write.
      /// </summary>
      private SetupItem MtaStsItem(List<DomainDkim> domains, IniFeatureStore ini, bool iniReadable, string iniExcuse)
      {
         var item = new SetupItem
         {
            Title = L("MTA-STS policy hosting - HTTPS listener plus two DNS entries per domain"),
            Purpose = L("Tells sending servers they must use TLS when delivering to your domains, closing the downgrade hole in opportunistic TLS."),
            Action = L("Set the HTTPS port on the Web services & autoconfiguration page (RFC 8461 requires the policy over HTTPS), give the listener a certificate that covers mta-sts.<domain>, then for each domain publish an A/AAAA (or CNAME) record for mta-sts.<domain> pointing at this server and a TXT record at _mta-sts.<domain> (v=STSv1; id=...). This page checks the TXT record; the address record and the certificate's name coverage cannot be checked from here."),
            Page = "webservices"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("The MTA-STS hosting settings could not be read: {0}", iniExcuse));
            item.AggregateFromFindings();
            return item;
         }

         // Defaults below mirror IniFileSettings.cpp exactly: hosting on,
         // both ports off, no certificate file, ACME off.
         bool hosting = ini.ReadBool("MtaStsHostingEnabled", true);
         int httpsPort = ReadIniInt(ini, "WebServicesHttpsPort", 0);
         string certFile = ini.Read("WebServicesCertificateFile", "").Trim();
         bool acme = ini.ReadBool("AcmeEnabled", false);

         if (!hosting)
         {
            item.Add(SetupItemState.NotNeeded, L("MTA-STS hosting is off (MtaStsHostingEnabled=0), so nothing needs serving and no record should be published."));
         }
         else if (httpsPort <= 0)
         {
            item.Add(SetupItemState.ActionNeeded,
               L("MTA-STS hosting is enabled (MtaStsHostingEnabled=1, the shipped default) but WebServicesHttpsPort is 0, so the policy cannot be served at all - RFC 8461 only allows it over HTTPS. Set WebServicesHttpsPort if you want MTA-STS; if you do not, this row is safe to ignore, or set MtaStsHostingEnabled=0 to silence it."));
         }
         else if (certFile.Length == 0 && !acme)
         {
            // Verified: with no certificate configured and ACME off, the HTTPS
            // listener logs "No TLS certificate available yet" and stays down.
            item.Add(SetupItemState.ActionNeeded,
               L("WebServicesHttpsPort is set, but no certificate is configured (WebServicesCertificateFile is empty and ACME is off), so the HTTPS listener does not start and the policy still cannot be fetched. Set WebServicesCertificateFile/WebServicesPrivateKeyFile or enable ACME."));
         }
         else
         {
            item.Add(SetupItemState.Done, L("The server side is in place: hosting is enabled, the HTTPS port is set, and a certificate source exists."));

            foreach (DomainDkim domain in domains.Where(d => d.Active))
            {
               string host = "_mta-sts." + domain.Name;

               var finding = new SetupFinding
               {
                  State = SetupItemState.CannotTell,
                  Text = F("{0}: checking DNS for the {1} TXT record…", domain.Name, host)
               };
               item.Findings.Add(finding);

               probes_.Add(new DnsProbe
               {
                  Kind = ProbeKind.MtaSts,
                  Host = host,
                  Label = domain.Name,
                  Target = finding
               });
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// TLS-RPT sending. Verified against TlsRptReporterTask: with
      /// TlsRptFromAddress empty (the shipped default) the task collects
      /// statistics every hour and discards each completed day unsent - the
      /// deliberate, safe default, since a mail server should not start mailing
      /// third parties without being told to, but invisible without this row.
      /// Choosing the sender address is the administrator's call, not the
      /// server's, which is why it cannot default itself on.
      /// </summary>
      private SetupItem TlsRptItem(IniFeatureStore ini, bool iniReadable, string iniExcuse)
      {
         var item = new SetupItem
         {
            Title = L("TLS reporting (TLS-RPT) - set a sender address to start sending"),
            Purpose = L("Mails daily TLS-failure reports to domains that ask for them, so their operators learn when connections to them are being downgraded."),
            Action = L("Set TlsRptFromAddress in the [Settings] section of hMailServer.ini to a mailbox reports should be sent from. Leaving it empty is a valid choice - it just means the collected statistics are discarded and no reports are ever sent."),
            Page = "security"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("TlsRptFromAddress could not be read: {0}", iniExcuse));
         }
         else
         {
            string from = ini.Read("TlsRptFromAddress", "").Trim();
            if (from.Length > 0)
            {
               item.Add(SetupItemState.Done,
                  F("TlsRptFromAddress is set to {0}, so completed days of statistics are mailed to domains that request reports. Nothing external remains.", from));
            }
            else
            {
               item.Add(SetupItemState.ActionNeeded,
                  L("TlsRptFromAddress is empty (the default), so the server collects TLS statistics hourly and discards every completed day unsent. Set an address if you want reports to go out; ignore this row if you deliberately do not."));
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// DANE for mail sent TO this server. Entirely a DNS matter: TLSA records
      /// for the MX hosts, in a DNSSEC-signed zone, published at the registrar or
      /// DNS host - three things hMailServer has no ability to do, and this panel
      /// has no TLSA/DNSSEC lookup to verify with, so the state is honestly
      /// unknown. The one thing that IS verifiable from here is the interaction
      /// with ACME key rollover, which the AcmeReuseKey default exists for.
      /// </summary>
      private SetupItem DaneItem(IniFeatureStore ini, bool iniReadable)
      {
         var item = new SetupItem
         {
            Title = L("DANE for inbound mail - publish TLSA records for your MX hosts"),
            Purpose = L("Lets sending servers pin your certificate through DNSSEC, so nobody can strip or spoof TLS on mail coming to you."),
            Action = L("In a DNSSEC-signed zone, publish a TLSA record (usually usage 3, selector 1, matching type 1 - '3 1 1' with the SHA-256 of your certificate's public key) at _25._tcp.<mx-host> for each MX host. Verify with: nslookup -type=TLSA _25._tcp.<mx-host>. hMailServer cannot publish DNS records or sign your zone, and this panel has no TLSA lookup to check them with."),
            Page = "security"
         };

         item.Add(SetupItemState.CannotTell,
            L("Whether TLSA records exist, and whether your zone is DNSSEC-signed, can only be seen in DNS. If you have never set up DNSSEC for the zone, no TLSA record will validate."));

         if (iniReadable && ini.ReadBool("AcmeEnabled", false) && !ini.ReadBool("AcmeReuseKey", true))
         {
            // Verified: AcmeReuseKey defaults to 1 precisely so '3 1 1' TLSA
            // records survive renewals; with it off, every renewal generates a
            // new key and silently invalidates any published record.
            item.Add(SetupItemState.ActionNeeded,
               L("ACME is enabled with AcmeReuseKey=0, so every certificate renewal generates a new private key. If you publish '3 1 1' TLSA records they will break on the next renewal - set AcmeReuseKey=1 (the default), or update the TLSA record after every renewal."));
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// Encrypted private keys. Verified against SslContextInitializer: when
      /// OpenSSL finds a key file passphrase-protected and no PrivateKeyPassword
      /// is configured for the certificate, it reports error 6170, the key is
      /// not loaded, and every listener bound to that certificate stays down.
      /// The passphrase was chosen by whoever produced the key file - outside
      /// this server - which is why the server cannot supply it itself.
      /// </summary>
      private SetupItem EncryptedKeysItem(bool local, string remoteExcuse)
      {
         var item = new SetupItem
         {
            Title = L("TLS private keys - a passphrase-protected key needs its passphrase configured"),
            Purpose = L("Every listener with TLS loads a certificate and its private key at startup; an encrypted key without its passphrase does not load, and the listener never starts."),
            Action = L("For each certificate whose key file is an encrypted PEM, set the certificate's private key password on the SSL certificates page (the passphrase was set by whoever created the key, so only you can supply it) - or replace the file with an unencrypted key."),
            Page = "certs"
         };

         dynamic settings = null;
         dynamic certs = null;
         try
         {
            settings = ServerSession.Current.Application.Settings;
            certs = settings.SSLCertificates;
            int count = (int)certs.Count;

            if (count == 0)
               item.Add(SetupItemState.NotNeeded, L("No SSL certificates are configured, so there are no key files to check."));

            for (int i = 0; i < count; i++)
            {
               dynamic cert = certs.Item[i];
               try
               {
                  string name = ((string)cert.Name ?? "").Trim();
                  string keyFile = ((string)cert.PrivateKeyFile ?? "").Trim();
                  string label = name.Length > 0 ? name : F("certificate {0}", (i + 1));

                  if (keyFile.Length == 0)
                  {
                     item.Add(SetupItemState.ActionNeeded,
                        F("{0}: no private key file is set, so this certificate cannot be used by any listener.", label));
                     continue;
                  }

                  if (!local)
                  {
                     item.Add(SetupItemState.CannotTell, F("{0}: the key file cannot be inspected because {1}", label, remoteExcuse));
                     continue;
                  }

                  if (!File.Exists(keyFile))
                  {
                     item.Add(SetupItemState.ActionNeeded,
                        F("{0}: the key file {1} does not exist on this machine, so every port using this certificate will fail to start.", label, keyFile));
                     continue;
                  }

                  bool encrypted;
                  try
                  {
                     encrypted = LooksLikeEncryptedPem(keyFile);
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                     // Readable by the service is what matters, but unreadable by
                     // this panel still means the state cannot be reported.
                     item.Add(SetupItemState.CannotTell, F("{0}: the key file exists but could not be read from this panel, so whether it is encrypted is unknown.", label));
                     continue;
                  }

                  if (!encrypted)
                  {
                     item.Add(SetupItemState.Done, F("{0}: the key file is not encrypted, so no passphrase is needed.", label));
                     continue;
                  }

                  // Only the EMPTINESS of the passphrase is tested; the value is
                  // never rendered anywhere. Reading it requires the server-
                  // administrator session, so a failure here degrades honestly.
                  bool passwordKnown = true;
                  bool passwordSet = false;
                  try
                  {
                     string password = (string)cert.PrivateKeyPassword;
                     passwordSet = !string.IsNullOrEmpty(password);
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                     passwordKnown = false;
                  }

                  if (!passwordKnown)
                     item.Add(SetupItemState.CannotTell,
                        F("{0}: the key file is encrypted, but whether a passphrase is configured could not be read (this requires the server-administrator account).", label));
                  else if (passwordSet)
                     item.Add(SetupItemState.Done,
                        F("{0}: the key file is encrypted and a passphrase is configured. Whether it is the RIGHT passphrase is only proven when the server loads the key - check the error log for error 6170 after a restart.", label));
                  else
                     item.Add(SetupItemState.ActionNeeded,
                        F("{0}: the key file is an encrypted PEM but no passphrase is configured, so the key will not load (server error 6170) and every port using this certificate will not start. Set the private key password on the certificate.", label));
               }
               finally
               {
                  ServerSession.Release((object)cert);
               }
            }
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
            item.Add(SetupItemState.CannotTell, F("The SSL certificate list could not be read: {0}", firstError_));
         }
         finally
         {
            ServerSession.Release((object)certs);
            ServerSession.Release((object)settings);
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// ACME certificate issuance. Verified against AcmeClient: enabled with no
      /// AcmeDomains it logs "No domains configured" and does nothing. Beyond
      /// that, http-01 issuance stands or falls on something no code here can
      /// see: whether the CA, out on the internet, can reach this server on the
      /// challenge port - firewalls, NAT and public DNS are all outside both
      /// the server and this panel.
      /// </summary>
      private SetupItem AcmeItem(IniFeatureStore ini, bool iniReadable, string iniExcuse)
      {
         var item = new SetupItem
         {
            Title = L("ACME certificates - the CA must reach this server from the internet"),
            Purpose = L("Issues and renews certificates automatically, but only if the certificate authority can fetch the http-01 challenge from this server."),
            Action = L("Publish a public A/AAAA record for every name listed in AcmeDomains, and allow inbound TCP on the challenge port from the internet (port-forward through any NAT). Reachability from outside cannot be probed from inside, so test it from an external network or an online port checker."),
            Page = "acme"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("The ACME settings could not be read: {0}", iniExcuse));
            item.AggregateFromFindings();
            return item;
         }

         if (!ini.ReadBool("AcmeEnabled", false))
         {
            item.Add(SetupItemState.NotNeeded, L("ACME is off (AcmeEnabled=0, the default), so nothing external is required."));
            item.AggregateFromFindings();
            return item;
         }

         string domainList = ini.Read("AcmeDomains", "").Trim();
         int challengePort = ReadIniInt(ini, "AcmeHttpPort", 80);

         var names = domainList.Split(',').Select(s => s.Trim()).Where(s => s.Length > 0).ToList();

         if (names.Count == 0)
         {
            // Verified: RequestCertificate logs and returns before contacting the CA.
            item.Add(SetupItemState.ActionNeeded,
               L("ACME is enabled but AcmeDomains is empty, so no certificate is ever requested - the client logs 'No domains configured' and stops. List the host names the certificate should cover (for example mail.yourdomain.com, mta-sts.yourdomain.com)."));
         }
         else
         {
            foreach (string name in names)
            {
               item.Add(SetupItemState.CannotTell,
                  F("{0}: the CA must be able to fetch http://{1}/.well-known/acme-challenge/... on port {2} from the internet. Confirm the public DNS record points here and the port is open end to end.", name, name, challengePort));
            }
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// Inbound client certificates (mutual TLS). Verified against TCPServer:
      /// with a policy of Require and a CA file that is missing, unloadable or
      /// holds no certificate, the listener does not start at all (error 6140);
      /// with Request it starts but collects nothing. The CA bundle and the
      /// client certificates themselves are things the administrator must
      /// produce or obtain - hMailServer verifies certificates, it does not
      /// operate a certificate authority.
      /// </summary>
      private SetupItem ClientCertificatesItem(bool local, string remoteExcuse)
      {
         var item = new SetupItem
         {
            Title = L("Inbound client certificates - the port needs a CA bundle, the clients need certificates"),
            Purpose = L("Requires (or requests) a TLS client certificate from anyone connecting to a port, so only holders of a certificate from your chosen CA get in."),
            Action = L("Produce or obtain a CA (for example with OpenSSL or your PKI), save the CA certificate, with any intermediate ones, as a PEM bundle on the server, point the port's CA file at it, and issue a client certificate from that CA to every client that must connect. hMailServer verifies against the bundle but cannot create the CA or the client certificates."),
            Page = "ports"
         };

         dynamic settings = null;
         dynamic ports = null;
         try
         {
            settings = ServerSession.Current.Application.Settings;
            ports = settings.TCPIPPorts;
            int count = (int)ports.Count;
            int withPolicy = 0;

            for (int i = 0; i < count; i++)
            {
               dynamic port = ports.Item[i];
               try
               {
                  int policy = (int)port.ClientCertificatePolicy;
                  if (policy == 0)
                     continue;

                  withPolicy++;

                  string label = ProtocolName((int)port.Protocol) + " " + (string)port.Address + ":" + (int)port.PortNumber
                                 + " (" + (policy == 2 ? "require" : "request") + ")";
                  int security = (int)port.ConnectionSecurity;
                  string caFile = ((string)port.ClientCertificateCAFile ?? "").Trim();

                  if (security == 0)
                  {
                     // Verified: a plaintext port never performs a handshake, so
                     // the policy can never run; for Require the listener is not
                     // even started (error 6142).
                     item.Add(SetupItemState.ActionNeeded, policy == 2
                        ? F("{0}: the port has a client certificate policy but no SSL/TLS or STARTTLS, so no handshake ever happens and the listener is not started (server error 6142). Enable connection security on the port, or set the policy to off.", label)
                        : F("{0}: the port has a client certificate policy but no SSL/TLS or STARTTLS, so no handshake ever happens and the policy verifies nothing. Enable connection security on the port, or set the policy to off.", label));
                     continue;
                  }

                  if (caFile.Length == 0)
                  {
                     item.Add(SetupItemState.ActionNeeded, policy == 2
                        ? F("{0}: no CA file is set, so there is no trust anchor to verify client certificates against and the listener will not start (server error 6140).", label)
                        : F("{0}: no CA file is set, so there is no trust anchor to verify client certificates against.", label));
                     continue;
                  }

                  if (!local)
                  {
                     item.Add(SetupItemState.CannotTell, F("{0}: the CA file {1} cannot be inspected because {2}", label, caFile, remoteExcuse));
                     continue;
                  }

                  if (!File.Exists(caFile))
                  {
                     item.Add(SetupItemState.ActionNeeded,
                        F("{0}: the CA file {1} does not exist on this machine{2}", label, caFile, (policy == 2 ? L(", so the listener will not start (server error 6140).") : L(", so verification can never succeed."))));
                     continue;
                  }

                  bool looksLikePem;
                  try
                  {
                     // The same first-pass test OpenSSL's PEM reader applies: a
                     // usable bundle must contain at least one CERTIFICATE block.
                     looksLikePem = File.ReadAllText(caFile).Contains("-----BEGIN CERTIFICATE-----");
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                     item.Add(SetupItemState.CannotTell, F("{0}: the CA file exists but could not be read from this panel.", label));
                     continue;
                  }

                  if (!looksLikePem)
                  {
                     item.Add(SetupItemState.ActionNeeded,
                        F("{0}: {1} contains no PEM certificate block, so it holds no usable CA certificate (server error 6140). Export the CA certificate in PEM (Base64) format.", label, caFile));
                  }
                  else
                  {
                     item.Add(SetupItemState.Done,
                        F("{0}: the CA bundle exists and contains a PEM certificate. What remains is outside the server: issue certificates from that CA to the connecting clients.", label));
                  }
               }
               finally
               {
                  ServerSession.Release((object)port);
               }
            }

            if (withPolicy == 0)
               item.Add(SetupItemState.NotNeeded, L("No port has a client certificate policy, so no CA bundle or client certificates are required."));
         }
         catch (Exception ex) when (!ExceptionPolicy.IsFatal(ex))
         {
            failedReads_++;
            firstError_ ??= ServerSession.DescribeComError(ex);
            linkLost_ |= ServerSession.IsTransportFailure(ex);
            item.Add(SetupItemState.CannotTell, F("The TCP/IP port list could not be read: {0}", firstError_));
         }
         finally
         {
            ServerSession.Release((object)ports);
            ServerSession.Release((object)settings);
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// OAuth2 / XOAUTH2 bearer tokens. Verified against OAuth2TokenValidator:
      /// RS256 tokens are verified against OAuth2PublicKeyFile and HS256 tokens
      /// against OAuth2HmacSecret, so with the feature enabled and the matching
      /// key material absent every token fails signature verification - enabled,
      /// and no client can ever log on with it. The key comes from the identity
      /// provider (an app registration at Microsoft, Google, or your own IdP),
      /// which is precisely why the server cannot conjure it.
      /// </summary>
      private SetupItem OAuth2Item(IniFeatureStore ini, bool iniReadable, string iniExcuse, bool local)
      {
         var item = new SetupItem
         {
            Title = L("OAuth2 / XOAUTH2 - the identity provider's key material must be installed"),
            Purpose = L("Lets clients log on with bearer tokens from an identity provider instead of passwords; tokens are only accepted if their signature verifies against key material you install."),
            Action = L("Register this server as an application with the identity provider, download the provider's public signing key as a PEM file, and point OAuth2PublicKeyFile in hMailServer.ini at it (or set OAuth2HmacSecret for HS256). Set OAuth2Issuer and OAuth2Audience to the values the provider puts in its tokens, or validation will reject them."),
            Page = "authentication"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("The OAuth2 settings could not be read: {0}", iniExcuse));
            item.AggregateFromFindings();
            return item;
         }

         if (!ini.ReadBool("OAuth2Enabled", false))
         {
            item.Add(SetupItemState.NotNeeded, L("OAuth2 is off (OAuth2Enabled=0, the default), so no identity provider or key material is required."));
            item.AggregateFromFindings();
            return item;
         }

         // Defaults mirror IniFileSettings.cpp: algorithms default to RS256 alone.
         string algorithms = ini.Read("OAuth2AllowedAlgorithms", "RS256").ToUpperInvariant();
         bool allowsRs256 = algorithms.Contains("RS256");
         bool allowsHs256 = algorithms.Contains("HS256");
         string publicKeyFile = ini.Read("OAuth2PublicKeyFile", "").Trim();
         // Only EMPTINESS is tested; the secret's value is never read into the UI.
         bool hmacSecretSet = ini.Read("OAuth2HmacSecret", "").Trim().Length > 0;

         bool anythingWrong = false;

         if (allowsRs256)
         {
            if (publicKeyFile.Length == 0)
            {
               anythingWrong = true;
               item.Add(SetupItemState.ActionNeeded,
                  L("OAuth2 is enabled and allows RS256, but OAuth2PublicKeyFile is empty - every RS256 token fails signature verification, so no client can log on with a token. Install the identity provider's public key as a PEM file and point OAuth2PublicKeyFile at it."));
            }
            else if (local && !File.Exists(publicKeyFile))
            {
               anythingWrong = true;
               item.Add(SetupItemState.ActionNeeded,
                  F("OAuth2PublicKeyFile points at {0}, which does not exist on this machine, so RS256 tokens cannot be verified.", publicKeyFile));
            }
         }

         if (allowsHs256 && !hmacSecretSet)
         {
            anythingWrong = true;
            item.Add(SetupItemState.ActionNeeded,
               L("HS256 is in OAuth2AllowedAlgorithms but OAuth2HmacSecret is empty, so HS256 tokens can never verify. Set the shared secret, or remove HS256 from the list."));
         }

         // Blank is not "will be checked and might mismatch" - it is "not checked".
         // OAuth2TokenValidator applies each of these only when it is set
         // (`if (!config.issuer.IsEmpty())`), and blank is the shipped default, so
         // the permissive case is the one an untouched configuration lands in. The
         // previous wording ("a mismatch rejects every token") described only the
         // case where they ARE set, which is the case that needs no warning.
         bool noIssuer = ini.Read("OAuth2Issuer", "").Trim().Length == 0;
         bool noAudience = ini.Read("OAuth2Audience", "").Trim().Length == 0;

         if (noIssuer || noAudience)
         {
            anythingWrong = true;

            string unchecked_ = noIssuer && noAudience
               ? L("Neither OAuth2Issuer nor OAuth2Audience is set, so neither is checked")
               : noIssuer ? L("OAuth2Issuer is not set, so the issuer is not checked")
                          : L("OAuth2Audience is not set, so the audience is not checked");

            item.Add(SetupItemState.ActionNeeded,
               F("{0}. Any token the identity provider signs with this key is then accepted, including one it issued to a different application - which is enough to log in as whichever mailbox that token names. Set both to the exact values the provider puts in its tokens. The values come from the provider, which is why they are on this list.", unchecked_));
         }

         if (!anythingWrong)
         {
            item.Add(SetupItemState.Done,
               L("OAuth2 is enabled, the key material its allowed algorithms need is in place, and both the issuer and the audience are checked - so a token has to have been minted by your provider for this server specifically."));
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// Client autoconfiguration. The same shape as MTA-STS hosting, verified
      /// against the same startup diagnostic: AutoconfigEnabled defaults to 1
      /// while both web-services ports default to 0, so a stock install has the
      /// feature "on" with nothing answering the URLs. The DNS names clients
      /// probe (autoconfig.&lt;domain&gt;, autodiscover.&lt;domain&gt;) live in the
      /// administrator's zone, where the server cannot create them.
      /// </summary>
      private SetupItem AutoconfigItem(IniFeatureStore ini, bool iniReadable, string iniExcuse)
      {
         var item = new SetupItem
         {
            Title = L("Client autoconfiguration - a listener plus the DNS names clients probe"),
            Purpose = L("Lets Outlook, Thunderbird and mobile clients set themselves up from an e-mail address alone, by fetching this server's configuration URLs."),
            Action = L("Set the HTTP or HTTPS port on the Web services & autoconfiguration page so the URLs answer, then publish autoconfig.<domain> and autodiscover.<domain> DNS records for each mail domain, pointing at this server. These are address records, and this page checks TXT records only, so they cannot be confirmed from here."),
            Page = "webservices"
         };

         if (!iniReadable)
         {
            item.Add(SetupItemState.CannotTell, F("The web services settings could not be read: {0}", iniExcuse));
         }
         else if (!ini.ReadBool("AutoconfigEnabled", true))
         {
            item.Add(SetupItemState.NotNeeded, L("Autoconfiguration is off (AutoconfigEnabled=0), so no listener or DNS names are required."));
         }
         else if (ReadIniInt(ini, "WebServicesHttpPort", 0) <= 0 && ReadIniInt(ini, "WebServicesHttpsPort", 0) <= 0)
         {
            item.Add(SetupItemState.ActionNeeded,
               L("Autoconfiguration is enabled (AutoconfigEnabled=1, the shipped default) but both WebServicesHttpPort and WebServicesHttpsPort are 0, so nothing answers the configuration URLs. Set a port if you want clients to self-configure; if you do not, this row is safe to ignore, or set AutoconfigEnabled=0 to silence it."));
         }
         else
         {
            item.Add(SetupItemState.CannotTell,
               L("The listener is configured. What remains is outside the server: publish autoconfig.<domain> and autodiscover.<domain> records for each mail domain, then test from a client machine."));
         }

         item.AggregateFromFindings();
         return item;
      }

      /// <summary>
      /// The attachment sandbox (docs/Sandbox.md): a service outside this server
      /// that opens a file and says what it did. The switch and the two addresses
      /// are read over COM; whether the service answers cannot be told from here,
      /// so a configured sandbox is "Cannot tell" with the way to find out.
      /// </summary>
      private SetupItem SandboxItem()
      {
         var item = new SetupItem
         {
            Title = L("Attachment sandbox - a service that opens files"),
            Purpose = L("Holds a message from outside with an executable or document attached until a sandbox has opened the file and answered clean or malicious."),
            Action = L("Run or subscribe to a sandbox that speaks the small HTTP contract in docs/Sandbox.md, set its submit and verdict addresses on the Anti-virus page's Sandbox tab, and turn it on."),
            Page = "antivirus"
         };

         int failuresBefore = failedReads_;

         bool enabled = Read(() =>
         {
            dynamic settings = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               return (bool)settings.SandboxEnabled;
            }
            finally
            {
               ServerSession.Release((object)settings);
            }
         });

         string submitUrl = Read(() =>
         {
            dynamic settings = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               return (string)settings.SandboxSubmitUrl ?? "";
            }
            finally
            {
               ServerSession.Release((object)settings);
            }
         });

         string verdictUrl = Read(() =>
         {
            dynamic settings = null;
            try
            {
               settings = ServerSession.Current.Application.Settings;
               return (string)settings.SandboxVerdictUrl ?? "";
            }
            finally
            {
               ServerSession.Release((object)settings);
            }
         });

         if (failedReads_ != failuresBefore)
         {
            item.Add(SetupItemState.CannotTell,
               L("The sandbox settings could not be read from the server, so this page cannot say whether one is configured."));
         }
         else if (!enabled)
         {
            item.Add(SetupItemState.NotNeeded,
               L("The sandbox is off, which is the default: nothing is held and nothing is submitted."));
         }
         else if (string.IsNullOrWhiteSpace(submitUrl) || string.IsNullOrWhiteSpace(verdictUrl))
         {
            item.Add(SetupItemState.ActionNeeded,
               L("The sandbox is ON but an address is empty, so nothing is held and nothing is submitted until both are set - the application log says so at every start. Set both addresses, or turn it off."));
         }
         else
         {
            item.Add(SetupItemState.CannotTell,
               L("Both addresses are set. Whether the service answers cannot be told from here: send a message with an attached file of a submitted type from outside, and look for it under Held for a sandbox on the Deck's queue page or for a line saying it was submitted to the sandbox in the application log."));
         }

         item.AggregateFromFindings();
         return item;
      }

      private static int ReadIniInt(IniFeatureStore ini, string key, int defaultValue)
      {
         return int.TryParse(ini.Read(key, defaultValue.ToString()).Trim(), out int value) ? value : defaultValue;
      }

      private static string ProtocolName(int protocol)
      {
         switch (protocol)
         {
            case ServerSession.SessionSmtp: return "SMTP";
            case ServerSession.SessionPop3: return "POP3";
            case ServerSession.SessionImap: return "IMAP";
            default: return L("Port");
         }
      }
   }
}
