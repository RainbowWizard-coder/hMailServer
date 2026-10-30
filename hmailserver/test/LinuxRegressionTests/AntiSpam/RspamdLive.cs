// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    Learning from the Junk folder against a real Rspamd (docs/JunkFolderLearning.md):
   ///    the server teaches the controller from IMAP moves, and the statistics Rspamd
   ///    keeps for each user are read back from its Redis. This project's own fixture,
   ///    since it needs an Rspamd and a Redis where the server and this test both reach
   ///    them; RegressionTests/AntiSpam/RspamdLearning.cs holds the request itself
   ///    against a stand-in, on Windows and here.
   ///
   ///    Run only when HMTEST_RSPAMD_PASSWORD is set: the controller's password, with
   ///    the controller at HMTEST_RSPAMD_CONTROLLER (http://127.0.0.1:11334 when unset)
   ///    and its Redis at HMTEST_RSPAMD_REDIS (127.0.0.1:6379), and the controller's
   ///    bayes classifier per user, named bayes_user, in that Redis
   ///    (local.d/classifier-bayes.conf: per_user = true; name = "bayes_user").
   ///    Otherwise every test is skipped, and says why.
   /// </summary>
   [TestFixture]
   public class RspamdLive : TestFixtureBase
   {
      private const string Password = "test";
      private const string Classifier = "bayes_user";
      private string _controller;
      private string _redisHost;
      private int _redisPort;

      [SetUp]
      public new void SetUp()
      {
         string password = Environment.GetEnvironmentVariable("HMTEST_RSPAMD_PASSWORD");
         if (string.IsNullOrEmpty(password))
            NotOnThisServer.Ignore("no Rspamd to teach: HMTEST_RSPAMD_PASSWORD is not set");

         if (!TestTarget.IsLocal)
            NotOnThisServer.Ignore("the Rspamd this test reads is on this machine, and the server under test is at " + TestTarget.Host);

         _controller = Environment.GetEnvironmentVariable("HMTEST_RSPAMD_CONTROLLER");
         if (string.IsNullOrEmpty(_controller))
            _controller = "http://127.0.0.1:11334";

         string redis = Environment.GetEnvironmentVariable("HMTEST_RSPAMD_REDIS");
         if (string.IsNullOrEmpty(redis))
            redis = "127.0.0.1:6379";
         _redisHost = redis.Substring(0, redis.LastIndexOf(':'));
         _redisPort = int.Parse(redis.Substring(redis.LastIndexOf(':') + 1));

         WaitForController();

         var antiSpam = _settings.AntiSpam;
         antiSpam.MoveLearningBackend = 1;
         antiSpam.RspamdControllerUrl = _controller;
         antiSpam.SetRspamdControllerPassword(password);
         antiSpam.RspamdClassifier = Classifier;
         antiSpam.MoveLearningFromCopy = true;
      }

      [TearDown]
      public new void TearDown()
      {
         var antiSpam = _settings.AntiSpam;
         antiSpam.MoveLearningBackend = 0;
         antiSpam.RspamdControllerUrl = "";
         antiSpam.SetRspamdControllerPassword("");
         antiSpam.RspamdClassifier = "";
      }

      // Rspamd answers /ping once its workers are up, which can be a while after
      // its container starts.
      private void WaitForController()
      {
         var watch = Stopwatch.StartNew();
         while (watch.Elapsed.TotalSeconds < 90)
         {
            try
            {
               var uri = new Uri(_controller);
               using (var client = new TcpClient(uri.Host, uri.Port))
               using (var stream = client.GetStream())
               {
                  byte[] request = Encoding.ASCII.GetBytes("GET /ping HTTP/1.0\r\nHost: " + uri.Host + "\r\n\r\n");
                  stream.Write(request, 0, request.Length);
                  string answer = new StreamReader(stream, Encoding.ASCII).ReadToEnd();
                  if (answer.Contains("pong"))
                     return;
               }
            }
            catch (Exception)
            {
            }

            Thread.Sleep(1000);
         }

         Assert.Fail("Rspamd's controller at " + _controller + " did not answer /ping within 90 seconds.");
      }

      // One RESP command to Redis, its reply as text: a bulk string's value, an
      // integer's digits, or null for a null bulk string.
      private string Redis(params string[] words)
      {
         using (var client = new TcpClient(_redisHost, _redisPort))
         using (var stream = client.GetStream())
         {
            var command = new StringBuilder();
            command.Append("*").Append(words.Length).Append("\r\n");
            foreach (string word in words)
            {
               int length = Encoding.UTF8.GetByteCount(word);
               command.Append("$").Append(length).Append("\r\n").Append(word).Append("\r\n");
            }

            byte[] bytes = Encoding.UTF8.GetBytes(command.ToString());
            stream.Write(bytes, 0, bytes.Length);

            var reader = new StreamReader(stream, Encoding.UTF8);
            string line = reader.ReadLine();
            if (line == null)
               return null;

            if (line.StartsWith(":") || line.StartsWith("+"))
               return line.Substring(1);

            if (line.StartsWith("$-1"))
               return null;

            if (line.StartsWith("$"))
               return reader.ReadLine();

            Assert.Fail("Redis answered " + line);
            return null;
         }
      }

      private int Learns(string user, string field)
      {
         string value = Redis("HGET", "RS" + user, field);
         return value == null ? 0 : int.Parse(value);
      }

      // Waits until the user's spam and ham learns are what is expected.
      private void WaitForLearns(string user, int spam, int ham)
      {
         var watch = Stopwatch.StartNew();
         while (watch.Elapsed.TotalSeconds < 30)
         {
            if (Learns(user, "learns_spam") == spam && Learns(user, "learns_ham") == ham)
               return;
            Thread.Sleep(250);
         }

         Assert.Fail(string.Format("Rspamd's statistics for {0}: {1} spam and {2} ham learned, where {3} and {4} were expected.",
            user, Learns(user, "learns_spam"), Learns(user, "learns_ham"), spam, ham));
      }

      private static string Spammy(int variant)
      {
         return "Buy cheap watches now " + variant + ", limited offer, click the link below to claim your prize, winners are chosen " +
                "every day, act today, free shipping on every order, discount pharmacy pills without prescription, best prices.";
      }

      [Test]
      [Description("Moves into and out of Junk teach a real Rspamd, into the owner's statistics alone: spam, the correction to ham, spam again, the same lesson twice changing nothing, and another account's move counted for that account.")]
      public void MovesTeachTheOwnersStatistics()
      {
         string suffix = Guid.NewGuid().ToString("N").Substring(0, 8);
         var owner = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rspamd-live-" + suffix + "@example.test", Password);
         var other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rspamd-other-" + suffix + "@example.test", Password);
         string ownerAddress = owner.Address;
         string otherAddress = other.Address;

         SmtpClientSimulator.StaticSend("sender@example.test", ownerAddress, "Cheap watches " + suffix, Spammy(1));
         Pop3ClientSimulator.AssertMessageCount(ownerAddress, Password, 1);

         var client = new ImapClientSimulator();
         Assert.IsTrue(client.ConnectAndLogon(ownerAddress, Password));
         Assert.IsTrue(client.CreateFolder("Junk"));
         Assert.IsTrue(client.SelectFolder("INBOX"));

         // Into Junk: spam, for the owner.
         StringAssert.Contains("A10 OK", client.SendSingleCommand("A10 MOVE 1 Junk"));
         WaitForLearns(ownerAddress, 1, 0);

         // Back out: the correction, unlearned as spam and learned as ham.
         Assert.IsTrue(client.SelectFolder("Junk"));
         StringAssert.Contains("A11 OK", client.SendSingleCommand("A11 UID MOVE 1:* INBOX"));
         WaitForLearns(ownerAddress, 0, 1);

         // A COPY into Junk: spam again, the ham unlearned.
         Assert.IsTrue(client.SelectFolder("INBOX"));
         Assert.IsTrue(client.Copy(1, "Junk"));
         WaitForLearns(ownerAddress, 1, 0);

         // The same message copied into Junk again: already learned as spam, nothing changes.
         Assert.IsTrue(client.Copy(1, "Junk"));
         Thread.Sleep(3000);
         Assert.AreEqual(1, Learns(ownerAddress, "learns_spam"), "The same lesson twice is learned once.");
         Assert.AreEqual(0, Learns(ownerAddress, "learns_ham"));
         client.Disconnect();

         // Another account's move is that account's, and the owner's are unchanged.
         SmtpClientSimulator.StaticSend("sender@example.test", otherAddress, "Cheap watches " + suffix, Spammy(2));
         Pop3ClientSimulator.AssertMessageCount(otherAddress, Password, 1);
         var second = new ImapClientSimulator();
         Assert.IsTrue(second.ConnectAndLogon(otherAddress, Password));
         Assert.IsTrue(second.CreateFolder("Junk"));
         Assert.IsTrue(second.SelectFolder("INBOX"));
         StringAssert.Contains("A20 OK", second.SendSingleCommand("A20 MOVE 1 Junk"));
         second.Disconnect();

         WaitForLearns(otherAddress, 1, 0);
         Assert.AreEqual(1, Learns(ownerAddress, "learns_spam"), "Another account's lesson is not the owner's.");

         // No lesson was given to a classifier without a user: Rspamd keeps one
         // under RS alone for that.
         Assert.AreEqual("0", Redis("EXISTS", "RS"), "The global statistics were not taught.");
      }
   }
}
