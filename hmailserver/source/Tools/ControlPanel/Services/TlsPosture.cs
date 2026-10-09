// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Linq;
using static hMailServer.ControlPanel.Services.Loc;

namespace hMailServer.ControlPanel.Services
{
   /// <summary>
   /// How a listener protects the conversation, in the terms the port editor uses.
   /// Values match eConnectionSecurity so the view can cast the COM value straight in.
   /// </summary>
   public enum TlsListenerSecurity
   {
      /// <summary>Nothing. Everything on this port - including AUTH - crosses the network in the clear.</summary>
      None = 0,

      /// <summary>TLS from the first byte (the old "SSL" ports: 465, 993, 995).</summary>
      Implicit = 1,

      /// <summary>STARTTLS offered. A client that does not ask for it continues unencrypted.</summary>
      StartTlsOptional = 2,

      /// <summary>STARTTLS offered and nothing else permitted until it has been used.</summary>
      StartTlsRequired = 3
   }

   /// <summary>One configured listener, as the port editor holds it.</summary>
   public sealed class TlsListener
   {
      /// <summary>"SMTP", "POP3" or "IMAP".</summary>
      public string Protocol { get; set; } = "";

      public string Address { get; set; } = "";
      public int Port { get; set; }
      public TlsListenerSecurity Security { get; set; }

      /// <summary>The name of the assigned certificate, or empty when none is assigned.</summary>
      public string CertificateName { get; set; } = "";

      /// <summary>
      /// Whether the certificates could not be read, so that an empty
      /// <see cref="CertificateName"/> says nothing: the port refers to a
      /// certificate by id, and the collection the id is resolved against was
      /// never seen. A TLS listener with no certificate does not start, and that
      /// verdict must not be drawn from a lookup that never happened.
      /// </summary>
      public bool CertificateUnread { get; set; }
   }

   /// <summary>A certificate as the SSL certificates page has already inspected it.</summary>
   public sealed class TlsCertificate
   {
      /// <summary>
      /// The database id a port refers to it by. Kept so a listener's certificate can
      /// be resolved against the collection this page has already read, rather than
      /// asking the server again once per port.
      /// </summary>
      public int Id { get; set; }

      public string Name { get; set; } = "";

      /// <summary>
      /// Whole days until it expires, the floor of the time left: 0 within the next
      /// 24 hours, -1 within the last 24. Decides the level and whether it is inside
      /// the warning window. Null when it could not be read.
      /// </summary>
      public int? DaysRemaining { get; set; }

      /// <summary>
      /// When it expires, in local time, as the certificate says. The day a note
      /// names - today, tomorrow, yesterday, in so many days - is counted on the
      /// calendar from this, not from <see cref="DaysRemaining"/>, whose 0 covers a
      /// certificate that expires at six tomorrow morning. Null when it could not be
      /// read, and the count then falls back to DaysRemaining.
      /// </summary>
      public DateTime? ExpiresOn { get; set; }

      /// <summary>A problem with the files themselves - missing, unreadable, mismatched pair. Null when there is none.</summary>
      public string Problem { get; set; }

      /// <summary>Whether any listener actually uses it.</summary>
      public bool InUse { get; set; }
   }

   /// <summary>
   /// What a listener's arrangement amounts to, apart from the words that say it,
   /// so that the verdict and the notes can count listeners by it in any language:
   /// they compared the translated word with "Will not start", which matched only
   /// in English.
   /// </summary>
   public enum TlsListenerState
   {
      NotRead,
      WillNotStart,
      CannotNegotiate,
      Encrypted,
      Opportunistic,
      InTheClear
   }

   /// <summary>What a listener's arrangement amounts to, with the wording to say it.</summary>
   public sealed class TlsListenerVerdict
   {
      internal TlsListenerVerdict(TlsListener listener, TlsListenerState state, StatusLevel level, string word, string detail)
      {
         Listener = listener;
         State = state;
         Level = level;
         Word = word;
         Detail = detail;
      }

      public TlsListener Listener { get; }
      public TlsListenerState State { get; }
      public StatusLevel Level { get; }

      /// <summary>"Encrypted", "Opportunistic", "In the clear", "Will not start" - never colour alone.</summary>
      public string Word { get; }

      /// <summary>What that means for this particular port, in one sentence.</summary>
      public string Detail { get; }
   }

   /// <summary>Something worth saying about the transport-security posture as a whole.</summary>
   public sealed class TlsPostureNote
   {
      internal TlsPostureNote(StatusLevel level, string text, string page = null)
      {
         Level = level;
         Text = text;
         Page = page ?? TlsPosture.NotesPage;
      }

      public StatusLevel Level { get; }
      public string Text { get; }

      /// <summary>
      /// The navigation key of the page that puts the note right, which a note
      /// at the Warning or Critical level opens from the overview: the SSL/TLS
      /// page unless the note names another.
      /// </summary>
      public string Page { get; }
   }

   /// <summary>
   /// A snapshot of everything that decides whether traffic to and from this server
   /// is encrypted, gathered from the four pages that own the pieces.
   /// </summary>
   public sealed class TlsPostureConfig
   {
      public List<TlsListener> Listeners { get; set; } = new();
      public List<TlsCertificate> Certificates { get; set; } = new();

      public bool Tls10Enabled { get; set; }
      public bool Tls11Enabled { get; set; }
      public bool Tls12Enabled { get; set; }
      public bool Tls13Enabled { get; set; }

      /// <summary>The OpenSSL cipher list for TLS 1.2 and below, or the AEAD-ONLY preset name.</summary>
      public string CipherList { get; set; } = "";

      /// <summary>Whether certificates are verified on the connections this server makes when delivering.</summary>
      public bool VerifyRemoteCertificates { get; set; }

      /// <summary>IP ranges that refuse AUTH until the session is encrypted, and how many ranges there are.</summary>
      public int RangesRequiringTlsForAuth { get; set; }
      public int RangesTotal { get; set; }

      /// <summary>Whether the certificate files could be examined from this machine at all.</summary>
      public bool CertificateFilesReadable { get; set; } = true;

      /// <summary>
      /// The properties that could not be read, by name, kept by the page that
      /// fills this snapshot. A value that could not be read is the type's
      /// default, which here is a real configuration - a threshold of 0, a check
      /// that is off - so nothing that follows from one may be said as a fact:
      /// the row is "Not read", the verdict is left out, and so is every note
      /// that depends on it.
      /// </summary>
      public HashSet<string> Unread { get; } = new(StringComparer.Ordinal);

      /// <summary>Whether every one of these properties was read.</summary>
      public bool WasRead(params string[] properties)
      {
         foreach (string property in properties)
         {
            if (Unread.Contains(property))
               return false;
         }

         return true;
      }
   }

   /// <summary>
   /// What this server's transport security actually amounts to, derived from the
   /// settings rather than from what they are called.
   ///
   /// No WPF here: every judgement is a pure function of a
   /// <see cref="TlsPostureConfig"/> and is tested directly.
   ///
   /// The pieces live on four separate pages - the ports decide per-listener
   /// security, the certificates page decides what can be presented, the SSL/TLS
   /// page decides which versions and ciphers are negotiable, and the IP ranges page
   /// decides whether AUTH is allowed before the session is encrypted. Each is a
   /// correct editor for its own subject, and no one of them can answer the question
   /// that matters: is anything on this server carrying a password in the clear, and
   /// will it still be serving a valid certificate next month?
   /// </summary>
   public static class TlsPosture
   {
      /// <summary>
      /// The same number CertificateInspector uses, taken from the one place that
      /// holds it, so this page and the certificates page cannot disagree about when
      /// a certificate becomes worth mentioning.
      /// </summary>
      public const int ExpiryWarningDays = StatusSemantics.CertificateExpiryWarningDays;

      /// <summary>
      /// Whether TLS is possible at all on this port's configuration.
      /// </summary>
      private static bool WantsTls(TlsListenerSecurity security) => security != TlsListenerSecurity.None;

      /// <summary>
      /// Whether this port is one where a mail CLIENT authenticates, as opposed to
      /// port 25, where the peer is normally another mail server.
      ///
      /// The distinction is the whole reason this page can be useful rather than
      /// merely alarming. Requiring TLS on 25 breaks inbound mail from senders that
      /// do not offer it, so opportunistic STARTTLS is the correct configuration
      /// there and flagging it would train the reader to ignore the page. On every
      /// other port the peer is a client with a password, and the same setting is a
      /// different thing entirely.
      /// </summary>
      public static bool IsClientPort(TlsListener listener) =>
         !(string.Equals(listener.Protocol, "SMTP", StringComparison.OrdinalIgnoreCase) && listener.Port == 25);

      /// <summary>Every listener, with what its arrangement amounts to.</summary>
      public static IReadOnlyList<TlsListenerVerdict> Listeners(TlsPostureConfig config)
      {
         var verdicts = new List<TlsListenerVerdict>();

         if (config?.Listeners == null)
            return verdicts;

         foreach (TlsListener listener in config.Listeners)
         {
            verdicts.Add(VerdictFor(listener, config));
         }

         return verdicts;
      }

      private static TlsListenerVerdict VerdictFor(TlsListener listener, TlsPostureConfig config)
      {
         // The certificates could not be read, so whether this listener has one is
         // not known - and with none it would not start. Neither verdict is given:
         // the row says the certificate was not read, at no level, as the spam and
         // virus overviews say of a check they could not read.
         if (WantsTls(listener.Security) && listener.CertificateUnread)
         {
            return new TlsListenerVerdict(listener, TlsListenerState.NotRead, StatusLevel.Normal, L("Not read"),
               L("Whether a certificate is assigned could not be read, so whether this listener starts cannot be said."));
         }

         // A TLS port with no certificate does not fall back to plaintext - it does
         // not come up. SslContextInitializer::InitServer reports HM5113 at High and
         // returns false, TCPServer::Run gives up, and the port simply is not there.
         // That is a different failure from "unencrypted" and gets its own word.
         if (WantsTls(listener.Security) && string.IsNullOrWhiteSpace(listener.CertificateName))
         {
            return new TlsListenerVerdict(listener, TlsListenerState.WillNotStart, StatusLevel.Critical, L("Will not start"),
               L("TLS is configured with no certificate assigned, so this listener fails to start and nothing can connect to this port at all. The server log records this as error HM5113 on every start."));
         }

         // Every TLS listener shares one set of enabled protocol versions. With none
         // of them enabled the handshake has nothing to agree on. Versions that
         // could not be read are not "none enabled".
         if (WantsTls(listener.Security) && VersionsRead(config) && !AnyVersionEnabled(config))
         {
            return new TlsListenerVerdict(listener, TlsListenerState.CannotNegotiate, StatusLevel.Critical, L("Cannot negotiate"),
               L("No TLS version is enabled anywhere on this server, so no handshake on this port can succeed."));
         }

         switch (listener.Security)
         {
            case TlsListenerSecurity.Implicit:
               return new TlsListenerVerdict(listener, TlsListenerState.Encrypted, StatusLevel.Good, L("Encrypted"),
                  L("TLS from the first byte. Nothing on this port is ever sent unencrypted."));

            case TlsListenerSecurity.StartTlsRequired:
               return new TlsListenerVerdict(listener, TlsListenerState.Encrypted, StatusLevel.Good, L("Encrypted"),
                  L("The session starts in the clear and nothing but STARTTLS is permitted until it has been used, so no credential and no message body crosses unencrypted."));

            case TlsListenerSecurity.StartTlsOptional:
               return IsClientPort(listener)
                  ? new TlsListenerVerdict(listener, TlsListenerState.Opportunistic, StatusLevel.Warning, L("Opportunistic"),
                     L("STARTTLS is offered but not required. A client that does not ask for it - or one talked out of asking by something in the middle - authenticates in the clear, and neither end is told."))
                  : new TlsListenerVerdict(listener, TlsListenerState.Opportunistic, StatusLevel.Good, L("Opportunistic"),
                     L("STARTTLS is offered and not required, which is the correct arrangement for port 25: requiring it would refuse mail from senders that cannot offer it."));

            // Port 25 unencrypted is a Warning and a client port a Critical, and
            // the two said the same "In the clear" in two colours: port 25 says
            // what is in the clear there, mail from other servers.
            default:
               return IsClientPort(listener)
                  ? new TlsListenerVerdict(listener, TlsListenerState.InTheClear, StatusLevel.Critical, L("In the clear"),
                     L("No encryption is available on this port at all. Every password and every message on it crosses the network readable by anything on the path."))
                  : new TlsListenerVerdict(listener, TlsListenerState.InTheClear, StatusLevel.Warning, L("Unencrypted (server mail)"),
                     L("No encryption is available on this port. Inbound mail from other servers is normally unencrypted anyway, but this port also accepts AUTH, and a client using it sends its password in the clear."));
         }
      }

      private static bool AnyVersionEnabled(TlsPostureConfig config) =>
         config != null && (config.Tls10Enabled || config.Tls11Enabled || config.Tls12Enabled || config.Tls13Enabled);

      /// <summary>
      /// Whether the four version switches were read. A switch the server did not
      /// answer is false in the snapshot, and four falses would say that no
      /// handshake can succeed.
      /// </summary>
      public static bool VersionsRead(TlsPostureConfig config) =>
         config != null && config.WasRead(nameof(config.Tls10Enabled), nameof(config.Tls11Enabled), nameof(config.Tls12Enabled), nameof(config.Tls13Enabled));

      /// <summary>The enabled protocol versions, newest first, as a readable list.</summary>
      public static string VersionSummary(TlsPostureConfig config)
      {
         if (config == null)
            return "";

         if (!VersionsRead(config))
            return L("The protocol versions could not be read.");

         var enabled = new List<string>();
         if (config.Tls13Enabled) enabled.Add(L("TLS 1.3"));
         if (config.Tls12Enabled) enabled.Add(L("TLS 1.2"));
         if (config.Tls11Enabled) enabled.Add(L("TLS 1.1"));
         if (config.Tls10Enabled) enabled.Add(L("TLS 1.0"));

         return enabled.Count == 0
            ? L("No TLS version is enabled.")
            : string.Join(", ", enabled);
      }

      /// <summary>One sentence answering "is anything here in the clear".</summary>
      public static string Verdict(TlsPostureConfig config)
      {
         if (config == null)
            return L("The transport security configuration could not be read.");

         // Listeners that could not be read are not "no listeners": the verdict
         // is not given.
         if (!config.WasRead(nameof(config.Listeners)))
            return L("Where this server stands cannot be said: the listeners could not be read.");

         IReadOnlyList<TlsListenerVerdict> listeners = Listeners(config);

         if (listeners.Count == 0)
            return L("No listeners are configured, so nothing can connect to this server.");

         List<TlsListenerVerdict> broken = Broken(listeners);
         List<TlsListenerVerdict> exposed = Exposed(listeners);
         List<TlsListenerVerdict> optional = Optional(listeners);

         // Each verdict is one whole text for one listener and one for several:
         // a sentence put together from a counted noun, its verb and a pronoun
         // read "przechodzą przez przez sieć" in Polish and "kann es aber nicht"
         // in German, since no translator can agree three pieces that are
         // translated apart. Each names its ports - "POP3 110 has no encryption
         // available" - where it said "1 client listener" and left the reader to
         // find which in the list below.
         if (broken.Count > 0)
         {
            return broken.Count == 1
               ? F("{0} is configured for TLS but cannot serve it, so that port is not listening at all.", Ports(broken))
               : F("{0} listeners are configured for TLS but cannot serve it ({1}), so those ports are not listening at all.", broken.Count, Ports(broken));
         }

         if (exposed.Count > 0)
         {
            return exposed.Count == 1
               ? F("{0} has no encryption available, so passwords used on it cross the network in the clear.", Ports(exposed))
               : F("{0} client listeners have no encryption available ({1}), so passwords used on them cross the network in the clear.", exposed.Count, Ports(exposed));
         }

         if (optional.Count > 0)
         {
            return optional.Count == 1
               ? F("{0} offers STARTTLS without requiring it, so a client that does not ask for encryption authenticates without it and nothing reports that it happened.", Ports(optional))
               : F("{0} client listeners offer STARTTLS without requiring it ({1}), so a client that does not ask for encryption authenticates without it and nothing reports that it happened.", optional.Count, Ports(optional));
         }

         return L("Every listener that a mail client connects to requires encryption before it will accept a password.");
      }

      private static List<TlsListenerVerdict> Broken(IReadOnlyList<TlsListenerVerdict> listeners) =>
         listeners.Where(v => v.State == TlsListenerState.WillNotStart || v.State == TlsListenerState.CannotNegotiate).ToList();

      private static List<TlsListenerVerdict> Exposed(IReadOnlyList<TlsListenerVerdict> listeners) =>
         listeners.Where(v => IsClientPort(v.Listener) && v.Listener.Security == TlsListenerSecurity.None).ToList();

      private static List<TlsListenerVerdict> Optional(IReadOnlyList<TlsListenerVerdict> listeners) =>
         listeners.Where(v => IsClientPort(v.Listener) && v.Listener.Security == TlsListenerSecurity.StartTlsOptional).ToList();

      /// <summary>
      /// The level the verdict is shown at: Critical for a listener that cannot
      /// serve TLS, one that offers a client no encryption, or none at all;
      /// a Warning for STARTTLS a client may skip; Good when every client port
      /// requires encryption; Information when the listeners could not be read.
      /// It was plain body text with no level.
      /// </summary>
      public static StatusLevel VerdictLevel(TlsPostureConfig config)
      {
         if (config == null || !config.WasRead(nameof(config.Listeners)))
            return StatusLevel.Information;

         IReadOnlyList<TlsListenerVerdict> listeners = Listeners(config);
         if (listeners.Count == 0 || Broken(listeners).Count > 0 || Exposed(listeners).Count > 0)
            return StatusLevel.Critical;

         return Optional(listeners).Count > 0 ? StatusLevel.Warning : StatusLevel.Good;
      }

      /// <summary>
      /// The page the verdict's action opens: TCP/IP ports, which owns each
      /// listener's security, while the verdict names listeners there; none when
      /// it has nothing to put right there.
      /// </summary>
      public static string VerdictPage(TlsPostureConfig config)
      {
         if (config == null || !config.WasRead(nameof(config.Listeners)))
            return null;

         IReadOnlyList<TlsListenerVerdict> listeners = Listeners(config);
         return listeners.Count == 0 || Broken(listeners).Count > 0 || Exposed(listeners).Count > 0 || Optional(listeners).Count > 0
            ? "ports"
            : null;
      }

      /// <summary>
      /// Whether the notes say what the verdict says: no listener at all, or,
      /// listener by listener, one that cannot serve TLS, one that offers a client
      /// no encryption, STARTTLS that a client may skip - each has a note at the
      /// verdict's level that names it, says why, and opens the page that puts it
      /// right. The overview then shows those notes and not the verdict as well:
      /// under it, "POP3 110 has no encryption available, so passwords used on it
      /// cross the network in the clear." and "POP3 on port 110 has no encryption
      /// available. Mail clients authenticate on this port..." were two Critical
      /// notices saying one thing, one more than the count beside the title. The
      /// verdict of a server whose client ports all require encryption, and of
      /// listeners not read, has no note to say it.
      /// </summary>
      public static bool NotesSayTheVerdict(TlsPostureConfig config)
      {
         if (config == null || !config.WasRead(nameof(config.Listeners)))
            return false;

         IReadOnlyList<TlsListenerVerdict> listeners = Listeners(config);
         return listeners.Count == 0 || Broken(listeners).Count > 0 || Exposed(listeners).Count > 0 || Optional(listeners).Count > 0;
      }

      /// <summary>The page a note is about unless it names another: SSL/TLS, which owns the versions and ciphers.</summary>
      public const string NotesPage = "tls";

      /// <summary>
      /// Everything about this posture an administrator would rather be told than
      /// discover from a packet capture.
      /// </summary>
      public static IReadOnlyList<TlsPostureNote> Notes(TlsPostureConfig config) => Notes(config, DateTime.Today);

      /// <summary>
      /// The notes as of <paramref name="today"/>, the calendar day the certificate
      /// notes count from. The tests pass a fixed day; the page passes today's.
      /// </summary>
      internal static IReadOnlyList<TlsPostureNote> Notes(TlsPostureConfig config, DateTime today)
      {
         var notes = new List<TlsPostureNote>();

         if (config == null)
            return notes;

         IReadOnlyList<TlsListenerVerdict> listeners = Listeners(config);

         // Nothing that follows from a value that could not be read is said: a
         // version switch the server did not answer is false here, and four
         // falses would make the worst note on the page.
         bool versions = VersionsRead(config);
         bool ranges = config.WasRead(nameof(config.RangesTotal), nameof(config.RangesRequiringTlsForAuth));

         // ---- cannot work ------------------------------------------------------

         // No listener at all is the verdict's Critical, and a note, so that the
         // count beside the title has it: that read "No problems found" beside
         // "No listeners are configured, so nothing can connect to this server."
         if (config.WasRead(nameof(config.Listeners)) && listeners.Count == 0)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Critical,
               L("No listeners are configured, so nothing can connect to this server."), "ports"));
         }

         if (versions && !AnyVersionEnabled(config) && listeners.Any(v => WantsTls(v.Listener.Security)))
         {
            notes.Add(new TlsPostureNote(StatusLevel.Critical,
               L("No TLS version is enabled, and there are listeners configured to use TLS. Not one of their handshakes can succeed until at least TLS 1.2 is switched on.")));
         }

         foreach (TlsListenerVerdict verdict in listeners.Where(v => v.State == TlsListenerState.WillNotStart))
         {
            notes.Add(new TlsPostureNote(StatusLevel.Critical,
               F("{0} on port {1} is set to use TLS with no certificate assigned, so that port does not come up. Anything expecting to reach this server on it gets a refused connection, not an unencrypted session.", verdict.Listener.Protocol, verdict.Listener.Port),
               "ports"));
         }

         foreach (TlsListenerVerdict verdict in listeners.Where(v =>
                     IsClientPort(v.Listener) && v.Listener.Security == TlsListenerSecurity.None))
         {
            notes.Add(new TlsPostureNote(StatusLevel.Critical,
               F("{0} on port {1} has no encryption available. Mail clients authenticate on this port, so every password used on it is readable by anything between the client and this server.", verdict.Listener.Protocol, verdict.Listener.Port),
               "ports"));
         }

         // ---- weak, rather than absent -----------------------------------------

         if (versions && config.Tls10Enabled && config.Tls11Enabled)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               L("TLS 1.0 and TLS 1.1 are enabled. Both are deprecated by RFC 8996 and are refused outright by current clients, so leaving them on rarely buys the compatibility they were kept for, and a client that does negotiate one gets materially weaker protection than the rest.")));
         }
         else if (versions && config.Tls10Enabled)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               L("TLS 1.0 is enabled. It is deprecated by RFC 8996 and is refused outright by current clients, so leaving it on rarely buys the compatibility it was kept for, and a client that does negotiate it gets materially weaker protection than the rest.")));
         }
         else if (versions && config.Tls11Enabled)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               L("TLS 1.1 is enabled. It is deprecated by RFC 8996 and is refused outright by current clients, so leaving it on rarely buys the compatibility it was kept for, and a client that does negotiate it gets materially weaker protection than the rest.")));
         }

         if (versions && config.WasRead(nameof(config.CipherList)) && IsAeadOnlyPreset(config.CipherList) && (config.Tls10Enabled || config.Tls11Enabled))
         {
            // From the cipher-list blurb on the SSL/TLS page, which says so in prose;
            // this is the same statement made about the configuration in front of you.
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               L("The cipher list is the AEAD-ONLY preset while TLS 1.0 or 1.1 is enabled. Those versions have no AEAD suite at all, so they are advertised and then cannot complete a handshake - which looks to the client like a broken server rather than a policy.")));
         }

         if (versions && !config.Tls13Enabled && config.Tls12Enabled)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Information,
               L("TLS 1.3 is off. It is faster to negotiate than 1.2 and removes the older key exchanges outright; there is rarely a reason to leave it off.")));
         }

         // ---- opportunistic where it should not be -----------------------------

         var optional = listeners
            .Where(v => IsClientPort(v.Listener) && v.Listener.Security == TlsListenerSecurity.StartTlsOptional)
            .ToList();

         if (optional.Count > 0)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               F("STARTTLS is optional on {0}. A client that never asks for it, or one whose STARTTLS offer is stripped in transit, carries on unencrypted and neither end is told. {1}", Ports(optional),
                  (!ranges ? L("Whether an IP range requires TLS for authentication could not be read.")
                     : config.RangesRequiringTlsForAuth > 0 ? L("Requiring TLS for authentication on the IP ranges closes the credential half of this.")
                     : L("No IP range requires TLS for authentication either, so nothing else is closing this."))),
               "ports"));
         }

         if (ranges && config.RangesTotal > 0 && config.RangesRequiringTlsForAuth == 0 && optional.Count > 0)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Warning,
               L("No IP range requires TLS before authentication. That setting lives on each IP range rather than with the rest of TLS, which is why it is easy to have never seen: it is the one control that refuses a plaintext password regardless of what the port allows."),
               "ipranges"));
         }

         // ---- certificates -----------------------------------------------------

         foreach (TlsCertificate certificate in config.Certificates ?? new List<TlsCertificate>())
         {
            if (certificate.Problem != null && certificate.InUse)
            {
               notes.Add(new TlsPostureNote(StatusLevel.Critical,
                  F("The certificate \"{0}\" is in use by a listener and has a problem: {1}", certificate.Name, certificate.Problem),
                  "certs"));
               continue;
            }

            if (certificate.DaysRemaining == null)
               continue;

            // DaysRemaining, the floor of the days to the expiry, decides the level
            // and the window: 0 is within the next 24 hours, -1 within the last 24.
            // The day the note names is counted on the calendar from the expiry
            // date, because "expires today" is a claim about the date: read at eight
            // in the evening, a certificate that expires at six the next morning has
            // a DaysRemaining of 0 and expires tomorrow, and one that expired at five
            // that morning has -1 and expired earlier today, not yesterday. Each
            // case is one whole text: "in {1}{2}" with " day" or " days" and a
            // trailing clause read "vor 5 Tage" in German and
            // "であり、リスナーが使用しています。後に期限切れになります" in
            // Japanese, the clause before the verb.
            int days = certificate.DaysRemaining.Value;
            int calendar = CalendarDays(certificate, today).Value;

            if (days < 0)
            {
               notes.Add(new TlsPostureNote(certificate.InUse ? StatusLevel.Critical : StatusLevel.Warning,
                  ExpiredNote(certificate.Name, Math.Max(0, -calendar), certificate.InUse), "certs"));
            }
            else if (days <= ExpiryWarningDays)
            {
               notes.Add(new TlsPostureNote(certificate.InUse ? StatusLevel.Warning : StatusLevel.Information,
                  ExpiringNote(certificate.Name, Math.Max(0, calendar), certificate.InUse), "certs"));
            }
         }

         if (!config.CertificateFilesReadable)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Information,
               L("The certificate files are on the server and this Control Panel is connected to it remotely, so expiry dates and file contents cannot be checked from here. Everything else on this page is read over COM and is accurate.")));
         }

         // ---- outbound ---------------------------------------------------------

         if (config.WasRead(nameof(config.VerifyRemoteCertificates)) && !config.VerifyRemoteCertificates)
         {
            notes.Add(new TlsPostureNote(StatusLevel.Information,
               L("Certificates are not verified on the connections this server makes when delivering. That is the normal setting for mail between servers - most of the internet's SMTP certificates would fail verification and the mail would stop - but it does mean outbound TLS protects against a passive listener only, not against something in the path. DANE and MTA-STS are how a specific destination gets more than that.")));
         }

         return notes;
      }

      /// <summary>The single worst note, for a one-line summary.</summary>
      public static StatusLevel WorstLevel(TlsPostureConfig config)
      {
         return Notes(config).Aggregate(StatusLevel.Normal, (worst, note) => note.Level > worst ? note.Level : worst);
      }

      /// <summary>
      /// Whether the cipher list is the AEAD-ONLY named preset rather than an
      /// OpenSSL cipher string. Matched case-insensitively, as the server does.
      /// </summary>
      public static bool IsAeadOnlyPreset(string cipherList) =>
         string.Equals((cipherList ?? "").Trim(), "AEAD-ONLY", StringComparison.OrdinalIgnoreCase); // no-loc

      private static string Ports(IEnumerable<TlsListenerVerdict> verdicts) =>
         string.Join(", ", verdicts.Select(v => v.Listener.Protocol + " " + v.Listener.Port));

      /// <summary>
      /// The calendar day the certificate expires, counted from <paramref name="today"/>:
      /// 0 is today, 1 tomorrow, -1 yesterday, whatever the time of day. Falls back to
      /// <see cref="TlsCertificate.DaysRemaining"/>, the floor of the time left, when
      /// the expiry date is not known; null when neither is.
      /// </summary>
      public static int? CalendarDays(TlsCertificate certificate, DateTime today) =>
         certificate.ExpiresOn != null
            ? (certificate.ExpiresOn.Value.Date - today.Date).Days
            : certificate.DaysRemaining;

      /// <summary>
      /// The note on a certificate that expired <paramref name="daysAgo"/> calendar
      /// days ago: 0 is earlier today, 1 yesterday.
      /// </summary>
      internal static string ExpiredNote(string name, int daysAgo, bool inUse)
      {
         if (inUse)
         {
            return daysAgo == 0
               ? F("The certificate \"{0}\" expired earlier today and is still assigned to a listener. Clients are refusing or warning about every connection to it.", name)
               : daysAgo == 1
               ? F("The certificate \"{0}\" expired yesterday and is still assigned to a listener. Clients are refusing or warning about every connection to it.", name)
               : PluralF(daysAgo,
                  "The certificate \"{0}\" expired {1} day ago and is still assigned to a listener. Clients are refusing or warning about every connection to it.",
                  "The certificate \"{0}\" expired {1} days ago and is still assigned to a listener. Clients are refusing or warning about every connection to it.",
                  name, daysAgo);
         }

         return daysAgo == 0
            ? F("The certificate \"{0}\" expired earlier today. It is not assigned to any listener.", name)
            : daysAgo == 1
            ? F("The certificate \"{0}\" expired yesterday. It is not assigned to any listener.", name)
            : PluralF(daysAgo,
               "The certificate \"{0}\" expired {1} day ago. It is not assigned to any listener.",
               "The certificate \"{0}\" expired {1} days ago. It is not assigned to any listener.",
               name, daysAgo);
      }

      /// <summary>
      /// The note on a certificate that expires in <paramref name="days"/> calendar
      /// days: 0 is today, 1 tomorrow.
      /// </summary>
      internal static string ExpiringNote(string name, int days, bool inUse)
      {
         if (inUse)
         {
            return days == 0 ? F("The certificate \"{0}\" expires today and is in use by a listener.", name)
               : days == 1 ? F("The certificate \"{0}\" expires tomorrow and is in use by a listener.", name)
               : PluralF(days,
                  "The certificate \"{0}\" expires in {1} day and is in use by a listener.",
                  "The certificate \"{0}\" expires in {1} days and is in use by a listener.",
                  name, days);
         }

         return days == 0 ? F("The certificate \"{0}\" expires today. It is not assigned to any listener.", name)
            : days == 1 ? F("The certificate \"{0}\" expires tomorrow. It is not assigned to any listener.", name)
            : PluralF(days,
               "The certificate \"{0}\" expires in {1} day. It is not assigned to any listener.",
               "The certificate \"{0}\" expires in {1} days. It is not assigned to any listener.",
               name, days);
      }

      /// <summary>
      /// The address a listener is bound to, as the overview shows it under the
      /// listener: "All addresses" for the wildcard the server stores as 0.0.0.0
      /// (or :: for IPv6, or nothing), which read as an address of its own under
      /// every listener (review finding F234); a particular address as it is.
      /// </summary>
      public static string ListenerAddress(string address)
      {
         string bound = (address ?? "").Trim();
         return bound is "" or "0.0.0.0" or "::" or "[::]" // no-loc: the wildcard addresses
            ? L("All addresses")
            : bound;
      }
   }
}
