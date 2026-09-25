// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    What Dovecot's imaptest found on its first run against this server, on the
   ///    project's test lab (25 September 2026), one test per finding - the
   ///    Roadmap2 row "IMAP that passes imaptest". Each test talks to the server
   ///    over a raw socket, so that what is asserted is the bytes a client gets.
   ///
   ///    The most important is the first: a command pipelined behind an APPEND was
   ///    never answered, because the bytes after the APPEND's final CRLF were thrown
   ///    away with the APPEND's buffer. mbsync pipelines its APPENDs, so copying a
   ///    mailbox into this server with it stopped after the first message.
   /// </summary>
   [TestFixture]
   public class ImapTestCompliance : TestFixtureBase
   {
      private const string Password = "test";

      [SetUp]
      public new void SetUp()
      {
         _application.Settings.IMAPSortEnabled = true;

         base.SetUp();
      }

      // ------------------------------------------------------------------ helpers

      private string CreateAccount(string address)
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         return address;
      }

      private static TcpConnection LogOn(string address)
      {
         var socket = new TcpConnection();
         ClassicAssert.IsTrue(socket.Connect(TestPorts.Imap), "Could not connect to the IMAP server on port " + TestPorts.Imap + ".");
         ReadUntilLine(socket, "* OK");

         string login = Command(socket, "L1", "LOGIN " + address + " " + Password);
         StringAssert.Contains("L1 OK", login, "The LOGIN failed: " + login);

         return socket;
      }

      private static TcpConnection LogOnAndSelect(string address, string folder)
      {
         var socket = LogOn(address);

         string select = Command(socket, "S1", "SELECT " + folder);
         StringAssert.Contains("S1 OK", select, "The SELECT failed: " + select);

         return socket;
      }

      /// <summary>
      ///    Reads until a whole line starting with the given text has arrived, or
      ///    fails after the timeout. Never blocks in a read with nothing to read,
      ///    so a server that does not answer fails the test instead of hanging it.
      /// </summary>
      private static string ReadUntilLine(TcpConnection socket, string lineStart, int seconds = 15)
      {
         var received = new StringBuilder();
         var deadline = DateTime.Now.AddSeconds(seconds);

         while (DateTime.Now < deadline)
         {
            if (socket.WaitForData(TimeSpan.FromMilliseconds(250)))
               received.Append(socket.Receive());

            string text = received.ToString();

            int at = text.StartsWith(lineStart, StringComparison.Ordinal)
               ? 0
               : text.IndexOf("\r\n" + lineStart, StringComparison.Ordinal);

            if (at > 0)
               at += 2;

            if (at >= 0 && text.IndexOf("\r\n", at, StringComparison.Ordinal) >= 0)
               return text;

            if (!socket.IsConnected)
               break;
         }

         Assert.Fail("No line starting \"" + lineStart + "\" within " + seconds + " seconds. Received: " + received);
         return null;
      }

      private static string Command(TcpConnection socket, string tag, string command)
      {
         socket.Send(tag + " " + command + "\r\n");
         return ReadUntilLine(socket, tag + " ");
      }

      private static string AppendSynchronizing(TcpConnection socket, string tag, string folder, string message, string extra = "")
      {
         socket.Send(tag + " APPEND " + folder + extra + " {" + Encoding.UTF8.GetByteCount(message) + "}\r\n");
         ReadUntilLine(socket, "+ ");
         socket.Send(message + "\r\n");

         string response = ReadUntilLine(socket, tag + " ");
         StringAssert.Contains(tag + " OK", response, "The APPEND failed: " + response);

         return response;
      }

      // The numbers of the untagged "* SEARCH" line (or "* SORT"), as one string.
      private static string ResultNumbers(string response, string keyword)
      {
         var match = Regex.Match(response, @"\* " + keyword + @"([ 0-9]*)\r\n");
         ClassicAssert.IsTrue(match.Success, "No " + keyword + " response in: " + response);
         return match.Groups[1].Value.Trim();
      }

      private static long UidNext(TcpConnection socket, string folder)
      {
         string status = Command(socket, "U1", "STATUS " + folder + " (UIDNEXT)");
         var match = Regex.Match(status, @"UIDNEXT (\d+)");
         ClassicAssert.IsTrue(match.Success, "No UIDNEXT in: " + status);
         return long.Parse(match.Groups[1].Value);
      }

      // ------------------------------------------------------------------ 5. pipelining

      [Test]
      [Description("A command pipelined behind an APPEND with a {n+} literal, in the same write, is answered.")]
      public void CommandPipelinedAfterNonSynchronizingAppendIsAnswered()
      {
         var socket = LogOn(CreateAccount("pipeline@example.test"));

         const string message = "Subject: pipelined\r\n\r\nSent without waiting.\r\n";

         // The APPEND, its octets, its final CRLF and the next command in one write -
         // what RFC 7888 lets a client do, and what mbsync does.
         socket.Send("A02 APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\nA03 NOOP\r\n");

         string response = ReadUntilLine(socket, "A03 ");

         StringAssert.Contains("A02 OK", response, "The APPEND itself must succeed. Got: " + response);
         StringAssert.Contains("A03 OK", response,
            "The NOOP sent behind the APPEND must be answered; it used to be thrown away with the APPEND's buffer. Got: " + response);

         socket.Disconnect();
      }

      [Test]
      [Description("Three APPENDs pipelined in one write, as mbsync sends them, store three messages.")]
      public void PipelinedAppendsAreAllStored()
      {
         var socket = LogOn(CreateAccount("mbsync@example.test"));

         var batch = new StringBuilder();
         for (int i = 1; i <= 3; i++)
         {
            string message = "Subject: message " + i + "\r\n\r\nBody " + i + ".\r\n";
            batch.Append("A0" + i + " APPEND INBOX (\\Seen) {" + message.Length + "+}\r\n" + message + "\r\n");
         }

         socket.Send(batch.ToString());

         string response = ReadUntilLine(socket, "A03 ");
         StringAssert.Contains("A01 OK", response, response);
         StringAssert.Contains("A02 OK", response, response);
         StringAssert.Contains("A03 OK", response, response);

         string status = Command(socket, "A04", "STATUS INBOX (MESSAGES)");
         StringAssert.Contains("MESSAGES 3", status, "All three pipelined messages must be stored. Got: " + status);

         socket.Disconnect();
      }

      [Test]
      [Description("After a synchronizing APPEND, a command sent in the same write as the message octets is answered.")]
      public void CommandSentWithTheSynchronizingLiteralIsAnswered()
      {
         var socket = LogOn(CreateAccount("syncpipe@example.test"));

         const string message = "Subject: sync\r\n\r\nWaited for the continuation.\r\n";

         socket.Send("A02 APPEND INBOX {" + message.Length + "}\r\n");
         ReadUntilLine(socket, "+ ");

         socket.Send(message + "\r\nA03 NOOP\r\n");

         string response = ReadUntilLine(socket, "A03 ");
         StringAssert.Contains("A02 OK", response, response);
         StringAssert.Contains("A03 OK", response, response);

         socket.Disconnect();
      }

      [Test]
      [Description("A refused APPEND with a {n+} literal reads the literal and runs none of its lines as commands.")]
      public void RefusedNonSynchronizingAppendConsumesTheLiteral()
      {
         string address = CreateAccount("refused@example.test");
         var socket = LogOn(address);

         // A message one of whose lines is a command. Before the fix the refusal
         // was answered at once and the octets that followed were read as command
         // lines - this one would have created a folder.
         const string message = "Subject: refused\r\n\r\nA99 CREATE Injected\r\n";

         socket.Send("A02 APPEND NoSuchFolder {" + message.Length + "+}\r\n" + message + "\r\nA03 NOOP\r\n");

         string response = ReadUntilLine(socket, "A03 ");

         StringAssert.Contains("A02 NO [TRYCREATE]", response, "The APPEND to a missing folder is refused NO [TRYCREATE]. Got: " + response);
         StringAssert.Contains("A03 OK", response, response);
         ClassicAssert.IsFalse(response.Contains("A99"), "A line of the refused message was run as a command. Got: " + response);

         string list = Command(socket, "A04", "LIST \"\" \"Injected\"");
         ClassicAssert.IsFalse(list.Contains("Injected\"\r\n"), "The refused message's line created a folder. Got: " + list);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ 4. exact bytes

      [Test]
      [Description("An appended message is stored as the literal, byte for byte: RFC822.SIZE, BODY[] and BODY[TEXT] are its own.")]
      public void AppendedMessageIsStoredByteForByte()
      {
         var socket = LogOnAndSelect(CreateAccount("exact@example.test"), "INBOX");

         // No line break at the end, deliberately: the server used to add the
         // command's CRLF to every appended message.
         const string text = "No final line break";
         const string message = "Subject: exact\r\n\r\n" + text;

         AppendSynchronizing(socket, "A02", "INBOX", message);

         string fetch = Command(socket, "A03", "FETCH 1 (RFC822.SIZE BODY.PEEK[TEXT] BODY.PEEK[])");

         StringAssert.Contains("RFC822.SIZE " + message.Length, fetch, "RFC822.SIZE must be the literal's size. Got: " + fetch);
         StringAssert.Contains("BODY[TEXT] {" + text.Length + "}\r\n" + text, fetch, "BODY[TEXT] must be the text as appended. Got: " + fetch);
         StringAssert.Contains("BODY[] {" + message.Length + "}\r\n" + message, fetch, "BODY[] must be the literal. Got: " + fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("A message that is a header with no line break after it still has that header.")]
      public void HeaderWithoutFinalLineBreakIsStillAHeader()
      {
         var socket = LogOnAndSelect(CreateAccount("headeronly@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX", "Date: Wed, 15 Dec 2010 13:00:00 +0000");

         string fetch = Command(socket, "A03", "FETCH 1 ENVELOPE");
         StringAssert.Contains("ENVELOPE (\"Wed, 15 Dec 2010 13:00:00 +0000\"", fetch,
            "The Date of a header-only message without a final CRLF must still be read. Got: " + fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("BODY[TEXT] of a multipart message is its text as stored, epilogue included, not a re-serialisation.")]
      public void BodyTextOfAMultipartIsTheStoredText()
      {
         var socket = LogOnAndSelect(CreateAccount("multitext@example.test"), "INBOX");

         const string text =
            "Prologue\r\n" +
            "\r\n" +
            "--b1\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "hello\r\n" +
            "\r\n" +
            "--b1--\r\n" +
            "Epilogue\r\n";

         const string message =
            "From: user@example.test\r\n" +
            "Content-Type: multipart/mixed; boundary=\"b1\"\r\n" +
            "\r\n" + text;

         AppendSynchronizing(socket, "A02", "INBOX", message);

         string fetch = Command(socket, "A03", "FETCH 1 BODY.PEEK[TEXT]");
         StringAssert.Contains("BODY[TEXT] {" + text.Length + "}\r\n" + text, fetch,
            "BODY[TEXT] lost the epilogue - it was the parsed tree written out again. Got: " + fetch);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ 1. NO, not BAD

      [Test]
      [Description("SELECT, EXAMINE and STATUS of a mailbox that does not exist are refused NO, not BAD; COPY adds [TRYCREATE].")]
      public void MissingMailboxIsRefusedNoNotBad()
      {
         var socket = LogOnAndSelect(CreateAccount("nosuch@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "Subject: one\r\n\r\nBody\r\n");

         StringAssert.Contains("A02 NO", Command(socket, "A02", "SELECT NoSuchMailbox"));

         var examiner = LogOn("nosuch@example.test");
         StringAssert.Contains("A03 NO", Command(examiner, "A03", "EXAMINE NoSuchMailbox"));
         StringAssert.Contains("A04 NO", Command(examiner, "A04", "STATUS NoSuchMailbox (MESSAGES)"));
         examiner.Disconnect();

         var copier = LogOnAndSelect("nosuch@example.test", "INBOX");
         string copy = Command(copier, "A05", "COPY 1 NoSuchMailbox");
         StringAssert.Contains("A05 NO [TRYCREATE]", copy, copy);
         ClassicAssert.IsFalse(copy.EndsWith("\r\n\r\n"), "The refusal must not be followed by an empty line. Got: " + copy);
         copier.Disconnect();

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ 2. UID ranges

      [Test]
      [Description("UID FETCH <uidnext>:* returns the last message - \"*\" is always the highest UID (RFC 3501 6.4.8).")]
      public void UidRangeEndingInStarAlwaysIncludesTheLastMessage()
      {
         var socket = LogOnAndSelect(CreateAccount("uidstar@example.test"), "INBOX");

         for (int i = 1; i <= 3; i++)
            AppendSynchronizing(socket, "A0" + i, "INBOX", "Subject: " + i + "\r\n\r\nBody\r\n");

         long uidNext = UidNext(socket, "INBOX");
         long lastUid = uidNext - 1;

         string fetch = Command(socket, "A05", "UID FETCH " + uidNext + ":* (UID)");
         StringAssert.Contains("* 3 FETCH (UID " + lastUid + ")", fetch,
            "A UID range ending in * must include the highest UID even when it starts above it. Got: " + fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("A UID set reaching 4294967295 is read as the unsigned number it is.")]
      public void UidSetUpToTheLargestUid()
      {
         var socket = LogOnAndSelect(CreateAccount("uidmax@example.test"), "INBOX");

         for (int i = 1; i <= 3; i++)
            AppendSynchronizing(socket, "A0" + i, "INBOX", "Subject: " + i + "\r\n\r\nBody\r\n");

         long uidNext = UidNext(socket, "INBOX");

         string all = ResultNumbers(Command(socket, "A05", "UID SEARCH UID 1:4294967295"), "SEARCH");
         ClassicAssert.AreEqual(3, all.Split(' ').Length, "UID 1:4294967295 is every message. Got: " + all);

         string none = ResultNumbers(Command(socket, "A06", "UID SEARCH UID " + uidNext + ":4294967295"), "SEARCH");
         ClassicAssert.AreEqual("", none, "No message has a UID from UIDNEXT up. Got: " + none);

         string fetch = Command(socket, "A07", "UID FETCH 1:4294967295 (UID)");
         StringAssert.Contains("* 3 FETCH", fetch, fetch);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ 3. ENVELOPE

      [Test]
      [Description("ENVELOPE: a missing Subject is NIL, and an address without a display name has NIL as its name.")]
      public void EnvelopeHasNilForWhatIsMissing()
      {
         var socket = LogOnAndSelect(CreateAccount("envelope@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX",
            "Date: Thu, 15 Feb 2007 01:02:03 +0200\r\n" +
            "From: user@domain\r\n" +
            "\r\n" +
            "body\r\n");

         string fetch = Command(socket, "A03", "FETCH 1 ENVELOPE");
         StringAssert.Contains(
            "ENVELOPE (\"Thu, 15 Feb 2007 01:02:03 +0200\" NIL ((NIL NIL \"user\" \"domain\")) ((NIL NIL \"user\" \"domain\")) ((NIL NIL \"user\" \"domain\")) NIL NIL NIL NIL NIL)",
            fetch, fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("ENVELOPE: groups, a name given as a comment, a source route, and a Sender that is present but empty.")]
      public void EnvelopeAddressForms()
      {
         var socket = LogOnAndSelect(CreateAccount("envforms@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX",
            "Date: Thu, 15 Feb 2007 01:02:03 +0200\r\n" +
            "From: user@domain (Real Name)\r\n" +
            "Sender: \r\n" +
            "To: group: g1@d1.org, g2@d2.org;, group2: g3@d3.org;\r\n" +
            "Cc: <@route:user@domain>\r\n" +
            "\r\n" +
            "body\r\n");

         string fetch = Command(socket, "A03", "FETCH 1 ENVELOPE");

         // The comment is the name; an empty Sender (and a missing Reply-To) is the From.
         StringAssert.Contains("NIL ((\"Real Name\" NIL \"user\" \"domain\")) ((\"Real Name\" NIL \"user\" \"domain\")) ((\"Real Name\" NIL \"user\" \"domain\"))", fetch, fetch);

         // RFC 3501 7.4.2: a group start is (NIL NIL "name" NIL), its end (NIL NIL NIL NIL).
         StringAssert.Contains("((NIL NIL \"group\" NIL)(NIL NIL \"g1\" \"d1.org\")(NIL NIL \"g2\" \"d2.org\")(NIL NIL NIL NIL)(NIL NIL \"group2\" NIL)(NIL NIL \"g3\" \"d3.org\")(NIL NIL NIL NIL))", fetch, fetch);

         // The route is the address's second member.
         StringAssert.Contains("((NIL \"@route\" \"user\" \"domain\"))", fetch, fetch);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ FETCH

      [Test]
      [Description("FETCH FULL is (FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODY) - BODY, not BODYSTRUCTURE.")]
      public void FetchFullGivesBody()
      {
         var socket = LogOnAndSelect(CreateAccount("full@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX", "Subject: full\r\n\r\nBody\r\n");

         string fetch = Command(socket, "A03", "FETCH 1 FULL");
         StringAssert.Contains(" BODY (", fetch, fetch);
         ClassicAssert.IsFalse(fetch.Contains("BODYSTRUCTURE"), "FULL answered BODYSTRUCTURE. Got: " + fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("BODYSTRUCTURE counts lines as lines, and a part's description is its Content-Description, not the message's Subject.")]
      public void BodyStructureLinesAndDescription()
      {
         var socket = LogOnAndSelect(CreateAccount("structure@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX",
            "From: user@example.test\r\n" +
            "Subject: not a description\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "\r\n" +
            "hello\r\n");

         AppendSynchronizing(socket, "A03", "INBOX",
            "From: user@example.test\r\n" +
            "Content-Type: multipart/mixed; boundary=\"b1\"\r\n" +
            "\r\n" +
            "--b1\r\n" +
            "Content-Type: text/plain; charset=us-ascii\r\n" +
            "Content-Description: the notes\r\n" +
            "\r\n" +
            "hello\r\n" +
            "\r\n" +
            "--b1--\r\n");

         string single = Command(socket, "A04", "FETCH 1 BODY");
         StringAssert.Contains("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"us-ascii\") NIL NIL \"7bit\" 7 1)", single,
            "\"hello\\r\\n\" is 7 octets and one line, and the Subject is no description. Got: " + single);

         string multi = Command(socket, "A05", "FETCH 2 BODY");
         StringAssert.Contains("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"us-ascii\") NIL \"the notes\" \"7bit\" 7 1)", multi, multi);

         socket.Disconnect();
      }

      [Test]
      [Description("BINARY of a quoted-printable part removes trailing white space and honours \"= \" soft line breaks (RFC 2045 6.7).")]
      public void BinaryDecodesQuotedPrintableAsRfc2045Says()
      {
         var socket = LogOnAndSelect(CreateAccount("binaryqp@example.test"), "INBOX");

         // imaptest's fetch-binary-mime-qp message.
         AppendSynchronizing(socket, "A02", "INBOX",
            "From: user@domain.org\r\n" +
            "Mime-Version: 1.0\r\n" +
            "Content-Type: multipart/mixed; boundary=\"foo bar\"\r\n" +
            "\r\n" +
            "--foo bar\r\n" +
            "Content-Type: text/x-myown; charset=us-ascii\r\n" +
            "Content-Transfer-Encoding: quoted-printable\r\n" +
            "\r\n" +
            "hello  \r\nbar=\r\n\r\nfoo\t=\r\nbar\r\nfoo\t \t= \r\n=62\r\nfoo = \t\r\nbar\r\nfoo =\r\n=62\r\nfoo  \r\nbar=\r\n\r\nfoo_bar\r\n" +
            "\r\n" +
            "--foo bar--\r\n");

         const string decoded = "hello\r\nbar\r\nfoo\tbar\r\nfoo\t \tb\r\nfoo bar\r\nfoo b\r\nfoo\r\nbar\r\nfoo_bar\r\n";

         string size = Command(socket, "A03", "FETCH 1 (BINARY.SIZE[1])");
         StringAssert.Contains("BINARY.SIZE[1] " + decoded.Length, size, size);

         string content = Command(socket, "A04", "FETCH 1 (BINARY.PEEK[1])");
         StringAssert.Contains(decoded, content, content);

         socket.Disconnect();
      }

      [Test]
      [Description("A FETCH that sets \\Seen says so in its response (RFC 3501 6.4.5).")]
      public void ImplicitSeenIsReportedInTheFetchResponse()
      {
         var socket = LogOnAndSelect(CreateAccount("implicitseen@example.test"), "INBOX");

         AppendSynchronizing(socket, "A02", "INBOX", "Subject: seen\r\n\r\nBody\r\n");

         string fetch = Command(socket, "A03", "FETCH 1 BODY[TEXT]");
         StringAssert.Contains("FLAGS (\\Seen)", fetch, "The \\Seen this FETCH set must be in its response. Got: " + fetch);

         socket.Disconnect();
      }

      [Test]
      [Description("BODY[HEADER] without PEEK sets \\Seen as every BODY[<section>] does; BODY.PEEK[HEADER] does not.")]
      public void BodyHeaderSetsSeenAndItsPeekDoesNot()
      {
         var socket = LogOnAndSelect(CreateAccount("headerseen@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "Subject: one\r\n\r\nBody\r\n");
         AppendSynchronizing(socket, "A02", "INBOX", "Subject: two\r\n\r\nBody\r\n");

         string header = Command(socket, "A03", "FETCH 1 BODY[HEADER]");
         StringAssert.Contains("FLAGS (\\Seen)", header, "BODY[HEADER] must set \\Seen and say so. Got: " + header);

         string peek = Command(socket, "A04", "FETCH 2 BODY.PEEK[HEADER]");
         ClassicAssert.IsFalse(peek.Contains("\\Seen"), "BODY.PEEK[HEADER] must not set \\Seen. Got: " + peek);

         ClassicAssert.AreEqual("2", ResultNumbers(Command(socket, "A05", "SEARCH UNSEEN"), "SEARCH"));

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ SEARCH

      [Test]
      [Description("SEARCH HEADER needs the field to be there - even for \"\" - and matches any occurrence of a repeated field.")]
      public void SearchHeaderNeedsTheField()
      {
         var socket = LogOnAndSelect(CreateAccount("searchheader@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "Subject: hello beautiful world\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A02", "INBOX", "X-Extra: hello beautiful header\r\nX-Extra: another one\r\n\r\nbody\r\n");

         ClassicAssert.AreEqual("1", ResultNumbers(Command(socket, "A03", "SEARCH HEADER SUBJECT \"\""), "SEARCH"));
         ClassicAssert.AreEqual("2", ResultNumbers(Command(socket, "A04", "SEARCH NOT HEADER SUBJECT \"\""), "SEARCH"));
         ClassicAssert.AreEqual("2", ResultNumbers(Command(socket, "A05", "SEARCH HEADER X-EXTRA \"another\""), "SEARCH"));

         socket.Disconnect();
      }

      [Test]
      [Description("FROM, TO and BCC search the addresses as parsed - comments, display names given as comments, groups.")]
      public void SearchAddressesAsParsed()
      {
         var socket = LogOnAndSelect(CreateAccount("searchaddr@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "From: user-from@domain.org\r\nBcc: user-bcc@domain.org\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A02", "INBOX", "From: RealFrom <user-from@domain.org>\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A03", "INBOX", "From: <user-from (comment)@ (comment) domain.org>\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A04", "INBOX", "From: user-from@domain.org (RealFrom)\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A05", "INBOX", "From: other@domain.org\r\nTo: groupname: groupuser1@domain.org;\r\n\r\nbody\r\n");

         ClassicAssert.AreEqual("1 2 3 4", ResultNumbers(Command(socket, "A06", "SEARCH FROM user-from@domain.org"), "SEARCH"));
         ClassicAssert.AreEqual("2 4", ResultNumbers(Command(socket, "A07", "SEARCH FROM realfrom"), "SEARCH"));
         ClassicAssert.AreEqual("1", ResultNumbers(Command(socket, "A08", "SEARCH BCC user-bcc@domain.org"), "SEARCH"));
         ClassicAssert.AreEqual("5", ResultNumbers(Command(socket, "A09", "SEARCH TO groupname"), "SEARCH"));
         ClassicAssert.AreEqual("5", ResultNumbers(Command(socket, "A10", "SEARCH TO groupuser1"), "SEARCH"));

         socket.Disconnect();
      }

      [Test]
      [Description("SENTON, SENTBEFORE and SENTSINCE compare the date the sender wrote, disregarding time and zone.")]
      public void SentDatesIgnoreTimeAndZone()
      {
         var socket = LogOnAndSelect(CreateAccount("sentdate@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "Date: Sat, 24 Mar 2007 23:00:00 +0200\r\n\r\nbody\r\n");
         AppendSynchronizing(socket, "A02", "INBOX", "Date: Sun, 25 Mar 2007 00:00:00 +0200\r\n\r\nbody\r\n");

         ClassicAssert.AreEqual("1", ResultNumbers(Command(socket, "A03", "SEARCH SENTON 24-Mar-2007"), "SEARCH"));
         ClassicAssert.AreEqual("2", ResultNumbers(Command(socket, "A04", "SEARCH SENTON 25-mar-2007"), "SEARCH"));
         ClassicAssert.AreEqual("", ResultNumbers(Command(socket, "A05", "SEARCH SENTBEFORE 24-Mar-2007"), "SEARCH"));
         ClassicAssert.AreEqual("1", ResultNumbers(Command(socket, "A06", "SEARCH SENTBEFORE 25-Mar-2007"), "SEARCH"));
         ClassicAssert.AreEqual("2", ResultNumbers(Command(socket, "A07", "SEARCH SENTSINCE 25-Mar-2007"), "SEARCH"));

         socket.Disconnect();
      }

      [Test]
      [Description("ESEARCH returns ALL as a compact sequence-set, and a first MODSEQ enables CONDSTORE and reports HIGHESTMODSEQ.")]
      public void EsearchAndCondstoreEnabling()
      {
         var socket = LogOnAndSelect(CreateAccount("esearch@example.test"), "INBOX");

         for (int i = 1; i <= 3; i++)
            AppendSynchronizing(socket, "A0" + i, "INBOX", "Subject: " + i + "\r\n\r\nBody\r\n");

         string all = Command(socket, "A04", "SEARCH RETURN (ALL) ALL");
         StringAssert.Contains(" ALL 1:3", all, all);

         // SELECT had no CONDSTORE; FETCH MODSEQ is the first enabling command.
         string fetch = Command(socket, "A05", "FETCH 1 (MODSEQ)");
         int highest = fetch.IndexOf("* OK [HIGHESTMODSEQ ", StringComparison.Ordinal);
         int line = fetch.IndexOf("* 1 FETCH", StringComparison.Ordinal);
         ClassicAssert.IsTrue(highest >= 0 && line > highest, "HIGHESTMODSEQ must be reported before the FETCH. Got: " + fetch);

         // Enabled now: STORE reports MODSEQ, and HIGHESTMODSEQ is not repeated.
         string store = Command(socket, "A06", "STORE 1 +FLAGS (\\Flagged)");
         StringAssert.Contains("MODSEQ (", store, store);

         string again = Command(socket, "A07", "FETCH 2 (MODSEQ)");
         ClassicAssert.IsFalse(again.Contains("HIGHESTMODSEQ"), again);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ MOVE

      [Test]
      [Description("MOVE sends COPYUID in an untagged OK before the EXPUNGE (RFC 6851 4.3).")]
      public void MoveSendsCopyUidBeforeTheExpunge()
      {
         var socket = LogOnAndSelect(CreateAccount("move@example.test"), "INBOX");

         StringAssert.Contains("A01 OK", Command(socket, "A01", "CREATE Destination"));
         AppendSynchronizing(socket, "A02", "INBOX", "Subject: moving\r\n\r\nBody\r\n");

         string move = Command(socket, "A03", "MOVE 1 Destination");

         int copyUid = move.IndexOf("* OK [COPYUID ", StringComparison.Ordinal);
         int expunge = move.IndexOf("* 1 EXPUNGE", StringComparison.Ordinal);

         ClassicAssert.IsTrue(copyUid >= 0 && expunge > copyUid, "COPYUID must come untagged, before the EXPUNGE. Got: " + move);
         StringAssert.Contains("A03 OK", move, move);

         socket.Disconnect();
      }

      [Test]
      [Description("MOVE into the selected mailbox itself reports the copy's EXISTS after the EXPUNGE, in the same response.")]
      public void MoveIntoTheSelectedMailboxReportsTheCopy()
      {
         var socket = LogOnAndSelect(CreateAccount("moveself@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "Subject: moving in place\r\n\r\nBody\r\n");

         string move = Command(socket, "A02", "MOVE 1 INBOX");

         int expunge = move.IndexOf("* 1 EXPUNGE", StringComparison.Ordinal);
         int exists = move.IndexOf("* 1 EXISTS", StringComparison.Ordinal);
         int tagged = move.IndexOf("A02 OK", StringComparison.Ordinal);

         ClassicAssert.IsTrue(expunge >= 0 && exists > expunge && tagged > exists,
            "The copy must be announced after the EXPUNGE and before the tagged OK. Got: " + move);

         string status = Command(socket, "A03", "STATUS INBOX (MESSAGES)");
         StringAssert.Contains("MESSAGES 1", status, status);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ LIST / LSUB

      [Test]
      [Description("A wildcard anywhere in a LIST pattern matches, not only at its end.")]
      public void ListWildcardsAnywhereInThePattern()
      {
         var socket = LogOn(CreateAccount("wildcards@example.test"));

         StringAssert.Contains("A01 OK", Command(socket, "A01", "CREATE Top.Mid.Leaf"));
         StringAssert.Contains("A02 OK", Command(socket, "A02", "CREATE Top.Other"));

         string star = Command(socket, "A03", "LIST \"\" \"Top.*Leaf\"");
         StringAssert.Contains("\"Top.Mid.Leaf\"", star, star);
         ClassicAssert.IsFalse(star.Contains("\"Top.Other\""), star);

         string percent = Command(socket, "A04", "LIST \"\" \"Top.%d.%\"");
         StringAssert.Contains("\"Top.Mid.Leaf\"", percent, percent);

         string leading = Command(socket, "A05", "LIST \"\" \"*Leaf\"");
         StringAssert.Contains("\"Top.Mid.Leaf\"", leading, leading);

         string several = Command(socket, "A06", "LIST \"\" (\"*Other\" \"Top.Mid\")");
         StringAssert.Contains("\"Top.Other\"", several, several);
         StringAssert.Contains("\"Top.Mid\"", several, several);

         socket.Disconnect();
      }

      [Test]
      [Description("LSUB with % lists the unsubscribed parent of a deeper subscription as \\Noselect (RFC 3501 6.3.9).")]
      public void LsubListsTheParentOfADeeperSubscription()
      {
         var socket = LogOn(CreateAccount("lsubparent@example.test"));

         StringAssert.Contains("A01 OK", Command(socket, "A01", "CREATE Top.Mid.Leaf"));
         StringAssert.Contains("A02 OK", Command(socket, "A02", "SUBSCRIBE Top.Mid.Leaf"));

         string lsub = Command(socket, "A03", "LSUB \"\" \"Top.%\"");

         string parentLine = null;
         foreach (string candidate in lsub.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (candidate.EndsWith("\"Top.Mid\"", StringComparison.Ordinal))
               parentLine = candidate;
         }

         ClassicAssert.IsNotNull(parentLine, "LSUB % must list Top.Mid for the subscription below it. Got: " + lsub);
         StringAssert.Contains("\\Noselect", parentLine, parentLine);
         ClassicAssert.IsFalse(lsub.Contains("\"Top.Mid.Leaf\""), lsub);

         string deep = Command(socket, "A04", "LSUB \"\" \"*Leaf\"");
         StringAssert.Contains("\"Top.Mid.Leaf\"", deep, deep);

         socket.Disconnect();
      }

      [Test]
      [Description("LIST-EXTENDED: unknown options are BAD, RECURSIVEMATCH alone is BAD, and with SUBSCRIBED it reports CHILDINFO.")]
      public void ListExtendedOptions()
      {
         var socket = LogOn(CreateAccount("listext@example.test"));

         StringAssert.Contains("A01 BAD", Command(socket, "A01", "LIST (IMAPTEST) \"\" \"\""));
         StringAssert.Contains("A02 BAD", Command(socket, "A02", "LIST (RECURSIVEMATCH) \"\" \"\""));
         StringAssert.Contains("A03 BAD", Command(socket, "A03", "LIST (RECURSIVEMATCH REMOTE) \"\" \"\""));
         StringAssert.Contains("A04 BAD", Command(socket, "A04", "LIST \"\" \"\" RETURN (IMAPTEST)"));

         StringAssert.Contains("A05 OK", Command(socket, "A05", "CREATE Top.Mid.Leaf"));
         StringAssert.Contains("A06 OK", Command(socket, "A06", "SUBSCRIBE Top.Mid.Leaf"));

         string recursive = Command(socket, "A07", "LIST (SUBSCRIBED RECURSIVEMATCH) \"\" \"Top\" RETURN (CHILDREN)");
         StringAssert.Contains("\"Top\" (\"CHILDINFO\" (\"SUBSCRIBED\"))", recursive, recursive);

         // The subscribed child is matched by the second pattern, so the parent
         // carries no CHILDINFO and is not listed.
         string both = Command(socket, "A08", "LIST (SUBSCRIBED RECURSIVEMATCH) \"\" (\"Top\" \"Top.Mid.Leaf\")");
         ClassicAssert.IsFalse(both.Contains("CHILDINFO"), both);
         StringAssert.Contains("\"Top.Mid.Leaf\"", both, both);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ SORT / THREAD

      [Test]
      [Description("SORT applies every criterion in turn, REVERSE reverses only its own, and ties go to sequence order.")]
      public void SortCriteriaTiesAndReverse()
      {
         var socket = LogOnAndSelect(CreateAccount("sorting@example.test"), "INBOX");

         AppendSynchronizing(socket, "A01", "INBOX", "From: Zed <a@z.test>\r\nSubject: Re: beta\r\n\r\n1\r\n", " \"22-Feb-2008 01:00:00 +0000\"");
         AppendSynchronizing(socket, "A02", "INBOX", "From: Amy <b@a.test>\r\nSubject: alpha\r\n\r\n2\r\n", " \"22-Feb-2008 02:00:00 +0000\"");
         AppendSynchronizing(socket, "A03", "INBOX", "From: Amy <b@a.test>\r\nSubject: gamma\r\n\r\n3 is longer\r\n", " \"22-Feb-2008 02:00:00 +0000\"");

         // 2 and 3 arrived together: REVERSE leaves their tie in sequence order.
         ClassicAssert.AreEqual("2 3 1", ResultNumbers(Command(socket, "A04", "SORT (REVERSE ARRIVAL) US-ASCII ALL"), "SORT"));

         // The second criterion breaks the first's tie - it used to be the only one.
         ClassicAssert.AreEqual("1 3 2", ResultNumbers(Command(socket, "A05", "SORT (ARRIVAL REVERSE SIZE) US-ASCII ALL"), "SORT"));

         // FROM is the first address's mailbox - "a" before "b" - not the display name.
         ClassicAssert.AreEqual("1 2 3", ResultNumbers(Command(socket, "A06", "SORT (FROM) US-ASCII ALL"), "SORT"));

         // SUBJECT is the base subject: "Re: beta" sorts as "beta".
         ClassicAssert.AreEqual("2 1 3", ResultNumbers(Command(socket, "A07", "SORT (SUBJECT) US-ASCII ALL"), "SORT"));

         socket.Disconnect();
      }

      [Test]
      [Description("The base subject unwraps nested [fwd: ...] and never takes \"[fwd: [x] ...\" for a blob (RFC 5256 2.1).")]
      public void BaseSubjectUnwrapsNestedForwards()
      {
         var socket = LogOnAndSelect(CreateAccount("basesubject@example.test"), "INBOX");

         // From imaptest's sort-subject messages.
         AppendSynchronizing(socket, "A01", "INBOX", "Subject: re: [fwd: [fwd: re: [fwd: babylon]]]\r\n\r\n1\r\n");
         AppendSynchronizing(socket, "A02", "INBOX", "Subject: [xyz]\r\n\r\n2\r\n");
         AppendSynchronizing(socket, "A03", "INBOX", "Subject: C\r\n\r\n3\r\n");
         AppendSynchronizing(socket, "A04", "INBOX", "Subject: b (a)\r\n\r\n4\r\n");

         // "B (A)", "BABYLON", "C", "[XYZ]". The first message used to sort as
         // "]]", after "[xyz]".
         ClassicAssert.AreEqual("4 1 3 2", ResultNumbers(Command(socket, "A05", "SORT (SUBJECT) US-ASCII ALL"), "SORT"));

         socket.Disconnect();
      }

      [Test]
      [Description("THREAD REFERENCES breaks a message's old parent link even when the new one would close a loop (RFC 5256 1.B).")]
      public void ThreadReferencesBreaksTheOldParentLink()
      {
         var socket = LogOnAndSelect(CreateAccount("threadloop@example.test"), "INBOX");

         // imaptest's thread test.
         AppendSynchronizing(socket, "A01", "INBOX", "Message-ID: <a@b>\r\n\r\nbody\r\n", " \"22-Feb-2008 17:06:23 +0000\"");
         AppendSynchronizing(socket, "A02", "INBOX", "Message-Id: <b@b>\r\nReferences: <a@b> <c@b>\r\n\r\nbody\r\n", " \"22-Feb-2008 15:06:23 +0000\"");
         AppendSynchronizing(socket, "A03", "INBOX", "Message-Id: <c@b>\r\nReferences: <b@b>\r\n\r\nbody\r\n", " \"22-Feb-2008 16:06:25 +0000\"");

         string thread = Command(socket, "A04", "THREAD REFERENCES US-ASCII ALL");
         StringAssert.Contains("* THREAD (3 2)(1)\r\n", thread, thread);

         socket.Disconnect();
      }

      // ------------------------------------------------------------------ under load

      [Test]
      [Description("A keyword another session created is announced in an untagged FLAGS before a FETCH carries it (RFC 3501 7.2.6).")]
      public void NewKeywordIsAnnouncedBeforeItIsUsed()
      {
         string address = CreateAccount("keywords@example.test");

         var first = LogOnAndSelect(address, "INBOX");
         AppendSynchronizing(first, "A01", "INBOX", "Subject: keyword\r\n\r\nBody\r\n");

         var second = LogOnAndSelect(address, "INBOX");

         string store = Command(second, "B01", "STORE 1 +FLAGS ($Imaptest1)");
         int flagsInStore = store.IndexOf("* FLAGS (", StringComparison.Ordinal);
         int fetchInStore = store.IndexOf("* 1 FETCH", StringComparison.Ordinal);
         ClassicAssert.IsTrue(flagsInStore >= 0 && fetchInStore > flagsInStore && store.Contains("$Imaptest1)\r\n"),
            "The STORE that created the keyword must announce it first. Got: " + store);

         string noop = Command(first, "A02", "NOOP");
         int flags = noop.IndexOf("* FLAGS (", StringComparison.Ordinal);
         int fetch = noop.IndexOf("* 1 FETCH", StringComparison.Ordinal);
         ClassicAssert.IsTrue(flags >= 0 && fetch > flags, "The other session must be told of the keyword before the FETCH. Got: " + noop);
         StringAssert.Contains("$Imaptest1", noop.Substring(flags, fetch - flags), noop);

         first.Disconnect();
         second.Disconnect();
      }

      [Test]
      [Description("A session with the mailbox selected is the first to be notified of a new message, so it is \\Recent there.")]
      public void NewMessageIsRecentToTheSessionThatHasTheMailboxOpen()
      {
         string address = CreateAccount("recent@example.test");

         var watcher = LogOnAndSelect(address, "INBOX");

         var copier = LogOn(address);
         StringAssert.Contains("C01 OK", Command(copier, "C01", "CREATE Source"));
         AppendSynchronizing(copier, "C02", "Source", "Subject: copied\r\n\r\nBody\r\n");
         StringAssert.Contains("C03 OK", Command(copier, "C03", "SELECT Source"));
         StringAssert.Contains("C04 OK", Command(copier, "C04", "COPY 1 INBOX"));

         string noop = Command(watcher, "W01", "NOOP");
         StringAssert.Contains("* 1 EXISTS", noop, noop);
         StringAssert.Contains("* 1 RECENT", noop, "It used to be \"* 0 RECENT\". Got: " + noop);

         copier.Disconnect();
         watcher.Disconnect();
      }

      [Test]
      [Description("A mailbox deleted and made again gets a new UIDVALIDITY, however quickly (RFC 3501 2.3.1.1).")]
      public void RecreatedMailboxHasANewUidValidity()
      {
         var socket = LogOn(CreateAccount("uidvalidity@example.test"));

         StringAssert.Contains("A01 OK", Command(socket, "A01", "CREATE Again"));
         string before = Command(socket, "A02", "STATUS Again (UIDVALIDITY)");
         StringAssert.Contains("A03 OK", Command(socket, "A03", "DELETE Again"));
         StringAssert.Contains("A04 OK", Command(socket, "A04", "CREATE Again"));
         string after = Command(socket, "A05", "STATUS Again (UIDVALIDITY)");

         string validityBefore = Regex.Match(before, @"UIDVALIDITY (\d+)").Groups[1].Value;
         string validityAfter = Regex.Match(after, @"UIDVALIDITY (\d+)").Groups[1].Value;

         ClassicAssert.IsNotEmpty(validityBefore, before);
         ClassicAssert.AreNotEqual(validityBefore, validityAfter, "The recreated mailbox kept the old UIDVALIDITY. " + before + after);

         socket.Disconnect();
      }
   }
}
