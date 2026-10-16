// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    SPF, RFC 7208, evaluated against a policy this suite publishes itself.
   ///
   ///    This fixture could not be written before 15 September 2026, and the reason is
   ///    the whole point of it. The evaluator was RMSPF, a vendored C library that did
   ///    its own DNS: it called DnsQuery_A in DNSAPI.DLL, so it ignored the DNSServer
   ///    setting every other lookup in this server honours, and <see cref="SuiteDns" />
   ///    - the zone the whole suite resolves through - was invisible to it. No test
   ///    could put a policy in front of it and no test could assert a verdict, which is
   ///    why the fixtures that touch SPF up to now all say, in as many words, that they
   ///    do not depend on which verdict it reached. The void-lookup limit had to be
   ///    pinned by handing the library a policy through a back door, and the module's
   ///    own self-test evaluated the live zone of hmailserver.com and died when that
   ///    zone began answering SERVFAIL.
   ///
   ///    The RFC 7208 evaluator that replaced it resolves through this server's own
   ///    DNSResolver. So the policy below is published in the suite's zone, the server
   ///    reads it from there, and the verdict is asserted - which is the end-to-end
   ///    proof that the evaluator is wired to this server's resolver rather than merely
   ///    correct in isolation. The 203 cases of the openspf.org conformance suite prove
   ///    the second half, in process, through Utilities.RunTestSuite.
   ///
   ///    All four results asserted here are reported through the RFC 7208 section 9.1
   ///    Received-SPF header, which needs ReceivedSpfHeaderEnabled=1, and through the
   ///    RFC 8601 Authentication-Results header, which needs
   ///    AuthenticationResultsEnabled=1. Both default to 0 and both are restored in a
   ///    finally block: IniFileSettings is cached for the life of the server process,
   ///    so a leaked value poisons every fixture after this one.
   ///
   ///    Two of the four - none and softfail - are results this server could not report
   ///    at all until the evaluator was replaced. SPF::Result had three values and
   ///    everything that was not a pass or a fail was collapsed onto neutral, so a
   ///    domain that publishes no policy, a domain asking that a message be accepted
   ///    anyway, and a domain that genuinely makes no assertion were one answer.
   /// </summary>
   [TestFixture]
   public class SpfEvaluation : TestFixtureBase
   {
      /// <summary>
      ///    The client address every message in this fixture arrives from: the suite's
      ///    simulators connect to the server over loopback, so this is what the policies
      ///    below are written about.
      /// </summary>
      private const string ClientAddress = "127.0.0.1";

      [SetUp]
      public new void SetUp()
      {
         var antiSpam = _settings.AntiSpam;

         antiSpam.UseSPF = true;
         antiSpam.UseSPFScore = 3;

         // The thresholds are already 10000 from the per-test setup, so a fail cannot
         // mark or delete the message this fixture then has to fetch. The verdict is
         // what is under test, not the scoring - Basics covers that.

         IniFileSetting.Write("ReceivedSpfHeaderEnabled", "1");
         IniFileSetting.Write("AuthenticationResultsEnabled", "1");
         _application.Reinitialize();
      }

      [TearDown]
      public new void TearDown()
      {
         IniFileSetting.Write("ReceivedSpfHeaderEnabled", "0");
         IniFileSetting.Write("AuthenticationResultsEnabled", "0");
         _application.Reinitialize();

         _settings.AntiSpam.UseSPF = false;

         // Nothing this fixture published may be visible to the fixture after it: the
         // zone is the whole suite's, and a policy left in it would decide a verdict
         // somewhere that is not asserting one.
         SuiteDns.Reset();
      }

      /// <summary>
      ///    The verdict word of the Received-SPF header, lower-cased, or "" where the
      ///    message carries no such header. RFC 7208 section 9.1 puts the result first.
      /// </summary>
      private static string ReceivedSpfResult(string messageData)
      {
         var match = Regex.Match(messageData, @"^Received-SPF:\s*(\w+)",
            RegexOptions.IgnoreCase | RegexOptions.Multiline);

         return match.Success ? match.Groups[1].Value.ToLowerInvariant() : "";
      }

      /// <summary>
      ///    The spf= keyword of the Authentication-Results header, lower-cased, or ""
      ///    where there is none.
      ///
      ///    Read out of the unfolded message, because MimeCode folds a header value at
      ///    76 characters on the last space: whether spf= lands on the first line or a
      ///    continuation line depends on how long the fields ahead of it happen to be,
      ///    which for this header means the length of the sender's own domain.
      /// </summary>
      private static string AuthenticationResultsSpf(string messageData)
      {
         string unfolded = Regex.Replace(messageData, "\r\n[ \t]+", " ");

         var match = Regex.Match(unfolded, @"spf=(\w+)", RegexOptions.IgnoreCase);

         return match.Success ? match.Groups[1].Value.ToLowerInvariant() : "";
      }

      /// <summary>
      ///    Sends one inbound message from senderDomain to a fresh local account and
      ///    returns the delivered message as stored. The envelope sender is what SPF is
      ///    evaluated against (RFC 7208 section 2.4), so the domain is the input.
      /// </summary>
      private string SendFrom(string accountName, string senderDomain)
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, accountName + "@example.test", "test");

         SmtpClientSimulator.StaticSend("sender@" + senderDomain, account.Address, "SPF probe", "Probe body");

         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 1);

         var imap = new ImapClientSimulator();
         imap.ConnectAndLogon(account.Address, "test");
         imap.SelectFolder("Inbox");

         return imap.Fetch("1 RFC822");
      }

      [Test]
      [Description("A domain whose policy names the connecting client passes, and both trace headers say so. " +
                   "This is the assertion no test could make before the RFC 7208 evaluator landed: the policy " +
                   "is published in the suite's own DNS zone, so a pass here proves the evaluator resolves " +
                   "through this server's configured resolver. If it regresses, SPF is either not being " +
                   "evaluated at all or is resolving somewhere other than where this server was told to.")]
      public void APolicyThatNamesTheClientPasses()
      {
         SuiteDns.Zone.WithTxt("spf-pass.test", "v=spf1 ip4:" + ClientAddress + " -all");

         var messageData = SendFrom("spf-pass", "spf-pass.test");

         Assert.AreEqual("pass", ReceivedSpfResult(messageData),
            "The policy names the connecting client, so the verdict is a pass.\r\n" + messageData);

         Assert.AreEqual("pass", AuthenticationResultsSpf(messageData),
            "Authentication-Results must agree with Received-SPF.\r\n" + messageData);
      }

      [Test]
      [Description("A domain whose policy ends in -all and names nobody fails, and both trace headers say so. " +
                   "The negative half of the pair: a pass that is reported for every sender is not a check. " +
                   "If this regresses, a forged envelope sender from a domain that publishes a strict policy " +
                   "is accepted with no verdict against it, and DMARC loses its SPF half with it.")]
      public void APolicyThatNamesNobodyFails()
      {
         SuiteDns.Zone.WithTxt("spf-fail.test", "v=spf1 -all");

         var messageData = SendFrom("spf-fail", "spf-fail.test");

         Assert.AreEqual("fail", ReceivedSpfResult(messageData),
            "The policy authorizes nobody, so the verdict is a fail.\r\n" + messageData);

         Assert.AreEqual("fail", AuthenticationResultsSpf(messageData),
            "Authentication-Results must agree with Received-SPF.\r\n" + messageData);
      }

      [Test]
      [Description("A domain whose policy ends in ~all softfails rather than failing. RFC 7208 section 8.4 " +
                   "is the domain saying the client is probably not authorized but asking that the message be " +
                   "accepted anyway, and it is one of the four results this server could not report before the " +
                   "evaluator was replaced. If it regresses as a fail, mail from every domain still rolling " +
                   "out SPF is scored as forged; as a neutral, a receiver downstream cannot tell the domain " +
                   "said anything at all.")]
      public void APolicyEndingInTildeAllSoftFails()
      {
         SuiteDns.Zone.WithTxt("spf-softfail.test", "v=spf1 ~all");

         var messageData = SendFrom("spf-softfail", "spf-softfail.test");

         Assert.AreEqual("softfail", ReceivedSpfResult(messageData),
            "~all is a softfail, which is neither a fail nor a neutral.\r\n" + messageData);

         Assert.AreEqual("softfail", AuthenticationResultsSpf(messageData),
            "Authentication-Results must agree with Received-SPF.\r\n" + messageData);
      }

      [Test]
      [Description("A domain that publishes no SPF record at all is a none, not a neutral. RFC 8601 keeps " +
                   "those apart because they say different things: none is 'the domain has no policy' and " +
                   "neutral is 'the domain has a policy and it makes no assertion about this client'. Until " +
                   "the RFC 7208 evaluator landed this server reported both as neutral, so a downstream " +
                   "filter acting on our header was told the domain had spoken when it had not.")]
      public void ADomainWithNoRecordIsNoneRatherThanNeutral()
      {
         // Nothing published: the suite's zone answers NODATA for a name it has not been
         // given, which is exactly what a domain with no SPF record looks like.
         var messageData = SendFrom("spf-none", "spf-none.test");

         Assert.AreEqual("none", ReceivedSpfResult(messageData),
            "A domain that publishes no policy produces a none.\r\n" + messageData);

         Assert.AreEqual("none", AuthenticationResultsSpf(messageData),
            "Authentication-Results must agree with Received-SPF.\r\n" + messageData);
      }

      [Test]
      [Description("An SPF check whose lookups are each answered, but slowly, runs on in the session's handler on one " +
                   "of the TCP/IP threads for longer than a stop of the servers waits for those threads. A reinitialise " +
                   "made while it runs goes on without that thread, the servers it starts greet on port 25 while the " +
                   "check still runs, and the server is still there, and greets again, once the check has ended.")]
      public void AnSpfCheckThatOutlastsAReinitialiseLeavesTheServersRunning()
      {
         // Eight includes, each answered three seconds after it is asked: at
         // least twenty-four seconds in all - about forty-eight on server1's
         // bench and on server2, where each lookup took about six - past the ten
         // the stop waits for a TCP/IP thread and the start after it, while each
         // lookup on its own answers inside its timeout: DNSQueryTimeout's ten
         // seconds on Windows, five a name server on Linux (DnssecResolver).
         // RFC 7208 allows ten terms that look up a name; the evaluator bounds
         // their number, not their time. The names are this run's own, so that
         // no cache answers.
         const int includes = 8;
         var delay = TimeSpan.FromSeconds(3);

         string domain = "spf-slow-" + Guid.NewGuid().ToString("N").Substring(0, 12) + ".test";
         string first = "16/i1." + domain;
         string last = "16/i" + includes + "." + domain;

         var policy = new StringBuilder("v=spf1");
         for (int i = 1; i <= includes; i++)
         {
            string name = "i" + i + "." + domain;
            SuiteDns.Zone.WithTxt(name, "v=spf1 -all").WithDelay(name, FakeDnsServer.TypeTxt, delay);
            policy.Append(" include:" + name);
         }
         policy.Append(" ?all");
         SuiteDns.Zone.WithTxt(domain, policy.ToString());

         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spf-slow@example.test", "test");

         SuiteDns.Zone.ClearQueries();

         using (var held = new TcpConnection())
         {
            ClassicAssert.IsTrue(held.Connect(25), "Could not connect to port 25.");
            held.ReadUntil("220");
            held.Send("EHLO spf-slow.test\r\n");
            held.ReadUntil("250 ");

            // The check runs where the spam tests before the data run: on MAIL FROM
            // by default, on RCPT TO with DNSBLChecksAfterMailFrom=0. Both are sent,
            // and no answer is waited for.
            held.Send("MAIL FROM:<sender@" + domain + ">\r\nRCPT TO:<" + account.Address + ">\r\n");

            ClassicAssert.IsTrue(WaitForQuestion_(first, TimeSpan.FromSeconds(20)),
               "The SPF check did not ask for its first include within twenty seconds.");

            _application.Reinitialize();

            ClassicAssert.IsFalse(Asked_(last),
               "The SPF check had reached its last include before the reinitialise returned, so the reinitialise did " +
               "not go on without its thread and nothing was tested.");

            string seen;
            ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
               "The servers the reinitialise started did not greet on port 25 within twenty seconds, while the SPF " +
               "check the reinitialise went on without was still running; last seen: " + seen + ".");

            ClassicAssert.IsFalse(Asked_(last),
               "The SPF check had reached its last include before the servers the reinitialise started were seen " +
               "to greet, so they were not seen beside it.");

            // Its last lookup asked and answered, the check ends and the thread
            // leaves the handler: what a server that freed what the thread was in
            // would not survive.
            ClassicAssert.IsTrue(WaitForQuestion_(last, TimeSpan.FromSeconds(60)),
               "The SPF check did not ask for its last include within a minute.");
            Thread.Sleep(delay + TimeSpan.FromSeconds(3));

            ClassicAssert.IsTrue(TcpConnection.GreetsWithin(25, TimeSpan.FromSeconds(20), out seen),
               "The servers did not greet on port 25 within twenty seconds once the SPF check the reinitialise went " +
               "on without had ended; last seen: " + seen + ".");
         }
      }

      private static bool Asked_(string question)
      {
         return SuiteDns.Zone.Queries.Contains(question, StringComparer.OrdinalIgnoreCase);
      }

      private static bool WaitForQuestion_(string question, TimeSpan bound)
      {
         var deadline = DateTime.UtcNow + bound;

         while (!Asked_(question))
         {
            if (DateTime.UtcNow >= deadline)
               return false;

            Thread.Sleep(100);
         }

         return true;
      }
   }
}
