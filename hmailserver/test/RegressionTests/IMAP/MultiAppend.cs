// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    MULTIAPPEND (RFC 3502). Several messages travel in one APPEND - the
   ///    shape migration tools want when filling a mailbox - and the command is
   ///    atomic: either every message is stored or none is, which is the entire
   ///    difference from issuing N separate APPENDs. With UIDPLUS the single
   ///    APPENDUID response carries the whole uid-set.
   /// </summary>
   [TestFixture]
   public class MultiAppend : TestFixtureBase
   {
      private Account _account;

      [SetUp]
      public new void SetUp()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "multiappend@example.test", "test");
      }

      [Test]
      [Description("Two messages in one APPEND: one tagged OK, an APPENDUID uid-set, both messages present.")]
      public void TwoMessagesArriveInOneCommand()
      {
         const string first = "Subject: first\r\n\r\nFirst body.\r\n";
         const string second = "Subject: second\r\n\r\nSecond body.\r\n";

         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");
         imapSim.SelectFolder("INBOX");

         imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "}\r\n");
         var continuation = imapSim.ReceiveUntil("+ ");
         StringAssert.Contains("+ Ready", continuation, "No continuation for the first literal. Got: " + continuation);

         imapSim.SendRaw(first + " (\\Seen) {" + second.Length + "}\r\n");
         continuation = imapSim.ReceiveUntil("+ ");
         StringAssert.Contains("+ Ready", continuation, "No continuation for the second literal. Got: " + continuation);

         imapSim.SendRaw(second + "\r\n");
         var response = imapSim.ReceiveUntil("A01 ");

         StringAssert.Contains("A01 OK", response,
            "The two-message APPEND must succeed with a single tagged OK. Got: " + response);

         var appendUid = Regex.Match(response, @"\[APPENDUID \d+ (\d+)[:,](\d+)\]");
         ClassicAssert.IsTrue(appendUid.Success,
            "RFC 4315 with MULTIAPPEND: APPENDUID must carry the uid-set of BOTH messages. Got: " + response);

         imapSim.SendRaw("A02 STATUS INBOX (MESSAGES)\r\n");
         var status = imapSim.ReceiveUntil("A02 OK");
         StringAssert.Contains("MESSAGES 2", status,
            "Both messages must be in the mailbox. Got: " + status);

         imapSim.Disconnect();
      }

      /// <summary>
      ///    The point of the extension: atomicity. The second message is above
      ///    the server's maximum size, so the FIRST - already fully received and
      ///    written - must be thrown away too. An implementation that saved
      ///    per-message would pass every other test here and fail this one.
      /// </summary>
      [Test]
      [Description("A failing later message discards the whole command - nothing is stored.")]
      public void AFailingLaterMessageDiscardsEverything()
      {
         var originalMaxSizeKB = _settings.MaxMessageSize;
         _settings.MaxMessageSize = 10; // KB

         try
         {
            const string first = "Subject: survivor?\r\n\r\nSmall and valid.\r\n";
            var oversized = 50 * 1024; // 50 KB declared, over the 10 KB limit

            var imapSim = new ImapClientSimulator();
            imapSim.Connect();
            imapSim.Logon(_account.Address, "test");

            imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "}\r\n");
            var continuation = imapSim.ReceiveUntil("+ ");
            StringAssert.Contains("+ Ready", continuation);

            // The second message's size is refused at its declaration, before
            // any of its octets move - the client is waiting for a continuation
            // and receives the tagged refusal instead.
            imapSim.SendRaw(first + " {" + oversized + "}\r\n");
            var response = imapSim.ReceiveUntil("A01 ");

            StringAssert.Contains("A01 NO", response,
               "The oversized second message must fail the command. Got: " + response);
            StringAssert.Contains("[TOOBIG]", response,
               "The refusal names the reason with the TOOBIG response code. Got: " + response);

            imapSim.SendRaw("A02 STATUS INBOX (MESSAGES)\r\n");
            var status = imapSim.ReceiveUntil("A02 OK");
            StringAssert.Contains("MESSAGES 0", status,
               "RFC 3502: refusing any message means storing none - the first, already " +
               "received in full, must be discarded too. Got: " + status);

            imapSim.Disconnect();
         }
         finally
         {
            _settings.MaxMessageSize = originalMaxSizeKB;
         }
      }

      [Test]
      [Description("Non-synchronizing literals throughout: the whole multi-message APPEND in one write.")]
      public void NonSynchronizingLiteralsNeedOneRoundTrip()
      {
         const string first = "Subject: one\r\n\r\nBody one.\r\n";
         const string second = "Subject: two\r\n\r\nBody two.\r\n";

         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");

         imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "+}\r\n" + first +
                         " {" + second.Length + "+}\r\n" + second + "\r\n");
         var response = imapSim.ReceiveUntil("A01 ");

         StringAssert.Contains("A01 OK", response,
            "MULTIAPPEND combined with LITERAL- must complete in a single round trip. Got: " + response);
         ClassicAssert.IsFalse(response.Contains("+ Ready"),
            "No continuation may be sent for non-synchronizing literals. Got: " + response);

         imapSim.SendRaw("A02 STATUS INBOX (MESSAGES)\r\n");
         var status = imapSim.ReceiveUntil("A02 OK");
         StringAssert.Contains("MESSAGES 2", status,
            "Both messages must be in the mailbox. Got: " + status);

         imapSim.Disconnect();
      }

      [Test]
      [Description("Flags and a date on a later message are applied to that message.")]
      public void FlagsOnALaterMessageApplyToIt()
      {
         const string first = "Subject: unflagged\r\n\r\nPlain.\r\n";
         const string second = "Subject: flagged\r\n\r\nSeen and flagged.\r\n";

         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");

         imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "+}\r\n" + first +
                         " (\\Seen \\Flagged) {" + second.Length + "+}\r\n" + second + "\r\n");
         var response = imapSim.ReceiveUntil("A01 ");
         StringAssert.Contains("A01 OK", response, "The APPEND must succeed. Got: " + response);

         imapSim.SelectFolder("INBOX");
         imapSim.SendRaw("A02 FETCH 1:2 (FLAGS)\r\n");
         var flags = imapSim.ReceiveUntil("A02 OK");

         StringAssert.Contains("* 2 FETCH (FLAGS (\\Flagged \\Seen))", flags,
            "The second message carries the flags given with it. Got: " + flags);
         ClassicAssert.IsFalse(flags.Contains("* 1 FETCH (FLAGS (\\Flagged"),
            "The first message was appended without flags and must have none. Got: " + flags);

         imapSim.Disconnect();
      }

      [Test]
      [Description("CAPABILITY advertises MULTIAPPEND.")]
      public void CapabilityAdvertisesMultiAppend()
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(143), "Could not connect to the IMAP server on port 143.");
         socket.ReadUntil("* OK");

         socket.Send("A01 CAPABILITY\r\n");
         var capabilities = socket.ReadUntil("A01 OK");

         StringAssert.Contains("MULTIAPPEND", capabilities,
            "CAPABILITY must advertise MULTIAPPEND. Got: " + capabilities);

         socket.Disconnect();
      }

      /// <summary>
      ///    A literal is placed in the store as soon as its octets are in, before
      ///    the command's final CRLF, and saved to the database only at that
      ///    CRLF. A client that went away in between left the placed file with
      ///    no row: nothing removed it and no quota counted it, so an
      ///    authenticated user could fill the data volume by repeating it. The
      ///    second literal's continuation proves the first was placed; the
      ///    client then closes without sending the rest, and the account's
      ///    directory must end as it began.
      /// </summary>
      [Test]
      [Description("An APPEND abandoned before its final CRLF leaves no message file behind.")]
      public void AnAbandonedAppendLeavesNoFile()
      {
         string accountDirectory = Paths.Combine(_settings.Directories.DataDirectory, "example.test", "multiappend");
         int filesBefore = FilesUnder(accountDirectory);

         const string first = "Subject: abandoned\r\n\r\nNever finished.\r\n";

         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");

         imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "}\r\n");
         var continuation = imapSim.ReceiveUntil("+ ");
         StringAssert.Contains("+ Ready", continuation);

         imapSim.SendRaw(first + " {" + first.Length + "}\r\n");
         continuation = imapSim.ReceiveUntil("+ ");
         StringAssert.Contains("+ Ready", continuation,
            "The second literal's continuation comes only after the first was placed. Got: " + continuation);

         // Gone before the second literal and the final CRLF.
         imapSim.Disconnect();

         int filesAfter = FilesUnder(accountDirectory);
         for (int attempt = 0; attempt < 50 && filesAfter != filesBefore; attempt++)
         {
            System.Threading.Thread.Sleep(200);
            filesAfter = FilesUnder(accountDirectory);
         }

         ClassicAssert.AreEqual(filesBefore, filesAfter,
            "The abandoned APPEND's placed message has no row, so its file must go with the connection.");

         var check = new ImapClientSimulator();
         check.Connect();
         check.Logon(_account.Address, "test");
         check.SendRaw("A02 STATUS INBOX (MESSAGES)\r\n");
         StringAssert.Contains("MESSAGES 0", check.ReceiveUntil("A02 OK"));
         check.Disconnect();
      }

      /// <summary>
      ///    A literal the command reads only to throw away - a later message
      ///    refused under {n+}, or any literal once the command has failed - was
      ///    read whatever its declared size, past the 2 GB ceiling too, so a
      ///    client could keep a connection reading and discarding for as long as
      ///    it liked. Declared larger than a refused literal is read for, it now
      ///    closes the connection with TOOBIG, as the first literal's does.
      /// </summary>
      [Test]
      [Description("A discarded later literal declared past the limit closes the connection with TOOBIG instead of being read.")]
      public void AnOversizedDiscardedLiteralClosesTheConnection()
      {
         const string first = "Subject: first\r\n\r\nSmall and valid.\r\n";

         // A later message refused at its declaration (past the 2 GB ceiling),
         // under {n+}: its octets would be on their way.
         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");
         imapSim.SendRaw("A01 APPEND INBOX {" + first.Length + "}\r\n");
         StringAssert.Contains("+ Ready", imapSim.ReceiveUntil("+ "));
         imapSim.SendRaw(first + " {3000000000+}\r\n");
         var response = imapSim.ReceiveUntil("* BYE");
         StringAssert.Contains("A01 BAD [TOOBIG]", response, "The refused {n+} literal must not be read. Got: " + response);
         StringAssert.Contains("* BYE [TOOBIG]", response, response);
         imapSim.Disconnect();

         // A literal after the command has failed: a small later message refused
         // under {n+} is read and discarded, and a third one declared past the
         // limit closes the connection rather than being read.
         var originalMaxSizeKB = _settings.MaxMessageSize;
         _settings.MaxMessageSize = 10; // KB
         try
         {
            string refused = "Subject: refused\r\n\r\n" + new string('x', 20 * 1024) + "\r\n";

            imapSim = new ImapClientSimulator();
            imapSim.Connect();
            imapSim.Logon(_account.Address, "test");
            imapSim.SendRaw("A02 APPEND INBOX {" + first.Length + "}\r\n");
            StringAssert.Contains("+ Ready", imapSim.ReceiveUntil("+ "));
            imapSim.SendRaw(first + " {" + refused.Length + "+}\r\n" + refused + " {3000000000}\r\n");
            response = imapSim.ReceiveUntilAny("+ ", "* BYE");
            StringAssert.Contains("A02 BAD [TOOBIG]", response, "The failed command's oversized literal must not be asked for. Got: " + response);
            StringAssert.Contains("* BYE [TOOBIG]", response, response);
            imapSim.Disconnect();
         }
         finally
         {
            _settings.MaxMessageSize = originalMaxSizeKB;
         }

         var check = new ImapClientSimulator();
         check.Connect();
         check.Logon(_account.Address, "test");
         check.SendRaw("A03 STATUS INBOX (MESSAGES)\r\n");
         StringAssert.Contains("MESSAGES 0", check.ReceiveUntil("A03 OK"));
         check.Disconnect();
      }

      /// <summary>
      ///    The destructor removes an abandoned APPEND's file, but a process that
      ///    stops between a literal and the command's final CRLF - a crash, a
      ///    kill, a power cut - runs no destructor, and the file stayed in the
      ///    store with no row for good. Every such file now has a marker in the
      ///    temp folder until its row is saved or it is removed, and the server
      ///    removes, when it starts, the file of every marker no row answers.
      ///    The markers here are written as a stopped process would have left
      ///    them: one for a file with no row (removed), one for a message that
      ///    was saved (kept), and one naming a file outside the message store
      ///    (never touched). An ordinary APPEND leaves no marker behind.
      /// </summary>
      [Test]
      [Description("The file of an APPEND a stopped process left unfinished is removed when the server starts; saved mail and other files are not.")]
      public void AnAppendTheServerStoppedInTheMiddleOfIsSweptAtStart()
      {
         string markers = Paths.Combine(_application.Settings.Directories.TempDirectory, "IMAPAppend");

         // An ordinary APPEND, finished: its marker goes with it.
         const string saved = "Subject: saved\r\n\r\nSaved.\r\n";
         var imapSim = new ImapClientSimulator();
         imapSim.Connect();
         imapSim.Logon(_account.Address, "test");
         imapSim.SendRaw("A01 APPEND INBOX {" + saved.Length + "}\r\n");
         StringAssert.Contains("+ Ready", imapSim.ReceiveUntil("+ "));
         imapSim.SendRaw(saved + "\r\n");
         StringAssert.Contains("A01 OK", imapSim.ReceiveUntil("A01 "));
         imapSim.Disconnect();

         if (System.IO.Directory.Exists(markers))
            ClassicAssert.AreEqual(0, System.IO.Directory.GetFiles(markers, "*.append").Length,
               "A finished APPEND leaves no marker.");

         var message = _account.IMAPFolders.get_ItemByName("INBOX").Messages[0];
         string savedFile = message.Filename;
         ClassicAssert.IsTrue(System.IO.File.Exists(savedFile), savedFile);

         // What a stopped process leaves: a literal's file with no row, where
         // the account's messages are, and its marker.
         string orphanName = System.Guid.NewGuid().ToString("B").ToUpperInvariant();
         string accountDirectory = System.IO.Path.GetDirectoryName(System.IO.Path.GetDirectoryName(savedFile));
         string orphan = Paths.Combine(accountDirectory, orphanName.Substring(1, 2), orphanName + ".eml");
         System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(orphan));
         System.IO.File.WriteAllText(orphan, "Subject: never finished\r\n\r\nThe process stopped.\r\n");

         System.IO.Directory.CreateDirectory(markers);
         WriteMarker(markers, orphan, "working");

         // A marker left after the row was saved: the file is mail.
         WriteMarker(markers, savedFile, "placed");

         // A marker naming a file outside the message store.
         string outside = Paths.Combine(_application.Settings.Directories.TempDirectory, System.Guid.NewGuid().ToString("B").ToUpperInvariant() + ".eml");
         System.IO.File.WriteAllText(outside, "Not a message of the store.");
         WriteMarker(markers, outside, "working");

         // A marker naming a file under the data directory but not where the
         // store keeps a message - here the domain's directory, with no mailbox
         // and no fan-out; on the Linux packages the temp, event and database
         // directories are inside the data directory too. Never touched.
         string misplaced = Paths.Combine(System.IO.Path.GetDirectoryName(accountDirectory), System.Guid.NewGuid().ToString("B").ToUpperInvariant() + ".eml");
         System.IO.File.WriteAllText(misplaced, "Under the data directory, not a message of the store.");
         WriteMarker(markers, misplaced, "working");

         try
         {
            _application.Stop();
            _application.Start();

            ClassicAssert.IsFalse(System.IO.File.Exists(orphan), "The unfinished APPEND's file has no row and is removed at start.");
            ClassicAssert.IsTrue(System.IO.File.Exists(savedFile), "A saved message's file is mail, whatever marker names it.");
            ClassicAssert.IsTrue(System.IO.File.Exists(outside), "A file outside the message store is never removed by the sweep.");
            ClassicAssert.IsTrue(System.IO.File.Exists(misplaced), "A file under the data directory but not at a message's place in the store is never removed by the sweep.");
            ClassicAssert.AreEqual(0, System.IO.Directory.GetFiles(markers, "*.append").Length, "Every marker is dealt with.");

            var check = new ImapClientSimulator();
            check.Connect();
            check.Logon(_account.Address, "test");
            check.SendRaw("A02 STATUS INBOX (MESSAGES)\r\n");
            StringAssert.Contains("MESSAGES 1", check.ReceiveUntil("A02 OK"));
            check.Disconnect();
         }
         finally
         {
            if (System.IO.File.Exists(outside))
               System.IO.File.Delete(outside);
            if (System.IO.File.Exists(misplaced))
               System.IO.File.Delete(misplaced);
         }
      }

      private static void WriteMarker(string markers, string file, string state)
      {
         string text = "hMailServer IMAP APPEND in progress\n" +
                       "state=" + state + "\n" +
                       "file=" + file + "\n";
         System.IO.File.WriteAllText(Paths.Combine(markers, System.IO.Path.GetFileName(file) + ".append"), text, new System.Text.UTF8Encoding(false));
      }

      private static int FilesUnder(string directory)
      {
         return System.IO.Directory.Exists(directory)
            ? System.IO.Directory.GetFiles(directory, "*", System.IO.SearchOption.AllDirectories).Length
            : 0;
      }
   }
}
