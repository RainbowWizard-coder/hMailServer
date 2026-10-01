// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Collections.Generic;
using System.Text;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.ExternalAccounts;
using RegressionTests.Infrastructure;
using RegressionTests.POP3.Fetching;
using RegressionTests.Shared;

namespace RegressionTests.SMTP
{
   /// <summary>
   ///    A message the server could not write to disk must not be acknowledged.
   ///
   ///    TransparentTransmissionBuffer::SaveToFile_ used to read
   ///
   ///        bool bResult = file_.Write(pBuffer, noOfBytesWritten);
   ///
   ///    without ever looking at bResult, and then `return true` unconditionally. So a
   ///    spool write that failed - a full disk, a quota, an I/O error - was invisible
   ///    twice: dropped where it happened, and reported to every caller as success. MSVC
   ///    does not warn about the unused variable at /W3, C4189 being a /W4 warning.
   ///
   ///    Which of the two failure shapes actually lost mail is the point of these tests,
   ///    and it is not the obvious one. Measured against the pre-fix build:
   ///
   ///      * If *no* write succeeds the spool file is left empty, and that was already
   ///        refused with 451 - not by any check that meant to catch it, but because the
   ///        accept path fails downstream on a message with no content. Nothing was
   ///        reported to the error log, so an operator got a refusal with no reason, but
   ///        no mail was lost. The fix's contribution here is diagnosis: HM5862.
   ///
   ///      * If the first write succeeds and a later one fails - which is exactly what a
   ///        disk filling up mid-message does - the spool file is left non-empty and
   ///        truncated. It has content, it has headers, its size is non-zero, so it
   ///        passes every downstream guard: the message was accepted with **250** and
   ///        delivered short. The sender's copy is gone the moment it is told 250. That
   ///        is the defect, and TruncatedSpoolFileIsRefusedRatherThanDeliveredShort is
   ///        the test that fails against the unfixed build.
   ///
   ///    Provoking either shape means [Settings] SimulateSpoolWriteFailure (1 = fail
   ///    every write, 2 = fail after the first), because the alternative is filling a
   ///    real disk. CrashSimulationMode is the existing precedent for such a switch.
   ///
   ///    451 rather than 554 is deliberate: a disk that is full now may not be in ten
   ///    minutes, and a permanent rejection makes the sending server bounce mail that a
   ///    retry would have delivered.
   ///
   ///    The third shape is the close. fwrite answers for the bytes it takes into the C
   ///    runtime's buffer, and the close writes what is still there - up to 4 KB of the
   ///    message's tail on most systems - so its answer is the only report of whether
   ///    the tail arrived, and File::Close dropped it. Every write succeeded, the close
   ///    did not, and the message was accepted short: a 250 over SMTP DATA and BDAT, an
   ///    OK to IMAP APPEND, and an external fetch that delivered the short copy and then
   ///    deleted the whole one from the remote server. Mode 3 lets every write land and
   ///    has the close say it failed (TransparentTransmissionBuffer::CloseSpoolFile_,
   ///    IMAPCommandAppend::WriteData_); each test below fails against a build that
   ///    ignores the close's answer. The real refusal - a full tmpfs, a quota,
   ///    /dev/full - is file-utilities-tests' (platform/file_utilities_tests_main.cpp).
   /// </summary>
   [TestFixture]
   public class SpoolWriteFailure : TestFixtureBase
   {
      // Comfortably more than one flush: TransparentTransmissionBuffer::GetRequiresFlush
      // flushes every 40000 bytes, so a body this size guarantees several writes and
      // therefore that mode 2 has a successful first write to let through.
      private const int BodyBytes = 200000;

      [TearDown]
      public void StopSimulatingTheFailure()
      {
         try
         {
            IniFileSetting.Write("SimulateSpoolWriteFailure", "0");
            _application.Reinitialize();
         }
         finally
         {
            // HM5862 is the point of these tests, so it is cleared here rather than left
            // to fail whichever fixture runs next.
            LogHandler.DeleteErrorLog();
         }
      }

      private void SimulateFailure(int mode)
      {
         IniFileSetting.Write("SimulateSpoolWriteFailure", mode.ToString());

         // The setting is cached by IniFileSettings::InitInstance; Reinitialize is what
         // re-reads it. Stop()/Start() does not.
         _application.Reinitialize();
      }

      private static string LargeBody()
      {
         // Deliberately not a repeated single character: the body is written through the
         // dot-unstuffing path, and a body of one repeated byte would hide an off-by-one
         // there. Lines of readable text, so a truncated delivery is obvious in a log.
         var builder = new StringBuilder(BodyBytes + 128);
         int line = 0;

         while (builder.Length < BodyBytes)
            builder.Append("Line ").Append(line++).Append(" of a message that must not be delivered truncated.\r\n");

         return builder.ToString();
      }

      [Test]
      [Description("A spool file truncated by a mid-message write failure is refused, not delivered short")]
      public void TruncatedSpoolFileIsRefusedRatherThanDeliveredShort()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spooltrunc@example.test", "test");

         // Mode 2: the first write lands, the rest fail. The file on disk is non-empty
         // and short - the one shape the old code accepted.
         SimulateFailure(2);

         var simulator = new SmtpClientSimulator();

         CustomAsserts.Throws<DeliveryFailedException>(() =>
            simulator.Send("test@example.test", account.Address, "Must not be accepted", LargeBody()));

         CustomAsserts.AssertReportedError("HM5862");

         // The half that matters. Against the unfixed build this is 1, and the message in
         // it is shorter than the one that was sent.
         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("A spool file that received nothing at all is refused, and now says why")]
      public void EmptySpoolFileIsRefusedAndReported()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spoolfail@example.test", "test");

         SimulateFailure(1);

         var simulator = new SmtpClientSimulator();

         CustomAsserts.Throws<DeliveryFailedException>(() =>
            simulator.Send("test@example.test", account.Address, "Must not be accepted", LargeBody()));

         // The refusal itself is not new - see the class comment, the empty case already
         // failed downstream. HM5862 is: the old code refused the message without
         // recording any reason anywhere, which is a support call with nothing to go on.
         CustomAsserts.AssertReportedError("HM5862");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      private const string ShortMessage = "From: sender@external.example\r\n" +
                                          "To: someone@example.test\r\n" +
                                          "Subject: Whole or refused\r\n" +
                                          "\r\n" +
                                          "A message short enough to wait whole in the buffer the close writes.\r\n";

      [Test]
      [Description("A DATA message whose spool file's close fails is refused with 451, not accepted short")]
      public void DataMessageWhoseCloseFailsIsRefused()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spoolclose@example.test", "test");

         SimulateFailure(3);

         var simulator = new SmtpClientSimulator();

         // Against a build that ignores the close's answer this is a 250, and the message
         // is delivered.
         CustomAsserts.Throws<DeliveryFailedException>(() =>
            simulator.Send("test@example.test", account.Address, "Must not be accepted", LargeBody()));

         CustomAsserts.AssertReportedError("HM5862");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("A BDAT message whose spool file's close fails is refused with 451, not accepted short")]
      public void BdatMessageWhoseCloseFailsIsRefused()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "spoolclosebdat@example.test", "test");

         SimulateFailure(3);

         using (var socket = new TcpConnection())
         {
            ClassicAssert.IsTrue(socket.Connect(TestPorts.Smtp));
            ClassicAssert.IsTrue(socket.Receive().StartsWith("220"));
            socket.Send("EHLO example.test\r\n");
            socket.ReadUntil("250 HELP");

            ClassicAssert.IsTrue(socket.SendAndReceive("MAIL FROM:<sender@external.example>\r\n").StartsWith("250"));
            ClassicAssert.IsTrue(socket.SendAndReceive("RCPT TO:<" + account.Address + ">\r\n").StartsWith("250"));

            socket.Send("BDAT " + Encoding.ASCII.GetByteCount(ShortMessage) + " LAST\r\n");
            socket.Send(ShortMessage);

            // The shared accept path (SMTPConnection::OnPreAcceptTransfer_) answers a
            // spool file that was not written with 451 4.3.0.
            var reply = socket.Receive();
            ClassicAssert.IsTrue(reply.StartsWith("451"), "BDAT LAST was not refused with 451. Got: " + reply);

            socket.Send("QUIT\r\n");
         }

         CustomAsserts.AssertReportedError("HM5862");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("An APPEND whose message file's close fails answers NO, and stores nothing")]
      public void AppendWhoseCloseFailsAnswersNo()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "appendclose@example.test", "test");

         SimulateFailure(3);

         var simulator = new ImapClientSimulator();
         simulator.Connect();
         simulator.LogonWithLiteral(account.Address, "test");

         var result = simulator.SendSingleCommandWithLiteral("A01 APPEND INBOX {" + Encoding.ASCII.GetByteCount(ShortMessage) + "}", ShortMessage);
         ClassicAssert.IsTrue(result.Contains("A01 NO"), "APPEND was not refused. Got: " + result);

         CustomAsserts.AssertReportedError("HM5211");

         ClassicAssert.AreEqual(0, simulator.GetMessageCount("INBOX"));
         simulator.Disconnect();
      }

      [Test]
      [Description("A message fetched over POP3 whose spool file's close fails is neither delivered nor deleted from the remote server")]
      public void Pop3FetchWhoseCloseFailsLeavesTheMessageOnTheRemoteServer()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fetchclose@example.test", "test");
         var port = TestSetup.GetNextFreePort();

         var fetchAccount = account.FetchAccounts.Add();
         fetchAccount.Enabled = true;
         fetchAccount.MinutesBetweenFetch = 60;
         fetchAccount.Name = "Close fails";
         fetchAccount.Username = "test@example.com";
         fetchAccount.Password = "test";
         fetchAccount.UseSSL = false;
         fetchAccount.ServerAddress = "localhost";
         fetchAccount.Port = port;
         fetchAccount.ProcessMIMERecipients = false;
         // -1: the POP3 fetcher deletes a message from the remote server as soon as it
         // is delivered (POP3ClientConnection::MessageCleanup_; 0 never deletes) -
         // which is what lost the only whole copy when a short one was taken for
         // delivered.
         fetchAccount.DaysToKeepMessages = -1;
         fetchAccount.UseAntiSpam = false;
         fetchAccount.UseAntiVirus = false;
         fetchAccount.Save();

         SimulateFailure(3);

         using (var pop3Server = new Pop3ServerSimulator(1, port, new List<string> { ShortMessage }))
         {
            pop3Server.StartListen();

            fetchAccount.DownloadNow();
            pop3Server.WaitForCompletion();
            LockHelper.WaitForUnlock(fetchAccount);

            // Collected - so the remote server's copy was the fetcher's to delete - and
            // not deleted.
            ClassicAssert.AreEqual(1, pop3Server.RetrievedMessages.Count, "The message was not collected at all.");
            ClassicAssert.AreEqual(0, pop3Server.DeletedMessages.Count, "The message was deleted from the remote server.");
         }

         CustomAsserts.AssertReportedError("HM5862", "HM5864");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }

      [Test]
      [Description("A message collected over IMAP whose spool file's close fails is neither delivered nor deleted from the remote server")]
      public void ImapFetchWhoseCloseFailsLeavesTheMessageOnTheRemoteServer()
      {
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "collectclose@example.test", "test");
         var port = TestSetup.GetNextFreePort();

         var fetchAccount = account.FetchAccounts.Add();
         fetchAccount.Enabled = true;
         fetchAccount.MinutesBetweenFetch = 60;
         fetchAccount.Name = "Close fails";
         fetchAccount.ServerType = 1;
         fetchAccount.Username = "remote@dummy-example.com";
         fetchAccount.Password = "far-side-secret-2026";
         fetchAccount.UseSSL = false;
         fetchAccount.ServerAddress = "localhost";
         fetchAccount.Port = port;
         fetchAccount.ProcessMIMERecipients = false;
         // 0: the IMAP fetcher flags a collected message \Deleted and expunges it in
         // the same session (IMAPClientConnection, as
         // ImapFetch.CollectedMessagesAreDeletedFromTheFarSideWhenAsked holds it).
         fetchAccount.DaysToKeepMessages = 0;
         fetchAccount.UseAntiSpam = false;
         fetchAccount.UseAntiVirus = false;
         fetchAccount.Save();

         SimulateFailure(3);

         var mailbox = new ScriptedImapServer.RemoteMailbox();
         mailbox.Add(new ScriptedImapServer.RemoteMessage(101, ShortMessage));

         using (var server = new ScriptedImapServer(port, mailbox))
         {
            server.StartListen();
            fetchAccount.DownloadNow();
            LockHelper.WaitForUnlock(fetchAccount);
            server.WaitForCompletion();

            CollectionAssert.AreEqual(new[] { 101 }, server.FetchedUids, "The message was not collected at all.");
            CollectionAssert.IsEmpty(server.StoredDeletedUids, "The message was marked deleted on the remote server.");
         }

         CustomAsserts.AssertReportedError("HM5862", "HM5865");

         Pop3ClientSimulator.AssertMessageCount(account.Address, "test", 0);
      }
   }
}
