// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Diagnostics;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    One FETCH from a signed-in account must not be able to hold an IMAP worker
   ///    thread - shared with SMTP and POP3 - for the better part of a minute, let
   ///    alone minutes.
   ///
   ///    Up to 6.3.5 two parsers of the FETCH command took time that grew with the
   ///    square of the command's size. IMAPFetchParser::ParseBODY_ ran
   ///    String::ReplaceNoCase("BODY.PEEK[", "BODY[") over the item, and ReplaceNoCase
   ///    copied and lower-cased the whole string once per occurrence. IMAPFetchParser::
   ///    ParseString_ cut each item off the front of the rest with Mid, copying the
   ///    rest every time. One command line, which the IMAP reader accepts up to a
   ///    quarter of a megabyte, reaches about 20,000 "BODY.PEEK[" or well over a
   ///    hundred thousand items; measured against a 6.3.5 server, each takes the
   ///    better part of a minute, and a few such commands from a few sessions stop the
   ///    IMAP, SMTP and POP3 ports answering anybody. (The larger sizes that the
   ///    standalone numbers in the commit message name - 40,000 occurrences, 200,000
   ///    items, and minutes each - are reached when the command goes on past a
   ///    literal onto further lines; the unit tester imap-parse-tests / ClassTester
   ///    drives the parsers to those sizes directly. This fixture stays inside one
   ///    command line, which is enough to tell the fix from the flaw end to end.)
   ///
   ///    ReplaceNoCase and FindNoCase now take one pass, and ParseString_ walks the
   ///    command by offsets, so each of these is answered in milliseconds. The bounds
   ///    - 2.5 s and 1.0 s, below - sit above the fixed server and below a 6.3.5 one.
   ///    Against a server built from 0689fab9c (batch42's tip before this fix) each
   ///    test fails: the command does not return inside the bound. A FETCH is not held
   ///    to one line - the text after each literal on its line joins the command - and
   ///    ClientTextCost holds one of four lines, and the shapes found beside these.
   /// </summary>
   [TestFixture]
   public class FetchParsingTime : TestFixtureBase
   {
      // The fix answers both of these in tens to a couple of hundred milliseconds
      // (the larger of them sends a quarter of a megabyte and parses it in one
      // pass). A 6.3.5 server on the same fast hardware takes several seconds for
      // the BODY.PEEK[ item and around a second and a half for the item list; on
      // slower hardware, much longer. The bounds sit well above the fixed server
      // and well below the flawed one. Against a server built from 0689fab9c
      // (batch42's tip before this fix) both tests fail on the elapsed time; the
      // unit tester imap-parse-tests / ClassTester drives the same two parsers to
      // the larger sizes the commit message names, where 6.3.5 takes 30 s and more.
      private static readonly TimeSpan BodyPeekBound = TimeSpan.FromSeconds(2.5);
      private static readonly TimeSpan ItemListBound = TimeSpan.FromSeconds(1.0);

      private hMailServer.Account DeliverOneMessage()
      {
         // A message must be in the mailbox, or FETCH resolves no target and never
         // parses the command (IMAPCommandRangeAction::DoForMails returns before
         // DoAction). One is enough; the command is parsed once, for the first.
         var account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "fetchtime@example.test", "test");
         SmtpClientSimulator.StaticSend(account.Address, account.Address, "Fetch timing", "Body");
         ImapClientSimulator.AssertMessageCount(account.Address, "test", "Inbox", 1);
         return account;
      }

      /// <summary>
      ///    Sends "A03 FETCH 1 " + the given item text on one line and returns the time
      ///    from the command to the tagged response. The item text is kept under the
      ///    quarter-megabyte an IMAP command line may carry.
      /// </summary>
      private static TimeSpan TimeFetch(hMailServer.Account account, string items)
      {
         ClassicAssert.Less(("A03 FETCH 1 " + items + "\r\n").Length, 250000,
            "The command line must fit in one IMAP read (a quarter of a megabyte).");

         var socket = new TcpConnection();
         try
         {
            ClassicAssert.IsTrue(socket.Connect(TestPorts.Imap),
               "Could not connect to the IMAP server on port " + TestPorts.Imap + ".");
            socket.ReadUntil("* OK");

            socket.Send("A01 LOGIN " + account.Address + " test\r\n");
            socket.ReadUntil("A01 OK");

            socket.Send("A02 SELECT INBOX\r\n");
            socket.ReadUntil("A02 OK");

            var stopwatch = Stopwatch.StartNew();
            socket.Send("A03 FETCH 1 " + items + "\r\n");

            // Longer than the bound, so a 6.3.5 server returns here (after the better
            // part of a minute) and the assertion fails on the elapsed time rather
            // than by timing out - a clearer negative control.
            var response = socket.ReadUntil("A03 ", TimeSpan.FromSeconds(90));
            stopwatch.Stop();

            StringAssert.Contains("A03 ", response, "No tagged response to the FETCH. Got: " + response);

            TestContext.Progress.WriteLine("FetchParsingTime: the FETCH returned in " +
               (long) stopwatch.Elapsed.TotalMilliseconds + " ms.");

            return stopwatch.Elapsed;
         }
         finally
         {
            socket.Disconnect();
         }
      }

      [Test]
      [Description("A FETCH item holding 22,000 \"BODY.PEEK[\" is answered within seconds (CWE-407).")]
      public void FetchOfManyBodyPeekOccurrencesIsAnsweredInSeconds()
      {
         var account = DeliverOneMessage();

         // One item made of 22,000 "BODY.PEEK[" and as many "]": the string
         // ParseBODY_ runs ReplaceNoCase over, with 22,000 occurrences in it. About
         // 242 KB - as much as one IMAP command line carries.
         var builder = new StringBuilder();
         for (var i = 0; i < 22000; i++)
            builder.Append("BODY.PEEK[");
         for (var i = 0; i < 22000; i++)
            builder.Append(']');

         var elapsed = TimeFetch(account, builder.ToString());

         Assert.That(elapsed, Is.LessThan(BodyPeekBound),
            "A FETCH of 22,000 \"BODY.PEEK[\" took " + (long) elapsed.TotalMilliseconds +
            " ms; a 6.3.5 server takes several seconds and grows with the square (CWE-407).");
      }

      [Test]
      [Description("A FETCH of 120,000 items is answered in well under a second (CWE-407).")]
      public void FetchOfManyItemsIsAnsweredInSeconds()
      {
         var account = DeliverOneMessage();

         // 120,000 single-character items, space separated: what ParseString_ cuts
         // off the front of the rest, one at a time. About 240 KB - as much as one
         // IMAP command line carries. "a" is not a FETCH item; it parses and is
         // classified unknown, which is all this exercises (the cost is in the split,
         // before classification).
         var builder = new StringBuilder();
         for (var i = 0; i < 120000; i++)
         {
            if (i > 0)
               builder.Append(' ');
            builder.Append('a');
         }

         var elapsed = TimeFetch(account, builder.ToString());

         Assert.That(elapsed, Is.LessThan(ItemListBound),
            "A FETCH of 120,000 items took " + (long) elapsed.TotalMilliseconds +
            " ms; a 6.3.5 server takes over a second and grows with the square (CWE-407).");
      }
   }
}
