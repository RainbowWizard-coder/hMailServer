// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    A DNS server on 127.0.0.1:53, UDP and TCP, that answers exactly what a test
   ///    tells it to and NODATA for everything else.
   ///
   ///    Bound once for the whole run by <see cref="SuiteDns"/>, which points the
   ///    server at it; a fixture adds the names it needs and calls SuiteDns.Reset in
   ///    its teardown. Adding is safe while the serve threads answer.
   ///
   ///    It exists because tests that resolve real names are not tests of this server.
   ///    Seven of them - three DKIM verification tests, two SURBL ones, the POP3
   ///    unresolvable-host test and the MX lookup - failed across two consecutive gate
   ///    runs on 19 August 2026 for no reason connected to the change being gated, and
   ///    the measurement explains why:
   ///    <c>selector1._domainkey.outlook.com</c>, which the DKIM fixture needs, is a
   ///    CNAME into <c>protection.outlook.com</c> and takes between 60 ms and 4
   ///    seconds depending on nothing the test can control. Against the server's
   ///    10-second query timeout that is a coin toss under load, and a DKIM key that
   ///    cannot be fetched is a TEMPORARY error - so the message is accepted, and a
   ///    test asserting it is rejected fails while reporting nothing about DKIM.
   ///
   ///    Point <c>DNSServer</c> at 127.0.0.1, restart the service, and the same tests
   ///    ask a resolver that is up, is local, and answers in microseconds.
   ///
   ///    Answering NODATA rather than SERVFAIL for unknown names is deliberate: it is a
   ///    definite answer, so the resolver returns immediately instead of retrying, and
   ///    the caller sees "no such record" - which is what an SPF or DMARC lookup for a
   ///    test domain should see anyway. The SOA in the AUTHORITY section is the RFC 2308
   ///    shape; without it the response parser answers DNS_ERROR_BAD_PACKET (9502) over
   ///    TCP and the lookup fails for a reason that has nothing to do with the code
   ///    under test. That was measured, not assumed - see CustomDnsServer.
   /// </summary>
   public sealed class FakeDnsServer : IDisposable
   {
      public const int TypeA = 1;
      public const int TypeCname = 5;
      public const int TypePtr = 12;
      public const int TypeMx = 15;
      public const int TypeTxt = 16;
      public const int TypeAaaa = 28;

      // CNAME targets by owner name. A query for an owner answers the CNAME and then
      // whatever the target holds of the requested type, which is what a real server
      // does and what the resolver follows.
      private readonly Dictionary<string, string> cnames_ =
         new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

      private readonly Dictionary<string, List<string>> answers_ =
         new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);

      // Names that must answer NXDOMAIN rather than the default NODATA. The two are
      // easy to conflate and are not interchangeable: NODATA says the name is there
      // and holds nothing of the type asked for, NXDOMAIN says the name is not there
      // at all. DMARC's np= tag turns on exactly that difference, so a fixture for it
      // cannot be written against a server that only knows how to say NODATA.
      private readonly HashSet<string> nonExistent_ =
         new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      // Names whose every query is answered SERVFAIL: a resolver that could not
      // answer, which is what an SPF temperror is made of. Distinct from NXDOMAIN
      // (the name is not there) and NODATA (the name is there and holds nothing of
      // the type): both of those are answers, and this is the absence of one. A
      // "type/name" entry fails that type's queries alone.
      private readonly HashSet<string> serverFailing_ =
         new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      // Names whose every query is recorded and then not answered at all: the
      // resolver waits until its own timeout, as it does for a reverse zone
      // delegated to a name server that never answers. The one way to make a
      // lookup HANG rather than fail, which is what the never-ban list's
      // verification deadline is about.
      private readonly HashSet<string> silent_ =
         new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      // Questions - "type/name" - answered as the zone otherwise would, but only
      // so long after they were asked: a name server that answers, slowly. Inside
      // the resolver's timeout such a lookup succeeds, and each one takes that
      // long, which is how a chain of lookups that each answer is made to take
      // longer than any one lookup's timeout.
      private readonly Dictionary<string, TimeSpan> delays_ =
         new Dictionary<string, TimeSpan>(StringComparer.OrdinalIgnoreCase);

      // UDP answers go out from the serve thread and, when delayed, from timers.
      private readonly object udpSendLock_ = new object();

      // Questions - "type/name" - answered SERVFAIL so many more times before they
      // are answered as the zone otherwise would: a resolver that failed once and
      // then recovered, which is what a lookup asked a second time is for.
      private readonly Dictionary<string, int> failuresLeft_ =
         new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

      // Records given in wire form, which is how DNSSEC's own types arrive: DNSKEY, DS,
      // TLSA and the TXT or MX records of a signed zone. Keyed like the text answers by
      // the type and name a query asks for; a record's owner may differ from that name
      // (an NSEC3's is a hash).
      private sealed class RawRecord
      {
         public string Owner;
         public int Type;
         public byte[] Rdata;
      }

      private readonly Dictionary<string, List<RawRecord>> raw_ =
         new Dictionary<string, List<RawRecord>>(StringComparer.OrdinalIgnoreCase);

      // RRSIGs by the type and name they cover, sent in the answer when the query set
      // the DO bit - a resolver that does not ask for DNSSEC is not handed it, as RFC
      // 4035 3.2.1 says.
      private readonly Dictionary<string, List<RawRecord>> rrsigs_ =
         new Dictionary<string, List<RawRecord>>(StringComparer.OrdinalIgnoreCase);

      // What goes into the AUTHORITY section of a negative answer for a type and name:
      // the NSEC or NSEC3 records and their RRSIGs that prove the absence - the proof a
      // validating resolver needs before it may treat "no DS" as an unsigned delegation
      // rather than a record somebody took away.
      private readonly Dictionary<string, List<RawRecord>> denials_ =
         new Dictionary<string, List<RawRecord>>(StringComparer.OrdinalIgnoreCase);

      private readonly UdpClient udp_;
      private readonly TcpListener tcp_;
      private volatile bool stopping_;

      // Every question the server was asked, in order. Recorded for the tests that
      // care about the SHAPE of a lookup rather than its answer - the DMARC tree
      // walk is bounded at eight queries however deep the name, and a bound can only
      // be shown to hold by counting. Locked rather than concurrent because both
      // serve threads write it and a test thread reads it.
      private readonly List<string> queries_ = new List<string>();


      /// <summary>The loopback address the zone answers on, port 53.</summary>
      public IPAddress Address { get; }

      /// <summary>
      ///    The root of the suite's chain of trust, whose key is the server's
      ///    DnssecTrustAnchors for the run (SuiteDns). With one set, a query for the
      ///    root's DNSKEY that the zone holds no record for is answered with this key,
      ///    signed by itself; and a DS query for a top-level name (one label) that
      ///    the zone holds neither a DS nor a denial for is answered NODATA with an
      ///    NSEC at the name that lists NS and not DS, signed by the root - the proof
      ///    a signed root gives of an unsigned delegation. So every name the suite
      ///    uses is provably unsigned, as most of the internet's are, unless a
      ///    fixture publishes a DS for its top-level name: the validating resolver
      ///    reads an unsigned answer as unsigned only with that proof (RFC 7672
      ///    section 2.1.1), and before the suite had a root of its own every MX and
      ///    TLSA lookup of the run would have been indeterminate. Records a fixture
      ///    publishes for these names win, as everywhere else in the zone. Kept by
      ///    <see cref="Reset" />.
      /// </summary>
      public DnssecZone RootSigner { get; set; }

      public FakeDnsServer() : this(IPAddress.Loopback)
      {
      }

      /// <summary>
      ///    Serves the zone on port 53 of the given loopback address. 127.0.0.1 is
      ///    the one every fixture on the Windows bench uses; the Linux suite's
      ///    SuiteDns chooses another when that one is held by something else or,
      ///    as inside WSL2, binds and then delivers nothing.
      /// </summary>
      public FakeDnsServer(IPAddress address)
      {
         Address = address;

         try
         {
            udp_ = new UdpClient(new IPEndPoint(address, 53));
            tcp_ = new TcpListener(address, 53);
            tcp_.Start();
         }
         catch (SocketException ex)
         {
            udp_?.Close();
            tcp_?.Stop();

            // An exception and not Assert.Fail: NUnit records a failed assertion
            // against whatever is running even when the caller catches it, and
            // the Linux suite's run-wide setup catches this one to try the next
            // address - an assertion recorded there failed every test of a run
            // (13 September 2026, on the hosted runner, whose hardening agent
            // holds 127.0.0.1:53). Thrown out of a test it fails that test just
            // the same, with the same words.
            throw new InvalidOperationException("Could not bind " + address + ":53 for the fake DNS server - " +
                                                "something on this machine is already serving DNS there: " + ex.Message, ex);
         }

         new Thread(ServeUdp_) { IsBackground = true }.Start();
         new Thread(ServeTcp_) { IsBackground = true }.Start();
      }

      /// <summary>
      ///    Adds a TXT answer. The value is split into 255-byte character-strings, which
      ///    is not a detail that can be skipped: a DKIM public key does not fit in one,
      ///    the length octet would overflow, and the packet would be malformed rather
      ///    than merely truncated. RFC 6376 3.6.2 requires a verifier to concatenate the
      ///    strings, so serving a real key this way tests that too.
      /// </summary>
      public FakeDnsServer WithTxt(string name, string value)
      {
         return Add_(TypeTxt, name, value);
      }

      /// <summary>
      ///    Adds an A answer. Used for the URI-blacklist tests, where a listed name
      ///    resolves to a 127.0.0.0/8 address and an unlisted one must not resolve at
      ///    all.
      /// </summary>
      public FakeDnsServer WithA(string name, string address)
      {
         return Add_(TypeA, name, address);
      }

      /// <summary>
      ///    Adds a PTR answer: name, a reverse name such as "4.2.0.192.in-addr.arpa",
      ///    points at target.
      /// </summary>
      public FakeDnsServer WithPtr(string name, string target)
      {
         return Add_(TypePtr, name, target.TrimEnd('.'));
      }

      /// <summary>
      ///    Adds an MX answer. The value is "preference exchange", e.g. "10 mail.x.test";
      ///    the exchange usually needs its own WithA so the caller can resolve it.
      /// </summary>
      public FakeDnsServer WithMx(string name, int preference, string exchange)
      {
         return Add_(TypeMx, name, preference + " " + exchange);
      }

      /// <summary>
      ///    Makes a name answer NXDOMAIN for every query type - the name does not
      ///    exist. Any answer added for the same name wins, so a zone can hold both.
      /// </summary>
      public FakeDnsServer WithNxDomain(string name)
      {
         lock (answers_)
            nonExistent_.Add(name.TrimEnd('.').ToLowerInvariant());

         return this;
      }

      /// <summary>
      ///    Makes a name answer SERVFAIL (RCODE 2) for every query type: the resolver
      ///    could not complete the lookup. An SPF check that meets it answers temperror,
      ///    RFC 7208 section 2.6.6, which is the one result no record can produce. Any
      ///    answer added for the same name wins, as with WithNxDomain.
      /// </summary>
      public FakeDnsServer WithServFail(string name)
      {
         lock (answers_)
            serverFailing_.Add(name.TrimEnd('.').ToLowerInvariant());

         return this;
      }

      /// <summary>
      ///    Makes the queries of one type for a name answer SERVFAIL, and the name's
      ///    other types answer as they otherwise would: a resolver that failed on
      ///    that one lookup.
      /// </summary>
      public FakeDnsServer WithServFail(string name, int type)
      {
         lock (answers_)
            serverFailing_.Add(Key_(type, name));

         return this;
      }

      /// <summary>
      ///    Makes every query for name go unanswered: it is recorded in
      ///    <see cref="Queries"/> and dropped, over UDP and TCP alike, so that the
      ///    resolver waits for its timeout. Undone by <see cref="WithoutSilence"/>
      ///    and by <see cref="Reset"/>; while it lasts it wins over every answer
      ///    added for the name.
      /// </summary>
      public FakeDnsServer WithSilence(string name)
      {
         lock (answers_)
            silent_.Add(name.TrimEnd('.').ToLowerInvariant());

         return this;
      }

      /// <summary>
      ///    Makes the queries of one type for a name go unanswered, and the name's
      ///    other types answer as they otherwise would: a lookup whose one query
      ///    times out. Undone by <see cref="Reset"/>.
      /// </summary>
      public FakeDnsServer WithSilence(string name, int type)
      {
         lock (answers_)
            silent_.Add(Key_(type, name));

         return this;
      }

      /// <summary>The name answers again, as whatever was added for it says.</summary>
      public FakeDnsServer WithoutSilence(string name)
      {
         lock (answers_)
            silent_.Remove(name.TrimEnd('.').ToLowerInvariant());

         return this;
      }

      /// <summary>
      ///    Answers the queries of one type for a name as the zone otherwise would,
      ///    each one <paramref name="delay"/> after it was asked: a name server that
      ///    answers slowly, within the resolver's timeout. Over UDP the answer is sent
      ///    from a timer, so that other questions are answered meanwhile; over TCP,
      ///    which the suite's lookups reach only for an answer too long for UDP, the
      ///    serve thread waits, and the TCP questions behind it with it. Undone by
      ///    <see cref="Reset"/>.
      /// </summary>
      public FakeDnsServer WithDelay(string name, int type, TimeSpan delay)
      {
         lock (answers_)
            delays_[Key_(type, name)] = delay;

         return this;
      }

      /// <summary>
      ///    Answers the next <paramref name="times"/> queries of one type for a name
      ///    SERVFAIL, and the ones after as the zone otherwise would - over any
      ///    answer added for the name, while the failures last. A resolver that
      ///    failed and then recovered. Undone by <see cref="Reset"/>.
      /// </summary>
      public FakeDnsServer WithServFailFirst(string name, int type, int times)
      {
         lock (answers_)
            failuresLeft_[Key_(type, name)] = times;

         return this;
      }

      /// <summary>
      ///    Makes name an alias of target. A query for name answers the CNAME and then
      ///    the target's records of the asked-for type, under the target's name.
      /// </summary>
      public FakeDnsServer WithCname(string name, string target)
      {
         lock (answers_)
            cnames_[name.TrimEnd('.').ToLowerInvariant()] = target.TrimEnd('.').ToLowerInvariant();

         return this;
      }

      /// <summary>Forgets every record and every recorded query. The server stays bound.</summary>
      /// <summary>A record in wire form under a name, answered for that type with the owner as given.</summary>
      public FakeDnsServer WithRaw(string name, int type, byte[] rdata)
      {
         return AddRaw_(raw_, Key_(type, name), name, type, rdata);
      }

      /// <summary>An RRSIG covering the RRset of that type under that name, sent with it when the query has DO set.</summary>
      public FakeDnsServer WithRrsig(string name, int coveredType, byte[] rrsigRdata)
      {
         return AddRaw_(rrsigs_, Key_(coveredType, name), name, DnsWire.TypeRrsig, rrsigRdata);
      }

      /// <summary>
      ///    A record for the AUTHORITY section of the negative answer to a query for that type
      ///    and name: an NSEC or NSEC3 and its RRSIG, under whatever owner the proof has.
      /// </summary>
      public FakeDnsServer WithDenial(string queriedName, int queriedType, string owner, int type, byte[] rdata)
      {
         return AddRaw_(denials_, Key_(queriedType, queriedName), owner, type, rdata);
      }

      public void Reset()
      {
         lock (answers_)
         {
            answers_.Clear();
            nonExistent_.Clear();
            serverFailing_.Clear();
            silent_.Clear();
            delays_.Clear();
            failuresLeft_.Clear();
            cnames_.Clear();
            raw_.Clear();
            rrsigs_.Clear();
            denials_.Clear();
         }
         ClearQueries();
      }

      private FakeDnsServer AddRaw_(Dictionary<string, List<RawRecord>> store, string key, string owner, int type, byte[] rdata)
      {
         lock (answers_)
         {
            if (!store.TryGetValue(key, out List<RawRecord> records))
            {
               records = new List<RawRecord>();
               store[key] = records;
            }
            records.Add(new RawRecord { Owner = owner.TrimEnd('.').ToLowerInvariant(), Type = type, Rdata = rdata });
         }
         return this;
      }

      private FakeDnsServer Add_(int type, string name, string value)
      {
         string key = Key_(type, name);

         // Locked because, with one zone for the whole run, a fixture adds names
         // while the serve threads are answering. Idempotent because a fixture may
         // add a name the suite already seeds.
         lock (answers_)
         {
            if (!answers_.TryGetValue(key, out List<string> values))
            {
               values = new List<string>();
               answers_[key] = values;
            }

            if (!values.Contains(value))
               values.Add(value);
         }

         return this;
      }

      private static string Key_(int type, string name)
      {
         return type + "/" + name.TrimEnd('.').ToLowerInvariant();
      }

      public void Dispose()
      {
         stopping_ = true;
         udp_.Close();
         tcp_.Stop();
      }

      private void ServeUdp_()
      {
         while (!stopping_)
         {
            try
            {
               var remote = new IPEndPoint(IPAddress.Any, 0);
               byte[] query = udp_.Receive(ref remote);
               byte[] response = TryBuildResponse_(query);
               if (response == null)
                  continue;

               TimeSpan delay = DelayFor_(query);
               if (delay > TimeSpan.Zero)
                  SendUdpLater_(response, remote, delay);
               else
                  SendUdp_(response, remote);
            }
            catch (SocketException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
            catch (ObjectDisposedException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }
      }

      private void ServeTcp_()
      {
         while (!stopping_)
         {
            try
            {
               using (TcpClient client = tcp_.AcceptTcpClient())
               using (NetworkStream stream = client.GetStream())
               {
                  byte[] lengthPrefix = ReadExactly_(stream, 2);
                  int queryLength = (lengthPrefix[0] << 8) | lengthPrefix[1];
                  byte[] query = ReadExactly_(stream, queryLength);

                  byte[] response = TryBuildResponse_(query);
                  if (response == null)
                     continue;

                  TimeSpan delay = DelayFor_(query);
                  if (delay > TimeSpan.Zero)
                     Thread.Sleep(delay);

                  stream.Write(new[] { (byte) (response.Length >> 8), (byte) (response.Length & 0xFF) }, 0, 2);
                  stream.Write(response, 0, response.Length);
                  stream.Flush();
               }
            }
            catch (SocketException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
            catch (ObjectDisposedException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
            catch (IOException)
            {
               // Deliberately ignored: best effort only, and the outcome of the surrounding operation does not depend on this succeeding.
            }
         }
      }

      // How long after it was asked a question is answered (WithDelay); zero for
      // every question not given a delay. Read from a query TryBuildResponse_ has
      // answered, so its question is whole.
      private TimeSpan DelayFor_(byte[] query)
      {
         string key = Key_(QueryType_(query), QueryName_(query));

         lock (answers_)
            return delays_.TryGetValue(key, out TimeSpan delay) ? delay : TimeSpan.Zero;
      }

      private void SendUdp_(byte[] response, IPEndPoint remote)
      {
         lock (udpSendLock_)
            udp_.Send(response, response.Length, remote);
      }

      // The answer goes from a timer, and the serve thread goes back to the next
      // question at once.
      private void SendUdpLater_(byte[] response, IPEndPoint remote, TimeSpan delay)
      {
         System.Threading.Tasks.Task.Delay(delay).ContinueWith(_ =>
         {
            if (stopping_)
               return;

            try
            {
               SendUdp_(response, remote);
            }
            catch (SocketException)
            {
               // Deliberately ignored: best effort only, as for an answer sent at once.
            }
            catch (ObjectDisposedException)
            {
               // Deliberately ignored: the zone was disposed while the answer waited.
            }
         });
      }

      private static byte[] ReadExactly_(NetworkStream stream, int count)
      {
         var buffer = new byte[count];
         int read = 0;
         while (read < count)
         {
            int n = stream.Read(buffer, read, count - read);
            if (n <= 0)
               throw new IOException("Peer closed mid-message.");
            read += n;
         }
         return buffer;
      }

      private static string QueryName_(byte[] query)
      {
         var name = new StringBuilder();
         int i = 12;
         while (query[i] != 0)
         {
            if (name.Length > 0)
               name.Append('.');
            for (int c = 1; c <= query[i]; c++)
               name.Append((char) query[i + c]);
            i += query[i] + 1;
         }
         return name.ToString().ToLowerInvariant();
      }

      private static int QueryType_(byte[] query)
      {
         int zero = 12;
         while (query[zero] != 0)
            zero += query[zero] + 1;
         return (query[zero + 1] << 8) | query[zero + 2];
      }

      private static int QuestionEnd_(byte[] query)
      {
         int i = 12;
         while (query[i] != 0)
            i += query[i] + 1;
         return i + 5;
      }

      private static void WriteSoaRdata_(List<byte> m)
      {
         var soa = new List<byte>();
         WriteName_(soa, "ns.example.test");
         WriteName_(soa, "hostmaster.example.test");
         for (int f = 0; f < 5; f++)
         {
            soa.Add(0); soa.Add(0); soa.Add(0); soa.Add(60);
         }
         WriteRdata_(m, soa);
      }

      private static void WriteHeaderAndQuestion_(List<byte> m, byte[] query, int answerCount, int authorityCount = 0, int rcode = 0)
      {
         int questionEnd = QuestionEnd_(query);
         m.Add(query[0]); m.Add(query[1]);            // ID
         m.Add(0x81); m.Add((byte) (0x80 | rcode));   // QR|RD|RA, and the RCODE
         m.Add(0); m.Add(1);                          // QDCOUNT
         m.Add(0); m.Add((byte) answerCount);         // ANCOUNT
         m.Add(0); m.Add((byte) authorityCount);      // NSCOUNT
         m.Add(0); m.Add(0);                          // ARCOUNT
         for (int i = 12; i < questionEnd; i++)
            m.Add(query[i]);
      }

      private static void WriteName_(List<byte> m, string name)
      {
         // No empty label: the root ("") is the terminating zero alone, as an NSEC
         // at the root's apex names it.
         foreach (string label in name.Split(new[] { '.' }, StringSplitOptions.RemoveEmptyEntries))
         {
            m.Add((byte) label.Length);
            foreach (char c in label)
               m.Add((byte) c);
         }
         m.Add(0);
      }

      private static void WriteAnswerHeader_(List<byte> m, int recordType, string owner = null)
      {
         if (owner == null)
         {
            m.Add(0xC0); m.Add(0x0C);                 // owner: the queried name
         }
         else
         {
            WriteName_(m, owner);                     // owner: a CNAME target, spelled out
         }

         m.Add((byte) (recordType >> 8)); m.Add((byte) (recordType & 0xFF));
         m.Add(0); m.Add(1);                          // CLASS IN
         m.Add(0); m.Add(0); m.Add(0); m.Add(60);     // TTL
      }

      private static void WriteRdata_(List<byte> m, IList<byte> rdata)
      {
         m.Add((byte) (rdata.Count >> 8));
         m.Add((byte) (rdata.Count & 0xFF));
         foreach (byte b in rdata)
            m.Add(b);
      }

      private static void WriteTxtAnswer_(List<byte> m, string value, string owner)
      {
         WriteAnswerHeader_(m, TypeTxt, owner);

         var rdata = new List<byte>();
         for (int offset = 0; offset < value.Length; offset += 255)
         {
            int length = Math.Min(255, value.Length - offset);
            rdata.Add((byte) length);
            for (int c = 0; c < length; c++)
               rdata.Add((byte) value[offset + c]);
         }

         // An empty TXT is still one zero-length character-string, not nothing.
         if (rdata.Count == 0)
            rdata.Add(0);

         WriteRdata_(m, rdata);
      }

      private static void WriteMxAnswer_(List<byte> m, string value, string owner)
      {
         WriteAnswerHeader_(m, TypeMx, owner);

         string[] parts = value.Split(new[] { ' ' }, 2);
         int preference = int.Parse(parts[0]);

         var rdata = new List<byte> { (byte) (preference >> 8), (byte) (preference & 0xFF) };

         // The exchange is written as an uncompressed name. A pointer would be shorter
         // and is what a real server sends, but only when the target actually appears
         // earlier in the packet - writing one that does not is how a hand-rolled DNS
         // server produces answers a resolver reports as malformed.
         foreach (string label in parts[1].TrimEnd('.').Split('.'))
         {
            rdata.Add((byte) label.Length);
            foreach (char c in label)
               rdata.Add((byte) c);
         }
         rdata.Add(0);

         WriteRdata_(m, rdata);
      }

      private static void WriteAAnswer_(List<byte> m, string address, string owner)
      {
         WriteAnswerHeader_(m, TypeA, owner);
         WriteRdata_(m, IPAddress.Parse(address).GetAddressBytes());
      }

      // A record whose RDATA is one name, uncompressed: a PTR's target.
      private static void WriteNameAnswer_(List<byte> m, int recordType, string target, string owner)
      {
         WriteAnswerHeader_(m, recordType, owner);
         var rdata = new List<byte>();
         WriteName_(rdata, target);
         WriteRdata_(m, rdata);
      }

      /// <summary>
      ///    The questions asked so far, oldest first, as "type/name". A snapshot: the
      ///    serve threads keep appending to the real list.
      /// </summary>
      public List<string> Queries
      {
         get
         {
            lock (queries_)
               return new List<string>(queries_);
         }
      }

      /// <summary>
      ///    Forgets every recorded question. Called at the point a test starts counting,
      ///    so unrelated lookups made while the server was starting are not counted.
      /// </summary>
      public void ClearQueries()
      {
         lock (queries_)
            queries_.Clear();
      }

      /// <summary>
      ///    BuildResponse_ for a packet that may not be a query at all. The zone is
      ///    bound on 127.0.0.1:53 for the whole run, and a packet that is not a DNS
      ///    query - shorter than a header, a name that runs off its end - used to
      ///    throw out of the serving thread and, as an unhandled exception on a
      ///    background thread, take the test host with it: the Linux half of
      ///    17 September 2026 was aborted that way after every one of its 765 tests
      ///    had run. Such a packet is nobody's test. It is dropped, and a resolver
      ///    that sent it retries.
      /// </summary>
      private byte[] TryBuildResponse_(byte[] query)
      {
         if (query == null || query.Length < 17)
            return null;

         try
         {
            return BuildResponse_(query);
         }
         catch (IndexOutOfRangeException)
         {
            return null;
         }
         catch (ArgumentOutOfRangeException)
         {
            return null;
         }
      }

      private byte[] BuildResponse_(byte[] query)
      {
         string name = QueryName_(query);
         int type = QueryType_(query);

         lock (queries_)
            queries_.Add(Key_(type, name));

         lock (answers_)
         {
            if (silent_.Contains(name) || silent_.Contains(Key_(type, name)))
               return null;

            // A failure still owed to this question, before any answer.
            if (failuresLeft_.TryGetValue(Key_(type, name), out int failuresLeft) && failuresLeft > 0)
            {
               failuresLeft_[Key_(type, name)] = failuresLeft - 1;

               var failure = new List<byte>();
               WriteHeaderAndQuestion_(failure, query, 0, 0, 2);
               return failure.ToArray();
            }
         }

         string cnameTarget;
         List<string> values;
         List<RawRecord> rawAnswers;
         List<RawRecord> signatures;
         List<RawRecord> denial;
         bool nonExistent;
         bool serverFailing;
         bool dnssec = HasDoBit_(query);

         lock (answers_)
         {
            cnames_.TryGetValue(name, out cnameTarget);
            answers_.TryGetValue(Key_(type, cnameTarget ?? name), out List<string> found);
            values = found == null ? new List<string>() : new List<string>(found);
            raw_.TryGetValue(Key_(type, cnameTarget ?? name), out List<RawRecord> foundRaw);
            rawAnswers = foundRaw == null ? new List<RawRecord>() : new List<RawRecord>(foundRaw);
            rrsigs_.TryGetValue(Key_(type, cnameTarget ?? name), out List<RawRecord> foundSignatures);
            signatures = !dnssec || foundSignatures == null ? new List<RawRecord>() : new List<RawRecord>(foundSignatures);
            denials_.TryGetValue(Key_(type, name), out List<RawRecord> foundDenial);
            denial = !dnssec || foundDenial == null ? new List<RawRecord>() : new List<RawRecord>(foundDenial);
            nonExistent = nonExistent_.Contains(name.TrimEnd('.').ToLowerInvariant());
            serverFailing = serverFailing_.Contains(name.TrimEnd('.').ToLowerInvariant()) || serverFailing_.Contains(Key_(type, name));

            // The suite's root (RootSigner): its key, and its proof that a top-level
            // name is an unsigned delegation, where the zone holds nothing of its own.
            DnssecZone root = RootSigner;
            if (root != null && cnameTarget == null && values.Count == 0)
            {
               string bare = name.TrimEnd('.');

               if (type == DnsWire.TypeDnskey && bare.Length == 0 && rawAnswers.Count == 0)
               {
                  rawAnswers.Add(new RawRecord { Owner = "", Type = DnsWire.TypeDnskey, Rdata = root.DnskeyRdata });
                  if (dnssec && signatures.Count == 0)
                     signatures.Add(new RawRecord { Owner = "", Type = DnsWire.TypeRrsig, Rdata = root.Sign("", DnsWire.TypeDnskey, 60, new[] { root.DnskeyRdata }) });
               }
               else if (type == DnsWire.TypeDs && bare.Length > 0 && bare.IndexOf('.') < 0 && rawAnswers.Count == 0 &&
                        !denials_.ContainsKey(Key_(type, name)) && dnssec && !nonExistent && !serverFailing)
               {
                  byte[] nsec = DnsWire.Nsec(bare + "0", new[] { DnsWire.TypeNs, DnsWire.TypeRrsig, DnsWire.TypeNsec });
                  denial.Add(new RawRecord { Owner = bare.ToLowerInvariant(), Type = DnsWire.TypeNsec, Rdata = nsec });
                  denial.Add(new RawRecord { Owner = bare.ToLowerInvariant(), Type = DnsWire.TypeRrsig, Rdata = root.Sign(bare, DnsWire.TypeNsec, 60, new[] { nsec }) });
               }
            }
         }

         if (cnameTarget != null || values.Count > 0 || rawAnswers.Count > 0)
         {
            var m = new List<byte>();
            WriteHeaderAndQuestion_(m, query, values.Count + rawAnswers.Count + signatures.Count + (cnameTarget != null ? 1 : 0));

            if (cnameTarget != null)
            {
               // The alias first, then the target's records under the target's own
               // name - the shape RFC 1034 3.6.2 prescribes and the resolver follows.
               WriteAnswerHeader_(m, TypeCname);
               var alias = new List<byte>();
               WriteName_(alias, cnameTarget);
               WriteRdata_(m, alias);
            }

            foreach (string value in values)
            {
               if (type == TypeTxt)
                  WriteTxtAnswer_(m, value, cnameTarget);
               else if (type == TypeMx)
                  WriteMxAnswer_(m, value, cnameTarget);
               else if (type == TypePtr)
                  WriteNameAnswer_(m, TypePtr, value, cnameTarget);
               else
                  WriteAAnswer_(m, value, cnameTarget);
            }

            foreach (RawRecord record in rawAnswers)
               WriteRawAnswer_(m, record, cnameTarget ?? name);
            foreach (RawRecord record in signatures)
               WriteRawAnswer_(m, record, cnameTarget ?? name);

            return m.ToArray();
         }

         // A name declared failing: RCODE 2 and nothing else, which is what a resolver
         // that could not answer sends. Checked after the answer table for the same
         // reason NXDOMAIN is.
         if (serverFailing)
         {
            var servfail = new List<byte>();
            WriteHeaderAndQuestion_(servfail, query, 0, 0, 2);
            return servfail.ToArray();
         }

         // A name declared non-existent: RCODE 3, with the same SOA in AUTHORITY so
         // the negative answer is cacheable (RFC 2308 again). Checked after the
         // answer table so a name can be given records and still have OTHER names
         // beneath it be absent.
         if (nonExistent)
         {
            var nxdomain = new List<byte>();
            WriteHeaderAndQuestion_(nxdomain, query, 0, 1 + denial.Count, 3);
            WriteAnswerHeader_(nxdomain, 6);
            WriteSoaRdata_(nxdomain);
            foreach (RawRecord record in denial)
               WriteRawAnswer_(nxdomain, record, name);
            return nxdomain.ToArray();
         }

         // Everything else: NODATA - NOERROR, no answers, an SOA in the AUTHORITY
         // section per RFC 2308. See the class comment for why the SOA is required.
         var nodata = new List<byte>();
         WriteHeaderAndQuestion_(nodata, query, 0, 1 + denial.Count);
         WriteAnswerHeader_(nodata, 6);
         WriteSoaRdata_(nodata);
         foreach (RawRecord record in denial)
            WriteRawAnswer_(nodata, record, name);
         return nodata.ToArray();
      }

      private static void WriteRawAnswer_(List<byte> m, RawRecord record, string queryName)
      {
         // The owner is a pointer to the question when it is the queried name, and
         // spelled out otherwise - an NSEC3's hashed owner, an NSEC under a name that
         // sorts before the one asked about.
         bool sameAsQuery = string.Equals(record.Owner, queryName.TrimEnd('.'), StringComparison.OrdinalIgnoreCase);
         WriteAnswerHeader_(m, record.Type, sameAsQuery ? null : record.Owner);
         WriteRdata_(m, record.Type == TypeMx ? CompressExchange_(m, record.Rdata) : record.Rdata);
      }

      /// <summary>
      ///    An MX record's RDATA as a resolver sends it: the exchange compressed
      ///    against the question wherever its last labels are the question's last
      ///    labels - "10 mail.example.test" for example.test as "10 mail" and a
      ///    pointer to the question's name, and live.com's
      ///    live-com.olc.protection.outlook.com as four labels and a pointer to its
      ///    "com". RFC 1035 section 4.1.4 allows it for MX, and Cloudflare's, Google's
      ///    and Quad9's resolvers all do it (work item 80, 7 October 2026). The
      ///    validating resolver has to write the name out again before it checks the
      ///    RRSIG, which signs it uncompressed and in lowercase (RFC 4034 section
      ///    6.2); until the fix it did not, and every signed MX RRset sent this way
      ///    was bogus - which a zone that only ever sent the exchange spelled out
      ///    could not show. The labels before the pointer keep the case they were
      ///    given in. The question is at offset 12 of every answer this server writes.
      /// </summary>
      private static byte[] CompressExchange_(List<byte> packet, byte[] rdata)
      {
         if (rdata.Length < 3)
            return rdata;

         var exchange = new List<byte[]>();
         int offset = 2;
         while (offset < rdata.Length && rdata[offset] != 0)
         {
            int length = rdata[offset];
            if ((length & 0xC0) != 0 || offset + 1 + length > rdata.Length)
               return rdata;
            exchange.Add(rdata.Skip(offset + 1).Take(length).ToArray());
            offset += 1 + length;
         }

         if (exchange.Count == 0 || offset != rdata.Length - 1)
            return rdata;

         // The question's labels and where each starts.
         var question = new List<byte[]>();
         var starts = new List<int>();
         int position = 12;
         while (position < packet.Count && packet[position] != 0)
         {
            int length = packet[position];
            starts.Add(position);
            question.Add(packet.Skip(position + 1).Take(length).ToArray());
            position += 1 + length;
         }

         // The longest run of the question's last labels that the exchange ends with.
         for (int first = 0; first < question.Count; first++)
         {
            int shared = question.Count - first;
            if (shared > exchange.Count)
               continue;

            bool same = true;
            for (int i = 0; i < shared && same; i++)
               same = Encoding.ASCII.GetString(question[first + i])
                         .Equals(Encoding.ASCII.GetString(exchange[exchange.Count - shared + i]), StringComparison.OrdinalIgnoreCase);
            if (!same)
               continue;

            var compressed = new List<byte> { rdata[0], rdata[1] };
            for (int i = 0; i < exchange.Count - shared; i++)
            {
               compressed.Add((byte) exchange[i].Length);
               compressed.AddRange(exchange[i]);
            }
            compressed.Add((byte) (0xC0 | (starts[first] >> 8)));
            compressed.Add((byte) (starts[first] & 0xFF));
            return compressed.ToArray();
         }

         return rdata;
      }

      /// <summary>RFC 3225: the DO bit lives in the flags of the OPT record in the query's ADDITIONAL section.</summary>
      private static bool HasDoBit_(byte[] query)
      {
         int additionalCount = (query[10] << 8) | query[11];
         if (additionalCount == 0)
            return false;

         // Past the question, then past any answer or authority records the query
         // carries (none, in practice); the OPT record has the root name, TYPE 41, the
         // UDP size where CLASS goes, and the DO bit at the top of the TTL's third byte.
         int offset = QuestionEnd_(query);
         if (offset + 11 > query.Length || query[offset] != 0)
            return false;
         int recordType = (query[offset + 1] << 8) | query[offset + 2];
         return recordType == 41 && (query[offset + 7] & 0x80) != 0;
      }
   }
}
