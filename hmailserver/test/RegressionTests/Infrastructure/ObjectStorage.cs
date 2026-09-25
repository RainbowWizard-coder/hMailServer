// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    The message store on an object store (docs/ObjectStorage.md): the server
   ///    run against MinIO (the S3 API, SigV4, path-style) and Azurite (Azure Blob
   ///    Storage, Shared Key), each over TLS verified against the lab's own CA.
   ///
   ///    The bench is Windows and the stores run in docker on the lab VM, so the
   ///    fixture is told where they are:
   ///
   ///       HMAILSERVER_OBJECTSTORE_LAB   the lab host, 192.168.11.154
   ///       HMAILSERVER_OBJECTSTORE_CA    the lab CA's PEM file, somewhere the
   ///                                     service can read it (C:\HMTest\lab-ca.crt)
   ///
   ///    and skips, saying which is missing, without them. The rest has the lab's
   ///    defaults (MinIO on 10240, hmslab-access / hmslab-secret-row111, bucket
   ///    hmslab; Azurite on 10242, devstoreaccount1 with the emulator's published
   ///    key, container hmslab) and can be named: HMAILSERVER_OBJECTSTORE_S3_PORT,
   ///    _S3_ACCESS, _S3_SECRET, _BUCKET, _AZURE_PORT, _AZURE_KEY.
   ///
   ///    The server reaches each store through a relay this fixture runs on the
   ///    loopback address (10244 for MinIO, 10245 for Azurite; the row's range is
   ///    10240-10249, and REST is on 10246). An outage is the relay stopped: the
   ///    server's connection is refused, exactly as it is when the container is
   ///    stopped. With HMAILSERVER_OBJECTSTORE_SSH set to an ssh target and its
   ///    options (-i C:\Users\chris\.ssh\hmail_ad chris@192.168.11.154), the
   ///    container itself is stopped and started instead.
   ///
   ///    What the server stored is looked at in the store directly, by a client of
   ///    this fixture's own - SigV4 and Shared Key written again here - so that a
   ///    signer that agreed only with itself would not pass. Every test works under
   ///    a key prefix of its own, removed in the teardown with every object under it.
   /// </summary>
   [TestFixture]
   public class ObjectStorage : TestFixtureBase
   {
      private const int RestPortPreferred = 10246;
      private const int S3RelayPort = 10244;
      private const int AzureRelayPort = 10245;
      private const string AdminPassword = "testar";
      private const string Password = "test";

      private const string EmulatorKey = "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==";

      private static int RestPort = RestPortPreferred;

      private string labHost_;
      private string caFile_;
      private X509Certificate2 labCa_;
      private int s3Port_;
      private int azurePort_;
      private string s3Access_;
      private string s3Secret_;
      private string bucket_;
      private string azureKey_;
      private string ssh_;

      private string prefix_;
      private Relay s3Relay_;
      private Relay azureRelay_;
      private LabStore s3_;
      private LabStore azure_;

      private readonly List<string> accounts_ = new List<string>();
      private readonly List<Action> putBack_ = new List<Action>();

      [SetUp]
      public void ReachTheLab()
      {
         labHost_ = Environment.GetEnvironmentVariable("HMAILSERVER_OBJECTSTORE_LAB");
         caFile_ = Environment.GetEnvironmentVariable("HMAILSERVER_OBJECTSTORE_CA");

         if (string.IsNullOrWhiteSpace(labHost_))
            Assert.Ignore("HMAILSERVER_OBJECTSTORE_LAB is not set: it names the host MinIO and Azurite run on (docs/ObjectStorage.md, 'The lab').");
         if (string.IsNullOrWhiteSpace(caFile_) || !File.Exists(caFile_))
            Assert.Ignore("HMAILSERVER_OBJECTSTORE_CA does not name the lab CA's PEM file, which the server needs to verify the stores' certificates.");

         labHost_ = labHost_.Trim();
         labCa_ = new X509Certificate2(caFile_);
         s3Port_ = Number("HMAILSERVER_OBJECTSTORE_S3_PORT", 10240);
         azurePort_ = Number("HMAILSERVER_OBJECTSTORE_AZURE_PORT", 10242);
         s3Access_ = Text("HMAILSERVER_OBJECTSTORE_S3_ACCESS", "hmslab-access");
         s3Secret_ = Text("HMAILSERVER_OBJECTSTORE_S3_SECRET", "hmslab-secret-row111");
         bucket_ = Text("HMAILSERVER_OBJECTSTORE_BUCKET", "hmslab");
         azureKey_ = Text("HMAILSERVER_OBJECTSTORE_AZURE_KEY", EmulatorKey);
         ssh_ = Environment.GetEnvironmentVariable("HMAILSERVER_OBJECTSTORE_SSH");

         s3_ = LabStore.S3(labHost_, s3Port_, labCa_, s3Access_, s3Secret_, bucket_);
         azure_ = LabStore.Azure(labHost_, azurePort_, labCa_, "devstoreaccount1", azureKey_, bucket_);

         // The stores answer before anything is changed: a lab that is down is a
         // lab that is down, not a failure of the server's.
         if (!s3_.Reachable(out string s3Problem))
            Assert.Ignore("MinIO does not answer at " + labHost_ + ":" + s3Port_ + ": " + s3Problem);
         if (!azure_.Reachable(out string azureProblem))
            Assert.Ignore("Azurite does not answer at " + labHost_ + ":" + azurePort_ + ": " + azureProblem);

         s3Relay_ = new Relay(S3RelayPort, labHost_, s3Port_);
         azureRelay_ = new Relay(AzureRelayPort, labHost_, azurePort_);
         s3Relay_.Start();
         azureRelay_.Start();

         prefix_ = "hmtest-" + Guid.NewGuid().ToString("N").Substring(0, 12) + "/";

         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPortPreferred);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void PutTheStoreBack()
      {
         try
         {
            if (s3Relay_ != null)
               s3Relay_.Start();
            if (azureRelay_ != null)
               azureRelay_.Start();
            if (!string.IsNullOrWhiteSpace(ssh_))
               Guarded(() => Container("start", "row111-minio"));

            if (!TestSetup.ConsoleServer && !ServiceRunning())
               Guarded(() => RestartServerAndReacquireCom());

            foreach (Action action in putBack_)
               Guarded(action);

            // The accounts first, while the store is still the one their messages
            // are in: their deletion removes the objects through the server.
            Guarded(() => _application.Reinitialize());
            foreach (string address in accounts_)
            {
               Guarded(() =>
               {
                  Domain domain = _application.Domains.get_ItemByName(address.Substring(address.IndexOf('@') + 1));
                  domain.Accounts.get_ItemByAddress(address).Delete();
               });
            }

            // Then the server back on the local disk, with no store named - over
            // REST, and not swallowed: a server left on an object store would
            // store every later fixture's mail there.
            (int status, string body) back = Admin("PUT", "/api/v1/settings",
               "{\"message_store_backend\":\"file\",\"message_store_object_service\":\"\",\"message_store_object_endpoint\":\"\"," +
               "\"message_store_object_bucket\":\"\",\"message_store_object_prefix\":\"\",\"message_store_object_access_key\":\"\"," +
               "\"message_store_object_secret\":\"\",\"message_store_object_sas_token\":\"\",\"message_store_object_ca_file\":\"\"," +
               "\"message_store_object_addressing\":\"auto\",\"message_store_object_region\":\"us-east-1\"}");
            ClassicAssert.AreEqual(200, back.status, "The message store's settings were not put back: " + back.body);

            // And whatever is left under this test's prefix, in both stores.
            if (s3_ != null)
               Guarded(() => s3_.RemoveAll(prefix_));
            if (azure_ != null)
               Guarded(() => azure_.RemoveAll(prefix_));
         }
         finally
         {
            putBack_.Clear();
            accounts_.Clear();
            if (s3Relay_ != null)
               s3Relay_.Dispose();
            if (azureRelay_ != null)
               azureRelay_.Dispose();
            RestListener.Stop();
            Guarded(() => _application.Reinitialize());
         }
      }

      // ---- the two stores, every door --------------------------------------------

      [Test]
      [Description("With the S3 backend (MinIO, SigV4, path-style), a delivered message is put into the store under its key by the committer and then taken off the local disk, and IMAP FETCH, POP3 RETR, an IMAP COPY (a second object), a MOVE (the old key gone, a new one there) and an EXPUNGE (the object gone) all work through it - and after a restart the message is read again. Negative control: the same key is not in the store before the delivery, and is gone after the EXPUNGE.")]
      public void OnS3EveryDoorWorksAndARestartReadsTheStoreAgain()
      {
         UseStore("s3", true);
         EveryDoor(s3_, "objects3");
      }

      [Test]
      [Description("The same with the Azure backend (Azurite, Shared Key, block blobs): delivered, fetched, retrieved, copied, moved and expunged through the store, and read again after a restart.")]
      public void OnAzureEveryDoorWorksAndARestartReadsTheStoreAgain()
      {
         UseStore("azure", true);
         EveryDoor(azure_, "objectaz");
      }

      private void EveryDoor(LabStore store, string name)
      {
         Account account = AddAccount(name);
         string word = Word("pangolin");
         string sent = Message(account.Address, "In the store " + word, "The body of " + word + ".");

         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, sent);
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         string fileName = Folder(account, "INBOX").Messages[0].Filename;
         string key = KeyOf(fileName);

         // Placed on the local disk as the file system places it, and put into
         // the store by the committer within a minute or so - then, and only
         // then, taken off the disk.
         WaitForCommitted(store, fileName, "The delivered message was not put into the store under " + key + " and taken off the local disk.");

         string fetched = FetchLiteral(account.Address, "INBOX", 1);
         ClassicAssert.IsNotNull(fetched, "IMAP handed out no message.");
         ClassicAssert.IsTrue(fetched.EndsWith(sent, StringComparison.Ordinal), "IMAP's message is not the message that was sent.");
         ClassicAssert.AreEqual(Encoding.ASCII.GetBytes(fetched), store.Get(key), "The object is not the message IMAP hands out.");

         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(account.Address, Password));
         StringAssert.Contains(word, pop3.RETR(1), "POP3 RETR does not read the message from the store.");
         pop3.QUIT();

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         ClassicAssert.IsTrue(imap.CreateFolder("Archive"));
         ClassicAssert.IsTrue(imap.CreateFolder("Moved"));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.IsTrue(imap.Copy(1, "Archive"), "IMAP COPY failed.");
         string moved = imap.SendSingleCommand("M1 MOVE 1 Moved");
         StringAssert.Contains("M1 OK", moved, "IMAP MOVE failed.");
         imap.Logout();
         imap.Disconnect();

         ImapClientSimulator.AssertMessageCount(account.Address, Password, "Archive", 1);
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "Moved", 1);
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 0);

         string copyKey = KeyOf(Folder(account, "Archive").Messages[0].Filename);
         string movedKey = KeyOf(Folder(account, "Moved").Messages[0].Filename);
         WaitFor(() => store.Head(copyKey) == 200, "The copy is not in the store.", CommitSeconds);
         WaitFor(() => store.Head(movedKey) == 200, "The moved message is not in the store.", CommitSeconds);
         ClassicAssert.AreEqual(404, store.Head(key), "The message moved out of the INBOX is still under its old key.");
         ClassicAssert.AreEqual(fetched, FetchLiteral(account.Address, "Archive", 1), "The copy is not the message.");

         // Expunged: the object goes.
         var expunge = new ImapClientSimulator();
         ClassicAssert.IsTrue(expunge.ConnectAndLogon(account.Address, Password));
         ClassicAssert.IsTrue(expunge.SelectFolder("Archive"));
         ClassicAssert.IsTrue(expunge.SetDeletedFlag(1));
         ClassicAssert.IsTrue(expunge.Expunge());
         expunge.Logout();
         expunge.Disconnect();
         WaitFor(() => store.Head(copyKey) == 404, "The expunged copy is still in the store.", 20);

         // Read again after a restart: the settings and the store are the server's.
         RestartServerAndReacquireCom();
         ClassicAssert.AreEqual(fetched, FetchLiteral(account.Address, "Moved", 1), "After a restart the moved message does not read from the store.");
      }

      // ---- an outage ---------------------------------------------------------------

      [Test]
      [Description("The store stopped, a message delivered: it is delivered at once, on the local disk - never held in the queue, never bounced - and read from there by IMAP while the store is down, while the committer's tries fail and HM7470 says why; the store started again, the committer puts the message into the store and only then takes it off the disk, and it reads byte for byte as it did. Negative control: while the store was down the key was not in the store and the file was on the disk.")]
      public void AnOutageIsWaitedOutOnTheLocalDiskAndNothingIsLost()
      {
         UseStore("s3", true);
         Account account = AddAccount("outage");

         string word = Word("tapir");
         StopStore();

         string fileName;
         string before;
         try
         {
            SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "While the store is down " + word, "Delivered at once: " + word));
            ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
            ClassicAssert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "The delivery was held in the queue for the store.");

            fileName = Folder(account, "INBOX").Messages[0].Filename;
            ClassicAssert.IsTrue(File.Exists(fileName), "The message delivered while the store was down is not on the local disk: " + fileName);

            before = FetchLiteral(account.Address, "INBOX", 1);
            StringAssert.Contains("Delivered at once: " + word, before, "The message delivered while the store was down does not read from the local disk.");

            // The committer's minute comes round and its try fails: the file
            // stays where it is.
            Thread.Sleep(75 * 1000);
            ClassicAssert.IsTrue(File.Exists(fileName), "The message was taken off the local disk while the store was down.");
         }
         finally
         {
            StartStore();
         }

         ClassicAssert.AreEqual(404, s3_.Head(KeyOf(fileName)), "The message is in the store although the store was down when it was delivered.");

         // Tried again after the backoff, on the committer's next minute.
         WaitForCommitted(s3_, fileName, "The message delivered during the outage was not put into the store once it answered.");
         ClassicAssert.AreEqual(before, FetchLiteral(account.Address, "INBOX", 1), "After the outage the message is not the one that was delivered.");
         ClassicAssert.AreEqual(0, TestSetup.GetNumberOfMessagesInDeliveryQueue(), "A bounce was queued for a message delivered while the store was down.");

         CustomAsserts.AssertReportedError("The object store could not be reached");
      }

      [Test]
      [Description("A message in the store, and the store stopped: IMAP FETCH answers NO [UNAVAILABLE] and POP3 RETR -ERR [SYS/TEMP] - temporary, not missing - and the message is not replaced by a placeholder: with the store back, it is read byte for byte. Negative control: the same FETCH before the outage hands out the message.")]
      public void AStoreThatCannotBeReachedIsTemporaryForImapAndPop3()
      {
         UseStore("s3", true);
         Account account = AddAccount("unreached");
         string word = Word("okapi");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Unreached " + word, "Body " + word));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         WaitForCommitted(s3_, Folder(account, "INBOX").Messages[0].Filename, "The message was not put into the store.");

         string before = FetchLiteral(account.Address, "INBOX", 1);
         StringAssert.Contains(word, before);

         StopStore();
         try
         {
            // Past the half minute what was read a moment ago is kept for.
            Thread.Sleep(31 * 1000);

            FetchLiteral(account.Address, "INBOX", 1, out string response);
            StringAssert.Contains("NO [UNAVAILABLE]", response, "FETCH was not answered as a temporary failure while the store was down.");

            string retr = Pop3Command(account.Address, Password, "RETR 1");
            StringAssert.StartsWith("-ERR [SYS/TEMP]", retr, "POP3 RETR was not answered as a temporary failure while the store was down.");
         }
         finally
         {
            StartStore();
         }

         Thread.Sleep(11 * 1000);
         ClassicAssert.AreEqual(before, FetchLiteral(account.Address, "INBOX", 1), "After the outage the message is not the one that was stored: a placeholder took its place.");

         CustomAsserts.AssertReportedError("The object store could not be reached");
      }

      [Test]
      [Description("A wrong secret is the store refusing this server (403), which is temporary and never a missing message: FETCH answers NO [UNAVAILABLE], HM7471 names the credentials, and with the secret put right the message reads as it was.")]
      public void AWrongSecretIsUnavailableAndNeverMissing()
      {
         UseStore("s3", true);
         Account account = AddAccount("refused");
         string word = Word("quoll");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Refused " + word, "Body " + word));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         WaitForCommitted(s3_, Folder(account, "INBOX").Messages[0].Filename, "The message was not put into the store.");
         string before = FetchLiteral(account.Address, "INBOX", 1);

         PutSettings("{\"message_store_object_secret\":\"not-the-secret\"}");
         Thread.Sleep(31 * 1000);

         FetchLiteral(account.Address, "INBOX", 1, out string response);
         StringAssert.Contains("NO [UNAVAILABLE]", response);

         PutSettings("{\"message_store_object_secret\":\"" + s3Secret_ + "\"}");
         Thread.Sleep(11 * 1000);
         ClassicAssert.AreEqual(before, FetchLiteral(account.Address, "INBOX", 1), "With the secret put right the message is not the one that was stored.");

         CustomAsserts.AssertReportedError("refused this server's credentials");
      }

      [Test]
      [Description("A message the store answers a confirmed 404 for - its object removed behind the server's back - is never replaced by a placeholder while a store is named: FETCH leaves it out and ends NO - the rest of a range still fetched - nothing is written on the local disk under its name or into the store under its key, and HM7476 names it. Negative control: before the object was removed the same FETCH handed out the message.")]
      public void AMessageTheStoreHasLostIsNeverReplacedByAPlaceholder()
      {
         UseStore("s3", true);
         Account account = AddAccount("lost");
         string word = Word("bandicoot");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Lost " + word, "Body " + word));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         string fileName = Folder(account, "INBOX").Messages[0].Filename;
         WaitForCommitted(s3_, fileName, "The message was not put into the store.");
         StringAssert.Contains(word, FetchLiteral(account.Address, "INBOX", 1));

         string key = KeyOf(fileName);
         s3_.Delete(key);
         ClassicAssert.AreEqual(404, s3_.Head(key), "The object could not be removed for the test.");

         // Past the half minute what was read a moment ago is kept for.
         Thread.Sleep(31 * 1000);

         FetchLiteral(account.Address, "INBOX", 1, out string response);
         StringAssert.Contains("F1 NO", response, "FETCH of a message the store has lost did not end NO.");
         StringAssert.Contains("missing from the message store", response);
         StringAssert.DoesNotContain("* 1 FETCH", response, "FETCH answered for a message the store has lost.");
         ClassicAssert.IsFalse(File.Exists(fileName), "A placeholder was written on the local disk for a message the store has lost.");

         // POP3: that message alone, and not as a temporary failure.
         string retr = Pop3Command(account.Address, Password, "RETR 1");
         StringAssert.StartsWith("-ERR", retr);
         StringAssert.DoesNotContain("[SYS/TEMP]", retr, "POP3 answered a lost message as a temporary failure.");

         // The committer's minute: nothing is put under the key either.
         Thread.Sleep(75 * 1000);
         ClassicAssert.AreEqual(404, s3_.Head(key), "Something was put into the store under the lost message's key.");

         CustomAsserts.AssertReportedError("is neither on the local disk nor in the object store");
      }

      // ---- the copy of the tree ------------------------------------------------------

      [Test]
      [Description("With a store named and new mail still on the local disk, hmailserver --copy-store-to-object-storage --account copies every message into the store, verified, and a second run finds them there and copies none (the resume). With the backend switched to object and the local files gone, the messages read from the store; --copy-store-from-object-storage puts them back on the disk. Negative control: before the copy the keys are not in the store.")]
      public void TheCopyVerbCopiesVerifiesAndResumes()
      {
         UseStore("s3", false);
         Account account = AddAccount("copied");
         string first = Word("numbat");
         string second = Word("bilby");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "First " + first, "Body " + first));
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Second " + second, "Body " + second));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 2);

         IMAPFolder inbox = Folder(account, "INBOX");
         string[] files = { inbox.Messages[0].Filename, inbox.Messages[1].Filename };
         foreach (string file in files)
         {
            ClassicAssert.IsTrue(File.Exists(file), "With the backend on file the message is not on the local disk: " + file);
            ClassicAssert.AreEqual(404, s3_.Head(KeyOf(file)), "The message is in the store before the copy.");
         }

         Verb copy = RunVerb("--copy-store-to-object-storage --account " + account.Address);
         ClassicAssert.AreEqual(0, copy.ExitCode, copy.Output);
         StringAssert.Contains("2 copied and verified", copy.Output);
         foreach (string file in files)
            ClassicAssert.AreEqual(200, s3_.Head(KeyOf(file)), "The copy did not put " + file + " into the store.");

         Verb again = RunVerb("--copy-store-to-object-storage --account " + account.Address);
         ClassicAssert.AreEqual(0, again.ExitCode, again.Output);
         StringAssert.Contains("0 copied and verified, 2 there already and verified", again.Output);

         // New mail to the store, and the local tree gone: read from the store.
         PutSettings("{\"message_store_backend\":\"object\"}");
         Thread.Sleep(6 * 1000);
         foreach (string file in files)
            File.Delete(file);

         StringAssert.Contains(first, FetchLiteral(account.Address, "INBOX", 1));
         StringAssert.Contains(second, FetchLiteral(account.Address, "INBOX", 2));

         // And back.
         Verb back = RunVerb("--copy-store-from-object-storage --account " + account.Address);
         ClassicAssert.AreEqual(0, back.ExitCode, back.Output);
         foreach (string file in files)
            ClassicAssert.IsTrue(File.Exists(file), "--copy-store-from-object-storage did not put " + file + " back.");
      }

      [Test]
      [Description("With no store named - the shipped default - a delivered message is a file under the data directory as it always was, and nothing is written under this test's prefix in either store. The negative control of every test above.")]
      public void WithNoStoreNamedNothingChanges()
      {
         Account account = AddAccount("nostore");
         string word = Word("dunnart");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Local " + word, "Body " + word));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         string fileName = Folder(account, "INBOX").Messages[0].Filename;
         ClassicAssert.IsTrue(File.Exists(fileName), "With no store named the message is not on the local disk.");
         ClassicAssert.AreEqual(0, s3_.List(prefix_).Count, "Something was written to the S3 store with no store named.");
         ClassicAssert.AreEqual(0, azure_.List(prefix_).Count, "Something was written to the Azure store with no store named.");
      }

      // ---- helpers ----------------------------------------------------------------------

      private void UseStore(string service, bool writes)
      {
         string endpoint = service == "s3"
            ? "https://127.0.0.1:" + S3RelayPort
            : "https://127.0.0.1:" + AzureRelayPort + "/devstoreaccount1";

         PutSettings("{\"message_store_object_service\":\"" + service + "\"," +
                  "\"message_store_object_endpoint\":\"" + endpoint + "\"," +
                  "\"message_store_object_region\":\"us-east-1\"," +
                  "\"message_store_object_bucket\":\"" + bucket_ + "\"," +
                  "\"message_store_object_addressing\":\"path\"," +
                  "\"message_store_object_prefix\":\"" + prefix_ + "\"," +
                  "\"message_store_object_access_key\":\"" + (service == "s3" ? s3Access_ : "devstoreaccount1") + "\"," +
                  "\"message_store_object_secret\":\"" + (service == "s3" ? s3Secret_ : azureKey_) + "\"," +
                  "\"message_store_object_ca_file\":\"" + caFile_.Replace("\\", "\\\\") + "\"," +
                  "\"message_store_backend\":\"" + (writes ? "object" : "file") + "\"}");

         // The backend reads its settings again within five seconds.
         Thread.Sleep(6 * 1000);
      }

      // How long the committer may take to put a message into the store: its
      // minute on the scheduler, a backoff after a try that failed, and the
      // pass itself.
      private const int CommitSeconds = 240;

      private void WaitForCommitted(LabStore store, string fileName, string failure)
      {
         string key = KeyOf(fileName);
         WaitFor(() => store.Head(key) == 200 && !File.Exists(fileName), failure, CommitSeconds);
      }

      private void PutSettings(string json)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/settings", json);
         ClassicAssert.AreEqual(200, answer.status, answer.body);
      }

      private void StopStore()
      {
         if (!string.IsNullOrWhiteSpace(ssh_))
            Container("stop", "row111-minio");
         else
            s3Relay_.Stop();
      }

      private void StartStore()
      {
         if (!string.IsNullOrWhiteSpace(ssh_))
         {
            Container("start", "row111-minio");
            WaitFor(() => s3_.Reachable(out string _), "MinIO did not come back.", 60);
         }

         s3Relay_.Start();
      }

      private void Container(string verb, string name)
      {
         var start = new ProcessStartInfo("ssh", "-o BatchMode=yes " + ssh_ + " docker " + verb + " " + name)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         };

         using (Process process = Process.Start(start))
         {
            string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
            ClassicAssert.IsTrue(process.WaitForExit(120 * 1000), "ssh docker " + verb + " did not finish.");
            ClassicAssert.AreEqual(0, process.ExitCode, "ssh docker " + verb + " " + name + " failed: " + output);
         }
      }

      // The key the server keeps a stored file under: its path under the data
      // directory, '/' between the parts, letters small (the fixture's names are
      // ASCII), after the prefix.
      private string KeyOf(string fileName)
      {
         string data = _application.Settings.Directories.DataDirectory.TrimEnd('\\', '/');
         ClassicAssert.IsTrue(fileName.StartsWith(data, StringComparison.OrdinalIgnoreCase), fileName + " is not under " + data);
         string relative = fileName.Substring(data.Length).TrimStart('\\', '/').Replace('\\', '/');

         var lower = new StringBuilder();
         foreach (char c in relative)
            lower.Append(c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c);

         return prefix_ + lower;
      }

      private Account AddAccount(string name)
      {
         string address = name + "@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         accounts_.Add(address);
         return account;
      }

      // Asked of the server each time: a COM object from before a reinitialise or
      // a restart belongs to the configuration that was.
      private IMAPFolder Folder(Account account, string name)
      {
         Domain domain = _application.Domains.get_ItemByName(account.Address.Substring(account.Address.IndexOf('@') + 1));
         return domain.Accounts.get_ItemByAddress(account.Address).IMAPFolders.get_ItemByName(name);
      }

      private static string Word(string stem)
         => stem + Guid.NewGuid().ToString("N").Substring(0, 8);

      private static string Message(string to, string subject, string body)
         => "From: sender@example.test\r\nTo: " + to + "\r\nSubject: " + subject + "\r\nMessage-ID: <" + Guid.NewGuid().ToString("N") +
            "@example.test>\r\n\r\n" + body + "\r\n";

      private static string FetchLiteral(string address, string folder, int index)
         => FetchLiteral(address, folder, index, out string _);

      private static string FetchLiteral(string address, string folder, int index, out string response)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password), "IMAP logon failed for " + address);
         ClassicAssert.IsTrue(imap.SelectFolder(folder), "SELECT " + folder + " failed for " + address);

         var all = new StringBuilder();
         string literal = null;
         DateTime until = DateTime.UtcNow.AddSeconds(90);

         try
         {
            all.Append(imap.Send("F1 FETCH " + index + " BODY.PEEK[]"));

            while (DateTime.UtcNow < until)
            {
               string text = all.ToString();
               int brace = text.IndexOf('{');
               int close = brace >= 0 ? text.IndexOf("}\r\n", brace, StringComparison.Ordinal) : -1;

               if (close > brace)
               {
                  int length = int.Parse(text.Substring(brace + 1, close - brace - 1));
                  int start = close + 3;
                  if (text.Length >= start + length)
                  {
                     int tagged = text.IndexOf("\r\nF1 ", start + length, StringComparison.Ordinal);
                     if (tagged >= 0 && text.IndexOf("\r\n", tagged + 2, StringComparison.Ordinal) >= 0)
                     {
                        literal = text.Substring(start, length);
                        break;
                     }
                  }
               }
               else if ((text.StartsWith("F1 ", StringComparison.Ordinal) || text.Contains("\r\nF1 ")) && text.EndsWith("\r\n", StringComparison.Ordinal))
                  break;   // answered with no literal: refused

               string more = imap.Receive();
               if (more.Length == 0)
                  break;
               all.Append(more);
            }
         }
         catch (IOException)
         {
         }

         response = all.ToString();
         try
         {
            imap.Disconnect();
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
         }

         return literal;
      }

      // One POP3 command's first line, after USER and PASS, over a connection of
      // this fixture's own: the simulator's RETR throws on -ERR, and the answer
      // is what is being tested.
      private static string Pop3Command(string address, string password, string command)
      {
         using (var client = new TcpClient("127.0.0.1", 110))
         using (var stream = client.GetStream())
         using (var reader = new StreamReader(stream, Encoding.ASCII))
         using (var writer = new StreamWriter(stream, Encoding.ASCII) { NewLine = "\r\n", AutoFlush = true })
         {
            client.ReceiveTimeout = 120 * 1000;
            reader.ReadLine();
            writer.WriteLine("USER " + address);
            reader.ReadLine();
            writer.WriteLine("PASS " + password);
            string pass = reader.ReadLine();
            StringAssert.StartsWith("+OK", pass, "POP3 did not sign in.");
            writer.WriteLine(command);
            string answer = reader.ReadLine();
            writer.WriteLine("QUIT");
            return answer;
         }
      }

      private static void WaitFor(Func<bool> condition, string failure, int seconds)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);
         while (DateTime.UtcNow < until)
         {
            if (condition())
               return;
            Thread.Sleep(500);
         }

         ClassicAssert.IsTrue(condition(), failure);
      }

      private static bool ServiceRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status != System.ServiceProcess.ServiceControllerStatus.Stopped;
         }
      }

      private static int Number(string variable, int fallback)
      {
         string value = Environment.GetEnvironmentVariable(variable);
         return !string.IsNullOrWhiteSpace(value) && int.TryParse(value.Trim(), out int number) ? number : fallback;
      }

      private static string Text(string variable, string fallback)
      {
         string value = Environment.GetEnvironmentVariable(variable);
         return string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("ObjectStorage teardown: " + e.Message);
         }
      }

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      private string executable_;

      private Verb RunVerb(string arguments)
      {
         var start = new ProcessStartInfo(ServerExecutable(), arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };

         foreach (string name in start.EnvironmentVariables.Keys.Cast<string>().ToList())
         {
            if (name.StartsWith("HM_", StringComparison.OrdinalIgnoreCase))
               start.EnvironmentVariables.Remove(name);
         }

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(300 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
               }

               Assert.Fail(arguments + " did not finish within five minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      private string ServerExecutable()
      {
         if (executable_ != null)
            return executable_;

         string program = _application.Settings.Directories.ProgramDirectory;
         foreach (string candidate in new[] { Path.Combine(program, "hMailServer.exe"), Path.Combine(program, "Bin", "hMailServer.exe") })
         {
            if (File.Exists(candidate))
               return executable_ = candidate;
         }

         Assert.Ignore("hMailServer.exe is not in the program directory the server reports (" + program + "), so the verb cannot be run here.");
         return null;
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);

      // ---- a relay on the loopback address -----------------------------------------------
      //
      // What the server reaches the store through: every connection accepted is
      // carried to the store and back, and Stop closes the listener and every
      // connection it carries, so that the server meets a refused connection -
      // what a stopped container gives.

      private sealed class Relay : IDisposable
      {
         private readonly int port_;
         private readonly string host_;
         private readonly int targetPort_;
         private readonly object lock_ = new object();
         private readonly List<TcpClient> carried_ = new List<TcpClient>();
         private TcpListener listener_;

         public Relay(int port, string host, int targetPort)
         {
            port_ = port;
            host_ = host;
            targetPort_ = targetPort;
         }

         public void Start()
         {
            lock (lock_)
            {
               if (listener_ != null)
                  return;

               listener_ = new TcpListener(IPAddress.Loopback, port_);
               listener_.Start();
               TcpListener listening = listener_;
               var thread = new Thread(() => Accept(listening)) { IsBackground = true, Name = "ObjectStorage relay " + port_ };
               thread.Start();
            }
         }

         public void Stop()
         {
            lock (lock_)
            {
               if (listener_ != null)
               {
                  listener_.Stop();
                  listener_ = null;
               }

               foreach (TcpClient client in carried_)
               {
                  try
                  {
                     client.Close();
                  }
                  catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
                  {
                  }
               }
               carried_.Clear();
            }
         }

         public void Dispose() => Stop();

         private void Accept(TcpListener listening)
         {
            for (;;)
            {
               TcpClient inbound;
               try
               {
                  inbound = listening.AcceptTcpClient();
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  return;   // stopped
               }

               TcpClient outbound = new TcpClient();
               try
               {
                  outbound.Connect(host_, targetPort_);
               }
               catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
               {
                  inbound.Close();
                  outbound.Close();
                  continue;
               }

               lock (lock_)
               {
                  carried_.Add(inbound);
                  carried_.Add(outbound);
               }

               Task.Run(() => Pump(inbound, outbound));
               Task.Run(() => Pump(outbound, inbound));
            }
         }

         private static void Pump(TcpClient from, TcpClient to)
         {
            var buffer = new byte[65536];
            try
            {
               NetworkStream source = from.GetStream();
               NetworkStream sink = to.GetStream();
               for (;;)
               {
                  int read = source.Read(buffer, 0, buffer.Length);
                  if (read <= 0)
                     break;
                  sink.Write(buffer, 0, read);
               }
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
            }

            try
            {
               to.Client.Shutdown(SocketShutdown.Send);
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
            }
         }
      }

      // ---- the stores, asked directly ------------------------------------------------------
      //
      // HTTP/1.0 over TLS, the certificate held to the lab CA, and each request
      // signed here: SigV4 for MinIO, Shared Key for Azurite - the same
      // algorithms as the server's, written again from the specifications.

      private sealed class LabStore
      {
         private string host_;
         private int port_;
         private X509Certificate2 ca_;
         private bool s3_;
         private string access_;
         private string secret_;
         private string bucket_;

         public static LabStore S3(string host, int port, X509Certificate2 ca, string access, string secret, string bucket)
            => new LabStore { host_ = host, port_ = port, ca_ = ca, s3_ = true, access_ = access, secret_ = secret, bucket_ = bucket };

         public static LabStore Azure(string host, int port, X509Certificate2 ca, string account, string key, string container)
            => new LabStore { host_ = host, port_ = port, ca_ = ca, s3_ = false, access_ = account, secret_ = key, bucket_ = container };

         public bool Reachable(out string problem)
         {
            problem = null;
            try
            {
               int status = s3_ ? Send("HEAD", "/" + bucket_, "", out byte[] _, out string _)
                                : Send("HEAD", "/" + access_ + "/" + bucket_, "restype=container", out byte[] _, out string _);
               if (status == 200)
                  return true;
               problem = "HTTP " + status;
               return false;
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               problem = e.Message;
               return false;
            }
         }

         public int Head(string key) => Send("HEAD", ObjectPath(key), "", out byte[] _, out string _);

         public int Delete(string key) => Send("DELETE", ObjectPath(key), "", out byte[] _, out string _);

         public byte[] Get(string key)
         {
            int status = Send("GET", ObjectPath(key), "", out byte[] body, out string _);
            ClassicAssert.AreEqual(200, status, "GET " + key);
            return body;
         }

         public List<string> List(string prefix)
         {
            var keys = new List<string>();
            string query = s3_ ? "list-type=2&prefix=" + Encode(prefix, true) : "comp=list&prefix=" + Encode(prefix, true) + "&restype=container";
            string path = s3_ ? "/" + bucket_ : "/" + access_ + "/" + bucket_;
            int status = Send("GET", path, query, out byte[] body, out string _);
            ClassicAssert.AreEqual(200, status, "LIST " + prefix);

            string xml = Encoding.UTF8.GetString(body);
            string element = s3_ ? "Key" : "Name";
            int at = 0;
            for (;;)
            {
               int open = xml.IndexOf("<" + element + ">", at, StringComparison.Ordinal);
               if (open < 0)
                  break;
               int close = xml.IndexOf("</" + element + ">", open, StringComparison.Ordinal);
               if (close < 0)
                  break;
               keys.Add(WebUtility.HtmlDecode(xml.Substring(open + element.Length + 2, close - open - element.Length - 2)));
               at = close;
            }
            return keys;
         }

         public void RemoveAll(string prefix)
         {
            foreach (string key in List(prefix))
               Send("DELETE", ObjectPath(key), "", out byte[] _, out string _);
         }

         private string ObjectPath(string key) => s3_ ? "/" + bucket_ + "/" + key : "/" + access_ + "/" + bucket_ + "/" + key;

         private int Send(string method, string path, string query, out byte[] body, out string headers)
         {
            string authority = host_ + ":" + port_;
            string target = Encode(path, false) + (query.Length > 0 ? "?" + query : "");
            var lines = new List<string> { method + " " + target + " HTTP/1.0", "Host: " + authority, "User-Agent: hMailServer-tests" };

            if (s3_)
            {
               string date = DateTime.UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'");
               string payload = Hex(SHA256.Create().ComputeHash(new byte[0]));
               var signed = new SortedDictionary<string, string>(StringComparer.Ordinal)
               {
                  { "host", authority }, { "x-amz-content-sha256", payload }, { "x-amz-date", date }
               };
               string canonicalHeaders = string.Concat(signed.Select(h => h.Key + ":" + h.Value + "\n"));
               string signedNames = string.Join(";", signed.Keys);
               string canonicalQuery = string.Join("&", query.Split(new[] { '&' }, StringSplitOptions.RemoveEmptyEntries)
                  .Select(p => { int eq = p.IndexOf('='); return eq < 0 ? p + "=" : p; }).OrderBy(p => p, StringComparer.Ordinal));
               string request = method + "\n" + Encode(path, false) + "\n" + canonicalQuery + "\n" + canonicalHeaders + "\n" + signedNames + "\n" + payload;
               string scope = date.Substring(0, 8) + "/us-east-1/s3/aws4_request";
               string toSign = "AWS4-HMAC-SHA256\n" + date + "\n" + scope + "\n" + Hex(SHA256.Create().ComputeHash(Encoding.UTF8.GetBytes(request)));
               byte[] key = Hmac(Encoding.UTF8.GetBytes("AWS4" + secret_), date.Substring(0, 8));
               key = Hmac(key, "us-east-1");
               key = Hmac(key, "s3");
               key = Hmac(key, "aws4_request");
               lines.Add("x-amz-content-sha256: " + payload);
               lines.Add("x-amz-date: " + date);
               lines.Add("Authorization: AWS4-HMAC-SHA256 Credential=" + access_ + "/" + scope + ", SignedHeaders=" + signedNames + ", Signature=" + Hex(Hmac(key, toSign)));
            }
            else
            {
               string date = DateTime.UtcNow.ToString("R");
               const string version = "2021-08-06";
               var parameters = query.Split(new[] { '&' }, StringSplitOptions.RemoveEmptyEntries)
                  .Select(p => { int eq = p.IndexOf('='); return eq < 0 ? new[] { p, "" } : new[] { p.Substring(0, eq), Uri.UnescapeDataString(p.Substring(eq + 1)) }; })
                  .OrderBy(p => p[0].ToLowerInvariant(), StringComparer.Ordinal);
               string resource = "/" + access_ + Encode(path, false) + string.Concat(parameters.Select(p => "\n" + p[0].ToLowerInvariant() + ":" + p[1]));
               string toSign = method + "\n\n\n\n\n\n\n\n\n\n\n\n" + "x-ms-date:" + date + "\nx-ms-version:" + version + "\n" + resource;
               string signature = Convert.ToBase64String(new HMACSHA256(Convert.FromBase64String(secret_)).ComputeHash(Encoding.UTF8.GetBytes(toSign)));
               lines.Add("x-ms-date: " + date);
               lines.Add("x-ms-version: " + version);
               lines.Add("Authorization: SharedKey " + access_ + ":" + signature);
            }

            lines.Add("Connection: close");
            byte[] requestBytes = Encoding.UTF8.GetBytes(string.Join("\r\n", lines) + "\r\n\r\n");

            using (var client = new TcpClient(host_, port_))
            using (var tls = new SslStream(client.GetStream(), false, Trusted))
            {
               client.ReceiveTimeout = 30 * 1000;
               tls.AuthenticateAsClient(host_);
               tls.Write(requestBytes, 0, requestBytes.Length);

               var answer = new MemoryStream();
               var buffer = new byte[65536];
               int read;
               while ((read = tls.Read(buffer, 0, buffer.Length)) > 0)
                  answer.Write(buffer, 0, read);

               byte[] all = answer.ToArray();
               string text = Encoding.ASCII.GetString(all);
               int end = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
               headers = end < 0 ? text : text.Substring(0, end);
               body = end < 0 ? new byte[0] : all.Skip(end + 4).ToArray();
               string[] status = headers.Split(' ');
               return status.Length > 1 ? int.Parse(status[1]) : 0;
            }
         }

         // The lab's certificate, and only a chain that ends at the lab CA.
         private bool Trusted(object sender, X509Certificate certificate, X509Chain chain, SslPolicyErrors errors)
         {
            if (certificate == null)
               return false;

            using (var built = new X509Chain())
            {
               built.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
               built.ChainPolicy.VerificationFlags = X509VerificationFlags.AllowUnknownCertificateAuthority;
               built.ChainPolicy.ExtraStore.Add(ca_);
               if (!built.Build(new X509Certificate2(certificate)))
                  return false;

               X509ChainElement root = built.ChainElements[built.ChainElements.Count - 1];
               return root.Certificate.Thumbprint == ca_.Thumbprint;
            }
         }

         private static byte[] Hmac(byte[] key, string data)
         {
            using (var hmac = new HMACSHA256(key))
               return hmac.ComputeHash(Encoding.UTF8.GetBytes(data));
         }

         private static string Hex(byte[] bytes) => string.Concat(bytes.Select(b => b.ToString("x2")));

         private static string Encode(string text, bool slash)
         {
            var encoded = new StringBuilder();
            foreach (byte b in Encoding.UTF8.GetBytes(text))
            {
               char c = (char) b;
               if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || (c == '/' && !slash))
                  encoded.Append(c);
               else
                  encoded.Append('%').Append(b.ToString("X2"));
            }
            return encoded.ToString();
         }
      }
   }
}
