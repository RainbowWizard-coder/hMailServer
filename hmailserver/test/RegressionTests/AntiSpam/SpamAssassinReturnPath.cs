// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.AntiSpam
{
   /// <summary>
   ///    The Return-Path a SpamAssassin scan is sent, and the stored message it must not
   ///    change.
   ///
   ///    SpamAssassin reads the envelope sender from a Return-Path at the top of the
   ///    message, and a message being accepted has none yet, so the scan sends one. Until
   ///    6.3.7 it was written into the stored message before the scan and taken out
   ///    afterwards by parsing the whole message and writing it again: a scan that failed
   ///    - spamd refusing the connection, resetting it, or cutting its reply short - left
   ///    it in the message, under the one the delivery then wrote, and a scan that
   ///    succeeded rewrote bytes SpamAssassin never touched (a multipart message's
   ///    epilogue, here). Now it is sent ahead of the message in the request alone, the
   ///    message's own Return-Path fields are left out of the request, the line is left
   ///    out of spamd's reply byte for byte, and the stored file is never written for it.
   ///
   ///    Against a simulated spamd, because the question is what was sent and what came
   ///    back, byte for byte, and only a spamd of our own can say. Each test reads the
   ///    stored message file itself: the delivery's Return-Path, then exactly what was
   ///    there before the scan - or, after a completed scan, exactly what spamd returned
   ///    less the line. And two collect a message from an external account: one is
   ///    saved straight after the scan, with the size of spamd's result, which the
   ///    rewrite used to set and which a global rule on the size reads; and one is
   ///    scanned with the Return-Path the other server's delivery recorded in it, the
   ///    message's topmost, where the server has only a guess at its envelope sender
   ///    from its From field.
   /// </summary>
   [TestFixture]
   public class SpamAssassinReturnPath : TestFixtureBase
   {
      // Pre-allocated to this fixture (47080-47099). Nothing listens on the first,
      // which the test that uses it checks first; the second and third are the
      // simulated remote POP3 servers messages are collected from.
      private const int RefusedPort = 47080;
      private const int FetchPort = 47081;
      private const int RecordedFetchPort = 47082;

      private const string Sender = "rp-sender@example.com";

      private static readonly Encoding Bytes = Encoding.GetEncoding(28591);

      // A message whose bytes a parse-and-write of the whole message changes: the
      // epilogue after the closing boundary is dropped by one. The folded field is
      // here because a field rewritten from its parsed value is refolded.
      private const string TestMessage =
         "From: Sender <" + Sender + ">\r\n" +
         "To: rp@example.test\r\n" +
         "Subject: Return-Path byte test\r\n" +
         "Message-ID: <return-path-byte-test@example.com>\r\n" +
         "X-Folded: the first part\r\n" +
         "\tand the second, after a tab\r\n" +
         "MIME-Version: 1.0\r\n" +
         "Content-Type: multipart/mixed; boundary=\"rp-boundary\"\r\n" +
         "\r\n" +
         "This is the preamble.\r\n" +
         "--rp-boundary\r\n" +
         "Content-Type: text/plain\r\n" +
         "\r\n" +
         "Part one.\r\n" +
         "--rp-boundary--\r\n" +
         "This is the epilogue, which a rewrite of the whole message drops.\r\n";

      private const string SpamHeaders =
         "X-Spam-Checker-Version: SpamAssassin 4.0.2 (2025-08-27) on simulator\r\n" +
         "X-Spam-Level: \r\n" +
         "X-Spam-Status: No, score=0.0 required=5.0 tests=none autolearn=no\r\n" +
         "\tversion=4.0.2\r\n";

      private Account _account;
      private SpamdSimulator _spamd;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "rp@example.test", "test");

         _spamd = new SpamdSimulator();

         var antiSpam = _settings.AntiSpam;
         antiSpam.SpamMarkThreshold = 1;
         antiSpam.SpamDeleteThreshold = 10000;
         antiSpam.SpamAssassinEnabled = true;
         antiSpam.SpamAssassinHost = "127.0.0.1";
         antiSpam.SpamAssassinPort = _spamd.Port;
         antiSpam.SpamAssassinMergeScore = false;
         antiSpam.SpamAssassinScore = 5;
      }

      [TearDown]
      public new void TearDown()
      {
         if (_spamd != null)
            _spamd.Dispose();
      }

      private static string ReturnPathLine
      {
         get { return "Return-Path: <" + Sender + ">\r\n"; }
      }

      [Test]
      [Description("A completed scan: the Return-Path goes ahead of the message, counted in Content-length, and spamd's result is stored byte for byte without it")]
      public void TheReturnPathIsSentAheadOfTheMessageAndTheResultIsStoredByteForByte()
      {
         // spamd's own layout: the Return-Path first, its X-Spam fields under it.
         _spamd.Reply = message => InsertAfterFirstLine(message, SpamHeaders);

         var stored = DeliverAndReadStoredFile(TestMessage);

         var request = _spamd.WaitForRequest(0);
         var sent = Bytes.GetString(_spamd.WaitForMessage(0));

         ClassicAssert.AreEqual(sent.Length.ToString(), request["Content-length"], "Content-length is not the length of what was sent.");
         StringAssert.StartsWith(ReturnPathLine, sent, "The envelope sender's Return-Path is not the first line of the message spamd was sent.");
         CollectionAssert.AreEqual(new[] { "<" + Sender + ">" }, ReturnPathFields(sent), "spamd was not sent exactly one Return-Path.");
         StringAssert.EndsWith(TestMessage, sent, "The message spamd was sent does not end with the message as it was sent to the server.");

         var before = sent.Substring(ReturnPathLine.Length);
         ClassicAssert.AreEqual(DeliveryPrefix() + SpamHeaders + before, stored,
            "The stored message is not the delivery's Return-Path and spamd's result less the line it was sent, byte for byte.");
      }

      [Test]
      [Description("A Return-Path the message arrived with is left out of what spamd is sent, so that spamd sees one envelope sender and it is the server's")]
      public void AReturnPathTheMessageArrivedWithIsNotSentToSpamd()
      {
         var message = "Return-Path: <arrived-with@example.net>\r\n" + TestMessage;

         var stored = DeliverAndReadStoredFile(message);

         var request = _spamd.WaitForRequest(0);
         var sent = Bytes.GetString(_spamd.WaitForMessage(0));

         CollectionAssert.AreEqual(new[] { "<" + Sender + ">" }, ReturnPathFields(sent),
            "spamd was not sent exactly one Return-Path, the envelope sender's.");
         ClassicAssert.AreEqual(sent.Length.ToString(), request["Content-length"], "Content-length is not the length of what was sent.");
         StringAssert.EndsWith(TestMessage, sent, "The rest of the message was not sent as it is.");

         // The scan completed, and its result - the message as spamd was sent it, less
         // the line - replaced the stored message, as a completed scan's result always
         // has: the Return-Path the message arrived with is not in it, as it was not
         // after the rewrite that went before.
         ClassicAssert.AreEqual(DeliveryPrefix() + sent.Substring(ReturnPathLine.Length), stored);
      }

      [Test]
      [Description("spamd refuses the connection: the stored message is as the server received it, with no Return-Path from the scan, and keeps the one it arrived with")]
      public void WhenSpamdRefusesTheConnectionTheStoredMessageIsUnchanged()
      {
         AssertNothingListensOn(RefusedPort);
         _settings.AntiSpam.SpamAssassinPort = RefusedPort;

         var message = "Return-Path: <arrived-with@example.net>\r\n" + TestMessage;

         var stored = DeliverAndReadStoredFile(message);

         CustomAsserts.AssertReportedError("The SpamAssassin tests did not complete");

         AssertStoredAsReceived(stored, message);
      }

      [Test]
      [Description("spamd reads the request and resets the connection instead of answering: the stored message is byte for byte what spamd was sent, less the line")]
      public void WhenSpamdResetsTheConnectionTheStoredMessageIsUnchanged()
      {
         _spamd.Mode = SpamdSimulatorMode.ResetAfterRequest;

         var stored = DeliverAndReadStoredFile(TestMessage);

         CustomAsserts.AssertReportedError("The SpamAssassin tests did not complete");

         var sent = Bytes.GetString(_spamd.WaitForMessage(0));
         StringAssert.StartsWith(ReturnPathLine, sent);
         ClassicAssert.AreEqual(DeliveryPrefix() + sent.Substring(ReturnPathLine.Length), stored,
            "The stored message is not what it was before the scan.");
      }

      [Test]
      [Description("spamd's reply stops half-way and the connection closes: the stored message is byte for byte what spamd was sent, less the line")]
      public void WhenSpamdCutsItsReplyShortTheStoredMessageIsUnchanged()
      {
         _spamd.Mode = SpamdSimulatorMode.CutShort;
         _spamd.Reply = message => InsertAfterFirstLine(message, SpamHeaders);

         var stored = DeliverAndReadStoredFile(TestMessage);

         CustomAsserts.AssertReportedError("The SpamAssassin tests did not complete");

         var sent = Bytes.GetString(_spamd.WaitForMessage(0));
         StringAssert.StartsWith(ReturnPathLine, sent);
         ClassicAssert.AreEqual(DeliveryPrefix() + sent.Substring(ReturnPathLine.Length), stored,
            "The stored message is not what it was before the scan.");
      }

      [Test]
      [Description("spamd hangs up while the server is still sending a large message: the stored message is as the server received it, with no Return-Path from the scan")]
      public void WhenSpamdHangsUpMidRequestTheStoredMessageIsUnchanged()
      {
         _spamd.Mode = SpamdSimulatorMode.HangUpMidRequest;
         _spamd.HangUpAfterBytes = 100;

         // Some 400 KB: well under the 1 MB the scan is limited to by default, and more
         // than a loopback connection takes before spamd has read anything.
         var body = new StringBuilder();
         for (int i = 0; i < 5500; i++)
            body.Append("Line ").Append(i.ToString("D5")).Append(" of a message long enough to be cut off while it is being sent.\r\n");

         var message =
            "From: Sender <" + Sender + ">\r\n" +
            "To: rp@example.test\r\n" +
            "Subject: Return-Path hang-up test\r\n" +
            "\r\n" +
            body;

         var stored = DeliverAndReadStoredFile(message);

         _spamd.WaitForRequest(0);
         CustomAsserts.AssertReportedError("The SpamAssassin tests did not complete");

         AssertStoredAsReceived(stored, message);
      }

      [Test]
      [Description("A message collected from an external account and scanned is saved with the size of spamd's result, which replaced its file, so that a global rule on the size reads the message's size")]
      public void ACollectedMessageIsSavedWithTheSizeOfTheScannedFile()
      {
         // SpamAssassin's report fields, some 6 KB: the result is that much larger
         // than the message collected.
         var report = new StringBuilder(SpamHeaders);
         report.Append("X-Spam-Report:\r\n");
         for (int i = 0; i < 80; i++)
            report.Append("\t* 0.0 RP_SIZE_").Append(i.ToString("D2")).Append(" A line of the report SpamAssassin adds to the message\r\n");
         _spamd.Reply = message => InsertAfterFirstLine(message, report.ToString());

         // Between the two sizes: the message as collected, with what the server adds
         // to it on the way in, is well under it, and the scanned one well over it.
         int threshold = TestMessage.Length + 3000;
         ClassicAssert.Greater(report.Length, 6000);

         var rule = _application.Rules.Add();
         rule.Name = "Larger than the message collected";
         rule.Active = true;

         var criteria = rule.Criterias.Add();
         criteria.UsePredefined = true;
         criteria.PredefinedField = eRulePredefinedField.eFTMessageSize;
         criteria.MatchType = eRuleMatchType.eMTGreaterThan;
         criteria.MatchValue = threshold.ToString();
         criteria.Save();

         var action = rule.Actions.Add();
         action.Type = eRuleActionType.eRASetHeaderValue;
         action.HeaderName = "X-Rp-Size-Rule";
         action.Value = "larger";
         action.Save();

         rule.Save();

         Collect(TestMessage, FetchPort, "Return-Path size");

         var stored = ReadStoredFile();

         var sent = Bytes.GetString(_spamd.WaitForMessage(0));
         StringAssert.StartsWith(ReturnPathLine, sent, "The collected message was not scanned with the envelope sender's Return-Path ahead of it.");
         StringAssert.Contains("X-Spam-Report:", stored, "spamd's result did not replace the collected message.");
         ClassicAssert.Less(sent.Length - ReturnPathLine.Length, threshold, "The message collected is not under the rule's size; the test proves nothing.");

         StringAssert.Contains("X-Rp-Size-Rule: larger", stored,
            "The global rule on the size did not match: the collected message was saved with its size from before the scan, not its file's.");
      }

      [Test]
      [Description("A message collected from an external account is scanned with the Return-Path the other server's delivery recorded - its topmost, under that server's own Received fields as Gmail writes it - not with a sender guessed from its From field")]
      public void ACollectedMessageIsScannedWithTheReturnPathTheOtherServerRecorded()
      {
         const string recorded = "<bounce-rp@lists.example.org>";
         const string upstream = "<upstream-rp@example.net>";
         const string accountName = "Return-Path recorded";

         // Gmail's layout: its own fields first, the Return-Path its delivery wrote
         // under them, and under that the fields and a Return-Path the message came
         // in with. Until 6.3.7 spamd was sent a Return-Path built from the From
         // field above all of these, and SpamAssassin, finding a Return-Path under a
         // Received field, trusted none of them.
         var header =
            "Delivered-To: rp-collected@example.com\r\n" +
            "Received: by 2002:a05:6000:1a8c:b0:3d4:1 with SMTP id x1csp1;\r\n" +
            "        Mon, 12 Oct 2026 10:00:00 -0700 (PDT)\r\n" +
            "X-Received: by 2002:a05:6000:1a8c with SMTP id y2; Mon, 12 Oct 2026 10:00:00 -0700 (PDT)\r\n" +
            "Return-Path: " + recorded + "\r\n" +
            "Received: from mail.lists.example.org by mx.example.com with ESMTPS id z3;\r\n" +
            "        Mon, 12 Oct 2026 10:00:00 -0700 (PDT)\r\n" +
            "Return-Path: " + upstream + "\r\n";

         _spamd.Reply = message => InsertAfterFirstLine(message, SpamHeaders);

         Collect(header + TestMessage, RecordedFetchPort, accountName);

         var stored = ReadStoredFile();

         var request = _spamd.WaitForRequest(0);
         var sent = Bytes.GetString(_spamd.WaitForMessage(0));
         var line = "Return-Path: " + recorded + "\r\n";

         StringAssert.StartsWith(line + "X-hMailServer-ExternalAccount: " + accountName + "\r\n", sent,
            "The collected message was not scanned with the Return-Path the other server recorded ahead of it.");
         CollectionAssert.AreEqual(new[] { recorded }, ReturnPathFields(sent),
            "spamd was not sent exactly one Return-Path, the one the other server recorded.");
         ClassicAssert.AreEqual(sent.Length.ToString(), request["Content-length"], "Content-length is not the length of what was sent.");

         // The rest of the message as it was collected, less its own Return-Path fields.
         var headerLeft = header.Replace("Return-Path: " + recorded + "\r\n", "").Replace("Return-Path: " + upstream + "\r\n", "");
         StringAssert.Contains(headerLeft + TestMessage.Substring(0, TestMessage.IndexOf("\r\n\r\n", StringComparison.Ordinal)), sent,
            "The header of the collected message was not sent as it is, less its own Return-Path fields.");

         // The result replaced the message: spamd's, less the line - and so without
         // the Return-Path fields the message was collected with, as after the
         // rewrite that went before.
         ClassicAssert.AreEqual(DeliveryPrefix() + SpamHeaders + sent.Substring(line.Length), stored,
            "The stored message is not the delivery's Return-Path and spamd's result less the line it was sent, byte for byte.");
      }

      // Collects message from a simulated POP3 server on port, for the fixture's
      // account, through an external account of the name given that runs the spam
      // tests, and waits until the simulator has served it.
      private void Collect(string message, int port, string accountName)
      {
         AssertNothingListensOn(port);

         using (var pop3Server = new Pop3ServerSimulator(1, port, new List<string> { message }))
         {
            pop3Server.StartListen();

            var fetchAccount = _account.FetchAccounts.Add();
            fetchAccount.Enabled = true;
            fetchAccount.MinutesBetweenFetch = 10;
            fetchAccount.Name = accountName;
            fetchAccount.Username = "test@example.com";
            fetchAccount.Password = "test";
            fetchAccount.UseSSL = false;
            fetchAccount.ServerAddress = "localhost";
            fetchAccount.Port = port;
            fetchAccount.ProcessMIMERecipients = false;
            fetchAccount.DaysToKeepMessages = -1;
            fetchAccount.UseAntiSpam = true;
            fetchAccount.Save();
            fetchAccount.DownloadNow();

            pop3Server.WaitForCompletion();
         }
      }

      private string DeliverAndReadStoredFile(string message)
      {
         new SmtpClientSimulator().SendRaw(Sender, _account.Address, message);

         return ReadStoredFile();
      }

      private string ReadStoredFile()
      {
         var stored = CustomAsserts.AssertGetFirstMessage(_account, "INBOX");

         using (var file = new FileStream(stored.Filename, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
         using (var copy = new MemoryStream())
         {
            file.CopyTo(copy);
            return Bytes.GetString(copy.ToArray());
         }
      }

      // What local delivery writes on top of every message it stores.
      private string DeliveryPrefix()
      {
         var prefix = ReturnPathLine;
         if (_settings.AddDeliveredToHeader)
            prefix += "Delivered-To: " + _account.Address + "\r\n";
         return prefix;
      }

      // For a scan that failed before spamd saw the whole message: the delivery's
      // fields, then what the server wrote when it received the message - its Received
      // field first, and no Return-Path among them - then the message as it was sent,
      // byte for byte.
      private void AssertStoredAsReceived(string stored, string message)
      {
         var prefix = DeliveryPrefix();
         StringAssert.StartsWith(prefix, stored, "The stored message does not start with the delivery's Return-Path.");
         StringAssert.EndsWith(message, stored, "The stored message does not end with the message as it was sent.");
         ClassicAssert.Greater(stored.Length, prefix.Length + message.Length, "The server's Received field is missing.");

         var receivedWith = stored.Substring(prefix.Length, stored.Length - prefix.Length - message.Length);
         StringAssert.StartsWith("Received:", receivedWith,
            "Something other than the server's Received field lies between the delivery's Return-Path and the message: " + receivedWith);
         CollectionAssert.IsEmpty(ReturnPathFields(receivedWith), "A Return-Path from the scan was left in the stored message: " + receivedWith);
      }

      private static void AssertNothingListensOn(int port)
      {
         using (var client = new TcpClient())
         {
            try
            {
               client.Connect(IPAddress.Loopback, port);
            }
            catch (SocketException)
            {
               return;
            }
         }

         Assert.Fail("Something listens on 127.0.0.1:" + port + ", the port this fixture needs refused.");
      }

      private static byte[] InsertAfterFirstLine(byte[] message, string lines)
      {
         var text = Bytes.GetString(message);
         int end = text.IndexOf("\r\n", StringComparison.Ordinal);
         ClassicAssert.GreaterOrEqual(end, 0, "The message spamd was sent has no first line.");
         return Bytes.GetBytes(text.Substring(0, end + 2) + lines + text.Substring(end + 2));
      }

      // The values of the Return-Path fields of the header block at the start of text
      // (up to its first empty line, or all of it).
      private static List<string> ReturnPathFields(string text)
      {
         var values = new List<string>();

         foreach (var line in text.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.Length == 0)
               break;

            var match = Regex.Match(line, "^Return-Path[ \\t]*:(.*)$", RegexOptions.IgnoreCase);
            if (match.Success)
               values.Add(match.Groups[1].Value.Trim());
         }

         return values;
      }
   }
}
