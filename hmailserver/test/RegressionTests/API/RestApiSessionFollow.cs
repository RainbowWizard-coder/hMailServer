// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    Follow one connection live: GET /api/v1/logs/sessions lists the sessions
   ///    the server has open and the last ones that ended, and
   ///    GET /api/v1/logs/sessions/{id} answers the log lines of one of them after
   ///    a cursor, waiting for the next one when there is none yet.
   ///
   ///    What is pinned here is the thing the row is for: that the lines a follower
   ///    sees are the session's own, arrive after the cursor and not before it, and
   ///    arrive when they are written rather than when a timer runs out. The IMAP
   ///    session the fixture opens is the one it follows, so the commands it sends
   ///    are the lines it then expects, by their tags.
   /// </summary>
   [TestFixture]
   public class RestApiSessionFollow : TestFixtureBase
   {
      // From the 9661-9665 range reserved for this work; the suite's own port
      // where RestListener finds one already on.
      private static int RestPort = 9661;

      private const string AdminPassword = "testar";
      private const string Password = "Follow-Passw0rd!";

      private bool _loggingWasEnabled;
      private bool _imapWasLogged;
      private bool _applicationWasLogged;

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         // The follow shows what the log mask lets through, so the IMAP
         // conversation is logged for the length of the fixture and put back
         // as it was found. The application log goes on with it: it is where
         // the lines that carry no session column and quote a name somebody
         // chose are written, which is what the last test here is about.
         _loggingWasEnabled = _settings.Logging.Enabled;
         _imapWasLogged = _settings.Logging.LogIMAP;
         _applicationWasLogged = _settings.Logging.LogApplication;
         _settings.Logging.Enabled = true;
         _settings.Logging.LogIMAP = true;
         _settings.Logging.LogApplication = true;

         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();
         Assert.AreEqual(200, Admin("GET", "/api/v1/status", null).status, "REST API did not answer /api/v1/status.");
      }

      [TearDown]
      public void StopRestApi()
      {
         try
         {
            _settings.Logging.LogApplication = _applicationWasLogged;
            _settings.Logging.LogIMAP = _imapWasLogged;
            _settings.Logging.Enabled = _loggingWasEnabled;
         }
         finally
         {
            RestListener.Stop();
            _application.Reinitialize();
         }
      }

      [Test]
      [Description("An IMAP session is listed as open with its account once it signs in; a follow from the start carries its LOGIN lines; a follow from the cursor carries the NOOP sent next, nothing from before the cursor and nothing from a second session running beside it; after the logout the session is listed as ended and the follow says so; an unknown id is 404 and no credential is 401.")]
      public void AnImapSessionIsListedAndFollowedLive()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "follow@test.com", Password);
         Account other = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "follow-other@test.com", Password);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, Password), "The IMAP logon failed.");

         // A second conversation running at the same time, so that "the lines
         // of one session" means something: its commands are logged into the
         // same ring, with a different session id, and must never appear in the
         // first session's follow.
         var beside = new ImapClientSimulator();
         Assert.IsTrue(beside.ConnectAndLogon(other.Address, Password), "The second IMAP logon failed.");

         long id;
         try
         {
            id = WaitForSession(account.Address, true);
            long besideId = WaitForSession(other.Address, true);
            Assert.AreNotEqual(id, besideId, "The two IMAP sessions were listed under one id.");

            // The whole of the session so far, from the first line held.
            Dictionary<string, object> first = Follow(id, 0, 0);
            List<string> lines = LinesOf(first);

            Assert.IsTrue(MiniJson.Flag(first, "known"), "The session was not known: " + Describe(first));
            Assert.IsTrue(MiniJson.Flag(first, "open"), "The session was not open: " + Describe(first));
            Assert.IsTrue(MiniJson.Flag(first, "logged"), "IMAP logging was reported off: " + Describe(first));
            Assert.AreEqual("IMAP", MiniJson.Text(first, "protocol"));
            Assert.AreEqual(account.Address, MiniJson.Text(first, "account"));
            Assert.IsTrue(lines.Exists(line => line.Contains("LOGIN")), "The follow from the start did not carry the LOGIN: " + string.Join(" | ", lines));
            Assert.IsFalse(lines.Exists(line => line.Contains(Password)), "The password reached the follow: " + string.Join(" | ", lines));
            Assert.IsFalse(lines.Exists(line => line.Contains(other.Address)), "The other session's sign-in was filed under this one: " + string.Join(" | ", lines));

            long cursor = MiniJson.Number(first, "cursor");
            Assert.Greater(cursor, 0, "No cursor was answered: " + Describe(first));

            // A command sent now is what a follow from the cursor sees - and the
            // LOGIN from before the cursor is not seen again. The other session's
            // command goes first, so that by the time this one's answer arrives
            // the ring certainly holds both.
            beside.SendSingleCommand("B7 NOOP");
            imap.SendSingleCommand("A7 NOOP");

            Dictionary<string, object> second = FollowUntil(id, cursor, "A7 NOOP");
            List<string> later = LinesOf(second);

            Assert.IsTrue(later.Exists(line => line.Contains("A7 NOOP")), "The follow from the cursor did not carry the NOOP: " + string.Join(" | ", later));
            Assert.IsTrue(later.Exists(line => line.Contains("A7 OK")), "The follow from the cursor did not carry the server's answer: " + string.Join(" | ", later));
            Assert.IsFalse(later.Exists(line => line.Contains("LOGIN")), "A follow from the cursor carried a line from before it: " + string.Join(" | ", later));
            Assert.IsFalse(later.Exists(line => line.Contains("B7 NOOP")), "Another session's line was filed under this one: " + string.Join(" | ", later));
            Assert.Greater(MiniJson.Number(second, "cursor"), cursor, "The cursor did not move: " + Describe(second));
            Assert.IsFalse(MiniJson.Flag(second, "dropped"), "The follow reported dropped lines: " + Describe(second));

            // Every line is either this session's conversation, which carries the
            // id in its own column, or one of the debug lines that names it in
            // its text - never a line of neither, whatever the log mask is.
            foreach (string line in later)
               Assert.IsTrue(line.StartsWith("IMAPD ") || line.Contains(id.ToString()),
                  "A line that is neither this session's conversation nor one naming it was filed under it: " + line);
         }
         finally
         {
            beside.Logout();
            beside.Disconnect();
            imap.Logout();
            imap.Disconnect();
         }

         // Ended, and listed as such; the follow of an ended session answers
         // what it holds and says the session is over.
         long endedId = WaitForSession(account.Address, false);
         Assert.AreEqual(id, endedId, "The ended session is not the one that was open.");

         Dictionary<string, object> after = Follow(id, 0, 0);
         Assert.IsTrue(MiniJson.Flag(after, "known"), Describe(after));
         Assert.IsFalse(MiniJson.Flag(after, "open"), "The session was still open after the logout: " + Describe(after));
         Assert.IsTrue(LinesOf(after).Exists(line => line.Contains("LOGOUT")), "The ended session's follow did not carry the LOGOUT: " + string.Join(" | ", LinesOf(after)));

         // A session nobody has heard of, and a request with no credential.
         Assert.AreEqual(404, Admin("GET", "/api/v1/logs/sessions/2000000000", null).status);
         Assert.AreEqual(400, Admin("GET", "/api/v1/logs/sessions/" + id + "?after=abc", null).status);
         Assert.AreEqual(401, RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/logs/sessions", null, null).status);
         Assert.AreEqual(401, RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/logs/sessions/" + id, null, null).status);
      }

      [Test]
      [Description("A follow with a wait is answered when the session's next line is logged, not when the wait runs out: a request for the lines after the cursor with wait=10 returns within a few seconds of a NOOP sent a second after it was made, carries the NOOP, and says it waited.")]
      public void AFollowWaitsForTheNextLineRatherThanPolling()
      {
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "waiter@test.com", Password);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(account.Address, Password), "The IMAP logon failed.");

         try
         {
            long id = WaitForSession(account.Address, true);

            Dictionary<string, object> first = Follow(id, 0, 0);
            long cursor = MiniJson.Number(first, "cursor");

            (int status, string body) waiting = (0, "");
            var stopwatch = Stopwatch.StartNew();
            var follower = new Thread(() =>
            {
               waiting = Admin("GET", "/api/v1/logs/sessions/" + id + "?after=" + cursor + "&wait=10", null);
            });
            follower.Start();

            // One follow waits at a time. A waiting follow holds one of the
            // four worker threads the REST listener has, and those four are
            // not the Deck's own - the same handler answers the webmail of
            // every mailbox on this server - so a second waiter would be half
            // the pool held by a page somebody left open, under nothing
            // stronger than view_logs. A second follow is therefore answered
            // at once and told that it did not wait; were the budget two it
            // would sit here for its whole ten seconds, because nothing is
            // logged for this session until the NOOP below.
            Thread.Sleep(500);
            var beside = Stopwatch.StartNew();
            (int status, string body) second = Admin("GET", "/api/v1/logs/sessions/" + id + "?after=" + cursor + "&wait=10", null);
            beside.Stop();
            Assert.AreEqual(200, second.status, second.body);
            Assert.IsFalse(MiniJson.Flag(MiniJson.Object(second.body), "waited"),
               "A second follow waited while one was already waiting, so the waiter budget is not one: " + second.body);
            Assert.Less(beside.ElapsedMilliseconds, 5000L,
               "The second follow was held rather than answered at once: " + second.body);

            Thread.Sleep(500);
            imap.SendSingleCommand("A8 NOOP");

            Assert.IsTrue(follower.Join(20000), "The waiting follow never answered.");
            stopwatch.Stop();

            Assert.AreEqual(200, waiting.status, waiting.body);
            Dictionary<string, object> answer = MiniJson.Object(waiting.body);
            Assert.IsTrue(MiniJson.Flag(answer, "waited"), "The follow did not wait: " + waiting.body);
            Assert.IsTrue(LinesOf(answer).Exists(line => line.Contains("A8 NOOP")), "The waiting follow did not carry the NOOP: " + waiting.body);
            Assert.Less(stopwatch.ElapsedMilliseconds, 8000L, "The follow waited out its whole wait rather than waking on the line.");
         }
         finally
         {
            imap.Logout();
            imap.Disconnect();
         }
      }

      [Test]
      [Description("A line with no session column is filed under the session its text names only when the number follows a space: an application line quoting a name of somebody's choosing that reads \"session1999123\" is filed under nothing, so the follow of that number is 404, while the same line with a space in it is filed, as the server's own DEBUG and TCPIP lines are.")]
      public void TextNamesASessionOnlyWhenTheNumberFollowsASpace()
      {
         // An IP range's name reaches the application log verbatim - "RestApi:
         // IP range '<name>' created." - on a line that carries no session
         // column, so it stands here for every log line that quotes something
         // a remote party chose: a request target, an envelope sender, a user
         // name. Two ranges, alike but for one space in the name.
         const long forged = 1999123;
         const long named = 1999124;

         long withoutSpace = 0;
         long withSpace = 0;

         try
         {
            // Made inside the try, so that a refusal of the second still takes
            // the first away again: a range this fixture left behind is a
            // change to the server's security configuration.
            withoutSpace = AddRange("follow-session" + forged + "-trap", "10.98.0.1", "10.98.0.2");
            withSpace = AddRange("follow-session " + named + "-trap", "10.98.0.3", "10.98.0.4");

            // Filed under nobody. The follow of the number the text names has
            // no session and no line, which is the 404 - rather than a
            // sentence of somebody else's writing in front of an
            // administrator following session 1999123.
            (int status, string body) forgedAnswer = Admin("GET", "/api/v1/logs/sessions/" + forged, null);
            Assert.AreEqual(404, forgedAnswer.status,
               "A log line quoting a stranger's text was filed under the session that text named: " + forgedAnswer.body);

            // And the same line with a space is filed, which is how the DEBUG
            // and TCPIP lines that carry no session column reach a follow at
            // all. FollowConnection.md says plainly that a line matched by its
            // text is an inference and that its text may be a stranger's.
            (int status, string body) namedAnswer = Admin("GET", "/api/v1/logs/sessions/" + named, null);
            Assert.AreEqual(200, namedAnswer.status, namedAnswer.body);
            Dictionary<string, object> answer = MiniJson.Object(namedAnswer.body);
            Assert.IsFalse(MiniJson.Flag(answer, "known"), "The server invented a session for a number only a log line's text named: " + namedAnswer.body);
            Assert.IsTrue(LinesOf(answer).Exists(line => line.Contains("follow-session " + named + "-trap")),
               "The line that names the session in its text did not reach the follow, so the line before it proved nothing: " + namedAnswer.body);
         }
         finally
         {
            DeleteRange(withoutSpace);
            DeleteRange(withSpace);
         }
      }

      // ----------------------------------------------------------------- helpers

      /// <summary>
      ///    An IP range whose name is written to the application log as it is
      ///    given. The addresses are in a range nothing in the suite connects
      ///    from, and the range is deleted again by the test that made it.
      /// </summary>
      private static long AddRange(string name, string lower, string upper)
      {
         (int status, string body) answer = Admin("POST", "/api/v1/ipranges",
            "{\"name\":\"" + name + "\",\"lower\":\"" + lower + "\",\"upper\":\"" + upper + "\"}");
         Assert.AreEqual(201, answer.status, answer.body);
         return MiniJson.Number(MiniJson.Object(answer.body), "id");
      }

      private static void DeleteRange(long id)
      {
         if (id > 0)
            Admin("DELETE", "/api/v1/ipranges/" + id, null);
      }

      private static (int status, string body) Admin(string method, string path, string body)
      {
         string credential = "Basic " + Convert.ToBase64String(Encoding.ASCII.GetBytes("Administrator:" + AdminPassword));
         return RestRequest.Send("127.0.0.1", RestPort, method, path, credential, body);
      }

      /// <summary>
      ///    The id of the IMAP session signed in as the address, once the listing
      ///    shows it open (or ended); the listing is asked again for up to ten
      ///    seconds, since the sign-in and the socket's close are recorded on the
      ///    server's threads.
      /// </summary>
      private static long WaitForSession(string address, bool open)
      {
         string last = "";
         for (int attempt = 0; attempt < 50; attempt++)
         {
            (int status, string body) answer = Admin("GET", "/api/v1/logs/sessions", null);
            Assert.AreEqual(200, answer.status, answer.body);
            last = answer.body;

            Dictionary<string, object> root = MiniJson.Object(answer.body);
            foreach (object item in MiniJson.List(root, "sessions"))
            {
               var session = item as Dictionary<string, object>;
               if (session == null)
                  continue;

               if (MiniJson.Text(session, "protocol") == "IMAP" && MiniJson.Text(session, "account") == address && MiniJson.Flag(session, "open") == open)
               {
                  Assert.AreEqual("127.0.0.1", MiniJson.Text(session, "peer"), answer.body);
                  Assert.IsFalse(MiniJson.Flag(session, "client"), answer.body);
                  Assert.IsFalse(string.IsNullOrEmpty(MiniJson.Text(session, "started")), answer.body);
                  Assert.AreEqual(!open, !string.IsNullOrEmpty(MiniJson.Text(session, "ended")), answer.body);
                  return MiniJson.Number(session, "id");
               }
            }

            Thread.Sleep(200);
         }

         Assert.Fail("No " + (open ? "open" : "ended") + " IMAP session for " + address + " was listed. Last answer: " + last);
         return 0;
      }

      private static Dictionary<string, object> Follow(long id, long after, int wait)
      {
         (int status, string body) answer = Admin("GET", "/api/v1/logs/sessions/" + id + "?after=" + after + "&wait=" + wait, null);
         Assert.AreEqual(200, answer.status, answer.body);
         return MiniJson.Object(answer.body);
      }

      /// <summary>
      ///    Follows from the cursor, waiting on the server, until an answer carries
      ///    the text - the lines of a command sent a moment ago are written by the
      ///    session's own thread, so the first answer may hold only the first of
      ///    them. Every answer's lines are gathered into the one returned.
      /// </summary>
      private static Dictionary<string, object> FollowUntil(long id, long after, string text)
      {
         var gathered = new List<object>();
         Dictionary<string, object> answer = null;
         long cursor = after;

         for (int attempt = 0; attempt < 10; attempt++)
         {
            answer = Follow(id, cursor, 5);
            gathered.AddRange(MiniJson.List(answer, "lines"));
            cursor = MiniJson.Number(answer, "cursor");

            answer["lines"] = gathered;
            if (LinesOf(answer).Exists(line => line.Contains(text)))
               return answer;
         }

         Assert.Fail("No line carrying " + text + " arrived: " + Describe(answer));
         return answer;
      }

      private static List<string> LinesOf(Dictionary<string, object> answer)
      {
         var lines = new List<string>();
         foreach (object item in MiniJson.List(answer, "lines"))
         {
            var line = item as Dictionary<string, object>;
            if (line != null)
               lines.Add(MiniJson.Text(line, "category") + " " + MiniJson.Text(line, "message"));
         }
         return lines;
      }

      private static string Describe(Dictionary<string, object> answer)
      {
         if (answer == null)
            return "(no answer)";

         var parts = new List<string>();
         foreach (KeyValuePair<string, object> pair in answer)
         {
            if (pair.Key == "lines")
               parts.Add("lines=" + string.Join(" | ", LinesOf(answer)));
            else
               parts.Add(pair.Key + "=" + pair.Value);
         }
         return string.Join(", ", parts);
      }
   }
}
