// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.Sieve
{
   /// <summary>
   ///    editheader (RFC 5293): addheader and deleteheader, asserted on the
   ///    DELIVERED MESSAGE'S BYTES read back over POP3 - the file the user's client
   ///    will download - never on what the evaluator reported. The edits are
   ///    applied by the delivery path rewriting the stored file, which is exactly
   ///    the step that can silently not happen while every summary looks right.
   /// </summary>
   [TestFixture]
   public class SieveEditheaderDelivery : TestFixtureBase
   {
      private const string Password = "secret";

      private static int accountSequence_;

      // Unique per account created by this fixture, however the tests are ordered or parallelised.
      private static int NextAccountSequence()
      {
         return Interlocked.Increment(ref accountSequence_);
      }

      private static void SetScript(Account account, string script)
      {
         account.GetType().InvokeMember(
            "SieveScript", BindingFlags.SetProperty, null, account, new object[] { script });
      }

      private Account NewRecipient()
      {
         int sequence = NextAccountSequence();
         return SingletonProvider<TestSetup>.Instance.AddAccount(
            _domain, "sieve-edith-" + sequence + "@example.test", Password);
      }

      /// <summary>
      ///    Delivers one message under the given script and returns the delivered
      ///    text as a POP3 client retrieves it.
      /// </summary>
      private string DeliverAndFetch(string script, string extraHeaders = "")
      {
         Account recipient = NewRecipient();
         SetScript(recipient, script);
         return SendAndFetch(recipient, extraHeaders, "Body text.\r\n");
      }

      private static string SendAndFetch(Account recipient, string extraHeaders, string body)
      {
         string raw =
            "From: sieve-edith-sender@example.test\r\n" +
            "To: " + recipient.Address + "\r\n" +
            "Subject: Editheader test\r\n" +
            extraHeaders +
            "\r\n" +
            body;

         var smtp = new SmtpClientSimulator();
         smtp.SendRaw("sieve-edith-sender@example.test", recipient.Address, raw);

         return Pop3ClientSimulator.AssertGetFirstMessageText(recipient.Address, Password);
      }

      /// <summary>
      ///    The lines of a delivered message's header that begin a field of this
      ///    name, as a mail client finds them: up to the first empty line.
      /// </summary>
      private static List<string> FieldLines(string message, string name)
      {
         var lines = new List<string>();
         foreach (string line in message.Replace("\r\n", "\n").Split('\n'))
         {
            if (line.Length == 0)
               break;

            if (line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase))
               lines.Add(line);
         }

         return lines;
      }

      [Test]
      [Description("addheader inserts the field, at the top of the header by default.")]
      public void AddheaderInsertsAtTheTop()
      {
         string message = DeliverAndFetch(
            "require \"editheader\";\r\n" +
            "addheader \"X-Sieve-Filtered\" \"yes\";\r\n");

         StringAssert.Contains("X-Sieve-Filtered: yes", message,
            "The added header is not in the delivered message.");

         int added = message.IndexOf("X-Sieve-Filtered:");
         int from = message.IndexOf("From:");
         Assert.Less(added, from,
            "addheader without :last must insert at the TOP of the header (RFC 5293 4), above the existing fields.");
      }

      [Test]
      [Description("addheader :last appends below the existing fields instead.")]
      public void AddheaderLastAppendsAtTheBottom()
      {
         string message = DeliverAndFetch(
            "require \"editheader\";\r\n" +
            "addheader :last \"X-Sieve-Filtered\" \"yes\";\r\n");

         int added = message.IndexOf("X-Sieve-Filtered:");
         int subject = message.IndexOf("Subject:");

         Assert.Greater(added, subject,
            "addheader :last must append below the existing fields.");
      }

      [Test]
      [Description("deleteheader removes every instance of the named field.")]
      public void DeleteheaderRemovesTheNamedField()
      {
         string message = DeliverAndFetch(
            "require \"editheader\";\r\n" +
            "deleteheader \"X-Advertisement\";\r\n",
            "X-Advertisement: one\r\nX-Advertisement: two\r\n");

         StringAssert.DoesNotContain("X-Advertisement", message,
            "deleteheader left an instance of the field in the delivered message.");
      }

      /// <summary>
      ///    The negative control: a delete aimed at a value that does not match
      ///    must remove nothing. An implementation that deletes by name alone and
      ///    ignores the patterns passes the removal test and fails here.
      /// </summary>
      [Test]
      [Description("deleteheader with a non-matching value pattern removes nothing - the control the rest depend on.")]
      public void DeleteheaderWithNonMatchingPatternRemovesNothing()
      {
         string message = DeliverAndFetch(
            "require \"editheader\";\r\n" +
            "deleteheader :is \"X-Advertisement\" \"absent-value\";\r\n",
            "X-Advertisement: one\r\n");

         StringAssert.Contains("X-Advertisement: one", message,
            "A value-filtered deleteheader removed a field whose value did not match.");
      }

      [Test]
      [Description("deleteheader :contains removes only the matching instances.")]
      public void DeleteheaderWithPatternIsSelective()
      {
         string message = DeliverAndFetch(
            "require \"editheader\";\r\n" +
            "deleteheader :contains \"X-Advertisement\" \"spam\";\r\n",
            "X-Advertisement: pure spam offer\r\nX-Advertisement: legitimate\r\n");

         StringAssert.DoesNotContain("pure spam offer", message,
            "The matching instance survived a value-filtered deleteheader.");
         StringAssert.Contains("X-Advertisement: legitimate", message,
            "The NON-matching instance was removed - the pattern is not being consulted per instance.");
      }

      [Test]
      [Description("deleteheader :index 2 removes only the second instance.")]
      public void DeleteheaderIndexSelectsOneInstance()
      {
         string message = DeliverAndFetch(
            "require [\"editheader\", \"index\"];\r\n" +
            "deleteheader :index 2 \"X-Route-Hop\";\r\n",
            "X-Route-Hop: first\r\nX-Route-Hop: second\r\n");

         StringAssert.Contains("X-Route-Hop: first", message,
            "':index 2' removed the first instance.");
         StringAssert.DoesNotContain("X-Route-Hop: second", message,
            "':index 2' did not remove the second instance.");
      }

      /// <summary>
      ///    The protection: the trace of how a message travelled is not a script's
      ///    to edit, so a script trying is refused AT UPLOAD - CHECKSCRIPT-style
      ///    validation through the COM syntax checker, since a stored script never
      ///    gets the chance.
      /// </summary>
      [Test]
      [Description("A script editing Received or Return-Path is refused at upload.")]
      public void TraceHeadersAreProtected()
      {
         object utilities = _application.Utilities;

         foreach (string script in new[]
         {
            "require \"editheader\";\r\ndeleteheader \"Received\";",
            "require \"editheader\";\r\naddheader \"Received\" \"forged\";",
            "require \"editheader\";\r\ndeleteheader \"Return-Path\";"
         })
         {
            string error = (string) utilities.GetType().InvokeMember(
               "CheckSieveSyntax", BindingFlags.InvokeMethod, null, utilities, new object[] { script });

            Assert.IsNotEmpty(error,
               "A script editing a trace header was accepted at upload: " + script);
         }
      }

      // A store refused, through reflection or not, for the reason the check at upload
      // gives an edit of the loop count, and for no other.
      private static bool RefusedForTheLoopCount(Exception refused)
      {
         Exception cause = refused is TargetInvocationException && refused.InnerException != null ? refused.InnerException : refused;
         return cause is COMException && cause.Message.Contains("mail loop");
      }

      private string CheckSyntax(string script)
      {
         object utilities = _application.Utilities;
         return (string) utilities.GetType().InvokeMember(
            "CheckSieveSyntax", BindingFlags.InvokeMethod, null, utilities, new object[] { script });
      }

      /// <summary>
      ///    X-hMailServer-LoopCount counts the forwards, redirects and replies a
      ///    message has been through, and the rule loop limit (RuleLoopLimit) ends a
      ///    mail loop on it. A script that deleted it, or added a 0 above it, ahead of
      ///    a redirect gave every redirected copy the whole limit again, so that the
      ///    account and an ordinary forwarder looped between them without bound. Such
      ///    a script is refused at upload, the name matched in any case, and the
      ///    refusal names the header and says why.
      ///
      ///    Against the unfixed server every script below is accepted.
      /// </summary>
      [Test]
      [Description("A script editing X-hMailServer-LoopCount is refused at upload, and the refusal says why.")]
      public void TheLoopCountIsProtected()
      {
         foreach (string name in new[] { "X-hMailServer-LoopCount", "x-hmailserver-loopcount", "X-HMAILSERVER-LOOPCOUNT" })
         {
            foreach (string script in new[]
            {
               "require \"editheader\";\r\ndeleteheader \"" + name + "\";",
               "require \"editheader\";\r\naddheader \"" + name + "\" \"0\";",
               "require \"editheader\";\r\naddheader :last \"" + name + "\" \"0\";",
               "require [\"editheader\", \"index\"];\r\ndeleteheader :index 1 \"" + name + "\";"
            })
            {
               string error = CheckSyntax(script);

               StringAssert.Contains("Line 2:", error,
                  "A script editing the loop count was accepted at upload: " + script);
               StringAssert.Contains(name, error, "The refusal does not name the header: " + error);
               StringAssert.Contains("mail loop", error, "The refusal does not say why: " + error);
            }
         }

         // The control: a header of a name beside it may be edited.
         Assert.IsEmpty(CheckSyntax("require \"editheader\";\r\ndeleteheader \"X-hMailServer-LoopCounter\";"),
            "An edit of a header that is not the loop count was refused.");
      }

      /// <summary>
      ///    A name a script's variables make is a legal name to the check at upload -
      ///    it sees "${count}" - so the edit is refused again where it is made, of the
      ///    name as the variables expanded it: the delivered message keeps the count
      ///    it came with, the script's other edit is made, and the application log
      ///    says which edit was not made and why.
      ///
      ///    Against the unfixed server the delivered message carries
      ///    X-hMailServer-LoopCount: 0 and no 3.
      /// </summary>
      [Test]
      [Description("An edit of X-hMailServer-LoopCount named by the script's variables is not made at delivery, and is logged; the script's other edit is made.")]
      public void AnEditOfTheLoopCountIsNotMadeAtDelivery()
      {
         Account recipient = NewRecipient();
         SetScript(recipient,
            "require [\"editheader\", \"variables\"];\r\n" +
            "set \"count\" \"x-HMAILSERVER-loopcount\";\r\n" +
            "addheader \"X-Sieve-Ran\" \"yes\";\r\n" +
            "deleteheader \"${count}\";\r\n" +
            "addheader \"${count}\" \"0\";\r\n");

         string message = SendAndFetch(recipient, "X-hMailServer-LoopCount: 3\r\n", "Body text.\r\n");

         StringAssert.Contains("X-Sieve-Ran: yes", message,
            "The script's other edit was not made, so the script did not run and the case proves nothing.");
         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 3" }, FieldLines(message, "X-hMailServer-LoopCount"),
            "The loop count the message came with was edited. The message was:\r\n" + message);

         Assert.IsTrue(LogHandler.DefaultLogContains("a header edit the Sieve script for " + recipient.Address +
               " asked for was not made: deleteheader: the x-HMAILSERVER-loopcount header counts"),
            "The deleteheader that was not made is not in the application log.");
         Assert.IsTrue(LogHandler.DefaultLogContains("a header edit the Sieve script for " + recipient.Address +
               " asked for was not made: addheader: the x-HMAILSERVER-loopcount header counts"),
            "The addheader that was not made is not in the application log.");
      }

      /// <summary>
      ///    A script stored before the check at upload refused this edit still runs:
      ///    a stored script that no longer passed the check would otherwise filter
      ///    nothing at all. Its edit of the count is not made, and is logged, and the
      ///    rest of it does what it did.
      ///
      ///    COM's SieveScript property writes the script as it is given, as a script
      ///    stored before the check was. The REST API's filter route, which is how the
      ///    Linux suite stores a script, checks it as PUTSCRIPT does and refuses this
      ///    one for the reason TheLoopCountIsProtected asserts, and there nothing can
      ///    be stored that this case needs: it is skipped, with that said.
      ///
      ///    Against the unfixed server the delivered message carries no loop count.
      /// </summary>
      [Test]
      [Description("A stored script that deletes X-hMailServer-LoopCount runs, without that edit, which is logged.")]
      public void AStoredScriptRunsWithoutItsEditOfTheLoopCount()
      {
         Account recipient = NewRecipient();

         try
         {
            SetScript(recipient,
               "require \"editheader\";\r\n" +
               "addheader \"X-Sieve-Ran\" \"yes\";\r\n" +
               "deleteheader \"X-hMailServer-LoopCount\";\r\n");
         }
         catch (Exception refused) when (RefusedForTheLoopCount(refused))
         {
            Assert.Ignore("This server checks a script when it is stored and refused this one, so no script of the kind " +
               "this case runs can be stored here: " + refused.GetBaseException().Message);
         }

         string message = SendAndFetch(recipient, "X-hMailServer-LoopCount: 3\r\n", "Body text.\r\n");

         StringAssert.Contains("X-Sieve-Ran: yes", message,
            "The stored script did not run: its other edit was not made.");
         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 3" }, FieldLines(message, "X-hMailServer-LoopCount"),
            "The loop count the message came with was deleted. The message was:\r\n" + message);

         Assert.IsTrue(LogHandler.DefaultLogContains("a header edit the Sieve script for " + recipient.Address +
               " asked for was not made: deleteheader: the X-hMailServer-LoopCount header counts"),
            "The deleteheader that was not made is not in the application log.");
      }

      /// <summary>
      ///    The writer wrote a line break in a value as it stood, so a value of "a",
      ///    a line break and "X-hMailServer-LoopCount: 0" wrote a second field - the
      ///    count, reset, under a name no check had seen - and a value a body test
      ///    captured is the sender's to write. A line break that does not fold the
      ///    line is now written as a space: the count stays the one the message came
      ///    with, the added fields are one each, and the header is not cut short by
      ///    a value that ended in a line break.
      ///
      ///    Against the unfixed server the delivered header holds
      ///    X-hMailServer-LoopCount: 0 as a field of its own.
      /// </summary>
      [Test]
      [Description("A value with a line break, the script's or a sender's from the body, is written as one field and writes no loop count.")]
      public void AValueWithALineBreakIsOneField()
      {
         Account recipient = NewRecipient();
         SetScript(recipient,
            "require [\"editheader\", \"variables\", \"body\"];\r\n" +
            "addheader \"X-Note\" \"a\r\nX-hMailServer-LoopCount: 0\";\r\n" +
            "if body :raw :matches \"*\" { addheader :last \"X-Body\" \"${1}\"; }\r\n");

         string message = SendAndFetch(recipient, "X-hMailServer-LoopCount: 3\r\n",
            "hello\r\nX-hMailServer-LoopCount: 0\r\n");

         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 3" }, FieldLines(message, "X-hMailServer-LoopCount"),
            "A value with a line break wrote a field of its own. The message was:\r\n" + message);
         CollectionAssert.AreEqual(new[] { "X-Note: a  X-hMailServer-LoopCount: 0" }, FieldLines(message, "X-Note"),
            "The script's value is not one field. The message was:\r\n" + message);
         Assert.AreEqual(1, FieldLines(message, "X-Body").Count,
            "The value from the body is not one field. The message was:\r\n" + message);
         Assert.AreEqual(1, FieldLines(message, "Subject").Count,
            "The header was cut short. The message was:\r\n" + message);
      }

      /// <summary>
      ///    The loop itself. Two accounts whose scripts each reset the count and
      ///    redirect to the other: against the unfixed server each redirected copy
      ///    carried 1 and the message went round for ever, and the queue never
      ///    emptied. With the edits not made the count goes 0, 1, 2 and so on, and at
      ///    the rule loop limit the redirect is refused and the message is kept by the
      ///    account it reached - the second, at 5.
      /// </summary>
      [Test]
      [Description("Two accounts whose scripts reset X-hMailServer-LoopCount and redirect to each other stop at the rule loop limit.")]
      public void ARedirectLoopThatResetsTheCountEnds()
      {
         // Stated rather than assumed, as SieveNotifyDelivery does.
         _settings.RuleLoopLimit = 5;

         Account first = NewRecipient();
         Account second = NewRecipient();

         string Resetting(Account target)
         {
            return
               "require [\"editheader\", \"variables\"];\r\n" +
               "set \"count\" \"X-hMailServer-LoopCount\";\r\n" +
               "deleteheader \"${count}\";\r\n" +
               "addheader \"${count}\" \"0\";\r\n" +
               "redirect \"" + target.Address + "\";\r\n";
         }

         SetScript(first, Resetting(second));
         SetScript(second, Resetting(first));

         SmtpClientSimulator.StaticSend("sieve-edith-sender@example.test", first.Address, "Loop", "Body.");
         CustomAsserts.AssertRecipientsInDeliveryQueue(0);

         // The first had it at 0, 2 and 4 and redirected each; the second at 1, 3 and
         // 5, and kept it at 5.
         CustomAsserts.AssertFolderMessageCount(first.IMAPFolders.get_ItemByName("INBOX"), 0);
         CustomAsserts.AssertFolderMessageCount(second.IMAPFolders.get_ItemByName("INBOX"), 1);

         string kept = Pop3ClientSimulator.AssertGetFirstMessageText(second.Address, Password);
         CollectionAssert.AreEqual(new[] { "X-hMailServer-LoopCount: 5" }, FieldLines(kept, "X-hMailServer-LoopCount"),
            "The message kept is not the one at the limit. It was:\r\n" + kept);

         // The refused redirect is reported, as at the limit; this reads and clears the log.
         CustomAsserts.AssertReportedError("Maximum loop count reached");
      }
   }
}
