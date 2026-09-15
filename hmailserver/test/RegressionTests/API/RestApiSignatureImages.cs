// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    A signature with an image in it, from the webmail's settings to the recipient's
   ///    mail client.
   ///
   ///    The webmail keeps an image in the formatted signature as a data: URL, and that
   ///    used to fail at every layer beneath the page: PUT /api/v1/me/settings refused a
   ///    signature over 20,000 characters, the listener refused the request over 64 KB
   ///    before the route saw it, MySQL's column holds 65,535 bytes, and a message whose
   ///    HTML carried the image went out with the data: URL in it, which Gmail and
   ///    Outlook do not display. One limit now - 64,000 bytes of UTF-8, images included,
   ///    what the narrowest column holds - and the send path moves each data: image into
   ///    an inline part of a multipart/related beside the HTML (InlineImageParts.cpp).
   ///
   ///    What is asserted is what left the server - the delivered message's own bytes,
   ///    read over POP3 - and not the route's answer, and each positive assertion has
   ///    the thing that would make it vacuous checked beside it: the image's bytes are
   ///    compared, not just the absence of data:, so a message that lost its HTML fails.
   ///
   ///    Two things beside the send path: a file added to such a message afterwards the
   ///    way an event script adds one (Message.Attachments.Add, MessageData::CreatePart)
   ///    keeps the HTML and its image together; and a message is given no more image
   ///    parts than the webmail's own view puts back in place, so every image of a Sent
   ///    copy or a draft still shows there.
   /// </summary>
   [TestFixture]
   public class RestApiSignatureImages : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the suite's
      // own where RestListener finds one already on.
      private static int RestPort = 9573;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      // The limit the route, the page (Portal.js SIGNATURE_HTML_BYTES) and the
      // narrowest database column agree on.
      private const int SignatureHtmlBytes = 64000;

      // A real 1x1 PNG.
      private const string SmallPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

      private Account _account;

      private string Address
      {
         get { return "signer@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         (int status, string body) probe = RestRequest.Send("127.0.0.1", RestPort, "GET", "/api/v1/status", BasicHeader("Administrator", AdminPassword), null);
         Assert.AreEqual(200, probe.status, "REST API did not answer /api/v1/status. Body: " + probe.body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("PUT /api/v1/me/settings keeps a formatted signature of 64,000 bytes with its image, beside a plain one of 20,000 characters, and refuses one byte more with a sentence")]
      public void ASignatureOf64KBWithItsImageIsKeptAndOneByteMoreIsRefused()
      {
         string plain = new string('a', 20000);
         string atLimit = SignatureHtml(ImageBytes(45000), SignatureHtmlBytes);

         // Bytes, not characters: the é makes this one character shorter than it is
         // bytes long, so a route that counted characters would accept the next one too.
         Assert.AreEqual(SignatureHtmlBytes - 1, atLimit.Length);

         // Some 84 KB on the wire - over the 64 KB every ordinary request is held to, so
         // this also proves the route is given the large cap.
         string saveBody = SettingsBody(plain, atLimit);
         Assert.Greater(Encoding.UTF8.GetByteCount(saveBody), 64 * 1024);

         (int status, string body) saved = Me("PUT", "/api/v1/me/settings", saveBody);
         Assert.AreEqual(200, saved.status, "Body: " + Truncated(saved.body));

         Account reread = _domain.Accounts.ItemByAddress[Address];
         Assert.IsTrue(reread.SignatureEnabled);
         Assert.AreEqual(plain, reread.SignaturePlainText);
         Assert.AreEqual(atLimit, reread.SignatureHTML, "The formatted signature must come back from the database byte for byte.");

         string overLimit = atLimit.Replace("</p>", "x</p>");
         Assert.AreEqual(SignatureHtmlBytes + 1, Encoding.UTF8.GetByteCount(overLimit));
         Assert.AreEqual(SignatureHtmlBytes, overLimit.Length);

         (int status, string body) refused = Me("PUT", "/api/v1/me/settings", SettingsBody(plain, overLimit));
         Assert.AreEqual(400, refused.status, "Body: " + Truncated(refused.body));
         StringAssert.Contains("a formatted signature is at most 64 KB (64,000 bytes), its images included", refused.body);

         reread = _domain.Accounts.ItemByAddress[Address];
         Assert.AreEqual(atLimit, reread.SignatureHTML, "A refused signature must leave the saved one as it was.");

         // The plain signature's own limit is still its own.
         (int status, string body) longPlain = Me("PUT", "/api/v1/me/settings", SettingsBody(plain + "a", "<p>Ada</p>"));
         Assert.AreEqual(400, longPlain.status, "Body: " + Truncated(longPlain.body));
         StringAssert.Contains("at most 20000 characters", longPlain.body);
      }

      [Test]
      [Description("A message whose HTML carries the signature's image goes as multipart/related with the image as a cid: part, and the Sent copy still shows the image in the webmail")]
      public void TheSignatureImageIsSentAsAnInlinePartAndTheSentCopyShowsIt()
      {
         string other = "reader@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, other, UserPassword);

         var imap = new ImapClientSimulator();
         Assert.IsTrue(imap.ConnectAndLogon(Address, UserPassword));
         Assert.IsTrue(imap.CreateFolder("Sent"));
         imap.Disconnect();

         byte[] image = ImageBytes(45000);
         string signature = SignatureHtml(image, SignatureHtmlBytes);
         (int status, string body) saved = Me("PUT", "/api/v1/me/settings", SettingsBody("-- \nAda", signature));
         Assert.AreEqual(200, saved.status, "Body: " + Truncated(saved.body));

         // As the page writes it: the text, then the signature block, which names the
         // image twice here so that one part for one image is checked too.
         string html = "<p>Hello</p><div><br>-- <br>" + signature + "</div><p>" + ImageTag(image) + "</p>";
         (int status, string body) sent = Me("POST", "/api/v1/me/messages",
            "{\"to\":\"" + other + "\",\"subject\":\"With the logo\",\"text\":\"Hello\\n\\n-- \\nAda\",\"html\":\"" + Json(html) + "\"}");
         Assert.AreEqual(201, sent.status, "Body: " + Truncated(sent.body));
         long sentId = long.Parse(Regex.Match(sent.body, "\"sent_id\":(\\d+)").Groups[1].Value);
         Assert.Greater(sentId, 0, "The account has a Sent folder, so a copy must have been kept. Body: " + sent.body);

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(other, UserPassword);
         AssertInlineImage(delivered, image, "The delivered message");

         // The top of the message is still the alternative, so no reader shows a
         // paperclip for a logo: the related part is inside it, around the HTML.
         string topContentType = Regex.Match(delivered, "^Content-Type:\\s*([^;\\r\\n]+)", RegexOptions.Multiline | RegexOptions.IgnoreCase).Groups[1].Value.Trim();
         Assert.AreEqual("multipart/alternative", topContentType.ToLowerInvariant(), Truncated(delivered));

         // The webmail's own view of the Sent copy: the attachment carries the Content-ID,
         // and the HTML document the page frames has the image back in place.
         string contentId = ContentIdOf(delivered);
         (int status, string body) sentJson = Me("GET", "/api/v1/me/messages/" + sentId, null);
         Assert.AreEqual(200, sentJson.status, "Body: " + Truncated(sentJson.body));
         StringAssert.Contains("\"content_id\":\"" + contentId + "\"", sentJson.body);

         (int status, string body) sentHtml = Me("GET", "/api/v1/me/messages/" + sentId + "/html", null);
         Assert.AreEqual(200, sentHtml.status, "Body: " + Truncated(sentHtml.body));
         StringAssert.Contains("data:image/png;base64," + Convert.ToBase64String(image), sentHtml.body);
         StringAssert.DoesNotContain("cid:", sentHtml.body);
      }

      [Test]
      [Description("A draft keeps the image as an inline part too - a scheduled send and another mail client see it - and the webmail still shows it")]
      public void ADraftKeepsItsImageAsAnInlinePartTheWebmailShows()
      {
         byte[] image = Convert.FromBase64String(SmallPng);
         string html = "<p>Draft</p><div><br>-- <br><p>Ada</p>" + ImageTag(image) + "</div>";

         (int status, string body) drafted = Me("POST", "/api/v1/me/drafts",
            "{\"to\":\"someone@example.com\",\"subject\":\"Not yet\",\"text\":\"Draft\",\"html\":\"" + Json(html) + "\"}");
         Assert.AreEqual(201, drafted.status, "Body: " + drafted.body);
         long draftId = long.Parse(Regex.Match(drafted.body, "\"id\":(\\d+)").Groups[1].Value);

         (int status, string body) source = Me("GET", "/api/v1/me/messages/" + draftId + "/source", null);
         Assert.AreEqual(200, source.status, "Body: " + Truncated(source.body));
         AssertInlineImage(source.body, image, "The draft");

         (int status, string body) shown = Me("GET", "/api/v1/me/messages/" + draftId + "/html", null);
         Assert.AreEqual(200, shown.status, "Body: " + Truncated(shown.body));
         StringAssert.Contains("data:image/png;base64," + SmallPng, shown.body);
      }

      [Test]
      [Description("The negative control: a message with no image the conversion takes - an SVG, a data-src - keeps its shape and its HTML exactly")]
      public void AMessageWithNothingToMoveKeepsItsShape()
      {
         string other = "reader@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, other, UserPassword);

         string html = "<p>Plain</p><img src=\"data:image/svg+xml;base64,PHN2Zz4=\" alt=\"\"><img data-src=\"data:image/png;base64," + SmallPng + "\">";
         (int status, string body) sent = Me("POST", "/api/v1/me/messages",
            "{\"to\":\"" + other + "\",\"subject\":\"Nothing to move\",\"text\":\"Plain\",\"html\":\"" + Json(html) + "\"}");
         Assert.AreEqual(201, sent.status, "Body: " + sent.body);

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(other, UserPassword);
         StringAssert.DoesNotContain("multipart/related", delivered.ToLowerInvariant());
         StringAssert.DoesNotContain("Content-ID:", delivered);
         string unfolded = QuotedPrintableUnfolded(delivered);
         StringAssert.Contains("data:image/svg+xml;base64,PHN2Zz4=", unfolded);
         StringAssert.Contains("data-src=\"data:image/png;base64," + SmallPng, unfolded);
      }

      [Test]
      [Description("The domain's signature, appended by the server to a message submitted over SMTP, sends its image as a cid: part too (SignatureAdder)")]
      public void TheDomainSignatureImageIsSentAsAnInlinePartOverSmtp()
      {
         string other = "reader@" + _domain.Name;
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, other, UserPassword);

         byte[] image = Convert.FromBase64String(SmallPng);
         _domain.SignatureEnabled = true;
         _domain.SignaturePlainText = "Example Ltd";
         _domain.SignatureHTML = "<p>Example Ltd</p>" + ImageTag(image);
         _domain.AddSignaturesToLocalMail = true;
         _domain.Save();

         string message =
            "From: " + Address + "\r\n" +
            "To: " + other + "\r\n" +
            "Subject: Signed by the domain\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/alternative; boundary=\"outer\"\r\n" +
            "\r\n" +
            "--outer\r\n" +
            "Content-Type: text/plain; charset=utf-8\r\n" +
            "\r\n" +
            "Hello\r\n" +
            "--outer\r\n" +
            "Content-Type: text/html; charset=utf-8\r\n" +
            "\r\n" +
            "<p>Hello</p>\r\n" +
            "--outer--\r\n";

         new SmtpClientSimulator().SendRaw(Address, UserPassword, Address, other, message);

         string delivered = Pop3ClientSimulator.AssertGetFirstMessageText(other, UserPassword);
         AssertInlineImage(delivered, image, "The message the domain signature was added to");
         StringAssert.Contains("Example Ltd", QuotedPrintableUnfolded(delivered));
      }

      [Test]
      [Description("A file added afterwards the way an event script adds one - Message.Attachments.Add - keeps the HTML and its inline image together, on a message with no file and on one that has a file already")]
      public void AFileAddedAfterwardsKeepsTheHtmlAndItsImageTogether()
      {
         byte[] image = Convert.FromBase64String(SmallPng);
         string html = "<p>Draft</p><div><br>-- <br><p>Ada</p>" + ImageTag(image) + "</div>";
         string file = Paths.Combine(Path.GetTempPath(), "signature-images-added.txt");
         File.WriteAllText(file, "added afterwards");

         try
         {
            // The webmail's two shapes: multipart/alternative at the top, and multipart/mixed
            // around it when the message carries a file. Before MessageData::CreatePart knew a
            // related part, adding a file dropped the HTML and the image from both.
            string[] withFiles = { "", ",\"attachments\":[{\"name\":\"first.txt\",\"type\":\"text/plain\",\"data\":\"Zmlyc3Q=\"}]" };
            foreach (string withFile in withFiles)
            {
               string what = withFile.Length == 0 ? "The draft a file was added to" : "The draft with a file that another was added to";

               (int status, string body) drafted = Me("POST", "/api/v1/me/drafts",
                  "{\"to\":\"someone@example.com\",\"subject\":\"Not yet\",\"text\":\"Draft\",\"html\":\"" + Json(html) + "\"" + withFile + "}");
               Assert.AreEqual(201, drafted.status, "Body: " + drafted.body);
               long draftId = long.Parse(Regex.Match(drafted.body, "\"id\":(\\d+)").Groups[1].Value);

               hMailServer.Message stored = _domain.Accounts.ItemByAddress[Address].IMAPFolders.get_ItemByName("Drafts").Messages.get_ItemByDBID(draftId);
               Assert.IsNotNull(stored, "COM must find the draft by its id.");
               stored.Attachments.Add(file);
               stored.Save();

               string rewritten = File.ReadAllText(stored.Filename);
               StringAssert.Contains("signature-images-added.txt", rewritten, what + " does not carry the file: " + Truncated(rewritten));
               if (withFile.Length > 0)
                  StringAssert.Contains("first.txt", rewritten, what + " lost the file it had: " + Truncated(rewritten));
               StringAssert.Contains("<p>Ada</p>", QuotedPrintableUnfolded(rewritten), what + " lost its HTML: " + Truncated(rewritten));
               AssertInlineImage(rewritten, image, what);

               // And the webmail still shows the image in place.
               (int status, string body) shown = Me("GET", "/api/v1/me/messages/" + draftId + "/html", null);
               Assert.AreEqual(200, shown.status, "Body: " + Truncated(shown.body));
               StringAssert.Contains("data:image/png;base64," + SmallPng, shown.body, what + " no longer shows its image in the webmail.");
            }
         }
         finally
         {
            File.Delete(file);
         }
      }

      [Test]
      [Description("Of sixty-five different images, sixty-four become parts - as many as the webmail's view of a message puts back in place - and the last stays a data: URL, so the webmail shows every one")]
      public void ImagesPastWhatTheWebmailPutsBackInPlaceStayDataUrls()
      {
         // InlineImageParts::MostImages, and the /html route's count.
         const int MostImages = 64;

         var html = new StringBuilder("<p>Many</p>");
         for (int i = 0; i <= MostImages; i++)
         {
            byte[] different = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, (byte) ('A' + i % 26), (byte) ('a' + i / 26) };
            html.Append(ImageTag(different));
         }

         (int status, string body) drafted = Me("POST", "/api/v1/me/drafts",
            "{\"to\":\"someone@example.com\",\"subject\":\"Many\",\"text\":\"Many\",\"html\":\"" + Json(html.ToString()) + "\"}");
         Assert.AreEqual(201, drafted.status, "Body: " + drafted.body);
         long draftId = long.Parse(Regex.Match(drafted.body, "\"id\":(\\d+)").Groups[1].Value);

         (int status, string body) source = Me("GET", "/api/v1/me/messages/" + draftId + "/source", null);
         Assert.AreEqual(200, source.status, "Body: " + Truncated(source.body));
         Assert.AreEqual(MostImages, Regex.Matches(source.body, "^Content-ID:", RegexOptions.Multiline | RegexOptions.IgnoreCase).Count,
            "Sixty-four images must be parts: " + Truncated(source.body));
         Assert.AreEqual(1, Regex.Matches(QuotedPrintableUnfolded(source.body), "data:image/png;base64,").Count,
            "The sixty-fifth must be left a data: URL: " + Truncated(source.body));

         (int status, string body) shown = Me("GET", "/api/v1/me/messages/" + draftId + "/html", null);
         Assert.AreEqual(200, shown.status, "Body: " + Truncated(shown.body));
         Assert.AreEqual(MostImages + 1, Regex.Matches(shown.body, "data:image/png;base64,").Count,
            "The webmail's view must show every image, the parts put back and the data: URL as it was: " + Truncated(shown.body));
         StringAssert.DoesNotContain("cid:", shown.body);
      }

      /// <summary>
      ///    The raw message has one inline image part with a Content-ID, holding exactly
      ///    these bytes; the HTML names that Content-ID; and no data: image is left.
      /// </summary>
      private static void AssertInlineImage(string raw, byte[] image, string what)
      {
         string lower = raw.ToLowerInvariant();
         StringAssert.Contains("content-type: multipart/related", lower, what + " has no related part: " + Truncated(raw));
         StringAssert.Contains("type=\"text/html\"", lower, what + "'s related part does not name its root: " + Truncated(raw));

         Assert.AreEqual(1, Regex.Matches(raw, "^Content-ID:", RegexOptions.Multiline | RegexOptions.IgnoreCase).Count,
            what + " must carry the image once, however many times the HTML names it: " + Truncated(raw));

         string contentId = ContentIdOf(raw);
         CollectionAssert.AreEqual(image, InlinePartBytes(raw, contentId), what + "'s image part does not hold the image.");

         string unfolded = QuotedPrintableUnfolded(raw);
         StringAssert.Contains("cid:" + contentId, unfolded, what + "'s HTML does not name the image part: " + Truncated(raw));
         StringAssert.DoesNotContain("data:image", unfolded.ToLowerInvariant(), what + " still carries a data: image: " + Truncated(raw));
      }

      private static string ContentIdOf(string raw)
      {
         Match id = Regex.Match(raw, "^Content-ID:\\s*<([^>]+)>", RegexOptions.Multiline | RegexOptions.IgnoreCase);
         Assert.IsTrue(id.Success, "No Content-ID in: " + Truncated(raw));
         return id.Groups[1].Value;
      }

      private static byte[] InlinePartBytes(string raw, string contentId)
      {
         int header = raw.IndexOf("<" + contentId + ">", StringComparison.Ordinal);
         int bodyStart = raw.IndexOf("\r\n\r\n", header, StringComparison.Ordinal) + 4;
         int bodyEnd = raw.IndexOf("\r\n--", bodyStart, StringComparison.Ordinal);
         Assert.IsTrue(header >= 0 && bodyStart > 4 && bodyEnd > bodyStart, "The image part could not be found: " + Truncated(raw));
         return Convert.FromBase64String(Regex.Replace(raw.Substring(bodyStart, bodyEnd - bodyStart), "\\s", ""));
      }

      // A quoted-printable HTML part with its soft line breaks taken out and = put back,
      // which is enough to search it for an address or a URL.
      private static string QuotedPrintableUnfolded(string raw)
      {
         return raw.Replace("=\r\n", "").Replace("=3D", "=").Replace("=3d", "=");
      }

      // Bytes that start as a PNG does, which is what the conversion checks.
      private static byte[] ImageBytes(int length)
      {
         byte[] bytes = new byte[length];
         byte[] signature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
         Array.Copy(signature, bytes, signature.Length);
         for (int i = signature.Length; i < length; i++)
            bytes[i] = (byte) (i % 251);
         return bytes;
      }

      private static string ImageTag(byte[] image)
      {
         return "<img src=\"data:image/png;base64," + Convert.ToBase64String(image) + "\" alt=\"\">";
      }

      // A formatted signature of exactly totalBytes bytes of UTF-8, as the page's cleaner
      // writes one: a paragraph with a name in it, then the image.
      private static string SignatureHtml(byte[] image, int totalBytes)
      {
         const string opening = "<p><b>Ada Lovelace</b> \u00e9 ";
         string tag = ImageTag(image);
         int fill = totalBytes - Encoding.UTF8.GetByteCount(opening + "</p>" + tag);
         Assert.Greater(fill, 0);
         string html = opening + new string('x', fill) + "</p>" + tag;
         Assert.AreEqual(totalBytes, Encoding.UTF8.GetByteCount(html));
         return html;
      }

      private static string SettingsBody(string text, string html)
      {
         return "{\"signature\":{\"enabled\":true,\"text\":\"" + Json(text) + "\",\"html\":\"" + Json(html) + "\"}}";
      }

      private static string Json(string value)
      {
         return value.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n").Replace("\r", "\\r");
      }

      private static string Truncated(string value)
      {
         return value.Length <= 2000 ? value : value.Substring(0, 2000) + "... (" + value.Length + " characters)";
      }

      private (int status, string body) Me(string method, string path, string requestBody)
      {
         return RestRequest.Send("127.0.0.1", RestPort, method, path, BasicHeader(Address, UserPassword), requestBody);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }
   }
}
