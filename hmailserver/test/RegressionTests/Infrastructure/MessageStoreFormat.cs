// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.Infrastructure
{
   /// <summary>
   ///    Storage compression and attachment de-duplication (docs/MessageStore.md):
   ///    with MessageStoreCompression on, a message placed in a mailbox is stored as
   ///    a container - deflated, each large MIME part held once as a blob under
   ///    &lt;data&gt;\Blobs - and every reader still sees the message as it arrived.
   ///
   ///    Each test proves the file on disk and what the clients see, separately:
   ///    the container is parsed and decoded here, by a reader written from the
   ///    doc and not from the server's code, and compared with IMAP, POP3 and a
   ///    DKIM check over what IMAP hands out. Each has its negative control -
   ///    compression off stores byte for byte, a part under the minimum is not
   ///    held, a damaged container is refused, a refused verb changes nothing.
   ///
   ///    The settings go through REST (the listener on 10080, the row's range is
   ///    10080-10089) and are put back in the teardown, with DeliveryHardLinks,
   ///    AddDeliveredToHeader, the consistency check and the backup settings; the
   ///    accounts a test made are deleted there, which lets go of every blob they
   ///    held, and an empty Blobs directory is removed, so the fixture leaves no
   ///    container and no blob behind. The command-line walk runs only with
   ///    --account, on this fixture's accounts.
   /// </summary>
   [TestFixture]
   public class MessageStoreFormat : TestFixtureBase
   {
      private static int RestPort = 10080;
      private const string AdminPassword = "testar";
      private const string Password = "test";

      private static readonly byte[] Magic = { 0x89, 0x48, 0x4D, 0x53, 0x0D, 0x0A, 0x1A, 0x0A };

      private string dataDirectory_;
      private readonly List<string> accounts_ = new List<string>();
      private readonly List<Action> putBack_ = new List<Action>();

      // The two settings as the fixture found them, put back as they were.
      private bool compressionFound_;
      private int dedupMinimumFound_ = 256;

      [SetUp]
      public void StartListener()
      {
         _settings.SetAdministratorPassword(AdminPassword);
         RestPort = RestListener.Start(RestPort);
         _application.Reinitialize();

         (int status, string body) probe = Admin("GET", "/api/v1/status", null);
         ClassicAssert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);

         dataDirectory_ = _application.Settings.Directories.DataDirectory;

         // Late-bound, as MoveTo is below: the two members are new on an
         // interface the suite's interop may predate.
         dynamic settings = _application.Settings;
         compressionFound_ = (bool) settings.MessageStoreCompression;
         dedupMinimumFound_ = (int) settings.MessageStoreDedupMinKB;
      }

      [TearDown]
      public void PutTheStoreBack()
      {
         Exception settingsNotPutBack = null;

         try
         {
            // A test that stopped the service and failed before it started it again.
            if (!TestSetup.ConsoleServer && !ServiceRunning())
               Guarded(() => RestartServerAndReacquireCom());

            // Over COM, and not swallowed: a server left compressing would store
            // every later fixture's mail as containers, and the fixtures that read
            // Message.Filename by path would read them.
            try
            {
               dynamic settings = _application.Settings;
               settings.MessageStoreCompression = compressionFound_;
               settings.MessageStoreDedupMinKB = dedupMinimumFound_;
               ClassicAssert.AreEqual(compressionFound_, (bool) settings.MessageStoreCompression, "MessageStoreCompression was not put back.");
            }
            catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
            {
               settingsNotPutBack = e;
            }

            foreach (Action action in putBack_)
               Guarded(action);

            // The accounts go now rather than at the next SetUp, so that what they
            // held once goes with them while this fixture can still look.
            Guarded(() => _application.Reinitialize());
            foreach (string address in accounts_)
            {
               Guarded(() =>
               {
                  Domain domain = _application.Domains.get_ItemByName(address.Substring(address.IndexOf('@') + 1));
                  domain.Accounts.get_ItemByAddress(address).Delete();
               });
            }

            Guarded(() =>
            {
               string blobs = Path.Combine(dataDirectory_, "Blobs");
               if (Directory.Exists(blobs) && Directory.GetFiles(blobs, "*", SearchOption.AllDirectories).Length == 0)
                  Directory.Delete(blobs, true);
            });
         }
         finally
         {
            putBack_.Clear();
            accounts_.Clear();
            RestListener.Stop();
            _application.Reinitialize();
         }

         if (settingsNotPutBack != null)
            throw new InvalidOperationException("MessageStoreFormat: the message store settings could not be put back as they were found: " + settingsNotPutBack.Message, settingsNotPutBack);
      }

      // ---- what every reader sees ------------------------------------------------

      [Test]
      [Description("With compression on, a delivered message is stored as a container - the magic, smaller than the message - and IMAP (BODY[], RFC822, RFC822.SIZE, SEARCH), POP3 RETR, COM's Body and a DKIM check over what IMAP hands out all see the message as it arrived; the container decoded here, from the doc, is those same bytes. Negative control: the file itself is not the message, and a changed byte fails the DKIM check.")]
      public void TurnedOnAMessageIsStoredAsAContainerAndEveryReaderSeesTheOriginal()
      {
         SetCompression(true, 256);
         Account account = AddAccount("compressed");

         string unique = "quokka" + Guid.NewGuid().ToString("N").Substring(0, 8);
         RSA key = RSA.Create(2048);
         string sent = Signed(Message(account.Address, "Stored compressed", TextBody(unique, 500), null), key);

         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, sent);
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         hMailServer.Message stored = Inbox(account).Messages[0];
         byte[] file = File.ReadAllBytes(stored.Filename);
         ClassicAssert.IsTrue(HasMagic(file), "The stored file is not a container: " + stored.Filename);

         string fetched = FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsNotNull(fetched, "IMAP handed out no message.");
         ClassicAssert.IsTrue(fetched.EndsWith(TextBody(unique, 500), StringComparison.Ordinal), "IMAP's message does not end with the body that was sent.");
         // Byte for byte what was sent, the signature's relaxed canonicalisation
         // notwithstanding: the server only puts its trace headers in front.
         ClassicAssert.IsTrue(fetched.EndsWith(sent, StringComparison.Ordinal), "IMAP's message is not the message that was sent, with the trace headers in front of it.");
         ClassicAssert.Less(file.Length, fetched.Length / 4, "The container is not smaller than the message it holds.");

         // The file decoded here, from the doc's description of it.
         byte[] decoded = Decode(file);
         ClassicAssert.AreEqual(fetched, Encoding.ASCII.GetString(decoded), "The container decoded by the doc's rules is not what IMAP hands out.");
         CollectionAssert.AreNotEqual(decoded, file, "The file is the message: nothing was stored in the new form.");

         ClassicAssert.AreEqual(fetched, FetchLiteral(account.Address, "INBOX", 1, "RFC822"), "RFC822 differs from BODY[].");
         ClassicAssert.AreEqual(fetched.Length, FetchSize(account.Address, 1), "RFC822.SIZE is not the message's size.");
         ClassicAssert.AreEqual(fetched.Length / 1024, Inbox(account).Messages[0].Size, "COM's Size (in KB) is not the message's.");
         StringAssert.Contains(unique, Inbox(account).Messages[0].Body, "COM's Body does not read the message.");

         ClassicAssert.AreEqual(fetched, Retrieve(account.Address, 1), "POP3 RETR differs from IMAP's message.");

         // The signature was made over the bytes that were sent, and verifies over
         // the bytes IMAP hands out: the store gave back exactly what it was given.
         ClassicAssert.IsTrue(DkimSignatureVerifies(fetched, key, out string why), "The DKIM signature does not verify over the stored copy: " + why);
         string tampered = fetched.Replace("Line 0001 ", "Line 0001X");
         ClassicAssert.IsFalse(DkimSignatureVerifies(tampered, key, out string _), "A changed body still verified: the check checks nothing.");

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.AreEqual("1", imap.Search("BODY " + unique), "SEARCH does not find a word of the compressed body.");
         ClassicAssert.AreEqual("", imap.Search("BODY absentword" + unique), "SEARCH finds a word the message does not have.");
         imap.Logout();
         imap.Disconnect();
      }

      [Test]
      [Description("Off - the default - a delivered message is stored byte for byte as IMAP hands it out, with no container and no blob, however large its attachment. The negative control of every test that turns it on.")]
      public void TurnedOffAMessageIsStoredByteForByteAsItAlwaysWas()
      {
         SetCompression(false, 256);
         Account account = AddAccount("plain");

         string attachment = Attachment(300, 11);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address,
            Message(account.Address, "Stored as it came", TextBody("plainword", 500), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         string fileName = Inbox(account).Messages[0].Filename;
         byte[] file = File.ReadAllBytes(fileName);
         ClassicAssert.IsFalse(HasMagic(file), "Compression is off and the file is a container.");

         string fetched = FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.AreEqual(fetched, Encoding.ASCII.GetString(file), "The file is not the message byte for byte.");
         ClassicAssert.AreEqual(file.Length, FetchSize(account.Address, 1));
         ClassicAssert.IsFalse(File.Exists(BlobPath(Sha256Hex(attachment))), "Compression is off and the attachment was held once.");
      }

      [Test]
      [Description("An IMAP APPEND is stored as a container, with its large part held once, and FETCH hands back exactly the bytes that were appended - the literal, byte for byte. Negative control: with compression off the file is those bytes.")]
      public void AnAppendedMessageIsStoredCompressedAndFetchedExactly()
      {
         Account account = AddAccount("appended");
         string attachment = Attachment(300, 12);
         string raw = Message(account.Address, "Appended", TextBody("appendword", 400), attachment);

         // The stored message is the literal, byte for byte (RFC 3501 6.3.11).
         // IMAPCommandAppend wrote a CRLF after every literal until the row "IMAP
         // that passes imaptest", and this test asserted that CRLF.
         string stored = raw;

         SetCompression(false, 256);
         Append(account.Address, raw);
         byte[] plain = File.ReadAllBytes(Inbox(account).Messages[0].Filename);
         ClassicAssert.AreEqual(stored, Encoding.ASCII.GetString(plain), "With compression off an appended message is not stored as it was appended.");

         SetCompression(true, 256);
         Append(account.Address, raw);
         byte[] file = File.ReadAllBytes(Inbox(account).Messages[1].Filename);
         ClassicAssert.IsTrue(HasMagic(file), "An appended message was not stored as a container.");
         ClassicAssert.IsTrue(File.Exists(BlobPath(Sha256Hex(attachment))), "The appended message's attachment was not held once.");
         CollectionAssert.Contains(ReferencesOf(file), Sha256Hex(attachment), "The container does not name the attachment's blob.");

         string fetched = FetchLiteral(account.Address, "INBOX", 2, "BODY.PEEK[]");
         ClassicAssert.AreEqual(Sha256Hex(stored), Sha256Hex(fetched), "FETCH does not hand back the appended bytes.");
         ClassicAssert.AreEqual(stored, Encoding.ASCII.GetString(Decode(file)));
         ClassicAssert.AreEqual(stored.Length, FetchSize(account.Address, 2), "RFC822.SIZE is not the appended message's size.");
      }

      [Test]
      [Description("A literal that begins with the container's magic and carries a header naming another account's blob - twice - is the client's bytes and nothing else: it is stored wrapped, FETCH and RFC822.SIZE answer the literal as it was sent, and an EXPUNGE of it lets go of nothing, so the other account's message keeps its part. Negative control: the blob's own message, expunged, takes the blob with it, so the count was one name and no more.")]
      public void AnAppendedContainerIsTheClientsBytesAndNeverTakenForOne()
      {
         SetCompression(true, 256);
         Account victim = AddAccount("sharer");
         Account attacker = AddAccount("crafter");

         string attachment = Attachment(300, 81);
         string sha = Sha256Hex(attachment);
         string blob = BlobPath(sha);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", victim.Address, Message(victim.Address, "Carries the part", TextBody("victimword", 50), attachment));
         ImapClientSimulator.AssertMessageCount(victim.Address, Password, "INBOX", 1);
         string victimsMessage = FetchLiteral(victim.Address, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsTrue(File.Exists(blob), "The part was not held once.");

         // A header the doc describes, naming the part twice and claiming a size
         // far below the literal's - everything a crafted container needs for a
         // reader that trusted it to let go of the part, and to charge the quota
         // what it claims.
         long length = Encoding.ASCII.GetByteCount(attachment);
         byte[] crafted = CraftedContainer(sha, length);

         byte[] literal = AppendRaw(attacker.Address, "INBOX", crafted);
         ImapClientSimulator.AssertMessageCount(attacker.Address, Password, "INBOX", 1);

         byte[] storedFile = File.ReadAllBytes(Inbox(attacker).Messages[0].Filename);
         ClassicAssert.IsTrue(HasMagic(storedFile), "The appended bytes were not wrapped.");
         ClassicAssert.IsEmpty(ReferencesOf(storedFile), "The wrapped message names a blob: the literal's header was taken for the store's.");
         CollectionAssert.AreEqual(literal, Decode(storedFile), "The wrapped container does not hold the literal as it was sent.");

         CollectionAssert.AreEqual(literal, FetchRaw(attacker.Address, "INBOX", 1), "FETCH does not hand back the literal as it was sent.");
         ClassicAssert.AreEqual(literal.Length, FetchSize(attacker.Address, 1), "RFC822.SIZE is not the literal's size.");

         ExpungeFirst(attacker.Address, "INBOX");
         Thread.Sleep(1000);
         ClassicAssert.IsTrue(File.Exists(blob), "Expunging the crafted message took another account's blob with it.");
         ClassicAssert.AreEqual(victimsMessage, FetchLiteral(victim.Address, "INBOX", 1, "BODY.PEEK[]"), "The other account's message no longer reads back.");

         // Negative control: the part's one real name goes, and so does the part.
         ExpungeFirst(victim.Address, "INBOX");
         WaitFor(() => !File.Exists(blob), "The blob outlived the one message that named it: its count was more than its names.");
      }

      // ---- parts held once ---------------------------------------------------------

      [Test]
      [Description("Two messages with the same 300 KB attachment keep it once: one blob, named by the SHA-256 of the part's wire bytes, that both containers name. A message rewritten through COM stores plain and lets go of it, and the blob goes with the last message that needs it. Negative control: a 100 KB attachment, under the 256 KB minimum, is not held.")]
      public void APartTwoMessagesShareIsKeptOnceAndGoesWithTheLastOfThem()
      {
         SetCompression(true, 256);
         Account account = AddAccount("shared");

         string attachment = Attachment(300, 21);
         string blob = BlobPath(Sha256Hex(attachment));

         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "First", TextBody("firstword", 50), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Second", TextBody("secondword", 60), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 2);

         ClassicAssert.IsTrue(File.Exists(blob), "The attachment was not held once: " + blob);
         ClassicAssert.AreEqual(1, Directory.GetFiles(Path.Combine(dataDirectory_, "Blobs"), "*", SearchOption.AllDirectories).Length,
            "The attachment is held more than once, or something else was.");
         ClassicAssert.AreEqual(Encoding.ASCII.GetByteCount(attachment), new FileInfo(blob).Length, "The blob is not the part's wire bytes.");

         for (int index = 0; index < 2; index++)
         {
            byte[] file = File.ReadAllBytes(Inbox(account).Messages[index].Filename);
            CollectionAssert.AreEqual(new[] { Sha256Hex(attachment) }, ReferencesOf(file), "Message " + (index + 1) + " does not name the one blob.");
            string fetched = FetchLiteral(account.Address, "INBOX", index + 1, "BODY.PEEK[]");
            StringAssert.Contains(attachment, fetched, "Message " + (index + 1) + " does not carry the attachment as it was sent.");
            ClassicAssert.AreEqual(fetched, Encoding.ASCII.GetString(Decode(file)));
         }

         // The second rewritten through COM: stored plain, with the attachment in it,
         // and no longer a name that needs the blob.
         hMailServer.Message second = Inbox(account).Messages[1];
         second.set_HeaderValue("X-Rewritten", "through COM");
         second.Save();

         byte[] rewritten = File.ReadAllBytes(Inbox(account).Messages[1].Filename);
         ClassicAssert.IsFalse(HasMagic(rewritten), "A message saved through COM is still a container.");
         StringAssert.Contains("X-Rewritten: through COM", Encoding.ASCII.GetString(rewritten));
         StringAssert.Contains(attachment, Encoding.ASCII.GetString(rewritten), "The rewrite lost the attachment it held once.");
         ClassicAssert.IsTrue(File.Exists(blob), "The first message still needs the blob, and it went.");

         // The first expunged: the last name that needed the blob, which goes with it.
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.IsTrue(imap.SetDeletedFlag(1));
         ClassicAssert.IsTrue(imap.Expunge());
         imap.Logout();
         imap.Disconnect();
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         WaitFor(() => !File.Exists(blob), "The blob outlived the last message that named it: " + blob);
         StringAssert.Contains(attachment, FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]"), "The rewritten message lost its attachment.");

         // Negative control: a part under the minimum stays in the container.
         string small = Attachment(100, 22);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Small", TextBody("smallword", 50), small));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 2);
         byte[] smallFile = File.ReadAllBytes(Inbox(account).Messages[1].Filename);
         ClassicAssert.IsTrue(HasMagic(smallFile), "The small message was not compressed.");
         ClassicAssert.IsEmpty(ReferencesOf(smallFile), "A 100 KB part was held once under a 256 KB minimum.");
         ClassicAssert.IsFalse(File.Exists(BlobPath(Sha256Hex(small))));
      }

      [Test]
      [Description("A part held once is kept through an IMAP COPY and a mailbox move - both copies read back exactly, and the consistency check finds no message missing a part and no part nothing needs - and goes when the last copy does. Negative control: a blob nothing names is listed by the check.")]
      public void APartHeldOnceIsKeptThroughACopyAMoveAndTheConsistencyCheck()
      {
         SetCompression(true, 256);
         Account account = AddAccount("mover");

         string attachment = Attachment(300, 31);
         string blob = BlobPath(Sha256Hex(attachment));
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Moved", TextBody("moveword", 50), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         string original = FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]");

         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(account.Address, Password));
         ClassicAssert.IsTrue(imap.CreateFolder("Kept"));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         ClassicAssert.IsTrue(imap.Copy(1, "Kept"));
         imap.Logout();
         imap.Disconnect();
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "Kept", 1);

         // The move: the mailbox renamed, its files carried under the new name.
         // The address the account had, taken before the move: MoveTo updates the
         // object it is called on, so account.Address after it is the new one, and
         // removing that left the old address for the teardown to look up - and
         // not find (the "MessageStoreFormat teardown: ... DISP_E_BADINDEX" line
         // in the gate logs).
         string before = account.Address;
         string moved = "moved-" + before;
         WaitUntilMovable(account);
         string summary = (string) ((dynamic) account).MoveTo(moved, false);
         StringAssert.Contains("\"new_address\":\"" + moved + "\"", summary, summary);
         accounts_.Remove(before);
         accounts_.Add(moved);

         ClassicAssert.AreEqual(original, FetchLiteral(moved, "INBOX", 1, "BODY.PEEK[]"), "The moved inbox copy does not read back exactly.");
         ClassicAssert.AreEqual(original, FetchLiteral(moved, "Kept", 1, "BODY.PEEK[]"), "The copy does not read back exactly.");
         ClassicAssert.IsTrue(File.Exists(blob), "The blob did not survive the copy and the move.");

         // The consistency check: none of this mailbox's messages is missing a file
         // or a part, and no blob is one nothing needs.
         string report = ConsistencyReport(null);
         StringAssert.DoesNotContain(moved, report, report);
         StringAssert.Contains("# Blobs nothing references: 0", report, report);

         // Negative control: a blob no message names is listed, and left.
         string stray = Sha256Hex("a blob no message names " + Guid.NewGuid());
         string strayPath = BlobPath(stray);
         Directory.CreateDirectory(Path.GetDirectoryName(strayPath));
         File.WriteAllText(strayPath, "stray");
         putBack_.Add(() => File.Delete(strayPath));
         report = ConsistencyReport(stray);
         StringAssert.Contains("# Blobs nothing references: 1", report, report);
         StringAssert.Contains("blob\t" + stray, report, report);
         ClassicAssert.IsTrue(File.Exists(strayPath), "The check deleted what it reported.");
         File.Delete(strayPath);

         // One copy gone: the other still needs the blob. Both gone: so is the blob.
         ExpungeFirst(moved, "INBOX");
         ClassicAssert.IsTrue(File.Exists(blob), "The blob went while the copy in Kept still named it.");
         ClassicAssert.AreEqual(original, FetchLiteral(moved, "Kept", 1, "BODY.PEEK[]"));
         ExpungeFirst(moved, "Kept");
         WaitFor(() => !File.Exists(blob), "The blob outlived both copies: " + blob);
      }

      [Test]
      [Description("With DeliveryHardLinks on, a message to three local recipients is one container with three names, each reads back the same, and the blob stays until the last of the three is expunged - so the count is three names and no fewer. Negative control: with the links off each recipient's copy is a container of its own, and the attachment is still held once.")]
      public void DeliveredToThreeRecipientsWithHardLinksTheContainerIsOneFile()
      {
         SetCompression(true, 256);
         Account one = AddAccount("linked-one");
         Account two = AddAccount("linked-two");
         Account three = AddAccount("linked-three");
         var recipients = new List<string> { one.Address, two.Address, three.Address };

         SetHardLinks(true);
         string attachment = Attachment(300, 41);
         string linkedBlob = BlobPath(Sha256Hex(attachment));
         new SmtpClientSimulator().SendRaw("sender@example.test", recipients, Message(one.Address, "Linked", TextBody("linkword", 200), attachment));
         foreach (string address in recipients)
            ImapClientSimulator.AssertMessageCount(address, Password, "INBOX", 1);

         var first = Identify(Inbox(one).Messages[0].Filename);
         foreach (Account account in new[] { one, two, three })
         {
            string fileName = Inbox(account).Messages[0].Filename;
            var identity = Identify(fileName);
            ClassicAssert.AreEqual(first.identity, identity.identity, "Every recipient's name is one file: " + fileName);
            ClassicAssert.AreEqual(3u, identity.links, "Three names, one container, no template left: " + fileName);
            ClassicAssert.IsTrue(HasMagic(File.ReadAllBytes(fileName)), "The shared file is not a container.");
         }

         string fetched = FetchLiteral(one.Address, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.AreEqual(fetched, FetchLiteral(two.Address, "INBOX", 1, "BODY.PEEK[]"));
         ClassicAssert.AreEqual(fetched, FetchLiteral(three.Address, "INBOX", 1, "BODY.PEEK[]"));
         StringAssert.Contains(attachment, fetched);

         // The count is three names: two recipients expunge their copy and the
         // blob stays, the third expunges and it goes. A count too low - the
         // link arithmetic wrong by one - would take the blob at the first or
         // the second expunge, and the survivors would not read back.
         ClassicAssert.IsTrue(File.Exists(linkedBlob), "The linked attachment was not held once.");
         ExpungeFirst(one.Address, "INBOX");
         ClassicAssert.IsTrue(File.Exists(linkedBlob), "The blob went with the first of three names.");
         ExpungeFirst(two.Address, "INBOX");
         ClassicAssert.IsTrue(File.Exists(linkedBlob), "The blob went with the second of three names.");
         StringAssert.Contains(attachment, FetchLiteral(three.Address, "INBOX", 1, "BODY.PEEK[]"), "The last recipient's copy no longer reads back.");
         ExpungeFirst(three.Address, "INBOX");
         WaitFor(() => !File.Exists(linkedBlob), "The blob outlived the last of the three names: " + linkedBlob);

         // Negative control: the links off, three files, one blob.
         SetHardLinks(false);
         new SmtpClientSimulator().SendRaw("sender@example.test", recipients, Message(one.Address, "Copies", TextBody("copyword", 200), attachment));
         // Each inbox is empty again - the three linked copies were expunged above.
         foreach (string address in recipients)
            ImapClientSimulator.AssertMessageCount(address, Password, "INBOX", 1);

         var identities = new HashSet<string>();
         foreach (Account account in new[] { one, two, three })
         {
            string fileName = Inbox(account).Messages[0].Filename;
            var identity = Identify(fileName);
            ClassicAssert.AreEqual(1u, identity.links, "A copy of its own: " + fileName);
            identities.Add(identity.identity);
            byte[] file = File.ReadAllBytes(fileName);
            CollectionAssert.AreEqual(new[] { Sha256Hex(attachment) }, ReferencesOf(file), "The copy does not name the one blob: " + fileName);
         }
         ClassicAssert.AreEqual(3, identities.Count, "Three distinct files.");
         ClassicAssert.AreEqual(1, Directory.GetFiles(Path.Combine(dataDirectory_, "Blobs"), "*", SearchOption.AllDirectories).Length,
            "Three copies of one attachment are more than one blob.");
      }

      // ---- the walk, from the command line ---------------------------------------------

      [Test]
      [Description("hmailserver --measure-store --account reports beside the running service and changes nothing; --compress-store is refused beside it (exit 4) and, with the service stopped, rewrites the account's files in place - a file shared by DeliveryHardLinks with another account's name left as one file - and --decompress puts every file back byte for byte, letting go of the blob. Negative control: the refused run and the measure change no byte.")]
      public void CompressStoreRewritesAnAccountInPlaceAndDecompressPutsItBack()
      {
         if (TestSetup.ConsoleServer)
            Assert.Ignore("The server under test is a console server, which the walk's service check cannot see stopped.");

         SetCompression(false, 256);
         SetHardLinks(true);

         Account walked = AddAccount("walked");
         Account other = AddAccount("walked-other");
         string attachment = Attachment(300, 51);

         SmtpClientSimulator.StaticSendRaw("sender@example.test", walked.Address, Message(walked.Address, "With a part", TextBody("walkone", 300), attachment));
         ImapClientSimulator.AssertMessageCount(walked.Address, Password, "INBOX", 1);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", walked.Address, Message(walked.Address, "Text", TextBody("walktwo", 300), null));
         ImapClientSimulator.AssertMessageCount(walked.Address, Password, "INBOX", 2);
         new SmtpClientSimulator().SendRaw("sender@example.test", new List<string> { walked.Address, other.Address },
            Message(walked.Address, "Shared", TextBody("walkthree", 300), null));
         ImapClientSimulator.AssertMessageCount(walked.Address, Password, "INBOX", 3);
         ImapClientSimulator.AssertMessageCount(other.Address, Password, "INBOX", 1);

         var files = new List<string>();
         for (int i = 0; i < 3; i++)
            files.Add(Inbox(walked).Messages[i].Filename);
         string shared = files[2];
         var sharedBefore = Identify(shared);
         ClassicAssert.AreEqual(2u, sharedBefore.links, "The message to both accounts is not one file with two names.");
         List<byte[]> before = files.Select(File.ReadAllBytes).ToList();

         // Asked while the service can still answer where it is.
         ServerExecutable();

         Verb measure = RunVerb("--measure-store --account " + walked.Address);
         ClassicAssert.AreEqual(0, measure.ExitCode, measure.Output);
         StringAssert.Contains("nothing was changed", measure.Output);
         AssertUnchanged(files, before, "--measure-store changed a file");

         Verb refused = RunVerb("--compress-store --account " + walked.Address);
         ClassicAssert.AreEqual(4, refused.ExitCode, refused.Output);
         StringAssert.Contains("service is running", refused.Output);
         AssertUnchanged(files, before, "A refused --compress-store changed a file");

         // The COM objects belong to the running service; the service stops and
         // is started again below, so what is needed of them is read now.
         string walkedAddress = walked.Address;
         string domainName = _domain.Name;

         StopService();
         try
         {
            Verb compress = RunVerb("--compress-store --account " + walkedAddress);
            ClassicAssert.AreEqual(0, compress.ExitCode, compress.Output);
            StringAssert.Contains("Compressed 2 of 3 stored files in " + walkedAddress, compress.Output);
            StringAssert.Contains("1 shared with names outside the walk and left", compress.Output);

            for (int i = 0; i < 2; i++)
            {
               byte[] file = File.ReadAllBytes(files[i]);
               ClassicAssert.IsTrue(HasMagic(file), "--compress-store left " + files[i] + " as it was.");
               CollectionAssert.AreEqual(before[i], Decode(file), "The rewritten " + files[i] + " does not hold the message it was.");
            }
            CollectionAssert.AreEqual(new[] { Sha256Hex(attachment) }, ReferencesOf(File.ReadAllBytes(files[0])));
            ClassicAssert.IsTrue(File.Exists(BlobPath(Sha256Hex(attachment))), "The walk did not hold the attachment once.");

            var sharedAfter = Identify(shared);
            ClassicAssert.AreEqual(sharedBefore.identity, sharedAfter.identity, "The shared file was split into a copy per name.");
            ClassicAssert.AreEqual(2u, sharedAfter.links);
            CollectionAssert.AreEqual(before[2], File.ReadAllBytes(shared), "The file shared with another account's name was rewritten.");

            Verb decompress = RunVerb("--compress-store --decompress --account " + walkedAddress);
            ClassicAssert.AreEqual(0, decompress.ExitCode, decompress.Output);
            StringAssert.Contains("Decompressed 2 of 3 stored files", decompress.Output);
            AssertUnchanged(files, before, "--decompress did not put a file back byte for byte");
            ClassicAssert.IsFalse(File.Exists(BlobPath(Sha256Hex(attachment))), "--decompress kept a blob nothing names.");

            // --domain covers the shared file, whose two names are both in the
            // walk: it is rewritten once and both names relinked to the new one,
            // never split into a copy per name. This is the walk's multi-name
            // relink branch, which --account leaves untouched.
            Verb byDomain = RunVerb("--compress-store --domain " + domainName);
            ClassicAssert.AreEqual(0, byDomain.ExitCode, byDomain.Output);
            // The summary always counts them; the domain walk holds both names, so
            // none is left for a name outside it.
            StringAssert.Contains(" 0 shared with names outside the walk", byDomain.Output);

            var sharedByDomain = Identify(shared);
            ClassicAssert.AreEqual(sharedBefore.identity, sharedByDomain.identity, "The shared file was split into a copy per name by the domain walk.");
            ClassicAssert.AreEqual(2u, sharedByDomain.links, "The shared file lost or gained a name in the domain walk.");
            ClassicAssert.IsTrue(HasMagic(File.ReadAllBytes(shared)), "The domain walk did not compress the shared file.");
            CollectionAssert.AreEqual(before[2], Decode(File.ReadAllBytes(shared)), "The rewritten shared file does not hold the message it was.");

            // Put it back, so the fixture leaves the shared file as it found it.
            RunVerb("--compress-store --decompress --domain " + domainName);
            AssertUnchanged(new List<string> { shared }, new List<byte[]> { before[2] }, "--decompress --domain did not put the shared file back");
         }
         finally
         {
            RestartServerAndReacquireCom();
         }

         StringAssert.Contains(attachment, FetchLiteral(walkedAddress, "INBOX", 1, "BODY.PEEK[]"), "The server does not read the store the walk left.");
      }

      [Test]
      [Description("A message whose row names a file outside the data directory - a full path elsewhere, which the service's own account can write into the database - stops a store walk before it reads or changes a file, naming the message: the walks run as an administrator on Windows, and the names they rewrite, remove and copy over are the rows'. --measure-store, which runs beside the service, shows it; every walk lists its files the same way (MessageStoreMaintenance::ListStoredFiles). Negative control: the same message under its own name is measured.")]
      public void AWalkTouchesNoFileARowNamesOutsideTheDataDirectory()
      {
         Account account = AddAccount("outside");
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Outside", TextBody("outsideword", 50), null));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         hMailServer.Message message = Inbox(account).Messages[0];
         string ownName = Path.GetFileName(message.Filename);
         long messageId = message.ID;

         ServerExecutable();

         Verb measured = RunVerb("--measure-store --account " + account.Address);
         ClassicAssert.AreEqual(0, measured.ExitCode, measured.Output);

         string outside = Path.Combine(Path.GetTempPath(), "hm-outside-" + Guid.NewGuid().ToString("N") + ".eml");
         File.Copy(message.Filename, outside);
         try
         {
            _application.Database.ExecuteSQL("update hm_messages set messagefilename = '" + outside.Replace("'", "''") + "' where messageid = " + messageId);

            Verb refused = RunVerb("--measure-store --account " + account.Address);
            ClassicAssert.AreNotEqual(0, refused.ExitCode, refused.Output);
            StringAssert.Contains("message " + messageId + " names the file " + outside, refused.Output);
            StringAssert.Contains("not in the data directory", refused.Output);
            StringAssert.DoesNotContain("Measured", refused.Output, "The walk went on past a row that names a file outside the data directory.");
         }
         finally
         {
            _application.Database.ExecuteSQL("update hm_messages set messagefilename = '" + ownName.Replace("'", "''") + "' where messageid = " + messageId);
            File.Delete(outside);
         }
      }

      // ---- damage --------------------------------------------------------------------

      [Test]
      [Description("A container cut short, and one whose SHA-256 does not match what it gives back, are refused - FETCH hands out no literal at all, neither the message nor the container's bytes nor half a message, its ENVELOPE too is refused, and the error log names the file (HM7350). Negative control: the same container put back whole is served, exactly.")]
      public void ADamagedContainerIsRefusedWithTheReasonAndNeverServed()
      {
         SetCompression(true, 256);
         Account account = AddAccount("damaged");

         string unique = "wombat" + Guid.NewGuid().ToString("N").Substring(0, 8);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Damaged", TextBody(unique, 500), null));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);

         string fileName = Inbox(account).Messages[0].Filename;
         string leaf = Path.GetFileName(fileName);
         byte[] whole = File.ReadAllBytes(fileName);
         ClassicAssert.IsTrue(HasMagic(whole));
         string original = Encoding.ASCII.GetString(Decode(whole));
         int headerLength = (int) BitConverter.ToUInt32(whole, 12);

         // Cut short inside its body: no literal, and the file named in the log.
         File.WriteAllBytes(fileName, whole.Take(headerLength + (whole.Length - headerLength) / 2).ToArray());
         string response;
         string fetched = FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]", out response);
         ClassicAssert.IsNull(fetched, "A container cut short was served, in whole or in part: " + response);
         StringAssert.DoesNotContain("HMS\r\n", response, "The container's own bytes were handed out.");
         StringAssert.DoesNotContain(" OK ", response, "BODY[] of a damaged container was answered OK.");
         StringAssert.Contains(" NO ", response, "BODY[] of a damaged container was not answered NO: " + response);
         CustomAsserts.AssertReportedError("HM7350", "could not be read", leaf);

         // The header-only readers refuse it too: FETCH ENVELOPE reads the
         // header alone (PersistentMessage::LoadHeader), and a damaged body is
         // caught there as well - the deflate stream is read to its end - so the
         // subject is not handed out from a container whose body will not decode.
         string damagedEnvelope = FetchResponseRaw(account.Address, "INBOX", 1, "ENVELOPE");
         StringAssert.DoesNotContain("Damaged", damagedEnvelope, "ENVELOPE served the header of a damaged container: " + damagedEnvelope);

         // Negative control: whole again, served exactly - and its ENVELOPE too.
         File.WriteAllBytes(fileName, whole);
         ClassicAssert.AreEqual(original, FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]"), "The container put back whole is not served.");
         StringAssert.Contains("Damaged", FetchResponseRaw(account.Address, "INBOX", 1, "ENVELOPE"), "ENVELOPE of the whole container does not carry the subject.");

         // A SHA-256 that is not the one of what the container gives back.
         byte[] wrongHash = (byte[]) whole.Clone();
         wrongHash[24] ^= 0x55;
         File.WriteAllBytes(fileName, wrongHash);
         fetched = FetchLiteral(account.Address, "INBOX", 1, "BODY.PEEK[]", out response);
         ClassicAssert.IsNull(fetched, "A container whose hash does not match was served: " + response);
         CustomAsserts.AssertReportedError("HM7350", "does not match the SHA-256", leaf);

         File.WriteAllBytes(fileName, whole);
      }

      [Test]
      [Description("One damaged container does not stop a FETCH over the folder at itself: it is left out, the other messages are fetched, the command ends NO, and the error log names the damaged file (HM7350) and holds nothing else. Negative control: with the container whole again the same FETCH answers both and OK, and logs no error.")]
      public void ADamagedContainerIsLeftOutOfARangeAndTheRestIsFetched()
      {
         SetCompression(true, 256);
         Account account = AddAccount("damagedrange");

         string unique = "quokka" + Guid.NewGuid().ToString("N").Substring(0, 8);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "FirstDamaged", TextBody(unique, 500), null));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "SecondWhole", TextBody(unique, 500), null));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 2);

         string fileName = Inbox(account).Messages[0].Filename;
         string leaf = Path.GetFileName(fileName);
         byte[] whole = File.ReadAllBytes(fileName);
         ClassicAssert.IsTrue(HasMagic(whole));
         int headerLength = (int) BitConverter.ToUInt32(whole, 12);

         try
         {
            File.WriteAllBytes(fileName, whole.Take(headerLength + (whole.Length - headerLength) / 2).ToArray());

            string response = FetchRange(account.Address, "1:2 (ENVELOPE)");
            StringAssert.DoesNotContain("FirstDamaged", response, "The damaged message was served: " + response);
            StringAssert.Contains("SecondWhole", response, "The message after the damaged one was not fetched - the range stopped at the damage: " + response);
            StringAssert.Contains("F1 NO", response, "A FETCH that left a message out must not end OK: " + response);

            // The store reports the damage as it does for a FETCH of that message
            // alone, naming the file - and the report is this test's to take: left
            // in the error log, it failed the SetUp of the next test to run
            // (PerformBasicSetup's AssertNoReportedError), which is not the test
            // that provoked it.
            //
            // Taken whole and read line by line, not with AssertReportedError
            // alone, which deletes the log once its words are there: that would
            // also delete, unread, anything else the range logged - a critical
            // error of the connection's catch-all for a throw that reached it, or
            // IMAP FETCH's HM10001 - which the next SetUp would otherwise have
            // seen. The damaged file's HM7350 is the one thing the range may log.
            string reported = null;
            try
            {
               RetryHelper.TryAction(TimeSpan.FromSeconds(10), () =>
               {
                  reported = LogHandler.ReadErrorLog();
                  RetryableAssert.StringContains(leaf, reported);
               });
            }
            finally
            {
               LogHandler.DeleteErrorLog();
            }

            StringAssert.Contains("could not be read", reported, reported);
            foreach (string line in reported.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).Where(l => l.Trim().Length > 0))
            {
               StringAssert.Contains("Code: HM7350,", line, "The range logged an error other than the damaged container's HM7350: " + line);
               StringAssert.Contains(leaf, line, "The range logged an HM7350 for a file other than the damaged one: " + line);
            }

            // Negative control: whole again, both and OK, and nothing reported.
            File.WriteAllBytes(fileName, whole);
            response = FetchRange(account.Address, "1:2 (ENVELOPE)");
            StringAssert.Contains("FirstDamaged", response);
            StringAssert.Contains("SecondWhole", response);
            StringAssert.Contains("F1 OK", response);
            CustomAsserts.AssertNoReportedError();
         }
         finally
         {
            File.WriteAllBytes(fileName, whole);
         }
      }

      private static string FetchRange(string address, string arguments)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         string response = imap.SendSingleCommand("F1 FETCH " + arguments);
         imap.Disconnect();
         return response;
      }

      // ---- backup ----------------------------------------------------------------------

      [Test]
      [Description("A backup with messages carries the containers and the blobs; a full restore after the account and its blob are gone puts both back, the message reads back exactly, and the count is rebuilt - the blob goes with the restored message. Negative control: deleting the account let the blob go before the restore.")]
      public void ABackupAndARestoreCarryTheContainersAndThePartsHeldOnce()
      {
         SetCompression(true, 256);
         Account account = AddAccount("backedup");
         string address = account.Address;

         string attachment = Attachment(300, 61);
         string blob = BlobPath(Sha256Hex(attachment));
         SmtpClientSimulator.StaticSendRaw("sender@example.test", address, Message(address, "Backed up", TextBody("backupword", 100), attachment));
         ImapClientSimulator.AssertMessageCount(address, Password, "INBOX", 1);
         string original = FetchLiteral(address, "INBOX", 1, "BODY.PEEK[]");
         ClassicAssert.IsTrue(File.Exists(blob));

         string backupDirectory = Path.Combine(Path.GetTempPath(), "hm-store-" + TestSetup.UniqueString());
         Directory.CreateDirectory(backupDirectory);
         putBack_.Add(() => Directory.Delete(backupDirectory, true));

         var backup = _application.Settings.Backup;
         string destination = backup.Destination;
         bool domains = backup.BackupDomains, messages = backup.BackupMessages, settings = backup.BackupSettings, compressed = backup.CompressDestinationFiles;
         putBack_.Add(() =>
         {
            var current = _application.Settings.Backup;
            current.Destination = destination;
            current.BackupDomains = domains;
            current.BackupMessages = messages;
            current.BackupSettings = settings;
            current.CompressDestinationFiles = compressed;
         });

         backup.Destination = backupDirectory;
         backup.BackupDomains = true;
         backup.BackupMessages = true;
         backup.BackupSettings = false;
         backup.CompressDestinationFiles = true;

         string log = backup.LogFile;
         if (File.Exists(log))
            File.Delete(log);
         _application.BackupManager.StartBackup();
         WaitFor(() => LogSays(log, "Backup completed successfully"), "The backup did not complete; its log is " + log, 60);
         ClassicAssert.IsFalse(LogSays(log, "BACKUP ERROR:"), "The backup reported an error; its log is " + log);

         // The account deleted: its message goes, and with it the only name of the blob.
         _application.Domains.get_ItemByName(_domain.Name).Accounts.get_ItemByAddress(address).Delete();
         accounts_.Remove(address);
         WaitFor(() => !File.Exists(blob), "Deleting the account kept a blob nothing names.");

         string startTime = _application.Status.StartTime;
         string archive = new DirectoryInfo(backupDirectory).GetFiles()[0].FullName;
         var restore = _application.BackupManager.LoadBackup(archive);
         restore.RestoreDomains = true;
         restore.RestoreMessages = true;
         restore.RestoreSettings = false;
         restore.StartRestore();
         WaitFor(() =>
         {
            try
            {
               string now = _application.Status.StartTime;
               return now.Length > 0 && now != startTime;
            }
            catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
            {
               return false;   // the server is between its two configurations
            }
         }, "The restore did not complete.", 90);

         accounts_.Add(address);
         ClassicAssert.IsTrue(File.Exists(blob), "The restore did not bring the blob back.");
         ClassicAssert.AreEqual(original, FetchLiteral(address, "INBOX", 1, "BODY.PEEK[]"), "The restored message does not read back exactly.");

         // The count was rebuilt: the restored message is the blob's one name.
         ExpungeFirst(address, "INBOX");
         WaitFor(() => !File.Exists(blob), "After the restore the blob outlived the one message that named it: its count was not rebuilt.");
      }

      // ---- what the store says ---------------------------------------------------------

      [Test]
      [Description("GET /api/v1/store answers the last measurement - measured on the server's own time - with the two settings beside it, which follow a PUT. Negative controls: no credentials is 401, and a minimum out of range is refused with the setting unchanged.")]
      public void TheStoreSaysWhatItHoldsAndWhatItSaves()
      {
         SetCompression(false, 256);

         JsonElement store = default;
         WaitFor(() =>
         {
            (int status, string body) answer = Admin("GET", "/api/v1/store", null);
            ClassicAssert.AreEqual(200, answer.status, answer.body);
            store = JsonDocument.Parse(answer.body).RootElement.Clone();
            return store.GetProperty("measured").GetInt64() > 0;
         }, "The store was never measured.", 60);

         foreach (string key in new[] { "messages", "logical_bytes", "stored_bytes", "compressed_files", "blobs", "blob_bytes", "blob_references", "saved_bytes" })
            ClassicAssert.AreEqual(JsonValueKind.Number, store.GetProperty(key).ValueKind, key);
         ClassicAssert.AreEqual(JsonValueKind.False, store.GetProperty("compression").ValueKind);
         ClassicAssert.AreEqual(256, store.GetProperty("dedup_min_kb").GetInt32());

         SetCompression(true, 512);
         JsonElement after = Json(Admin("GET", "/api/v1/store", null), 200);
         ClassicAssert.AreEqual(JsonValueKind.True, after.GetProperty("compression").ValueKind);
         ClassicAssert.AreEqual(512, after.GetProperty("dedup_min_kb").GetInt32());

         (int status, string body) outOfRange = Admin("PUT", "/api/v1/settings", "{\"message_store_dedup_min_kb\":70000}");
         ClassicAssert.AreEqual(400, outOfRange.status, outOfRange.body);
         ClassicAssert.AreEqual(512, Json(Admin("GET", "/api/v1/store", null), 200).GetProperty("dedup_min_kb").GetInt32(), "A refused value was stored.");

         (int status, string body) anonymous = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/store", null, null);
         ClassicAssert.AreEqual(401, anonymous.status, anonymous.body);

         // A key confined to named domains is refused: the store is every
         // domain's. The key is created and revoked here.
         (int status, string body) created = Admin("POST", "/api/v1/apikeys", "{\"label\":\"store - one domain\",\"scope\":\"full\",\"domains\":\"" + _domain.Name + "\"}");
         ClassicAssert.AreEqual(201, created.status, created.body);
         string keyId = JsonDocument.Parse(created.body).RootElement.GetProperty("id").GetString();
         string token = JsonDocument.Parse(created.body).RootElement.GetProperty("key").GetString();
         try
         {
            (int status, string body) confined = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/store", "Bearer " + token, null);
            ClassicAssert.AreEqual(403, confined.status, "A domain-restricted key was answered the server-wide store: " + confined.body);
            (int status, string body) confinedMeasure = RestRequest.Send("127.0.0.1", RestPort, "POST", "/api/v1/store/measure", "Bearer " + token, "{}");
            ClassicAssert.AreEqual(403, confinedMeasure.status, confinedMeasure.body);
         }
         finally
         {
            Admin("DELETE", "/api/v1/apikeys/" + keyId, null);
         }
      }

      [Test]
      [Description("After two compressed messages that share a 300 KB part, POST /api/v1/store/measure forces a measurement whose stored bytes are below the messages' size, whose blobs are at least one, and whose saving is positive; COM's Settings.MessageStoreStatus is the same JSON, and the two COM settings put and get and refuse a value out of range. Negative control: with compression off there is no saving to claim.")]
      public void TheMeasurementAndTheComSettingsSayWhatIsSaved()
      {
         SetCompression(true, 256);
         Account account = AddAccount("measured");

         string attachment = Attachment(300, 71);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "One", TextBody("measureone", 400), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 1);
         SmtpClientSimulator.StaticSendRaw("sender@example.test", account.Address, Message(account.Address, "Two", TextBody("measuretwo", 400), attachment));
         ImapClientSimulator.AssertMessageCount(account.Address, Password, "INBOX", 2);

         // Forced now, not waited for: POST measures on the server's own queue,
         // and the answer follows once measuring goes back to false with a fresh
         // measured time.
         JsonElement measured = default;
         long before = Json(Admin("GET", "/api/v1/store", null), 200).GetProperty("measured").GetInt64();
         WaitFor(() =>
         {
            (int status, string body) started = Admin("POST", "/api/v1/store/measure", "{}");
            ClassicAssert.AreEqual(202, started.status, started.body);
            (int status, string body) now = Admin("GET", "/api/v1/store", null);
            ClassicAssert.AreEqual(200, now.status, now.body);
            measured = JsonDocument.Parse(now.body).RootElement.Clone();
            return measured.GetProperty("measured").GetInt64() > before && !measured.GetProperty("measuring").GetBoolean();
         }, "A forced measurement never landed.", 60);

         ClassicAssert.GreaterOrEqual(measured.GetProperty("compressed_files").GetInt64(), 2, "The two compressed messages were not counted.");
         ClassicAssert.GreaterOrEqual(measured.GetProperty("blobs").GetInt64(), 1, "The shared part was not counted as a blob.");
         ClassicAssert.Less(measured.GetProperty("stored_bytes").GetInt64(), measured.GetProperty("logical_bytes").GetInt64(), "The files are not smaller than the messages.");
         ClassicAssert.Greater(measured.GetProperty("saved_bytes").GetInt64(), 0, "Nothing was saved by two shared compressed messages.");

         // COM's Settings.MessageStoreStatus is the same JSON.
         dynamic settings = _application.Settings;
         string comJson = (string) settings.MessageStoreStatus;
         JsonElement com = JsonDocument.Parse(comJson).RootElement;
         foreach (string key in new[] { "messages", "logical_bytes", "stored_bytes", "compressed_files", "blobs", "saved_bytes" })
            ClassicAssert.AreEqual(JsonValueKind.Number, com.GetProperty(key).ValueKind, key);

         // The two settings, put and got over COM, and a value out of range refused.
         settings.MessageStoreCompression = false;
         ClassicAssert.IsFalse((bool) settings.MessageStoreCompression);
         settings.MessageStoreCompression = true;
         ClassicAssert.IsTrue((bool) settings.MessageStoreCompression);
         settings.MessageStoreDedupMinKB = 128;
         ClassicAssert.AreEqual(128, (int) settings.MessageStoreDedupMinKB);
         ClassicAssert.Throws<System.Runtime.InteropServices.COMException>(() => { settings.MessageStoreDedupMinKB = 65537; },
            "A dedup minimum above 65536 was accepted over COM.");
         ClassicAssert.AreEqual(128, (int) settings.MessageStoreDedupMinKB, "The refused COM value was stored.");

         // Negative control: compression off, and there is no saving to claim.
         for (int i = 0; i < 2; i++)
            ExpungeFirst(account.Address, "INBOX");
         SetCompression(false, 256);
      }

      [Test]
      [Description("build/check-message-store-seam.py passes on this checkout, and proves its own negative controls as it runs: a stored file's name asked for in a file it does not list, one more call in a listed file and a size taken from a file are each refused, while the same call in a comment or a string is not.")]
      public void TheSeamCheckerPassesAndRefusesAReadAroundTheStore()
      {
         string python = FindPython();
         if (python == null)
            Assert.Ignore("No python.exe on the PATH, so the checker cannot be run here.");

         string checker = RepositoryFile(Path.Combine("build", "check-message-store-seam.py"));
         (int code, string output) run = RunPython(python, "\"" + checker + "\"");
         ClassicAssert.AreEqual(0, run.code, run.output);
         StringAssert.Contains("the negative controls", run.output);
         StringAssert.Contains("are refused while a call in a comment or a string is not", run.output);
      }

      // ---- the container, read from the doc ----------------------------------------------

      private static bool HasMagic(byte[] file)
         => file.Length >= Magic.Length && Magic.SequenceEqual(file.Take(Magic.Length));

      private sealed class Container
      {
         public int HeaderLength;
         public long OriginalSize;
         public byte[] Sha256;
         public readonly List<int> Stages = new List<int>();
         public readonly List<(long offset, long length, string sha256)> References = new List<(long, long, string)>();
      }

      /// <summary>The header, byte by byte as docs/MessageStore.md lays it out.</summary>
      private static Container Parse(byte[] file)
      {
         ClassicAssert.IsTrue(BitConverter.IsLittleEndian, "This reader assumes a little-endian machine.");
         ClassicAssert.IsTrue(HasMagic(file), "Not a container.");
         ClassicAssert.AreEqual(1, file[8], "The version.");
         ClassicAssert.AreEqual(0, BitConverter.ToUInt16(file, 10), "The reserved bytes.");

         var container = new Container
         {
            HeaderLength = (int) BitConverter.ToUInt32(file, 12),
            OriginalSize = (long) BitConverter.ToUInt64(file, 16),
            Sha256 = file.Skip(24).Take(32).ToArray()
         };

         int at = 56;
         int stages = file[at++];
         for (int i = 0; i < stages; i++)
         {
            container.Stages.Add(file[at]);
            at += 2 + file[at + 1];
         }

         uint references = BitConverter.ToUInt32(file, at);
         at += 4;
         for (uint i = 0; i < references; i++)
         {
            container.References.Add(((long) BitConverter.ToUInt64(file, at), (long) BitConverter.ToUInt64(file, at + 8), Hex(file.Skip(at + 16).Take(32).ToArray())));
            at += 48;
         }

         ClassicAssert.AreEqual(references > 0 ? 1 : 0, file[9] & 1, "Flag bit 0 says whether references follow.");
         ClassicAssert.LessOrEqual(at, container.HeaderLength, "The header's length is shorter than what it holds.");
         return container;
      }

      private static List<string> ReferencesOf(byte[] file)
         => HasMagic(file) ? Parse(file).References.Select(r => r.sha256).ToList() : new List<string>();

      /// <summary>
      ///    The original bytes of a container: the body put back through its stages
      ///    in reverse, the referenced parts read from their blobs and put back where
      ///    the references say, and the whole checked against the size and the
      ///    SHA-256 the header carries.
      /// </summary>
      private byte[] Decode(byte[] file)
      {
         Container container = Parse(file);
         byte[] rest = file.Skip(container.HeaderLength).ToArray();

         for (int i = container.Stages.Count - 1; i >= 0; i--)
         {
            ClassicAssert.AreEqual(1, container.Stages[i], "Deflate is the one stage this row writes.");
            rest = Inflate(rest);
         }

         using (var original = new MemoryStream())
         {
            long position = 0;
            int consumed = 0;
            foreach ((long offset, long length, string sha256) reference in container.References)
            {
               int take = (int) (reference.offset - position);
               original.Write(rest, consumed, take);
               consumed += take;

               byte[] blob = File.ReadAllBytes(BlobPath(reference.sha256));
               ClassicAssert.AreEqual(reference.length, blob.Length, "The blob is not the length its reference names.");
               ClassicAssert.AreEqual(reference.sha256, Sha256Hex(blob), "The blob's bytes are not the ones its name hashes.");
               original.Write(blob, 0, blob.Length);
               position = reference.offset + reference.length;
            }
            original.Write(rest, consumed, rest.Length - consumed);

            byte[] result = original.ToArray();
            ClassicAssert.AreEqual(container.OriginalSize, result.Length, "The container gives back another size than its header says.");
            CollectionAssert.AreEqual(container.Sha256, Sha256(result), "The container gives back other bytes than its SHA-256 says.");
            return result;
         }
      }

      // RFC 1950: two header bytes, the DEFLATE stream, the Adler-32. .NET
      // Framework has no reader for the wrapper, so it comes off here.
      private static byte[] Inflate(byte[] bytes)
      {
         ClassicAssert.IsTrue(bytes.Length > 6, "A zlib stream is a header, a body and a checksum.");
         ClassicAssert.AreEqual(8, bytes[0] & 0x0f, "The low nibble of a zlib header names the method: 8 is DEFLATE.");
         ClassicAssert.AreEqual(0, ((bytes[0] << 8) + bytes[1]) % 31, "The two header bytes of a zlib stream are a multiple of 31.");

         using (var input = new MemoryStream(bytes, 2, bytes.Length - 6))
         using (var raw = new DeflateStream(input, CompressionMode.Decompress))
         using (var output = new MemoryStream())
         {
            raw.CopyTo(output);
            return output.ToArray();
         }
      }

      /// <summary>
      ///    A container by this row's own rules, from the doc's table: the magic,
      ///    version 1, the references flag, a one-stage-free header naming the one
      ///    part twice - offset 0 and offset length, each the length given - and a
      ///    body of the size the header claims. What a client would APPEND to make
      ///    the store let go of a part it does not own, if the store trusted a
      ///    header from outside. The bytes hash to nothing in particular: the point
      ///    is that they are the client's, stored as they are.
      /// </summary>
      private static byte[] CraftedContainer(string sha256Hex, long length)
      {
         byte[] sha = new byte[32];
         for (int i = 0; i < 32; i++)
            sha[i] = Convert.ToByte(sha256Hex.Substring(i * 2, 2), 16);

         var references = new List<byte>();
         references.AddRange(BitConverter.GetBytes((ulong) 0));       // offset
         references.AddRange(BitConverter.GetBytes((ulong) length));  // length
         references.AddRange(sha);
         references.AddRange(BitConverter.GetBytes((ulong) length));  // offset, in order and non-overlapping
         references.AddRange(BitConverter.GetBytes((ulong) length));  // length
         references.AddRange(sha);

         var header = new List<byte>();
         header.AddRange(Magic);
         header.Add(1);                                               // version
         header.Add(1);                                               // flags: references follow
         header.AddRange(BitConverter.GetBytes((ushort) 0));          // reserved
         header.AddRange(BitConverter.GetBytes((uint) 0));            // header length, filled in below
         header.AddRange(BitConverter.GetBytes((ulong) (length * 2 + 32))); // a small original size, far below the file
         header.AddRange(new byte[32]);                              // a SHA-256 of nothing
         header.Add(0);                                               // no stages
         header.AddRange(BitConverter.GetBytes((uint) 2));            // two references
         header.AddRange(references);

         byte[] length4 = BitConverter.GetBytes((uint) header.Count);
         for (int i = 0; i < 4; i++)
            header[12 + i] = length4[i];

         var whole = new List<byte>(header);
         whole.AddRange(new byte[64]);                               // a little body, so the file is not just a header
         return whole.ToArray();
      }

      private string BlobPath(string sha256)
         => Path.Combine(dataDirectory_, "Blobs", sha256.Substring(0, 2), sha256);

      private static byte[] Sha256(byte[] bytes)
      {
         using (SHA256 sha = SHA256.Create())
            return sha.ComputeHash(bytes);
      }

      private static string Sha256Hex(byte[] bytes) => Hex(Sha256(bytes));

      private static string Sha256Hex(string ascii) => Sha256Hex(Encoding.ASCII.GetBytes(ascii));

      private static string Hex(byte[] bytes)
      {
         var text = new StringBuilder(bytes.Length * 2);
         foreach (byte b in bytes)
            text.Append(b.ToString("x2"));
         return text.ToString();
      }

      // ---- messages ------------------------------------------------------------------

      private static string TextBody(string unique, int lines)
      {
         var text = new StringBuilder();
         for (int i = 0; i < lines; i++)
            text.Append("Line ").Append(i.ToString("D4")).Append(" of a message the store keeps, with the same words on every line of it.\r\n");
         text.Append("The word to search for is ").Append(unique).Append(".\r\n");
         return text.ToString();
      }

      // Random bytes, so that the part does not compress and is the blob's whole
      // bytes; base64 in lines of 76, as a mail client writes it.
      private static string Attachment(int kilobytes, int seed)
      {
         var bytes = new byte[kilobytes * 1024];
         new Random(seed).NextBytes(bytes);
         return Convert.ToBase64String(bytes, Base64FormattingOptions.InsertLineBreaks);
      }

      private static string Message(string to, string subject, string text, string attachment)
      {
         var message = new StringBuilder();
         message.Append("From: sender@example.test\r\n");
         message.Append("To: ").Append(to).Append("\r\n");
         message.Append("Subject: ").Append(subject).Append("\r\n");
         message.Append("Date: Thu, 24 Sep 2026 10:00:00 +0000\r\n");
         message.Append("Message-ID: <").Append(Guid.NewGuid().ToString("N")).Append("@example.test>\r\n");
         message.Append("MIME-Version: 1.0\r\n");

         if (attachment == null)
         {
            message.Append("Content-Type: text/plain; charset=us-ascii\r\n\r\n");
            message.Append(text);
            return message.ToString();
         }

         message.Append("Content-Type: multipart/mixed; boundary=\"b1\"\r\n\r\n");
         message.Append("--b1\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\n").Append(text).Append("\r\n");
         message.Append("--b1\r\nContent-Type: application/octet-stream; name=\"data.bin\"\r\n");
         message.Append("Content-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"data.bin\"\r\n\r\n");
         message.Append(attachment).Append("\r\n--b1--\r\n");
         return message.ToString();
      }

      // ---- DKIM, signed and verified here (RFC 6376 3.4, 3.5, 3.7) ------------------------

      private const string SignedFields = "from:to:subject:date:message-id";

      /// <summary>The message with a relaxed/relaxed DKIM-Signature of this key at its top.</summary>
      private static string Signed(string message, RSA key)
      {
         int separator = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         string header = message.Substring(0, separator + 2);
         string messageBody = message.Substring(separator + 4);

         string bodyHash;
         using (SHA256 sha = SHA256.Create())
            bodyHash = Convert.ToBase64String(sha.ComputeHash(Encoding.ASCII.GetBytes(CanonicalBody(messageBody, true))));

         string unsigned = "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=example.test; s=store;\r\n" +
                           "\th=" + SignedFields + "; bh=" + bodyHash + "; b=";

         List<KeyValuePair<string, string>> fields = HeaderFields(header);
         var signedText = new StringBuilder();
         foreach (string wanted in SignedFields.Split(':'))
         {
            for (int i = fields.Count - 1; i >= 0; i--)
            {
               if (!string.Equals(fields[i].Key, wanted, StringComparison.OrdinalIgnoreCase))
                  continue;
               signedText.Append(RelaxedHeader(fields[i].Value));
               break;
            }
         }
         string canonical = RelaxedHeader(unsigned + "\r\n");
         signedText.Append(canonical.Substring(0, canonical.Length - 2));

         byte[] signature = key.SignData(Encoding.ASCII.GetBytes(signedText.ToString()), HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
         return unsigned + Convert.ToBase64String(signature) + "\r\n" + message;
      }

      private static bool DkimSignatureVerifies(string message, RSA publicKey, out string why)
      {
         int separator = message.IndexOf("\r\n\r\n", StringComparison.Ordinal);
         string header = separator >= 0 ? message.Substring(0, separator + 2) : message;
         string messageBody = separator >= 0 ? message.Substring(separator + 4) : "";

         List<KeyValuePair<string, string>> fields = HeaderFields(header);
         int at = fields.FindIndex(field => string.Equals(field.Key, "DKIM-Signature", StringComparison.OrdinalIgnoreCase));
         if (at < 0)
         {
            why = "the message carries no DKIM-Signature";
            return false;
         }

         string signatureField = fields[at].Value;
         Dictionary<string, string> tags = Tags(signatureField.Substring(signatureField.IndexOf(':') + 1));
         if (!tags.ContainsKey("bh") || !tags.ContainsKey("b") || !tags.ContainsKey("h"))
         {
            why = "the DKIM-Signature lacks bh=, b= or h=: " + signatureField;
            return false;
         }

         string[] methods = (tags.ContainsKey("c") ? tags["c"] : "simple/simple").Split('/');
         bool relaxedHeader = methods[0] == "relaxed";
         bool relaxedBody = methods.Length > 1 && methods[1] == "relaxed";

         string bodyHash;
         using (SHA256 sha = SHA256.Create())
            bodyHash = Convert.ToBase64String(sha.ComputeHash(Encoding.ASCII.GetBytes(CanonicalBody(messageBody, relaxedBody))));

         if (bodyHash != tags["bh"])
         {
            why = "bh= is " + tags["bh"] + " and the body hashes to " + bodyHash;
            return false;
         }

         var used = new HashSet<int> { at };
         var signed = new StringBuilder();
         foreach (string wanted in tags["h"].Split(':'))
         {
            for (int i = fields.Count - 1; i >= 0; i--)
            {
               if (used.Contains(i) || !string.Equals(fields[i].Key, wanted, StringComparison.OrdinalIgnoreCase))
                  continue;
               used.Add(i);
               signed.Append(relaxedHeader ? RelaxedHeader(fields[i].Value) : fields[i].Value);
               break;
            }
         }

         string withoutSignature = Regex.Replace(signatureField, "(^|;)(\\s*b\\s*=)[^;]*", "$1$2");
         string canonical = relaxedHeader ? RelaxedHeader(withoutSignature) : withoutSignature;
         if (canonical.EndsWith("\r\n", StringComparison.Ordinal))
            canonical = canonical.Substring(0, canonical.Length - 2);
         signed.Append(canonical);

         bool verified = publicKey.VerifyData(Encoding.ASCII.GetBytes(signed.ToString()), Convert.FromBase64String(tags["b"]),
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

         why = verified ? "" : "the RSA signature does not verify over:\r\n" + signed;
         return verified;
      }

      /// <summary>Each header field as its name and its whole text, folded lines and line end included.</summary>
      private static List<KeyValuePair<string, string>> HeaderFields(string header)
      {
         var fields = new List<KeyValuePair<string, string>>();
         StringBuilder current = null;
         string name = null;

         foreach (string line in header.Split(new[] { "\r\n" }, StringSplitOptions.None))
         {
            if (line.Length == 0)
               continue;

            if ((line[0] == ' ' || line[0] == '\t') && current != null)
            {
               current.Append(line).Append("\r\n");
               continue;
            }

            if (current != null)
               fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

            int colon = line.IndexOf(':');
            name = colon > 0 ? line.Substring(0, colon).Trim() : line;
            current = new StringBuilder(line).Append("\r\n");
         }

         if (current != null)
            fields.Add(new KeyValuePair<string, string>(name, current.ToString()));

         return fields;
      }

      private static string RelaxedHeader(string field)
      {
         int colon = field.IndexOf(':');
         string name = field.Substring(0, colon).Trim().ToLowerInvariant();
         string value = field.Substring(colon + 1).Replace("\r\n", "");
         value = Regex.Replace(value, "[ \t]+", " ").Trim();
         return name + ":" + value + "\r\n";
      }

      private static string CanonicalBody(string body, bool relaxed)
      {
         var lines = new List<string>(body.Split(new[] { "\r\n" }, StringSplitOptions.None));
         if (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (relaxed)
            lines = lines.Select(line => Regex.Replace(line, "[ \t]+", " ").TrimEnd(' ')).ToList();

         while (lines.Count > 0 && lines[lines.Count - 1].Length == 0)
            lines.RemoveAt(lines.Count - 1);

         if (lines.Count == 0)
            return relaxed ? "" : "\r\n";

         return string.Join("\r\n", lines) + "\r\n";
      }

      private static Dictionary<string, string> Tags(string value)
      {
         var tags = new Dictionary<string, string>(StringComparer.Ordinal);
         foreach (string part in value.Split(';'))
         {
            int equals = part.IndexOf('=');
            if (equals < 0)
               continue;
            tags[Regex.Replace(part.Substring(0, equals), "\\s", "")] = Regex.Replace(part.Substring(equals + 1), "\\s", "");
         }
         return tags;
      }

      // ---- the clients ---------------------------------------------------------------

      /// <summary>
      ///    One FETCH of a literal item, read to the end of the literal and the tagged
      ///    reply rather than to the first line that starts with the tag: a base64
      ///    line of an attachment may begin with any three letters.
      /// </summary>
      private static string FetchLiteral(string address, string folder, int index, string item)
         => FetchLiteral(address, folder, index, item, out string _);

      private static string FetchLiteral(string address, string folder, int index, string item, out string response)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password), "IMAP logon failed for " + address);
         ClassicAssert.IsTrue(imap.SelectFolder(folder), "SELECT " + folder + " failed for " + address);

         var all = new StringBuilder();
         string literal = null;
         DateTime until = DateTime.UtcNow.AddSeconds(60);

         try
         {
            all.Append(imap.Send("F1 FETCH " + index + " " + item));

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
               else if (text.StartsWith("F1 ", StringComparison.Ordinal) || text.Contains("\r\nF1 "))
                  break;   // answered with no literal: refused

               string more = imap.Receive();
               if (more.Length == 0)
                  break;   // the server closed the connection
               all.Append(more);
            }
         }
         catch (IOException)
         {
            // The server stopped answering, or closed the connection: no literal.
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

      /// <summary>A FETCH whose whole reply is read to the tagged line: for items with no literal, like ENVELOPE.</summary>
      private static string FetchResponseRaw(string address, string folder, int index, string item)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder(folder));
         string response = imap.SendSingleCommand("F1 FETCH " + index + " " + item);
         imap.Disconnect();
         return response;
      }

      private static int FetchSize(string address, int index)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         ClassicAssert.IsTrue(imap.SelectFolder("INBOX"));
         string response = imap.Fetch(index + " RFC822.SIZE");
         imap.Logout();
         imap.Disconnect();

         Match size = Regex.Match(response, "RFC822\\.SIZE (\\d+)");
         ClassicAssert.IsTrue(size.Success, "No RFC822.SIZE in: " + response);
         return int.Parse(size.Groups[1].Value);
      }

      private static void Append(string address, string raw)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         StringAssert.Contains("A01 OK", imap.SendSingleCommandWithLiteral("A01 APPEND INBOX {" + raw.Length + "}", raw));
         imap.Logout();
         imap.Disconnect();
      }

      // A byte-for-byte IMAP client for the crafted-container test: bytes that
      // are not text (the magic is 0x89, and a container carries NUL) cannot go
      // through the simulator, which is UTF-8. ISO-8859-1 maps every byte to the
      // code point of the same value and back, so a string is a faithful carrier
      // of bytes here, and the socket is written and read in it.
      private static readonly Encoding Bytes8 = Encoding.GetEncoding("ISO-8859-1");

      /// <summary>APPEND raw bytes with literal8 (~{n}); the message as stored, which is the literal's bytes exactly.</summary>
      private static byte[] AppendRaw(string address, string folder, byte[] message)
      {
         using (var client = new System.Net.Sockets.TcpClient("127.0.0.1", ImapPort))
         using (var stream = client.GetStream())
         {
            RawExpect(stream, "* OK");
            RawSend(stream, "a1 LOGIN " + address + " " + Password + "\r\n");
            RawExpect(stream, "a1 OK");

            RawSend(stream, "a2 APPEND " + folder + " ~{" + message.Length + "}\r\n");
            RawExpect(stream, "+");

            byte[] payload = new byte[message.Length + 2];
            Array.Copy(message, payload, message.Length);
            payload[message.Length] = (byte) '\r';
            payload[message.Length + 1] = (byte) '\n';
            stream.Write(payload, 0, payload.Length);
            RawExpect(stream, "a2 OK");

            RawSend(stream, "a3 LOGOUT\r\n");
         }

         byte[] stored = new byte[message.Length];
         Array.Copy(message, stored, message.Length);
         return stored;
      }

      /// <summary>FETCH BODY.PEEK[] of a message, its literal read as bytes.</summary>
      private static byte[] FetchRaw(string address, string folder, int index)
      {
         using (var client = new System.Net.Sockets.TcpClient("127.0.0.1", ImapPort))
         using (var stream = client.GetStream())
         {
            RawExpect(stream, "* OK");
            RawSend(stream, "a1 LOGIN " + address + " " + Password + "\r\n");
            RawExpect(stream, "a1 OK");
            RawSend(stream, "a2 SELECT " + folder + "\r\n");
            RawExpect(stream, "a2 OK");
            RawSend(stream, "a3 FETCH " + index + " BODY.PEEK[]\r\n");

            // Read to the literal marker "{n}\r\n", then exactly n bytes.
            var buffer = new List<byte>();
            int open = -1, close = -1;
            while (close < 0)
            {
               int b = stream.ReadByte();
               if (b < 0)
                  Assert.Fail("The connection closed before the FETCH literal: " + Bytes8.GetString(buffer.ToArray()));
               buffer.Add((byte) b);
               string text = Bytes8.GetString(buffer.ToArray());
               open = text.LastIndexOf('{');
               close = open >= 0 ? text.IndexOf("}\r\n", open, StringComparison.Ordinal) : -1;
            }

            string head = Bytes8.GetString(buffer.ToArray());
            int length = int.Parse(head.Substring(open + 1, close - open - 1));
            var literal = new byte[length];
            int read = 0;
            while (read < length)
            {
               int got = stream.Read(literal, read, length - read);
               if (got <= 0)
                  Assert.Fail("The FETCH literal ended early.");
               read += got;
            }

            RawSend(stream, "a4 LOGOUT\r\n");
            return literal;
         }
      }

      // The IMAP port the suite uses (143 unless the Linux suite moved it),
      // through the same TestPorts the simulator reads.
      private static int ImapPort => TestPorts.Imap;

      private static void RawSend(System.Net.Sockets.NetworkStream stream, string command)
      {
         byte[] bytes = Bytes8.GetBytes(command);
         stream.Write(bytes, 0, bytes.Length);
      }

      private static void RawExpect(System.Net.Sockets.NetworkStream stream, string prefixOfALine)
      {
         var buffer = new List<byte>();
         DateTime until = DateTime.UtcNow.AddSeconds(30);
         while (DateTime.UtcNow < until)
         {
            int b = stream.ReadByte();
            if (b < 0)
               break;
            buffer.Add((byte) b);
            if (b == (byte) '\n')
            {
               string text = Bytes8.GetString(buffer.ToArray());
               foreach (string line in text.Split('\n'))
               {
                  if (line.StartsWith(prefixOfALine, StringComparison.Ordinal))
                     return;
               }
            }
         }

         Assert.Fail("Did not see a line beginning \"" + prefixOfALine + "\"; got: " + Bytes8.GetString(buffer.ToArray()));
      }

      private static void ExpungeFirst(string address, string folder)
      {
         var imap = new ImapClientSimulator();
         ClassicAssert.IsTrue(imap.ConnectAndLogon(address, Password));
         int before = imap.GetMessageCount(folder);
         ClassicAssert.IsTrue(imap.SetDeletedFlag(1));
         ClassicAssert.IsTrue(imap.Expunge());
         imap.Logout();
         imap.Disconnect();
         ImapClientSimulator.AssertMessageCount(address, Password, folder, before - 1);
      }

      /// <summary>
      ///    The message without its POP3 framing, read with RETR and not deleted: the
      ///    reply line and the terminating ".\r\n" taken off. The messages here have no
      ///    line that begins with a dot, so nothing is stuffed.
      /// </summary>
      private static string Retrieve(string address, int index)
      {
         var pop3 = new Pop3ClientSimulator();
         ClassicAssert.IsTrue(pop3.ConnectAndLogon(address, Password));
         string response = pop3.RETR(index);
         pop3.QUIT();

         int firstLine = response.IndexOf("\r\n", StringComparison.Ordinal);
         ClassicAssert.IsTrue(response.StartsWith("+OK", StringComparison.Ordinal) && firstLine > 0, response.Substring(0, Math.Min(200, response.Length)));
         ClassicAssert.IsTrue(response.EndsWith("\r\n.\r\n", StringComparison.Ordinal));
         return response.Substring(firstLine + 2, response.Length - (firstLine + 2) - 3);
      }

      // ---- the server --------------------------------------------------------------------

      private Account AddAccount(string name)
      {
         string address = name + "@" + _domain.Name;
         Account account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);
         accounts_.Add(address);
         return account;
      }

      // Asked of the server each time: a COM object from before a reinitialise
      // belongs to the configuration that was.
      private IMAPFolder Inbox(Account account)
      {
         Domain domain = _application.Domains.get_ItemByName(account.Address.Substring(account.Address.IndexOf('@') + 1));
         return domain.Accounts.get_ItemByAddress(account.Address).IMAPFolders.get_ItemByName("INBOX");
      }

      private void SetCompression(bool on, int minimumKilobytes)
      {
         (int status, string body) answer = Admin("PUT", "/api/v1/settings",
            "{\"message_store_compression\":" + (on ? "true" : "false") + ",\"message_store_dedup_min_kb\":" + minimumKilobytes + "}");
         ClassicAssert.AreEqual(200, answer.status, answer.body);
      }

      // Sharing needs every recipient's copy to be the same bytes, and Delivered-To
      // names the recipient, so the header goes off with the links on.
      private void SetHardLinks(bool on)
      {
         if (on)
         {
            IniFileSetting.Write("DeliveryHardLinks", "1");
            putBack_.Add(() =>
            {
               IniFileSetting.Delete("DeliveryHardLinks");
               _application.Reinitialize();
               _application.Settings.AddDeliveredToHeader = true;
            });
         }
         else
            IniFileSetting.Delete("DeliveryHardLinks");

         _application.Reinitialize();
         _application.Settings.AddDeliveredToHeader = !on;
      }

      /// <summary>
      ///    The consistency report of a run started now: the check switched on and the
      ///    server reinitialised, which runs it, and the report read once it is the new
      ///    one - naming the given blob, when one is given.
      /// </summary>
      private string ConsistencyReport(string mustName)
      {
         string reportPath = Path.Combine(_application.Settings.Directories.LogDirectory, "hMailServer_messagestore_consistency.report");
         if (File.Exists(reportPath))
            File.Delete(reportPath);

         IniFileSetting.Write("MessageStoreConsistencyCheck", "1");
         putBack_.Add(() =>
         {
            IniFileSetting.Delete("MessageStoreConsistencyCheck");
            _application.Reinitialize();
         });
         _application.Reinitialize();

         string report = null;
         WaitFor(() =>
         {
            if (!File.Exists(reportPath))
               return false;
            report = TestSetup.ReadExistingTextFile(reportPath);
            return report.Contains("# Blobs nothing references:") && (mustName == null || report.Contains(mustName));
         }, "No consistency report was written to " + reportPath, 30);

         return report;
      }

      private static void WaitUntilMovable(Account account)
      {
         DateTime until = DateTime.Now.AddSeconds(30);
         string reason = "";
         while (DateTime.Now < until)
         {
            reason = (string) ((dynamic) account).InUseReason;
            if (reason.Length == 0)
               return;
            Thread.Sleep(100);
         }

         Assert.Fail("The mailbox was still in use after 30 seconds: " + reason);
      }

      private static void WaitFor(Func<bool> condition, string failure, int seconds = 15)
      {
         DateTime until = DateTime.UtcNow.AddSeconds(seconds);
         while (DateTime.UtcNow < until)
         {
            if (condition())
               return;
            Thread.Sleep(200);
         }

         Assert.Fail(failure);
      }

      // Whether a log the server is still writing says this yet; a sharing
      // violation while it writes is "not yet".
      private static bool LogSays(string log, string text)
      {
         try
         {
            return File.Exists(log) && TestSetup.ReadExistingTextFile(log).Contains(text);
         }
         catch (Exception fatalCheck) when (!ExceptionPolicy.IsFatal(fatalCheck))
         {
            return false;
         }
      }

      private static void AssertUnchanged(List<string> files, List<byte[]> before, string failure)
      {
         for (int i = 0; i < files.Count; i++)
            CollectionAssert.AreEqual(before[i], File.ReadAllBytes(files[i]), failure + ": " + files[i]);
      }

      // The identity of a file and how many names it has, from "fsutil hardlink
      // list", as DeliveryHardLinks reads them.
      private static (string identity, uint links) Identify(string path)
      {
         string fullPath = Path.GetFullPath(path);
         var fsutil = new ProcessStartInfo("fsutil", "hardlink list \"" + fullPath + "\"")
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
         };

         using (var process = Process.Start(fsutil))
         {
            string output = process.StandardOutput.ReadToEnd();
            string errors = process.StandardError.ReadToEnd();
            process.WaitForExit();
            ClassicAssert.AreEqual(0, process.ExitCode, "fsutil hardlink list failed for " + path + ": " + errors + output);

            var names = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
               .Select(name => name.Trim().ToLowerInvariant())
               .Where(name => name.Length > 0)
               .OrderBy(name => name, StringComparer.Ordinal)
               .ToList();
            ClassicAssert.IsTrue(names.Count > 0, "fsutil listed no names for " + path + ": " + output);

            return (Path.GetPathRoot(fullPath).ToLowerInvariant() + string.Join("|", names), (uint) names.Count);
         }
      }

      private sealed class Verb
      {
         public int ExitCode;
         public string Output;
      }

      /// <summary>hMailServer.exe with these arguments and no HM_ variable of the runner's.</summary>
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
            if (!process.WaitForExit(180 * 1000))
            {
               try
               {
                  process.Kill();
               }
               catch (InvalidOperationException)
               {
                  // It finished between the wait and the kill.
               }

               Assert.Fail(arguments + " did not finish within three minutes, and was stopped.");
            }
            process.WaitForExit();

            return new Verb { ExitCode = process.ExitCode, Output = output.Result + error.Result };
         }
      }

      private string executable_;

      // Asked of the server once, over COM, and kept: a verb run with the service
      // stopped has no server to ask.
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

      /// <summary>The service stopped, and its process gone, as TestSetup's restart stops it.</summary>
      private static void StopService()
      {
         using (Process process = Process.Start(new ProcessStartInfo("net", "stop hMailServer")
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
         }))
         {
            process.StandardOutput.ReadToEnd();
            process.StandardError.ReadToEnd();
            process.WaitForExit(90000);
         }

         DateTime deadline = DateTime.UtcNow.AddSeconds(60);
         while (DateTime.UtcNow < deadline && Process.GetProcessesByName("hmailserver").Length > 0)
            Thread.Sleep(250);

         ClassicAssert.IsFalse(ServiceRunning(), "The hMailServer service did not stop.");
      }

      private static bool ServiceRunning()
      {
         using (var controller = new System.ServiceProcess.ServiceController("hMailServer"))
         {
            controller.Refresh();
            return controller.Status != System.ServiceProcess.ServiceControllerStatus.Stopped;
         }
      }

      private static (int code, string output) RunPython(string python, string arguments)
      {
         var start = new ProcessStartInfo(python, arguments)
         {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
         };
         start.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";

         using (Process process = Process.Start(start))
         {
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(120 * 1000))
            {
               process.Kill();
               Assert.Fail("The checker did not finish within two minutes, and was stopped.");
            }
            process.WaitForExit();
            return (process.ExitCode, output.Result + error.Result);
         }
      }

      private static string FindPython()
      {
         foreach (string entry in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
         {
            string directory = entry.Trim().Trim('"');
            if (directory.Length == 0 || directory.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) >= 0)
               continue;

            try
            {
               string candidate = Path.Combine(directory, "python.exe");
               if (File.Exists(candidate))
                  return candidate;
            }
            catch (ArgumentException)
            {
            }
         }

         return null;
      }

      private static string RepositoryFile(string relativePath)
      {
         for (DirectoryInfo directory = new DirectoryInfo(TestContext.CurrentContext.TestDirectory); directory != null; directory = directory.Parent)
         {
            string candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
               return candidate;
         }

         Assert.Ignore("These binaries are not inside a checkout, so " + relativePath + " cannot be found.");
         return null;
      }

      private static JsonElement Json((int status, string body) answer, int status)
      {
         ClassicAssert.AreEqual(status, answer.status, answer.body);
         return JsonDocument.Parse(answer.body).RootElement.Clone();
      }

      private static void Guarded(Action step)
      {
         try
         {
            step();
         }
         catch (Exception e) when (!ExceptionPolicy.IsFatal(e))
         {
            Console.WriteLine("MessageStoreFormat teardown: " + e.Message);
         }
      }

      private static string Basic(string user, string password)
         => "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));

      private (int status, string body) Admin(string method, string path, string requestBody)
         => RestRequest.Send("127.0.0.1", RestPort, method, path, Basic("Administrator", AdminPassword), requestBody);
   }
}
