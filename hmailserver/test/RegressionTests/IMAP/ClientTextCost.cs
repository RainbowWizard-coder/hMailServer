// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    What one command from a signed-in account costs the server, where the
   ///    command's text decides it: the shapes the re-check of the FETCH fix
   ///    (FetchParsingTime) found beside it. Each is answered in a time that grows
   ///    with what the command and the mailbox hold, not with their product.
   ///
   ///    - FETCH items that each end before the one space there is ("[][][]... x"):
   ///      the item splitter looked for the first space from each item's start.
   ///    - A FETCH command longer than one line: the text after each literal on its
   ///      line joins the command, so one FETCH is not held to the quarter of a
   ///      megabyte one line carries, and reaches the command buffer's eleven.
   ///    - "$" (SEARCHRES) named over and over in one message set, which copied the
   ///      saved result once for each.
   ///    - A SORT criterion named over and over, which made a key of each for every
   ///      message.
   ///    - "$" named over and over as a SEARCH key, which copied the saved result
   ///      into each.
   ///    - A SEARCH TEXT key sent as a long literal, over a message that holds its
   ///      beginning everywhere: the case-ignoring search compared afresh at each
   ///      position, the product of the two lengths, in one call.
   ///    - A FETCH section cut to a byte and named over and over - HEADER,
   ///      HEADER.FIELDS, HEADER.FIELDS.NOT - which read or made its whole
   ///      source for each, while the ceiling of a message's sections counted
   ///      the byte it held.
   ///    - FETCH sections of the parts of a forwarded message, or of a message of
   ///      thousands of parts, which parsed the forwarded message afresh, or
   ///      listed the parts from the first, for each.
   ///
   ///    Each bound sits well above what the fixed server takes and well below the
   ///    flawed one; the negative control is the same tests against a server built
   ///    without the fixes, recorded in the commit that adds them.
   /// </summary>
   [TestFixture]
   public class ClientTextCost : TestFixtureBase
   {
      private const string Password = "test";

      private static readonly TimeSpan Bound = TimeSpan.FromSeconds(5);
      private static readonly TimeSpan SearchBound = TimeSpan.FromSeconds(10);

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
         ReadUntilLine(socket, "* OK", 15);

         string login = Command(socket, "L1", "LOGIN " + address + " " + Password, 15);
         StringAssert.Contains("L1 OK", login, "The LOGIN failed: " + login);

         return socket;
      }

      /// <summary>
      ///    Reads until a whole line starting with the given text has arrived, or
      ///    fails after the timeout.
      ///
      ///    Each look reads only what arrived since the one before (and the few
      ///    characters before it a line start could straddle). It copied and
      ///    searched all that had arrived at every read: over a response of 199 MB
      ///    on bench150, the slow Windows runner, 96 reads copied 4.4 billion
      ///    characters, and the response took 11.3 seconds to read where reading
      ///    it once took 4.9.
      /// </summary>
      private static string ReadUntilLine(TcpConnection socket, string lineStart, int seconds)
      {
         var received = new StringBuilder();
         var deadline = DateTime.Now.AddSeconds(seconds);

         string marker = "\r\n" + lineStart;
         int lineAt = -1;          // where the line starts, once found
         int markerSearched = 0;   // how far the line start has been looked for
         int endSearched = 0;      // how far the line's end has been looked for

         while (DateTime.Now < deadline)
         {
            if (socket.WaitForData(TimeSpan.FromMilliseconds(100)))
               received.Append(socket.Receive());

            if (lineAt < 0 && received.Length >= lineStart.Length &&
                received.ToString(0, lineStart.Length) == lineStart)
               lineAt = 0;

            if (lineAt < 0)
            {
               int from = Math.Max(0, markerSearched - (marker.Length - 1));
               int found = received.ToString(from, received.Length - from).IndexOf(marker, StringComparison.Ordinal);

               if (found >= 0)
                  lineAt = from + found + 2;

               markerSearched = received.Length;
            }

            if (lineAt >= 0)
            {
               int from = Math.Max(lineAt, endSearched - 1);

               if (from < received.Length &&
                   received.ToString(from, received.Length - from).IndexOf("\r\n", StringComparison.Ordinal) >= 0)
                  return received.ToString();

               endSearched = received.Length;
            }

            if (!socket.IsConnected)
               break;
         }

         Assert.Fail("No line starting \"" + lineStart + "\" within " + seconds + " seconds. Received " +
            received.Length + " characters, ending: " + Tail(received.ToString()));
         return null;
      }

      private static string Tail(string text)
      {
         return text.Length <= 400 ? text : text.Substring(text.Length - 400);
      }

      private static string Command(TcpConnection socket, string tag, string command, int seconds)
      {
         socket.Send(tag + " " + command + "\r\n");
         return ReadUntilLine(socket, tag + " ", seconds);
      }

      /// <summary>
      ///    Sends the text (holding the command tagged "T1" and anything it needs)
      ///    and answers what came back up to T1's tagged line, and how long it took.
      /// </summary>
      private static string Timed(TcpConnection socket, string text, out TimeSpan elapsed)
      {
         var stopwatch = Stopwatch.StartNew();
         socket.Send(text);

         // Longer than every bound, so a flawed server fails the test on the time
         // it took rather than on a missing answer.
         string response = ReadUntilLine(socket, "T1 ", 120);
         stopwatch.Stop();

         elapsed = stopwatch.Elapsed;
         TestContext.Progress.WriteLine("ClientTextCost: answered in " + (long) elapsed.TotalMilliseconds + " ms.");
         return response;
      }

      /// <summary>
      ///    Appends count messages, pipelined with {n+} literals, the i-th with i % 7
      ///    more characters in its body so that their sizes differ, and selects INBOX.
      ///    Each header holds the given fields before its Subject.
      /// </summary>
      private static void AppendAndSelect(TcpConnection socket, int count, string fields = "")
      {
         var batch = new StringBuilder();
         for (int i = 1; i <= count; i++)
         {
            string message = fields + "Subject: message " + i + "\r\n\r\nBody" + new string('x', i % 7) + ".\r\n";
            batch.Append("P" + i + " APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\n");
         }

         socket.Send(batch.ToString());
         string appended = ReadUntilLine(socket, "P" + count + " ", 120);
         StringAssert.Contains("P" + count + " OK", appended, "The last APPEND failed: " + Tail(appended));

         string select = Command(socket, "S1", "SELECT INBOX", 30);
         StringAssert.Contains("S1 OK", select, "The SELECT failed: " + Tail(select));
         StringAssert.Contains("* " + count + " EXISTS", select, "Not every message was appended: " + Tail(select));
      }

      private static int CountMatches(string text, string pattern)
      {
         return Regex.Matches(text, pattern, RegexOptions.Multiline).Count;
      }

      private static string RepeatJoined(string item, string separator, int count)
      {
         var builder = new StringBuilder(item.Length * count + separator.Length * count);
         for (int i = 0; i < count; i++)
         {
            if (i > 0)
               builder.Append(separator);
            builder.Append(item);
         }
         return builder.ToString();
      }

      // -------------------------------------------------------------------- tests

      [Test]
      [Description("A FETCH of 120,000 \"[]\" before one space is answered in seconds (CWE-407).")]
      public void FetchOfItemsEndingBeforeOneFarSpaceIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("fetchspace@example.test"));
         AppendAndSelect(socket, 1);

         // Each "[]" is an item that ends at its "]", before the one space, which
         // the splitter looked for from each item's start: 120,000 times, to the
         // end of a quarter of a megabyte.
         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1 (" + RepeatJoined("[]", "", 120000) + " FLAGS)\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         StringAssert.Contains("FLAGS", response, Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "A FETCH of 120,000 \"[]\" before one space took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("A FETCH command of four lines joined by literals - 800 KB - is parsed in one pass (CWE-407).")]
      public void FetchCommandOfSeveralLinesIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("fetchlines@example.test"));
         AppendAndSelect(socket, 1);

         // What follows each one-octet literal on its line joins the command, so
         // the four lines make one FETCH of 800 KB, more than the quarter of a
         // megabyte one line carries.
         string items = RepeatJoined("[]", "", 100000);
         var command = new StringBuilder();
         command.Append("T1 FETCH 1 (" + items + " {1+}\r\n");
         command.Append("x" + items + " {1+}\r\n");
         command.Append("x" + items + " {1+}\r\n");
         command.Append("x" + items + " FLAGS)\r\n");

         TimeSpan elapsed;
         string response = Timed(socket, command.ToString(), out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         StringAssert.Contains("FLAGS", response, Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "A FETCH of four lines of 100,000 \"[]\" each took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("\"$\" named 100,000 times in a message set stands in once for a saved result of 1,000 messages.")]
      public void SavedResultNamedOverAndOverStandsInOnce()
      {
         var socket = LogOn(CreateAccount("searchressets@example.test"));
         AppendAndSelect(socket, 1000);

         string save = Command(socket, "A1", "UID SEARCH RETURN (SAVE) ALL", 30);
         StringAssert.Contains("A1 OK", save, Tail(save));

         // Each "$" used to be replaced by the whole saved result as text: 100,000
         // copies of 1,000 UIDs, before a message was looked at.
         TimeSpan elapsed;
         string response = Timed(socket, "T1 UID FETCH " + RepeatJoined("$", ",", 100000) + " (UID)\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         Assert.AreEqual(1000, CountMatches(response, @"^\* \d+ FETCH "),
            "Each saved message is fetched once. Got: " + Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "A UID FETCH of 100,000 \"$\" took " + (long) elapsed.TotalMilliseconds + " ms.");

         // "$" stands alone for the saved result; as part of a range it is no set.
         string range = Command(socket, "A2", "FETCH 1:$ (UID)", 30);
         StringAssert.Contains("A2 BAD", range, Tail(range));

         // And with other elements, in their order, each message once.
         string mixed = Command(socket, "A3", "UID FETCH 3,$,1 (UID)", 30);
         StringAssert.Contains("A3 OK", mixed, Tail(mixed));
         Assert.AreEqual(1000, CountMatches(mixed, @"^\* \d+ FETCH "), Tail(mixed));

         socket.Disconnect();
      }

      [Test]
      [Description("A SORT criterion named 40,000 times sorts as it does named once, in seconds.")]
      public void SortCriterionNamedOverAndOverSortsAsOnce()
      {
         var socket = LogOn(CreateAccount("sortrepeat@example.test"));
         AppendAndSelect(socket, 1000);

         string once = Command(socket, "A1", "SORT (REVERSE SIZE) US-ASCII ALL", 30);
         StringAssert.Contains("A1 OK", once, Tail(once));
         string expected = Regex.Match(once, @"^\* SORT.*$", RegexOptions.Multiline).Value.TrimEnd('\r');

         // A key of each for every message: 40,000 x 1,000, before the ceiling of
         // the search's time was asked.
         TimeSpan elapsed;
         string response = Timed(socket, "T1 SORT (REVERSE SIZE " + RepeatJoined("SIZE", " ", 40000) + ") US-ASCII ALL\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         Assert.AreEqual(expected, Regex.Match(response, @"^\* SORT.*$", RegexOptions.Multiline).Value.TrimEnd('\r'),
            "The criterion named again changes nothing.");
         Assert.That(elapsed, Is.LessThan(Bound),
            "A SORT of 40,000 SIZE criteria over 1,000 messages took " + (long) elapsed.TotalMilliseconds + " ms.");

         // Criteria this server does not know tie every message, however many.
         string unknown = Command(socket, "A2", "SORT (REVERSE SIZE " + RepeatJoined("X", " ", 40000) + ") US-ASCII ALL", 60);
         StringAssert.Contains("A2 OK", unknown, Tail(unknown));
         Assert.AreEqual(expected, Regex.Match(unknown, @"^\* SORT.*$", RegexOptions.Multiline).Value.TrimEnd('\r'));

         socket.Disconnect();
      }

      [Test]
      [Description("\"$\" named 100,000 times as a SEARCH key shares one copy of the saved result.")]
      public void SavedResultNamedOverAndOverAsASearchKeyIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("searchreskeys@example.test"));
         AppendAndSelect(socket, 100);

         string save = Command(socket, "A1", "SEARCH RETURN (SAVE) ALL", 30);
         StringAssert.Contains("A1 OK", save, Tail(save));

         TimeSpan elapsed;
         string response = Timed(socket, "T1 SEARCH " + RepeatJoined("$", " ", 100000) + "\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         string result = Regex.Match(response, @"^\* SEARCH.*$", RegexOptions.Multiline).Value.TrimEnd('\r');
         Assert.AreEqual(100, result.Split(' ').Length - 2, "Every saved message matches. Got: " + result);
         Assert.That(elapsed, Is.LessThan(SearchBound),
            "A SEARCH of 100,000 \"$\" over 100 messages took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("HEADER.FIELDS answers each name once, in the order asked, with the first field's value, as it did.")]
      public void HeaderFieldsAreAnsweredAsTheyWere()
      {
         var socket = LogOn(CreateAccount("headerfields@example.test"));

         string message = "From: a@example.test\r\nTo: c@example.test\r\nSubject: s\r\nX-A: 1\r\nX-A: 2\r\n\r\nBody.\r\n";
         socket.Send("P1 APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\n");
         StringAssert.Contains("P1 OK", ReadUntilLine(socket, "P1 ", 30));
         StringAssert.Contains("S1 OK", Command(socket, "S1", "SELECT INBOX", 30));

         // The request's spelling, its order, each name once, the first X-A.
         string fields = Command(socket, "A1", "FETCH 1 (BODY.PEEK[HEADER.FIELDS (x-a subject Missing X-A FROM)])", 30);
         StringAssert.Contains("x-a: 1\r\nsubject: s\r\nFROM: a@example.test\r\n\r\n", fields, fields);

         // Every field but those named, in the header's order and spelling.
         string not = Command(socket, "A2", "FETCH 1 (BODY.PEEK[HEADER.FIELDS.NOT (x-a SUBJECT)])", 30);
         StringAssert.Contains("From: a@example.test\r\nTo: c@example.test\r\n", not, not);
         StringAssert.DoesNotContain("X-A:", not, not);
         StringAssert.DoesNotContain("Subject:", not, not);
         StringAssert.Contains("\r\n\r\n)", not, not);

         socket.Disconnect();
      }

      [Test]
      [Description("FETCH 1:* of HEADER.FIELDS naming 30,000 fields over 50 messages of 10,001 fields each is answered in seconds.")]
      public void HeaderFieldsOfManyNamesOverManyMessagesIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("headernames@example.test"));

         var fields = new StringBuilder();
         for (int i = 1; i <= 10000; i++)
            fields.Append("X-F" + i + ": v\r\n");
         AppendAndSelect(socket, 50, fields.ToString());

         // The list was copied, and the header searched field by field for each
         // name in it, for every message: 30,000 names x 10,001 fields x 50
         // messages. The header is read once per message now, each field looked
         // up among the names.
         //
         // The cost is made by the fields rather than by more messages because
         // the answer grows with the names and the messages whatever the server
         // does: each message's FETCH line repeats the section asked for, list
         // and all (RFC 3501's section-spec), so these names over 1,000 messages
         // of one field were 199 MB to write, log and read - 13 seconds on the
         // slow Windows runner, where the same FETCH naming one field took two.
         // This answer is 10 MB.
         var names = new StringBuilder("Subject");
         for (int i = 1; i < 30000; i++)
            names.Append(" n" + i);

         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1:* (BODY.PEEK[HEADER.FIELDS (" + names + ")])\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         Assert.AreEqual(50, CountMatches(response, @"^Subject: message \d+\r$"), Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "A FETCH of 30,000 header names over 50 messages of 10,001 fields took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("A FETCH asking for the whole message 40 times is refused with LIMIT before it is read 40 times.")]
      public void FetchOfTheWholeMessageManyTimesIsRefused()
      {
         var socket = LogOn(CreateAccount("fetchcopies@example.test"));

         string message = "Subject: copies\r\n\r\n" + new string('z', 100000) + "\r\n";
         socket.Send("P1 APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\n");
         StringAssert.Contains("P1 OK", ReadUntilLine(socket, "P1 ", 30));
         StringAssert.Contains("S1 OK", Command(socket, "S1", "SELECT INBOX", 30));

         // What a client asks for: each section once or so.
         string some = Command(socket, "A1", "FETCH 1 (BODY.PEEK[] BODY.PEEK[TEXT] BODY.PEEK[HEADER] BINARY.PEEK[] RFC822.SIZE)", 30);
         StringAssert.Contains("A1 OK", some, Tail(some));

         // Forty copies: each was read, and all held, before any was sent.
         string copies = Command(socket, "A2", "FETCH 1 (" + RepeatJoined("BODY.PEEK[]", " ", 40) + ")", 30);
         StringAssert.Contains("A2 NO [LIMIT]", copies, Tail(copies));
         StringAssert.DoesNotContain("* 1 FETCH", copies, "Nothing of the message is sent before the refusal.");

         socket.Disconnect();
      }

      /// <summary>
      ///    Appends one message, whole, and selects INBOX.
      /// </summary>
      private static void AppendOneAndSelect(TcpConnection socket, string message)
      {
         socket.Send("P1 APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\n");
         string appended = ReadUntilLine(socket, "P1 ", 120);
         StringAssert.Contains("P1 OK", appended, "The APPEND failed: " + Tail(appended));

         string select = Command(socket, "S1", "SELECT INBOX", 30);
         StringAssert.Contains("S1 OK", select, "The SELECT failed: " + Tail(select));
      }

      [Test]
      [Description("BODY.PEEK[HEADER]<0.1> named 10,000 times over a message of 4 MB is refused with LIMIT in seconds.")]
      public void HeaderOfALargeMessageNamedOverAndOverIsRefusedInSeconds()
      {
         var socket = LogOn(CreateAccount("headercopies@example.test"));

         var body = new StringBuilder();
         for (int i = 0; i < 4000; i++)
            body.Append(new string('x', 998) + "\r\n");
         AppendOneAndSelect(socket, "Subject: large\r\n\r\n" + body);

         // What a client asks for.
         string some = Command(socket, "A1", "FETCH 1 (BODY.PEEK[HEADER] BODY.PEEK[TEXT]<0.100> RFC822.HEADER)", 30);
         StringAssert.Contains("A1 OK", some, Tail(some));
         StringAssert.Contains("Subject: large", some, Tail(some));

         // Each section read the message file whole for its one byte: 40 GB,
         // where what they held, a byte each, never came near the ceiling.
         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1 (" + RepeatJoined("BODY.PEEK[HEADER]<0.1>", " ", 10000) + ")\r\n", out elapsed);

         StringAssert.Contains("T1 NO [LIMIT]", response, Tail(response));
         StringAssert.DoesNotContain("* 1 FETCH", response, "Nothing of the message is sent before the refusal.");
         Assert.That(elapsed, Is.LessThan(Bound),
            "10,000 BODY.PEEK[HEADER]<0.1> over a message of 4 MB took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("HEADER.FIELDS and HEADER.FIELDS.NOT of a field of a megabyte, <0.1>, named 6,000 and 5,000 times, are refused with LIMIT in seconds.")]
      public void HeaderFieldsOfALargeFieldNamedOverAndOverAreRefusedInSeconds()
      {
         var socket = LogOn(CreateAccount("largefield@example.test"));

         AppendOneAndSelect(socket, "Subject: field\r\nX-Big: " + new string('y', 1000000) + "\r\n\r\nBody.\r\n");

         // What a client asks for.
         string some = Command(socket, "A1", "FETCH 1 (BODY.PEEK[HEADER.FIELDS (Subject X-Big)] BODY.PEEK[HEADER.FIELDS.NOT (X-Big)])", 30);
         StringAssert.Contains("A1 OK", some, Tail(some));
         StringAssert.Contains("X-Big: yyyy", some, Tail(some));

         // Each section made the megabyte field's line for its one byte: 6 GB
         // made, a byte each held; and HEADER.FIELDS.NOT the same.
         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1 (" + RepeatJoined("BODY.PEEK[HEADER.FIELDS (X-Big)]<0.1>", " ", 6000) + ")\r\n", out elapsed);

         StringAssert.Contains("T1 NO [LIMIT]", response, Tail(response));
         StringAssert.DoesNotContain("* 1 FETCH", response, "Nothing of the message is sent before the refusal.");
         Assert.That(elapsed, Is.LessThan(Bound),
            "6,000 HEADER.FIELDS (X-Big)<0.1> over a field of a megabyte took " + (long) elapsed.TotalMilliseconds + " ms.");

         response = Timed(socket, "T1 FETCH 1 (" + RepeatJoined("BODY.PEEK[HEADER.FIELDS.NOT (Subject)]<0.1>", " ", 5000) + ")\r\n", out elapsed);

         StringAssert.Contains("T1 NO [LIMIT]", response, Tail(response));
         StringAssert.DoesNotContain("* 1 FETCH", response, "Nothing of the message is sent before the refusal.");
         Assert.That(elapsed, Is.LessThan(Bound),
            "5,000 HEADER.FIELDS.NOT (Subject)<0.1> over a field of a megabyte took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("The MIME headers of the parts of a forwarded message of 8 MB, 1,500 of them, are answered in seconds.")]
      public void SectionsOfAForwardedMessageAreAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("forwarded@example.test"));

         var inner = new StringBuilder("Subject: inner\r\nContent-Type: multipart/mixed; boundary=\"i\"\r\n\r\n");
         for (int i = 1; i <= 40; i++)
         {
            inner.Append("--i\r\nX-N: " + i + "\r\n\r\n");
            for (int line = 0; line < 200; line++)
               inner.Append(new string('v', 998) + "\r\n");
         }
         inner.Append("--i--\r\n");

         AppendOneAndSelect(socket,
            "Subject: forwarded\r\nContent-Type: multipart/mixed; boundary=\"o\"\r\n\r\n" +
            "--o\r\nContent-Type: text/plain\r\n\r\nSee below.\r\n" +
            "--o\r\nContent-Type: message/rfc822\r\n\r\n" + inner + "\r\n" +
            "--o--\r\n");

         // Each section parsed the forwarded message afresh on its way to a part
         // in it: 1,500 parses of 8 MB. It is parsed once for the message's
         // sections now - and counted to their ceiling once, so these sections,
         // a client's own, are not refused. (Few sections of a large message:
         // each section's answer costs the slow Windows runner, with the suite's
         // debug log, about a millisecond whatever the server reads for it.)
         var items = new StringBuilder();
         for (int i = 0; i < 1500; i++)
            items.Append((i > 0 ? " " : "") + "BODY.PEEK[2." + (1 + i % 40) + ".MIME]");

         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1 (" + items + ")\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         Assert.AreEqual(1500, CountMatches(response, @"^X-N: \d+\r$"), Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "1,500 MIME sections of a forwarded message of 8 MB took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("The MIME header of each part of a message of 2,000 parts, in one FETCH, is answered in seconds.")]
      public void MimeHeaderOfEveryPartOfAManyPartMessageIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("manyparts@example.test"));

         var message = new StringBuilder("Subject: parts\r\nContent-Type: multipart/mixed; boundary=\"p\"\r\n\r\n");
         for (int i = 1; i <= 2000; i++)
            message.Append("--p\r\nX-N: " + i + "\r\n\r\nv\r\n");
         message.Append("--p--\r\n");
         AppendOneAndSelect(socket, message.ToString());

         // Each section listed the parts from the first to its own. A client's
         // own sections, which the ceiling of a message's sections leaves be:
         // were the list counted to it for each, the square of the parts, four
         // million, would pass it.
         var items = new StringBuilder();
         for (int i = 1; i <= 2000; i++)
            items.Append((i > 1 ? " " : "") + "BODY.PEEK[" + i + ".MIME]");

         TimeSpan elapsed;
         string response = Timed(socket, "T1 FETCH 1 (" + items + ")\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         Assert.AreEqual(2000, CountMatches(response, @"^X-N: \d+\r$"), Tail(response));
         Assert.That(elapsed, Is.LessThan(Bound),
            "The MIME header of each of 2,000 parts took " + (long) elapsed.TotalMilliseconds + " ms.");

         socket.Disconnect();
      }

      [Test]
      [Description("SEARCH TEXT with a literal of 20,001 over a message of a million \"a\" is answered in seconds (CWE-407).")]
      public void SearchTextOfLongLiteralIsAnsweredInSeconds()
      {
         var socket = LogOn(CreateAccount("searchlong@example.test"));

         // One body line of a million "a": every position begins the needle.
         string message = "Subject: long\r\n\r\n" + new string('a', 1000000) + "\r\n";
         socket.Send("P1 APPEND INBOX {" + message.Length + "+}\r\n" + message + "\r\n");
         string appended = ReadUntilLine(socket, "P1 ", 60);
         StringAssert.Contains("P1 OK", appended, Tail(appended));

         string select = Command(socket, "S1", "SELECT INBOX", 30);
         StringAssert.Contains("S1 OK", select, Tail(select));

         // Not there: "aa...ab", 20,001 long. Compared afresh at each position, a
         // million positions by 20,001 characters.
         string needle = new string('a', 20000) + "b";
         TimeSpan elapsed;
         string response = Timed(socket, "T1 SEARCH TEXT {" + needle.Length + "+}\r\n" + needle + "\r\n", out elapsed);

         StringAssert.Contains("T1 OK", response, Tail(response));
         StringAssert.DoesNotContain("* SEARCH 1", response, Tail(response));
         Assert.That(elapsed, Is.LessThan(SearchBound),
            "A SEARCH TEXT of 20,001 over a million took " + (long) elapsed.TotalMilliseconds + " ms.");

         // There: 20,000 "a", in a case of its own.
         string found = Command(socket, "A2", "SEARCH TEXT {20000+}\r\n" + new string('A', 20000), 60);
         StringAssert.Contains("* SEARCH 1", found, Tail(found));

         socket.Disconnect();
      }
   }
}
