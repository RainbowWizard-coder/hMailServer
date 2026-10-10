// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using hMailServer.ControlPanel.Services;
using Xunit;

namespace hMailServer.ControlPanel.Tests.Services
{
   /// <summary>
   /// The transport encryption overview: the page that answers "is anything on this
   /// server carrying a password in the clear, and will it still serve a valid
   /// certificate next month".
   ///
   /// Four pages own the pieces and each is a correct editor for its own subject.
   /// What none of them can do is put the four together, and every behaviour pinned
   /// below only exists in the combination — each was read out of the server rather
   /// than assumed:
   ///
   ///   - SslContextInitializer::InitServer returns false when the certificate is
   ///     null, and TCPServer::Run gives up on it, so a TLS port with no certificate
   ///     assigned does not fall back to plaintext: the port never comes up;
   ///   - the AEAD-ONLY cipher preset excludes every CBC-mode suite, and TLS 1.0 and
   ///     1.1 have nothing else, so enabling them alongside it advertises versions
   ///     that then cannot complete a handshake;
   ///   - SecurityRange::GetRequireTLSForAuth is per IP range, not global, and it is
   ///     the only control that refuses a plaintext password whatever the port
   ///     permits;
   ///   - and port 25 has to be judged differently from every other port, because
   ///     requiring TLS there refuses mail from senders that cannot offer it.
   /// </summary>
   public class TlsPostureTests
   {
      private static TlsListener Listener(string protocol, int port, TlsListenerSecurity security,
         string certificate = "server cert")
      {
         return new TlsListener
         {
            Protocol = protocol,
            Address = "0.0.0.0",
            Port = port,
            Security = security,
            CertificateName = security == TlsListenerSecurity.None ? "" : certificate
         };
      }

      /// <summary>
      /// A well-configured server: submission and the mailbox protocols require
      /// encryption, port 25 offers it opportunistically, TLS 1.2 and 1.3 only.
      /// </summary>
      private static TlsPostureConfig Healthy()
      {
         return new TlsPostureConfig
         {
            Listeners = new List<TlsListener>
            {
               Listener("SMTP", 25, TlsListenerSecurity.StartTlsOptional),
               Listener("SMTP", 587, TlsListenerSecurity.StartTlsRequired),
               Listener("IMAP", 993, TlsListenerSecurity.Implicit),
               Listener("POP3", 995, TlsListenerSecurity.Implicit)
            },
            Certificates = new List<TlsCertificate>
            {
               new() { Name = "server cert", DaysRemaining = 200, InUse = true }
            },
            Tls12Enabled = true,
            Tls13Enabled = true,
            CipherList = "",
            VerifyRemoteCertificates = false,
            RangesTotal = 2,
            RangesRequiringTlsForAuth = 2
         };
      }

      /// <summary>hMailServer's stock port set: 25, 110, 143 and 587, none of them with TLS.</summary>
      private static TlsPostureConfig StockPorts()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener>
         {
            Listener("SMTP", 25, TlsListenerSecurity.None),
            Listener("POP3", 110, TlsListenerSecurity.None),
            Listener("IMAP", 143, TlsListenerSecurity.None),
            Listener("SMTP", 587, TlsListenerSecurity.None)
         };
         config.Certificates = new List<TlsCertificate>();
         config.RangesRequiringTlsForAuth = 0;
         return config;
      }

      private static bool HasNote(TlsPostureConfig config, StatusLevel level, string fragment) =>
         TlsPosture.Notes(config).Any(n => n.Level == level && n.Text.Contains(fragment));

      private static TlsListenerVerdict VerdictFor(TlsPostureConfig config, int port) =>
         TlsPosture.Listeners(config).Single(v => v.Listener.Port == port);

      // ---- port 25 is not the other ports -------------------------------------

      [Fact]
      public void OpportunisticStartTlsOnPortTwentyFiveIsCorrectRatherThanAFinding()
      {
         // Requiring TLS on 25 refuses mail from senders that cannot offer it, so
         // flagging this would train the reader to ignore the page.
         TlsPostureConfig config = Healthy();

         TlsListenerVerdict verdict = VerdictFor(config, 25);

         Assert.Equal(StatusLevel.Good, verdict.Level);
         Assert.Contains("correct arrangement for port 25", verdict.Detail);
      }

      [Fact]
      public void OpportunisticStartTlsOnASubmissionPortIsAFinding()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener> { Listener("SMTP", 587, TlsListenerSecurity.StartTlsOptional) };

         Assert.Equal(StatusLevel.Warning, VerdictFor(config, 587).Level);
         Assert.True(HasNote(config, StatusLevel.Warning, "STARTTLS is optional on"));
      }

      [Fact]
      public void PlaintextOnPortTwentyFiveIsAWarningAndOnAClientPortIsCritical()
      {
         TlsPostureConfig config = StockPorts();

         // Port 25 still accepts AUTH, so it is not nothing - but it is not the same
         // as a port whose entire purpose is authenticated clients.
         Assert.Equal(StatusLevel.Warning, VerdictFor(config, 25).Level);
         Assert.Equal(StatusLevel.Critical, VerdictFor(config, 110).Level);
         Assert.Equal(StatusLevel.Critical, VerdictFor(config, 143).Level);
         Assert.Equal(StatusLevel.Critical, VerdictFor(config, 587).Level);

         Assert.True(HasNote(config, StatusLevel.Critical, "POP3 on port 110 has no encryption available"));
         Assert.Null(TlsPosture.Verdict(config));
         Assert.Equal(StatusLevel.Critical, TlsPosture.WorstLevel(config));
      }

      [Fact]
      public void PortTwentyFiveIsTheOnlyPortTreatedAsServerToServer()
      {
         Assert.False(TlsPosture.IsClientPort(Listener("SMTP", 25, TlsListenerSecurity.None)));
         Assert.True(TlsPosture.IsClientPort(Listener("SMTP", 587, TlsListenerSecurity.None)));
         Assert.True(TlsPosture.IsClientPort(Listener("POP3", 110, TlsListenerSecurity.None)));

         // Protocol and port both, so a POP3 listener that happens to be on 25 is
         // still judged as a client port.
         Assert.True(TlsPosture.IsClientPort(Listener("POP3", 25, TlsListenerSecurity.None)));
      }

      // ---- the states that are not "encrypted or not" -------------------------

      [Fact]
      public void ATlsPortWithNoCertificateIsReportedAsNotStartingRatherThanAsPlaintext()
      {
         // The distinction matters operationally: the administrator is not looking
         // for an unencrypted session, they are looking for a refused connection.
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener>
         {
            new() { Protocol = "IMAP", Port = 993, Security = TlsListenerSecurity.Implicit, CertificateName = "" }
         };

         TlsListenerVerdict verdict = VerdictFor(config, 993);

         Assert.Equal(StatusLevel.Critical, verdict.Level);
         Assert.Equal("Will not start", verdict.Word);
         Assert.Contains("server error 5113", verdict.Detail);

         Assert.Null(TlsPosture.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Critical, "does not come up"));
      }

      [Fact]
      public void NoTlsVersionEnabledMeansNoTlsListenerCanNegotiate()
      {
         TlsPostureConfig config = Healthy();
         config.Tls10Enabled = false;
         config.Tls11Enabled = false;
         config.Tls12Enabled = false;
         config.Tls13Enabled = false;

         Assert.Equal("Cannot negotiate", VerdictFor(config, 993).Word);
         Assert.True(HasNote(config, StatusLevel.Critical, "No TLS version is enabled"));
         Assert.Equal("No TLS version is enabled.", TlsPosture.VersionSummary(config));
      }

      // ---- versions and ciphers ----------------------------------------------

      [Fact]
      public void DeprecatedProtocolVersionsAreReported()
      {
         TlsPostureConfig config = Healthy();
         config.Tls10Enabled = true;

         Assert.True(HasNote(config, StatusLevel.Warning, "RFC 8996"));
      }

      [Fact]
      public void TheAeadOnlyPresetWithOldProtocolsIsReportedAsSelfDefeating()
      {
         // The cipher-list blurb on the settings page says this in prose about the
         // preset. Here it is said about the configuration actually in front of you.
         TlsPostureConfig config = Healthy();
         config.CipherList = "aead-only";
         config.Tls11Enabled = true;

         Assert.True(TlsPosture.IsAeadOnlyPreset(config.CipherList));
         Assert.True(HasNote(config, StatusLevel.Warning, "no AEAD suite at all"));
      }

      [Fact]
      public void TheAeadOnlyPresetIsMatchedCaseInsensitivelyAndTrimmed()
      {
         Assert.True(TlsPosture.IsAeadOnlyPreset("AEAD-ONLY"));
         Assert.True(TlsPosture.IsAeadOnlyPreset(" aead-only "));
         Assert.False(TlsPosture.IsAeadOnlyPreset("ECDHE-RSA-AES256-GCM-SHA384"));
         Assert.False(TlsPosture.IsAeadOnlyPreset(""));
         Assert.False(TlsPosture.IsAeadOnlyPreset(null));
      }

      [Fact]
      public void VersionsAreListedNewestFirst()
      {
         TlsPostureConfig config = Healthy();
         config.Tls10Enabled = true;
         config.Tls11Enabled = true;

         Assert.Equal("TLS 1.3, TLS 1.2, TLS 1.1, TLS 1.0", TlsPosture.VersionSummary(config));
      }

      // ---- certificates -------------------------------------------------------

      [Fact]
      public void AnExpiredCertificateInUseIsCriticalAndOneNotInUseIsNot()
      {
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "live", DaysRemaining = -3, InUse = true },
            new() { Name = "spare", DaysRemaining = -90, InUse = false }
         };

         Assert.True(HasNote(config, StatusLevel.Critical, "\"live\" expired 3 days ago"));
         Assert.True(HasNote(config, StatusLevel.Warning, "\"spare\" expired 90 days ago"));
      }

      [Fact]
      public void ACertificateExpiringSoonIsReportedAtTheSameThresholdTheCertificatesPageUses()
      {
         // One threshold, in one place. Two pages disagreeing about when a
         // certificate becomes urgent is worse than neither of them saying so.
         //
         // Compared against StatusSemantics rather than against
         // CertificateInspector.ExpiryWarningDays, which is the same number: the
         // inspector reaches ServerSession, and ServerSession reaches COM and
         // System.ServiceProcess, so compiling it into this assembly to read one
         // constant would be a poor trade. StatusSemantics is where the number lives
         // and both of the others are defined from it.
         Assert.Equal(StatusSemantics.CertificateExpiryWarningDays, TlsPosture.ExpiryWarningDays);

         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "soon", DaysRemaining = TlsPosture.ExpiryWarningDays, InUse = true }
         };

         Assert.True(HasNote(config, StatusLevel.Warning, "expires in " + TlsPosture.ExpiryWarningDays));
      }

      [Fact]
      public void AHealthyCertificateProducesNoNote()
      {
         TlsPostureConfig config = Healthy();

         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Text.Contains("server cert"));
      }

      [Fact]
      public void AFileProblemOnACertificateInUseIsReported()
      {
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "server cert", DaysRemaining = 200, InUse = true, Problem = "The certificate file does not exist." }
         };

         Assert.True(HasNote(config, StatusLevel.Critical, "The certificate file does not exist."));
      }

      [Fact]
      public void RemoteSessionsSayWhyExpiryIsUnknownRatherThanReportingItMissing()
      {
         TlsPostureConfig config = Healthy();
         config.CertificateFilesReadable = false;
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "server cert", DaysRemaining = null, InUse = true }
         };

         Assert.True(HasNote(config, StatusLevel.Information, "cannot be checked from here"));
         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Level == StatusLevel.Critical);
      }

      // ---- the control that lives somewhere else ------------------------------

      [Fact]
      public void RangesNotRequiringTlsForAuthAreReportedWhenAPortIsOptional()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener> { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional) };
         config.RangesRequiringTlsForAuth = 0;

         Assert.True(HasNote(config, StatusLevel.Warning, "No IP range requires TLS before authentication"));
      }

      [Fact]
      public void RangesRequiringTlsForAuthAreCreditedInTheOptionalStartTlsNote()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener> { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional) };
         config.RangesRequiringTlsForAuth = 2;

         Assert.True(HasNote(config, StatusLevel.Warning, "closes the credential half of this"));
         Assert.False(HasNote(config, StatusLevel.Warning, "No IP range requires TLS before authentication"));
      }

      // ---- outbound -----------------------------------------------------------

      [Fact]
      public void NotVerifyingRemoteCertificatesIsExplainedRatherThanCondemned()
      {
         // It is the correct default for mail between servers, and a page that
         // called it a fault would be wrong. Information, with the reason.
         TlsPostureConfig config = Healthy();
         config.VerifyRemoteCertificates = false;

         Assert.True(HasNote(config, StatusLevel.Information, "passive listener only"));
         Assert.False(HasNote(config, StatusLevel.Warning, "passive listener only"));
      }

      // ---- the healthy case has to actually be quiet --------------------------

      [Fact]
      public void AWellConfiguredServerRaisesNothingWorseThanInformation()
      {
         // A page that shouts at a correct configuration is a page that gets
         // ignored, so this is as much a test of the page's usefulness as of it.
         TlsPostureConfig config = Healthy();

         Assert.Equal(StatusLevel.Information, TlsPosture.WorstLevel(config));
         Assert.Contains("requires encryption before it will accept a password", TlsPosture.Verdict(config));
      }

      [Fact]
      public void EveryNoteCarriesTextAndAUsableLevel()
      {
         var configs = new List<TlsPostureConfig> { Healthy(), StockPorts() };

         foreach (TlsPostureConfig config in configs)
         {
            foreach (TlsPostureNote note in TlsPosture.Notes(config))
            {
               Assert.False(string.IsNullOrWhiteSpace(note.Text));
               Assert.NotNull(StatusSemantics.For(note.Level));
            }
         }
      }

      [Fact]
      public void ANullConfigurationIsSurvivedRatherThanThrown()
      {
         Assert.Empty(TlsPosture.Listeners(null));
         Assert.Empty(TlsPosture.Notes(null));
         Assert.Equal(StatusLevel.Normal, TlsPosture.WorstLevel(null));
         Assert.False(string.IsNullOrWhiteSpace(TlsPosture.Verdict(null)));
      }

      [Fact]
      public void AServerWithNoListenersSaysSo()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener>();

         Assert.Null(TlsPosture.Verdict(config));
         Assert.Contains(TlsPosture.Notes(config), n => n.Level == StatusLevel.Critical && n.Text.StartsWith("No listeners are configured", StringComparison.Ordinal));
      }

      // ---- values that could not be read ---------------------------------------
      //
      // A value the server did not answer is the type's default in the snapshot:
      // no listeners, no certificates, every version switch off. The transport
      // encryption overview drew those as the configuration when the link was
      // lost - "No listeners are configured", "No TLS version is enabled."

      [Fact]
      public void UnreadListeners_AreNotNoListeners()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener>();
         config.Unread.Add(nameof(config.Listeners));

         Assert.Contains("could not be read", TlsPosture.Verdict(config));
         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Text.StartsWith("No listeners are configured", StringComparison.Ordinal));
      }

      /// <summary>
      /// The certificates could not be read but the ports could: each port's
      /// certificate id resolves against an empty collection, and an empty name is
      /// the "Will not start" verdict at Critical - "N listeners are configured for
      /// TLS but cannot serve it", a Critical note per port - while the certificates
      /// card on the same page says the certificates could not be read. A listener
      /// whose certificate was not read says so, at no level, and nothing that
      /// follows from "no certificate" is said of it.
      /// </summary>
      [Fact]
      public void UnreadCertificates_AreNotNoCertificate()
      {
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>();
         config.Unread.Add(nameof(config.Certificates));
         foreach (TlsListener listener in config.Listeners)
         {
            listener.CertificateName = "";
            listener.CertificateUnread = true;
         }

         IReadOnlyList<TlsListenerVerdict> verdicts = TlsPosture.Listeners(config);
         Assert.DoesNotContain(verdicts, v => v.Word == "Will not start");

         TlsListenerVerdict imaps = VerdictFor(config, 993);
         Assert.Equal(StatusLevel.Normal, imaps.Level);
         Assert.Equal("Not read", imaps.Word);
         Assert.Contains("could not be read", imaps.Detail);

         Assert.Equal("Every listener that a mail client connects to requires encryption before it will accept a password.", TlsPosture.Verdict(config));
         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Level == StatusLevel.Critical);
         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Text.Contains("does not come up"));

         // ... and a listener that was read with its certificate is judged as before.
         TlsPostureConfig read = Healthy();
         Assert.Equal("Encrypted", VerdictFor(read, 993).Word);
      }

      [Fact]
      public void UnreadVersions_AreNotNoVersions()
      {
         TlsPostureConfig config = Healthy();
         config.Tls12Enabled = false;
         config.Tls13Enabled = false;
         foreach (string version in new[] { nameof(config.Tls10Enabled), nameof(config.Tls11Enabled), nameof(config.Tls12Enabled), nameof(config.Tls13Enabled) })
            config.Unread.Add(version);

         Assert.False(TlsPosture.VersionsRead(config));
         Assert.Contains("could not be read", TlsPosture.VersionSummary(config));
         Assert.DoesNotContain(TlsPosture.Listeners(config), v => v.Word == "Cannot negotiate");
         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Text.Contains("No TLS version is enabled"));
      }

      [Fact]
      public void UnreadRanges_AreNotNoRanges()
      {
         TlsPostureConfig config = Healthy();
         config.RangesTotal = 0;
         config.RangesRequiringTlsForAuth = 0;
         config.Unread.Add(nameof(config.RangesTotal));
         config.Unread.Add(nameof(config.RangesRequiringTlsForAuth));

         Assert.DoesNotContain(TlsPosture.Notes(config), n => n.Text.Contains("No IP range requires TLS"));
      }

      [Fact]
      public void AConfigurationReadInFull_HasNothingUnread()
      {
         TlsPostureConfig config = Healthy();
         Assert.True(TlsPosture.VersionsRead(config));
         Assert.True(config.WasRead(nameof(config.Listeners), nameof(config.Certificates), nameof(config.RangesTotal)));
         Assert.DoesNotContain("could not be read", TlsPosture.Verdict(config));
      }

      // ---- whole sentences, one per case ----------------------------------------
      //
      // A verdict put together from a counted noun ("1 client listener has"), a
      // pronoun ("it cross") and a template could not be translated: the pieces
      // are translated apart, and Polish read "przechodzą przez przez sieć", German
      // "kann es aber nicht". Each case is now one text in the catalogue, and these
      // pin the English of each so that no piece can come back.

      /// <summary>
      /// The verdict has a sentence only where no note says it: every client port
      /// requiring encryption, or listeners not read. Where it is about listeners
      /// the notes name each one - "POP3 on port 110 has no encryption
      /// available..." - where it said "1 client listener" and left the reader to
      /// find which; the sentences that named them again above the notes, "POP3
      /// 110 has no encryption available..." and "2 listeners are configured for
      /// TLS but cannot serve it (IMAP 993, POP3 995)...", were never shown and are
      /// gone.
      /// </summary>
      [Fact]
      public void TheVerdictHasASentence_OnlyWhereNoNoteSaysIt_AndTheNotesNameEachListener()
      {
         TlsPostureConfig config = Healthy();
         Assert.Equal("Every listener that a mail client connects to requires encryption before it will accept a password.", TlsPosture.Verdict(config));

         config.Listeners = new List<TlsListener> { Listener("IMAP", 143, TlsListenerSecurity.None), Listener("POP3", 110, TlsListenerSecurity.None) };
         Assert.Null(TlsPosture.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Critical, "IMAP on port 143 has no encryption available."));
         Assert.True(HasNote(config, StatusLevel.Critical, "POP3 on port 110 has no encryption available."));

         config.Listeners = new List<TlsListener> { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional), Listener("POP3", 110, TlsListenerSecurity.StartTlsOptional) };
         Assert.Null(TlsPosture.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Warning, "STARTTLS is optional on IMAP 143, POP3 110."));

         config.Listeners = new List<TlsListener> { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: ""), Listener("POP3", 995, TlsListenerSecurity.Implicit, certificate: "") };
         Assert.Null(TlsPosture.Verdict(config));
         Assert.True(HasNote(config, StatusLevel.Critical, "IMAP on port 993 is set to use TLS with no certificate assigned"));
         Assert.True(HasNote(config, StatusLevel.Critical, "POP3 on port 995 is set to use TLS with no certificate assigned"));

         config.Unread.Add(nameof(config.Listeners));
         Assert.Equal("Where this server stands cannot be said: the listeners could not be read.", TlsPosture.Verdict(config));
      }

      // ---- the verdict's level, and the problems' pages ----------------------------

      /// <summary>
      /// The verdict is shown at its own level - it was plain body text - Good or
      /// Information; at the Warning level and above the notes say it, each with
      /// TCP/IP ports or the page that puts it right.
      /// </summary>
      [Fact]
      public void TheVerdict_HasALevel_AndTheNotesSayItAtWarningAndAbove()
      {
         TlsPostureConfig config = Healthy();
         Assert.Equal(StatusLevel.Good, TlsPosture.VerdictLevel(config));
         Assert.False(TlsPosture.NotesSayTheVerdict(config));

         config.Listeners.Add(Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional));
         Assert.Equal(StatusLevel.Warning, TlsPosture.VerdictLevel(config));
         Assert.True(TlsPosture.NotesSayTheVerdict(config));

         config.Listeners.Add(Listener("POP3", 110, TlsListenerSecurity.None));
         Assert.Equal(StatusLevel.Critical, TlsPosture.VerdictLevel(config));
         Assert.True(TlsPosture.NotesSayTheVerdict(config));

         config.Listeners.Clear();
         Assert.Equal(StatusLevel.Critical, TlsPosture.VerdictLevel(config));
         Assert.True(TlsPosture.NotesSayTheVerdict(config));

         config.Unread.Add(nameof(config.Listeners));
         Assert.Equal(StatusLevel.Information, TlsPosture.VerdictLevel(config));
         Assert.False(TlsPosture.NotesSayTheVerdict(config));
      }

      /// <summary>
      /// Where the verdict is about listeners, the notes say it listener by
      /// listener - each named, at the verdict's level - so the overview shows
      /// them and not the verdict as well, which read as the same Critical notice
      /// twice. A listener that cannot negotiate is said by the note that no TLS
      /// version is enabled, which opens SSL/TLS, where the fix is, and no
      /// listener at all by a note of its own. The verdict of a server whose
      /// client ports all require encryption, and of listeners not read, has no
      /// note to say it, and is shown.
      /// </summary>
      [Fact]
      public void WhereTheVerdictIsAboutListeners_TheNotesSayIt_EachListenerAtTheVerdictsLevel()
      {
         var cases = new List<List<TlsListener>>
         {
            new() { Listener("POP3", 110, TlsListenerSecurity.None) },
            new() { Listener("IMAP", 143, TlsListenerSecurity.None), Listener("POP3", 110, TlsListenerSecurity.None) },
            new() { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional) },
            new() { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional), Listener("SMTP", 587, TlsListenerSecurity.StartTlsOptional) },
            new() { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: "") },
            new() { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: ""), Listener("POP3", 110, TlsListenerSecurity.None) }
         };

         foreach (List<TlsListener> listeners in cases)
         {
            TlsPostureConfig config = Healthy();
            config.Listeners = listeners;
            IReadOnlyList<TlsPostureNote> notes = TlsPosture.Notes(config);

            Assert.True(TlsPosture.NotesSayTheVerdict(config));
            Assert.True(TlsPosture.VerdictLevel(config) >= StatusLevel.Warning);
            Assert.Contains(notes, n => n.Level == TlsPosture.VerdictLevel(config) && n.Page == "ports");
            foreach (TlsListener listener in listeners.Where(l => VerdictFor(config, l.Port).Level >= StatusLevel.Warning))
            {
               Assert.Contains(notes, n => n.Level >= StatusLevel.Warning && n.Page == "ports"
                  && (n.Text.Contains(listener.Protocol + " on port " + listener.Port, StringComparison.Ordinal)
                     || n.Text.Contains(listener.Protocol + " " + listener.Port, StringComparison.Ordinal)));
            }
         }

         TlsPostureConfig noVersion = Healthy();
         noVersion.Tls12Enabled = false;
         noVersion.Tls13Enabled = false;
         Assert.Equal(TlsListenerState.CannotNegotiate, VerdictFor(noVersion, 993).State);
         Assert.True(TlsPosture.NotesSayTheVerdict(noVersion));
         Assert.Contains(TlsPosture.Notes(noVersion), n => n.Level == StatusLevel.Critical && n.Page == "tls"
            && n.Text.StartsWith("No TLS version is enabled", StringComparison.Ordinal));

         TlsPostureConfig healthy = Healthy();
         Assert.False(TlsPosture.NotesSayTheVerdict(healthy));

         healthy.Listeners.Clear();
         Assert.True(TlsPosture.NotesSayTheVerdict(healthy));
         Assert.Null(TlsPosture.Verdict(healthy));
         Assert.Contains(TlsPosture.Notes(healthy), n => n.Level == StatusLevel.Critical && n.Page == "ports"
            && n.Text == "No listeners are configured, so nothing can connect to this server.");

         healthy.Listeners.Add(Listener("POP3", 110, TlsListenerSecurity.None));
         healthy.Unread.Add(nameof(healthy.Listeners));
         Assert.False(TlsPosture.NotesSayTheVerdict(healthy));
         Assert.False(TlsPosture.NotesSayTheVerdict(null));
      }

      /// <summary>
      /// A verdict at Warning or above is always said by a note at its level, so
      /// that the count of problems beside the title - the notes at Warning and
      /// above - is never "No problems found" beside a Critical verdict, as it was
      /// beside "No listeners are configured, so nothing can connect to this
      /// server.", which no note said.
      /// </summary>
      [Fact]
      public void AVerdictAtWarningOrAbove_IsSaidByANoteAtItsLevel()
      {
         var listenerSets = new List<List<TlsListener>>
         {
            new(),
            new() { Listener("SMTP", 25, TlsListenerSecurity.None) },
            new() { Listener("SMTP", 25, TlsListenerSecurity.StartTlsOptional) },
            new() { Listener("SMTP", 587, TlsListenerSecurity.StartTlsOptional) },
            new() { Listener("POP3", 110, TlsListenerSecurity.None), Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional) },
            new() { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: ""), Listener("SMTP", 587, TlsListenerSecurity.StartTlsRequired) },
            new() { Listener("IMAP", 993, TlsListenerSecurity.Implicit), Listener("SMTP", 465, TlsListenerSecurity.Implicit) }
         };

         foreach (List<TlsListener> listeners in listenerSets)
         {
            foreach (bool versions in new[] { true, false })
            {
               TlsPostureConfig config = Healthy();
               config.Listeners = listeners;
               config.Tls12Enabled = versions;
               config.Tls13Enabled = versions;

               StatusLevel verdict = TlsPosture.VerdictLevel(config);
               Assert.Equal(verdict >= StatusLevel.Warning, TlsPosture.NotesSayTheVerdict(config));
               Assert.Equal(verdict >= StatusLevel.Warning, TlsPosture.Verdict(config) == null);
               if (verdict < StatusLevel.Warning)
                  continue;

               Assert.Contains(TlsPosture.Notes(config), n => n.Level >= verdict);
            }
         }
      }

      /// <summary>
      /// Each note names the page that puts it right: a listener's on TCP/IP
      /// ports, the require-TLS-for-authentication note on IP ranges, a
      /// certificate's on SSL certificates, the rest on SSL/TLS.
      /// </summary>
      [Fact]
      public void EachNote_NamesThePageThatPutsItRight()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener>
         {
            Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: ""),
            Listener("POP3", 110, TlsListenerSecurity.None),
            Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional)
         };
         config.Tls10Enabled = true;
         config.RangesTotal = 2;
         config.RangesRequiringTlsForAuth = 0;
         config.Certificates = new List<TlsCertificate> { new() { Name = "old", DaysRemaining = -3, InUse = true } };

         IReadOnlyList<TlsPostureNote> notes = TlsPosture.Notes(config);
         Assert.Equal("ports", notes.Single(n => n.Text.StartsWith("IMAP on port 993", StringComparison.Ordinal)).Page);
         Assert.Equal("ports", notes.Single(n => n.Text.StartsWith("POP3 on port 110", StringComparison.Ordinal)).Page);
         Assert.Equal("ports", notes.Single(n => n.Text.StartsWith("STARTTLS is optional", StringComparison.Ordinal)).Page);
         Assert.Equal("ipranges", notes.Single(n => n.Text.StartsWith("No IP range requires TLS before authentication", StringComparison.Ordinal)).Page);
         Assert.Equal("certs", notes.Single(n => n.Text.Contains("\"old\"", StringComparison.Ordinal)).Page);
         Assert.Equal("tls", notes.Single(n => n.Text.StartsWith("TLS 1.0 is enabled", StringComparison.Ordinal)).Page);
         Assert.All(notes, n => Assert.NotNull(NavigationMap.Find(n.Page)));
      }

      /// <summary>
      /// The verdict and the notes count listeners by what they amount to, not
      /// by the word that says it: they compared the word with "Will not start",
      /// which matched only in English, so in every other language a listener
      /// that cannot start was neither the verdict nor a note.
      /// </summary>
      [Fact]
      public void AListenerThatWillNotStart_IsCounted_InATranslatedCatalogue()
      {
         try
         {
            Loc.Use((text, culture) => text == "Will not start" ? "Startet nicht" : null, System.Globalization.CultureInfo.GetCultureInfo("de"));
            TlsPostureConfig config = Healthy();
            config.Listeners = new List<TlsListener> { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: "") };

            Assert.Equal("Startet nicht", VerdictFor(config, 993).Word);
            Assert.Equal(TlsListenerState.WillNotStart, VerdictFor(config, 993).State);
            Assert.True(TlsPosture.NotesSayTheVerdict(config));
            Assert.Contains(TlsPosture.Notes(config), n => n.Level == StatusLevel.Critical && n.Text.StartsWith("IMAP on port 993", StringComparison.Ordinal));
            Assert.Equal(StatusLevel.Critical, TlsPosture.VerdictLevel(config));
         }
         finally
         {
            Loc.Reset();
         }
      }

      /// <summary>
      /// Port 25 unencrypted says what is in the clear there, at the Warning
      /// level; a client port says In the clear at the Critical level. The two
      /// said the same words in two colours.
      /// </summary>
      [Fact]
      public void Port25Unencrypted_SaysServerMail_AndAClientPortInTheClear()
      {
         TlsPostureConfig config = Healthy();
         config.Listeners = new List<TlsListener> { Listener("SMTP", 25, TlsListenerSecurity.None), Listener("POP3", 110, TlsListenerSecurity.None) };
         Assert.Equal("Unencrypted (server mail)", VerdictFor(config, 25).Word);
         Assert.Equal(StatusLevel.Warning, VerdictFor(config, 25).Level);
         Assert.Equal("In the clear", VerdictFor(config, 110).Word);
         Assert.Equal(StatusLevel.Critical, VerdictFor(config, 110).Level);
      }

      [Fact]
      public void ACertificateNoteIsOneWholeSentencePerCase()
      {
         // "expires in {1}{2}{3}" took " day" or " days" and a clause: German read
         // "vor 5 Tage", and Japanese put the clause before the verb.
         Assert.Equal("The certificate \"server cert\" expired earlier today and is still assigned to a listener. Clients are refusing or warning about every connection to it.",
            TlsPosture.ExpiredNote("server cert", 0, true));
         Assert.Equal("The certificate \"server cert\" expired yesterday and is still assigned to a listener. Clients are refusing or warning about every connection to it.",
            TlsPosture.ExpiredNote("server cert", 1, true));
         Assert.Equal("The certificate \"server cert\" expired 5 days ago and is still assigned to a listener. Clients are refusing or warning about every connection to it.",
            TlsPosture.ExpiredNote("server cert", 5, true));
         Assert.Equal("The certificate \"server cert\" expired earlier today. It is not assigned to any listener.",
            TlsPosture.ExpiredNote("server cert", 0, false));
         Assert.Equal("The certificate \"server cert\" expired yesterday. It is not assigned to any listener.",
            TlsPosture.ExpiredNote("server cert", 1, false));
         Assert.Equal("The certificate \"server cert\" expired 5 days ago. It is not assigned to any listener.",
            TlsPosture.ExpiredNote("server cert", 5, false));

         Assert.Equal("The certificate \"server cert\" expires today and is in use by a listener.",
            TlsPosture.ExpiringNote("server cert", 0, true));
         Assert.Equal("The certificate \"server cert\" expires tomorrow and is in use by a listener.",
            TlsPosture.ExpiringNote("server cert", 1, true));
         Assert.Equal("The certificate \"server cert\" expires in 30 days and is in use by a listener.",
            TlsPosture.ExpiringNote("server cert", 30, true));
         Assert.Equal("The certificate \"server cert\" expires today. It is not assigned to any listener.",
            TlsPosture.ExpiringNote("server cert", 0, false));
         Assert.Equal("The certificate \"server cert\" expires tomorrow. It is not assigned to any listener.",
            TlsPosture.ExpiringNote("server cert", 1, false));
         Assert.Equal("The certificate \"server cert\" expires in 30 days. It is not assigned to any listener.",
            TlsPosture.ExpiringNote("server cert", 30, false));
      }

      [Fact]
      public void TheNotesNameTheCalendarDayACertificateExpiresOn()
      {
         // DaysRemaining is the floor of the days to the expiry and decides the
         // level: -1 is within the last 24 hours, 0 within the next 24. The day a
         // note names is read from the expiry date, because "expires today" is a
         // claim about the date: at eight in the evening a certificate that expires
         // at six the next morning has 0 days remaining and expires tomorrow, and
         // one that expired at five that morning has -1 and expired earlier today,
         // not yesterday.
         var today = new DateTime(2026, 10, 9);
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "this morning", DaysRemaining = -1, ExpiresOn = today.AddHours(5), InUse = true },
            new() { Name = "last night", DaysRemaining = -1, ExpiresOn = today.AddHours(-1), InUse = true },
            new() { Name = "spare", DaysRemaining = -12, ExpiresOn = today.AddDays(-12).AddHours(14), InUse = false },
            new() { Name = "tonight", DaysRemaining = 0, ExpiresOn = today.AddHours(23), InUse = true },
            new() { Name = "dawn", DaysRemaining = 0, ExpiresOn = today.AddDays(1).AddHours(6), InUse = true },
            new() { Name = "soon", DaysRemaining = 1, ExpiresOn = today.AddDays(2).AddHours(2), InUse = false },
            new() { Name = "edge", DaysRemaining = TlsPosture.ExpiryWarningDays, ExpiresOn = today.AddDays(TlsPosture.ExpiryWarningDays + 1).AddHours(2), InUse = true }
         };

         IReadOnlyList<TlsPostureNote> notes = TlsPosture.Notes(config, today);

         Assert.Contains(notes, n => n.Level == StatusLevel.Critical
            && n.Text == "The certificate \"this morning\" expired earlier today and is still assigned to a listener. Clients are refusing or warning about every connection to it.");
         Assert.Contains(notes, n => n.Level == StatusLevel.Critical
            && n.Text == "The certificate \"last night\" expired yesterday and is still assigned to a listener. Clients are refusing or warning about every connection to it.");
         Assert.Contains(notes, n => n.Level == StatusLevel.Warning
            && n.Text == "The certificate \"spare\" expired 12 days ago. It is not assigned to any listener.");
         Assert.Contains(notes, n => n.Level == StatusLevel.Warning
            && n.Text == "The certificate \"tonight\" expires today and is in use by a listener.");
         Assert.Contains(notes, n => n.Level == StatusLevel.Warning
            && n.Text == "The certificate \"dawn\" expires tomorrow and is in use by a listener.");
         Assert.Contains(notes, n => n.Level == StatusLevel.Information
            && n.Text == "The certificate \"soon\" expires in 2 days. It is not assigned to any listener.");

         // The window is DaysRemaining's: 30 whole days left is inside it even when
         // the calendar says the 31st day, and the note counts the calendar's days.
         Assert.Contains(notes, n => n.Level == StatusLevel.Warning
            && n.Text == "The certificate \"edge\" expires in 31 days and is in use by a listener.");
      }

      [Fact]
      public void ACertificateWithoutAnExpiryDateIsCountedFromItsDaysRemaining()
      {
         // A certificate read somewhere that gave the whole days but not the date
         // keeps the day-count sentence, the floor standing in for the calendar.
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "gone", DaysRemaining = -1, InUse = true },
            new() { Name = "spare", DaysRemaining = -12, InUse = false },
            new() { Name = "today", DaysRemaining = 0, InUse = true },
            new() { Name = "soon", DaysRemaining = 1, InUse = false }
         };

         Assert.True(HasNote(config, StatusLevel.Critical, "The certificate \"gone\" expired yesterday and is still assigned to a listener."));
         Assert.True(HasNote(config, StatusLevel.Warning, "The certificate \"spare\" expired 12 days ago. It is not assigned to any listener."));
         Assert.True(HasNote(config, StatusLevel.Warning, "The certificate \"today\" expires today and is in use by a listener."));
         Assert.True(HasNote(config, StatusLevel.Information, "The certificate \"soon\" expires tomorrow. It is not assigned to any listener."));
      }

      [Fact]
      public void CalendarDaysCountsTheDateAndNotTheHours()
      {
         var evening = new DateTime(2026, 10, 9, 20, 0, 0);   // the time of day of "today" is ignored

         Assert.Equal(1, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = 0, ExpiresOn = new DateTime(2026, 10, 10, 6, 0, 0) }, evening));
         Assert.Equal(0, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = 0, ExpiresOn = new DateTime(2026, 10, 9, 23, 30, 0) }, evening));
         Assert.Equal(0, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = -1, ExpiresOn = new DateTime(2026, 10, 9, 5, 0, 0) }, evening));
         Assert.Equal(-1, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = -1, ExpiresOn = new DateTime(2026, 10, 8, 23, 0, 0) }, evening));
         Assert.Equal(2, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = 1, ExpiresOn = new DateTime(2026, 10, 11, 2, 0, 0) }, evening));
         Assert.Equal(7, TlsPosture.CalendarDays(new TlsCertificate { DaysRemaining = 7 }, evening));
         Assert.Null(TlsPosture.CalendarDays(new TlsCertificate(), evening));
      }

      [Fact]
      public void AMidnightBetweenTheReadAndThePageMakesNoNegativeCount()
      {
         // The certificates were inspected just before midnight and the notes are
         // written just after: a certificate that was expiring with 0 whole days
         // left is now on yesterday's date, and its note stays on the side
         // DaysRemaining chose with the count held at today, rather than "expires
         // in -1 days". One that expired five seconds after that midnight, read
         // just after it, expired earlier today.
         TlsPostureConfig config = Healthy();
         config.Certificates = new List<TlsCertificate>
         {
            new() { Name = "late", DaysRemaining = 0, ExpiresOn = new DateTime(2026, 10, 9, 23, 59, 50), InUse = true },
            new() { Name = "early", DaysRemaining = -1, ExpiresOn = new DateTime(2026, 10, 10, 0, 0, 5), InUse = true }
         };

         IReadOnlyList<TlsPostureNote> notes = TlsPosture.Notes(config, new DateTime(2026, 10, 10));

         Assert.Contains(notes, n => n.Text == "The certificate \"late\" expires today and is in use by a listener.");
         Assert.Contains(notes, n => n.Text.StartsWith("The certificate \"early\" expired earlier today"));
         Assert.DoesNotContain(notes, n => n.Text.Contains("-1"));
      }

      [Fact]
      public void TheDeprecatedVersionsNoteIsWrittenWholeForEachCombination()
      {
         TlsPostureConfig config = Healthy();

         config.Tls10Enabled = true;
         Assert.True(HasNote(config, StatusLevel.Warning, "TLS 1.0 is enabled. It is deprecated by RFC 8996"));
         Assert.False(HasNote(config, StatusLevel.Warning, "TLS 1.1"));

         config.Tls10Enabled = false;
         config.Tls11Enabled = true;
         Assert.True(HasNote(config, StatusLevel.Warning, "TLS 1.1 is enabled. It is deprecated by RFC 8996"));
         Assert.False(HasNote(config, StatusLevel.Warning, "TLS 1.0"));

         config.Tls10Enabled = true;
         Assert.True(HasNote(config, StatusLevel.Warning, "TLS 1.0 and TLS 1.1 are enabled. Both are deprecated by RFC 8996"));
         Assert.Equal(1, TlsPosture.Notes(config).Count(n => n.Text.Contains("RFC 8996")));
      }

      [Fact]
      public void NoVerdictOrNoteBeginsOrEndsInTheMiddleOfASentence()
      {
         // A text that starts with a lower-case word or ends without a full stop is
         // a piece of a sentence, which is what the catalogue must never hold.
         var configs = new List<TlsPostureConfig> { Healthy(), StockPorts() };
         TlsPostureConfig optional = Healthy();
         optional.Listeners = new List<TlsListener> { Listener("IMAP", 143, TlsListenerSecurity.StartTlsOptional), Listener("POP3", 110, TlsListenerSecurity.StartTlsOptional) };
         optional.Tls10Enabled = true;
         optional.Certificates = new List<TlsCertificate> { new() { Name = "a", DaysRemaining = -3, InUse = true }, new() { Name = "b", DaysRemaining = 2, InUse = false } };
         optional.RangesRequiringTlsForAuth = 0;
         configs.Add(optional);
         TlsPostureConfig broken = Healthy();
         broken.Listeners = new List<TlsListener> { Listener("IMAP", 993, TlsListenerSecurity.Implicit, certificate: ""), Listener("POP3", 995, TlsListenerSecurity.Implicit, certificate: "") };
         configs.Add(broken);

         foreach (TlsPostureConfig config in configs)
         {
            var texts = new List<string>();
            if (TlsPosture.Verdict(config) is string verdict)
               texts.Add(verdict);
            texts.AddRange(TlsPosture.Notes(config).Select(n => n.Text));
            foreach (string text in texts)
            {
               Assert.True(char.IsUpper(text[0]) || char.IsDigit(text[0]), text);
               Assert.EndsWith(".", text);
            }
         }
      }
   }
}
