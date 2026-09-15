// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;

namespace RegressionTests.Shared
{
   /// <summary>
   ///    What the three fixtures for bounces read and the suppression list share:
   ///    the eight settings put back, a delivery to the SMTP simulator answered with
   ///    a chosen reply, and the few readings of the documents the REST routes
   ///    answer - an item by address, a number, a string field.
   ///
   ///    The documents are read as text, the way every REST fixture in the suite
   ///    reads its answers. Every item the routes write is one flat object with no
   ///    braces in it, so "the object around this address" is a substring and no
   ///    JSON library is needed on either bench.
   /// </summary>
   public static class BounceTestSupport
   {
      public const string AdminPassword = "testar";

      // The domain every route in the suite already sends to through a route
      // pointing at localhost (TestSetup.AddRoutePointingAtLocalhost).
      public const string RemoteDomain = "dummy-example.com";

      /// <summary>
      ///    Every bounce setting back to what the server ships, whatever it holds now.
      ///    Unconditional, for a TearDown: a test that failed half way may have
      ///    changed any of them.
      /// </summary>
      public static void RestoreDefaults(Settings settings)
      {
         BounceTestSupport.ComSetBounceProcessingEnabled(settings, false);
         BounceTestSupport.ComSetBounceHardBounceThreshold(settings, 1);
         BounceTestSupport.ComSetBounceMailboxFullIsHard(settings, false);
         BounceTestSupport.ComSetBounceSuppressionAction(settings, 0);
         BounceTestSupport.ComSetBounceSuppressionExpiryDays(settings, 0);
         BounceTestSupport.ComSetBounceReadDeliveryReports(settings, true);
         BounceTestSupport.ComSetBounceCountRetentionDays(settings, 90);
         BounceTestSupport.ComSetBounceReportWindowDays(settings, 7);
      }

      /// <summary>
      ///    One route, for the remote domain, to the simulator on this port. Any route
      ///    an earlier delivery in the same test added is removed first, so a test
      ///    that delivers several times talks to the simulator it just started.
      /// </summary>
      public static void RouteTo(int port)
      {
         SingletonProvider<TestSetup>.Instance.RemoveAllRoutes();
         TestSetup.AddRoutePointingAtLocalhost(0, port, false);
      }

      /// <summary>
      ///    Sends one message from sender to recipient through this server, which
      ///    delivers it to a simulator that answers RCPT TO with code and - when
      ///    replyText is not null - with that text after the code instead of the bare
      ///    address. Returns when the queue is empty, which is after the delivery pass
      ///    has counted whatever it counts, and answers how many RCPT TOs the
      ///    simulator saw and what it was sent.
      /// </summary>
      public static int DeliverThroughSimulator(string sender, string recipient, int code, string replyText, out string messageData)
      {
         int port = TestSetup.GetNextFreePort();

         using (var server = new SmtpServerSimulator(1, port))
         {
            server.AddRecipientResult(new Dictionary<string, int> { [recipient] = code });

            if (replyText != null)
               server.RecipientReplyTexts[recipient] = replyText;

            server.StartListen();
            RouteTo(port);

            SmtpClientSimulator.StaticSend(sender, recipient, "Bounce test", "Body text.");

            server.WaitForCompletion();

            messageData = server.MessageData;

            CustomAsserts.AssertRecipientsInDeliveryQueue(0, true);

            return server.RcptTosReceived;
         }
      }

      public static int DeliverThroughSimulator(string sender, string recipient, int code, string replyText)
      {
         string ignored;
         return DeliverThroughSimulator(sender, recipient, code, replyText, out ignored);
      }

      public static string AdminHeader
      {
         get { return "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword)); }
      }

      public static (int status, string body) Http(int port, string method, string path, string requestBody = null)
      {
         return RestRequest.Send("127.0.0.1", port, method, path, AdminHeader, requestBody);
      }

      public static string Read(int port, string path)
      {
         (int status, string body) answer = Http(port, "GET", path);
         Assert.AreEqual(200, answer.status, "GET " + path + " answered " + answer.status + ": " + answer.body);
         return answer.body;
      }

      /// <summary>The item in a list document whose address is this one, or null.</summary>
      public static string ItemFor(string document, string address)
      {
         int at = document.IndexOf("\"address\":\"" + address.ToLowerInvariant() + "\"", StringComparison.Ordinal);
         if (at < 0)
            return null;

         int start = document.LastIndexOf('{', at);
         int end = document.IndexOf('}', at);

         if (start < 0 || end < 0)
            return null;

         return document.Substring(start, end - start + 1);
      }

      public static string AssertItemFor(string document, string address)
      {
         string item = ItemFor(document, address);
         Assert.IsNotNull(item, "No item for " + address + " in: " + document);
         return item;
      }

      /// <summary>A number field, as it is written: "hard":2.</summary>
      public static long Number(string json, string name)
      {
         Match match = Regex.Match(json, "\"" + Regex.Escape(name) + "\":(-?\\d+)");
         Assert.IsTrue(match.Success, "No number '" + name + "' in: " + json);
         return long.Parse(match.Groups[1].Value);
      }

      /// <summary>A string field's value, unescaped only as far as these documents need.</summary>
      public static string Text(string json, string name)
      {
         Match match = Regex.Match(json, "\"" + Regex.Escape(name) + "\":\"((?:[^\"\\\\]|\\\\.)*)\"");
         Assert.IsTrue(match.Success, "No string '" + name + "' in: " + json);
         return match.Groups[1].Value.Replace("\\\"", "\"").Replace("\\\\", "\\");
      }

      public static bool Flag(string json, string name)
      {
         Match match = Regex.Match(json, "\"" + Regex.Escape(name) + "\":(true|false)");
         Assert.IsTrue(match.Success, "No boolean '" + name + "' in: " + json);
         return match.Groups[1].Value == "true";
      }

      /// <summary>
      ///    Waits up to ten seconds for the server's default log to hold this text,
      ///    since a line written as a delivery pass ends can reach the file a moment
      ///    after the queue is empty.
      /// </summary>
      public static void AssertLogContains(string text, string because)
      {
         DateTime deadline = DateTime.UtcNow.AddSeconds(10);
         string log = LogHandler.ReadCurrentDefaultLog();

         while (log.IndexOf(text, StringComparison.Ordinal) < 0 && DateTime.UtcNow < deadline)
         {
            System.Threading.Thread.Sleep(200);
            log = LogHandler.ReadCurrentDefaultLog();
         }

         StringAssert.Contains(text, log, because);
      }

      /// <summary>
      ///    Opens an EHLO session - so the server's replies carry their enhanced codes -
      ///    gives MAIL FROM and one RCPT TO, and answers the reply to the RCPT TO. The
      ///    session is ended with QUIT before any DATA, so nothing is queued whatever
      ///    the answer was.
      /// </summary>
      public static string RcptReply(string sender, string recipient)
      {
         using (var socket = new TcpConnection())
         {
            Assert.IsTrue(socket.Connect(25), "Could not connect to the SMTP server.");
            StringAssert.StartsWith("220", socket.Receive());

            socket.Send("EHLO example.test\r\n");
            string ehlo = socket.ReadUntil("250 HELP");
            StringAssert.Contains("250", ehlo, "EHLO was not accepted.");

            string mailFrom = socket.SendAndReceive("MAIL FROM:<" + sender + ">\r\n");
            StringAssert.StartsWith("250", mailFrom, "MAIL FROM was not accepted.");

            string reply = socket.SendAndReceive("RCPT TO:<" + recipient + ">\r\n");

            socket.SendAndReceive("QUIT\r\n");

            return reply;
         }
      }

      // The bounce members of Settings and Utilities are newer than the type library
      // build-tests.ps1 compiles against until an elevated build/register-com.ps1 has
      // run, so every fixture reaches them through these, which bind late as Roles.cs's
      // helpers do; a refusal still arrives as a COMException.
      public static bool ComBounceProcessingEnabled(Settings settings) => (bool) ((dynamic) settings).BounceProcessingEnabled;

      public static void ComSetBounceProcessingEnabled(Settings settings, bool value) => ((dynamic) settings).BounceProcessingEnabled = value;

      public static bool ComBounceMailboxFullIsHard(Settings settings) => (bool) ((dynamic) settings).BounceMailboxFullIsHard;

      public static void ComSetBounceMailboxFullIsHard(Settings settings, bool value) => ((dynamic) settings).BounceMailboxFullIsHard = value;

      public static bool ComBounceReadDeliveryReports(Settings settings) => (bool) ((dynamic) settings).BounceReadDeliveryReports;

      public static void ComSetBounceReadDeliveryReports(Settings settings, bool value) => ((dynamic) settings).BounceReadDeliveryReports = value;

      public static int ComBounceHardBounceThreshold(Settings settings) => (int) ((dynamic) settings).BounceHardBounceThreshold;

      public static void ComSetBounceHardBounceThreshold(Settings settings, int value) => ((dynamic) settings).BounceHardBounceThreshold = value;

      public static int ComBounceSuppressionAction(Settings settings) => (int) ((dynamic) settings).BounceSuppressionAction;

      public static void ComSetBounceSuppressionAction(Settings settings, int value) => ((dynamic) settings).BounceSuppressionAction = value;

      public static int ComBounceSuppressionExpiryDays(Settings settings) => (int) ((dynamic) settings).BounceSuppressionExpiryDays;

      public static void ComSetBounceSuppressionExpiryDays(Settings settings, int value) => ((dynamic) settings).BounceSuppressionExpiryDays = value;

      public static int ComBounceCountRetentionDays(Settings settings) => (int) ((dynamic) settings).BounceCountRetentionDays;

      public static void ComSetBounceCountRetentionDays(Settings settings, int value) => ((dynamic) settings).BounceCountRetentionDays = value;

      public static int ComBounceReportWindowDays(Settings settings) => (int) ((dynamic) settings).BounceReportWindowDays;

      public static void ComSetBounceReportWindowDays(Settings settings, int value) => ((dynamic) settings).BounceReportWindowDays = value;

      public static string ComGetSuppressionList(Utilities utilities, string domainName, string search, int maxRows) =>
         (string) ((dynamic) utilities).GetSuppressionList(domainName, search, maxRows);

      public static bool ComAddSuppression(Utilities utilities, string domainName, string address, string note, int expiresDays) =>
         (bool) ((dynamic) utilities).AddSuppression(domainName, address, note, expiresDays);

      public static bool ComRemoveSuppression(Utilities utilities, string domainName, string address) =>
         (bool) ((dynamic) utilities).RemoveSuppression(domainName, address);

      public static string ComGetBounceCounts(Utilities utilities, string domainName, string search, int maxRows) =>
         (string) ((dynamic) utilities).GetBounceCounts(domainName, search, maxRows);

      public static string ComClassifyBounce(Utilities utilities, string status, string diagnostic) =>
         (string) ((dynamic) utilities).ClassifyBounce(status, diagnostic);

      public static int ComRunBounceMaintenance(Utilities utilities) => (int) ((dynamic) utilities).RunBounceMaintenance();
   }
}
