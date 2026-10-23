// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.IO;
using System.IO.Compression;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using hMailServer;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Infrastructure;
using RegressionTests.Shared;

namespace RegressionTests.API
{
   /// <summary>
   ///    What the HTTP listener compresses, and what it leaves exactly as it
   ///    found it.
   ///
   ///    The webmail's shell is half a megabyte of text - the page, its script,
   ///    its S/MIME module - and until HttpCompression.cpp existed every byte of
   ///    it went out uncompressed to every browser on every cold load, although
   ///    zlib had been linked into this binary for years for IMAP's COMPRESS
   ///    extension. The measured saving on the script alone is 349,442 bytes
   ///    down to 90,595.
   ///
   ///    Every assertion here is made against the bytes on the socket rather
   ///    than against a header: the compressed answer is decompressed and
   ///    compared with the uncompressed one byte for byte, because a coding that
   ///    truncates or re-encodes is exactly the defect a header cannot show. The
   ///    negotiation itself - the weights, the star, the zero - is proved at the
   ///    function in HttpCompressionTester, which ClassTester::DoTests runs;
   ///    what is proved here is that the negotiation reaches the wire, that
   ///    Content-Length follows the body it framed, and that the answers this
   ///    server must not touch are untouched: a download, and anything built
   ///    from the database, which is the BREACH line HttpCompression.cpp draws
   ///    (a subject an attacker sent, compressed beside every other subject in
   ///    the listing the webmail re-reads on its own, would leak the others
   ///    through the answer's length to whoever can watch the wire).
   /// </summary>
   [TestFixture]
   public class RestApiCompression : TestFixtureBase
   {
      // The port the listener answers on: this one on the Windows bench, the
      // suite's own where RestListener finds one already on.
      private static int RestPort = 9591;
      private const string AdminPassword = "testar";
      private const string UserPassword = "Original-Passw0rd!";

      // HttpCompression::MinimumBytes. An answer shorter than this is served as
      // it is, whatever the client asked for.
      private const int MinimumBytes = 1024;

      private string Address
      {
         get { return "reader@" + _domain.Name; }
      }

      [SetUp]
      public void StartRestApi()
      {
         _settings.SetAdministratorPassword(AdminPassword);

         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, Address, UserPassword);

         RestPort = RestListener.Start(RestPort);
         IniFileSetting.Write("RestApiCertificateFile", "");
         IniFileSetting.Write("RestApiPrivateKeyFile", "");

         _application.Reinitialize();

         Response probe = Raw("GET", "/api/v1/status", AdminHeader(), null);
         Assert.AreEqual(200, probe.Status, "REST API did not answer /api/v1/status. Body: " + probe.Body);
      }

      [TearDown]
      public void StopRestApi()
      {
         RestListener.Stop();
         _application.Reinitialize();
      }

      [Test]
      [Description("A client that offers no content coding gets exactly the bytes it always got, and a Vary that says the answer would have differed")]
      public void AnAnswerIsWholeWhenNoCodingIsOffered()
      {
         Response plain = Raw("GET", "/portal.js", null, null);

         Assert.AreEqual(200, plain.Status, plain.Body);
         Assert.AreEqual("", plain.Header("Content-Encoding"), "Nothing was asked for, so nothing is encoded.");
         Assert.AreEqual("Accept-Encoding", plain.Header("Vary"),
            "A cache must key this answer on what the client can decode, or the next client gets the wrong form.");
         Assert.AreEqual(plain.BodyBytes.Length, int.Parse(plain.Header("Content-Length")),
            "Content-Length is the number of bytes that followed it.");
         Assert.IsTrue(plain.BodyBytes.Length > 100000, "Portal.js is the whole webmail; it is not a short file.");
         // REUSE-IgnoreStart
         StringAssert.Contains("SPDX-License-Identifier: AGPL-3.0-or-later", plain.Body);
         // REUSE-IgnoreEnd
      }

      [Test]
      [Description("A client that offers gzip gets gzip, framed by its own length, and it decompresses to exactly the uncompressed answer")]
      public void GzipDecompressesToExactlyTheUncompressedAnswer()
      {
         Response plain = Raw("GET", "/portal.js", null, null);
         Response zipped = Raw("GET", "/portal.js", null, null, "Accept-Encoding: gzip\r\n");

         Assert.AreEqual(200, zipped.Status, zipped.Body);
         Assert.AreEqual("gzip", zipped.Header("Content-Encoding"));
         Assert.AreEqual("Accept-Encoding", zipped.Header("Vary"));
         Assert.AreEqual("text/javascript; charset=utf-8", zipped.Header("Content-Type"),
            "The media type describes what is under the coding, not the coding.");

         Assert.AreEqual(zipped.BodyBytes.Length, int.Parse(zipped.Header("Content-Length")),
            "Content-Length is the length of the compressed body, which is what was sent.");

         Assert.IsTrue(zipped.BodyBytes.Length > 2, "There is a body.");
         Assert.AreEqual(0x1f, zipped.BodyBytes[0], "A gzip member begins 1f 8b.");
         Assert.AreEqual(0x8b, zipped.BodyBytes[1], "A gzip member begins 1f 8b.");

         Assert.AreEqual(plain.BodyBytes, Gunzip(zipped.BodyBytes),
            "The compressed answer must decompress to exactly the uncompressed one.");

         Assert.IsTrue(zipped.BodyBytes.Length * 2 < plain.BodyBytes.Length,
            "The point of the exercise: the script goes out at less than half its size. Plain " +
            plain.BodyBytes.Length + ", gzip " + zipped.BodyBytes.Length + ".");
      }

      [Test]
      [Description("A client that offers only deflate gets a zlib stream under that name, and it too decompresses to the uncompressed answer")]
      public void DeflateIsTheZlibFormatAndDecompressesToTheSameAnswer()
      {
         Response plain = Raw("GET", "/portal.js", null, null);
         Response zipped = Raw("GET", "/portal.js", null, null, "Accept-Encoding: deflate\r\n");

         Assert.AreEqual(200, zipped.Status, zipped.Body);
         Assert.AreEqual("deflate", zipped.Header("Content-Encoding"));
         Assert.AreEqual(zipped.BodyBytes.Length, int.Parse(zipped.Header("Content-Length")));
         Assert.AreEqual(plain.BodyBytes, Inflate(zipped.BodyBytes),
            "The compressed answer must decompress to exactly the uncompressed one.");
      }

      [Test]
      [Description("The sign-in page, the service worker and the OpenAPI description are compressed as well as the script")]
      public void ThePageTheWorkerAndTheOpenApiDocumentAreCompressedToo()
      {
         Response page = Raw("GET", "/portal", null, null);
         Response zippedPage = Raw("GET", "/portal", null, null, "Accept-Encoding: gzip\r\n");
         Assert.AreEqual(200, zippedPage.Status, zippedPage.Body);
         Assert.AreEqual("gzip", zippedPage.Header("Content-Encoding"));
         Assert.AreEqual(page.BodyBytes, Gunzip(zippedPage.BodyBytes));
         StringAssert.Contains("<!doctype html>", Encoding.UTF8.GetString(Gunzip(zippedPage.BodyBytes)).ToLowerInvariant());

         Response worker = Raw("GET", "/portal-sw.js", null, null);
         Response zippedWorker = Raw("GET", "/portal-sw.js", null, null, "Accept-Encoding: gzip\r\n");
         Assert.AreEqual(200, zippedWorker.Status, zippedWorker.Body);
         Assert.AreEqual("gzip", zippedWorker.Header("Content-Encoding"));
         Assert.AreEqual(worker.BodyBytes, Gunzip(zippedWorker.BodyBytes));
         Assert.AreEqual("/", zippedWorker.Header("Service-Worker-Allowed"),
            "The worker's own header survives the coding.");

         // The one answer under /api/v1 that is compressed: the OpenAPI
         // description, the same bytes for every caller, JSON and large.
         Response document = Raw("GET", "/api/v1/openapi.json", AdminHeader(), null);
         Response zippedDocument = Raw("GET", "/api/v1/openapi.json", AdminHeader(), null, "Accept-Encoding: gzip\r\n");
         Assert.AreEqual(200, zippedDocument.Status, zippedDocument.Body);
         Assert.AreEqual("gzip", zippedDocument.Header("Content-Encoding"));
         Assert.AreEqual(document.BodyBytes, Gunzip(zippedDocument.BodyBytes));
      }

      [Test]
      [Description("A weight of zero is a refusal, and a star asks for whatever the server has")]
      public void TheWeightsAreHonoured()
      {
         Response plain = Raw("GET", "/portal.js", null, null);

         Response refused = Raw("GET", "/portal.js", null, null, "Accept-Encoding: gzip;q=0\r\n");
         Assert.AreEqual("", refused.Header("Content-Encoding"), "q=0 is a refusal of that coding.");
         Assert.AreEqual(plain.BodyBytes, refused.BodyBytes);

         Response instead = Raw("GET", "/portal.js", null, null, "Accept-Encoding: gzip;q=0, deflate\r\n");
         Assert.AreEqual("deflate", instead.Header("Content-Encoding"),
            "Refusing gzip leaves deflate, which was offered.");
         Assert.AreEqual(plain.BodyBytes, Inflate(instead.BodyBytes));

         Response anything = Raw("GET", "/portal.js", null, null, "Accept-Encoding: *\r\n");
         Assert.AreEqual("gzip", anything.Header("Content-Encoding"), "A star stands for whatever the server has.");
         Assert.AreEqual(plain.BodyBytes, Gunzip(anything.BodyBytes));

         Response none = Raw("GET", "/portal.js", null, null, "Accept-Encoding: br, identity;q=0.5\r\n");
         Assert.AreEqual("", none.Header("Content-Encoding"),
            "This server speaks neither Brotli nor anything else the client named.");
         Assert.AreEqual(plain.BodyBytes, none.BodyBytes);
      }

      [Test]
      [Description("An answer under a kilobyte is served as it is: the framing and the poorer ratio would eat the saving")]
      public void AShortAnswerIsLeftAlone()
      {
         Response manifest = Raw("GET", "/portal.webmanifest", null, null, "Accept-Encoding: gzip, deflate\r\n");

         Assert.AreEqual(200, manifest.Status, manifest.Body);
         Assert.IsTrue(manifest.BodyBytes.Length < MinimumBytes,
            "Portal.webmanifest is the short answer this test is about; it is " + manifest.BodyBytes.Length + " bytes.");
         Assert.AreEqual("", manifest.Header("Content-Encoding"), "Under the floor, nothing is compressed.");
         Assert.AreEqual("Accept-Encoding", manifest.Header("Vary"),
            "Its type is one this server would compress, so the answer still varies on what the client can decode.");
         Assert.AreEqual(manifest.BodyBytes.Length, int.Parse(manifest.Header("Content-Length")));
         StringAssert.Contains("\"name\"", manifest.Body);
      }

      // A PNG: bytes that are already compressed, under a media type that
      // says so.
      private static readonly byte[] Png = Convert.FromBase64String(
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

      // And a text file large enough that the floor is not what keeps an
      // answer carrying it uncompressed - only the policy is.
      private static byte[] Prose()
      {
         var text = new StringBuilder();
         while (text.Length < 40000)
            text.Append("The quick brown fox jumps over the lazy dog. ");
         return Encoding.ASCII.GetBytes(text.ToString());
      }

      /// <summary>
      ///    Delivers a message with the two files attached and answers its id
      ///    and the inbox's.
      /// </summary>
      private long DeliverTwoFiles(out long inboxId)
      {
         string raw =
            "From: sender@example.com\r\n" +
            "To: " + Address + "\r\n" +
            "Subject: Two files\r\n" +
            "MIME-Version: 1.0\r\n" +
            "Content-Type: multipart/mixed; boundary=\"b1\"\r\n" +
            "\r\n" +
            "--b1\r\n" +
            "Content-Type: text/plain\r\n" +
            "\r\n" +
            "See attached.\r\n" +
            Base64Part("image/png", "dot.png", Png) +
            Base64Part("text/plain", "prose.txt", Prose()) +
            "--b1--\r\n";

         SmtpClientSimulator.StaticSendRaw("sender@example.com", Address, raw);
         Pop3ClientSimulator.AssertMessageCount(Address, UserPassword, 1);

         Response folders = Raw("GET", "/api/v1/me/folders", UserHeader(), null);
         inboxId = IdBefore(folders.Body, "\"path\":\"INBOX\"");
         Response listing = Raw("GET", "/api/v1/me/folders/" + inboxId + "/messages", UserHeader(), null);
         return IdBefore(listing.Body, "\"subject\":\"Two files\"");
      }

      [Test]
      [Description("A download is never compressed - not the already-compressed image, and not the text file either, whose type says nothing about what is under it")]
      public void ADownloadIsNeverCompressed()
      {
         byte[] png = Png;
         byte[] prose = Prose();

         long inboxId;
         long messageId = DeliverTwoFiles(out inboxId);

         string path = "/api/v1/me/messages/" + messageId + "/attachments/";

         Response image = Raw("GET", path + "0", UserHeader(), null, "Accept-Encoding: gzip, deflate\r\n");
         Assert.AreEqual(200, image.Status, image.Body);
         Assert.AreEqual("image/png", image.Header("Content-Type"));
         Assert.AreEqual("", image.Header("Content-Encoding"), "A PNG is already compressed.");
         Assert.AreEqual("", image.Header("Vary"), "And nothing about it varies on what the client can decode.");
         Assert.AreEqual(png, image.BodyBytes, "The bytes come back exactly as attached.");

         Response file = Raw("GET", path + "1", UserHeader(), null, "Accept-Encoding: gzip, deflate\r\n");
         Assert.AreEqual(200, file.Status, file.Body);
         Assert.AreEqual("text/plain", file.Header("Content-Type"));
         Assert.IsTrue(file.BodyBytes.Length > MinimumBytes,
            "The text file is well over the floor, so the floor is not what is keeping it plain.");
         Assert.AreEqual("", file.Header("Content-Encoding"),
            "A download is bytes to keep, and its media type was chosen by whoever sent the message.");
         Assert.AreEqual(prose, file.BodyBytes, "The bytes come back exactly as attached.");
      }

      [Test]
      [Description("An answer built from the database is never compressed, however large and however textual: a mailbox listing, and the message file the account can save")]
      public void AMailboxAnswerIsNeverCompressed()
      {
         long inboxId;
         long messageId = DeliverTwoFiles(out inboxId);

         // The listing: the subject the sender chose beside everything else in
         // the folder, which is the answer the webmail re-reads on its own.
         // Small here, so the floor would keep it plain on its own; what is
         // asserted is the Vary, which a compressible type would carry even
         // under the floor (AShortAnswerIsLeftAlone) and a refused one does
         // not.
         Response listing = Raw("GET", "/api/v1/me/folders/" + inboxId + "/messages", UserHeader(), null,
            "Accept-Encoding: gzip, deflate\r\n");
         Assert.AreEqual(200, listing.Status, listing.Body);
         StringAssert.Contains("\"subject\":\"Two files\"", listing.Body);
         Assert.AreEqual("", listing.Header("Content-Encoding"), "A mailbox listing is never compressed.");
         Assert.AreEqual("", listing.Header("Vary"),
            "And nothing about it varies on what the client can decode - it was refused, not merely short.");

         // The message file: message/rfc822, a download, and well over the
         // floor, so the only thing keeping it plain is the policy.
         Response plain = Raw("GET", "/api/v1/me/messages/" + messageId + "/source", UserHeader(), null);
         Response offered = Raw("GET", "/api/v1/me/messages/" + messageId + "/source", UserHeader(), null,
            "Accept-Encoding: gzip, deflate\r\n");
         Assert.AreEqual(200, offered.Status, offered.Body);
         Assert.AreEqual("message/rfc822", offered.Header("Content-Type"));
         StringAssert.StartsWith("attachment;", offered.Header("Content-Disposition"));
         Assert.IsTrue(offered.BodyBytes.Length > MinimumBytes,
            "The message file is well over the floor; it is " + offered.BodyBytes.Length + " bytes.");
         Assert.AreEqual("", offered.Header("Content-Encoding"), "A message file is a download, and built from the database.");
         Assert.AreEqual("", offered.Header("Vary"));
         Assert.AreEqual(plain.BodyBytes, offered.BodyBytes, "The same bytes whatever the client offered.");
         Assert.AreEqual(offered.BodyBytes.Length, int.Parse(offered.Header("Content-Length")));
      }

      // ---------------------------------------------------------------------

      private static byte[] Gunzip(byte[] bytes)
      {
         using (var input = new MemoryStream(bytes))
         using (var gzip = new GZipStream(input, CompressionMode.Decompress))
         using (var output = new MemoryStream())
         {
            gzip.CopyTo(output);
            return output.ToArray();
         }
      }

      /// <summary>
      ///    "deflate" on the wire is the zlib format of RFC 1950 - two header
      ///    bytes, the DEFLATE stream, a four-byte Adler-32 - which is what RFC
      ///    9110 means by the name. .NET Framework has no reader for it
      ///    (ZLibStream arrived in .NET 6), so the wrapper comes off here and
      ///    the middle goes to DeflateStream. That is the assertion worth
      ///    making: a server that sent a bare DEFLATE stream under this name -
      ///    the mistake half the web made reading RFC 2616 - fails at the
      ///    header check below.
      /// </summary>
      private static byte[] Inflate(byte[] bytes)
      {
         Assert.IsTrue(bytes.Length > 6, "A zlib stream is a header, a body and a checksum.");
         Assert.AreEqual(8, bytes[0] & 0x0f, "The low nibble of a zlib header names the method: 8 is DEFLATE.");
         Assert.AreEqual(0, ((bytes[0] << 8) + bytes[1]) % 31, "The two header bytes of a zlib stream are a multiple of 31.");

         using (var input = new MemoryStream(bytes, 2, bytes.Length - 6))
         using (var raw = new DeflateStream(input, CompressionMode.Decompress))
         using (var output = new MemoryStream())
         {
            raw.CopyTo(output);
            return output.ToArray();
         }
      }

      private static string Base64Part(string contentType, string name, byte[] content)
      {
         return "--b1\r\n" +
                "Content-Type: " + contentType + "; name=\"" + name + "\"\r\n" +
                "Content-Transfer-Encoding: base64\r\n" +
                "Content-Disposition: attachment; filename=\"" + name + "\"\r\n" +
                "\r\n" +
                Convert.ToBase64String(content, Base64FormattingOptions.InsertLineBreaks) + "\r\n";
      }

      private static long IdBefore(string body, string marker)
      {
         int at = body.IndexOf(marker, StringComparison.Ordinal);
         Assert.IsTrue(at >= 0, "Missing " + marker + " in: " + body);
         int idAt = body.LastIndexOf("\"id\":", at, StringComparison.Ordinal);
         Assert.IsTrue(idAt >= 0, "No id before " + marker + " in: " + body);

         int digits = idAt + 5;
         int end = digits;
         while (end < body.Length && body[end] >= '0' && body[end] <= '9')
            end++;
         return long.Parse(body.Substring(digits, end - digits));
      }

      private static string AdminHeader()
      {
         return BasicHeader("Administrator", AdminPassword);
      }

      private string UserHeader()
      {
         return BasicHeader(Address, UserPassword);
      }

      private static string BasicHeader(string user, string password)
      {
         return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes(user + ":" + password));
      }

      private sealed class Response
      {
         public int Status;
         public string Body = "";
         public byte[] BodyBytes = new byte[0];
         public readonly System.Collections.Generic.Dictionary<string, string> Headers =
            new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

         public string Header(string name)
         {
            string value;
            return Headers.TryGetValue(name, out value) ? value : "";
         }
      }

      /// <summary>
      ///    One request, with the head parsed and the body kept as bytes. It has
      ///    to be bytes here: a compressed body is not text, and reading it as
      ///    UTF-8 would replace every byte the decoder did not like.
      /// </summary>
      private static Response Raw(string method, string path, string authorization, string requestBody,
                                  string extraHeaders = null)
      {
         using (var client = new TcpClient())
         {
            Exception last = null;
            for (int attempt = 0; attempt < 25; attempt++)
            {
               try
               {
                  client.Connect("127.0.0.1", RestPort);
                  last = null;
                  break;
               }
               catch (SocketException ex)
               {
                  last = ex;
                  Thread.Sleep(200);
               }
            }

            if (last != null)
               throw last;

            using (NetworkStream stream = client.GetStream())
            using (var memory = new MemoryStream())
            {
               var headers = new StringBuilder();
               headers.Append(method + " " + path + " HTTP/1.0\r\n");
               headers.Append("Host: 127.0.0.1\r\n");
               if (authorization != null)
                  headers.Append("Authorization: " + authorization + "\r\n");
               if (extraHeaders != null)
                  headers.Append(extraHeaders);

               byte[] bodyBytes = requestBody == null ? new byte[0] : Encoding.UTF8.GetBytes(requestBody);
               if (requestBody != null)
               {
                  headers.Append("Content-Type: application/json\r\n");
                  headers.Append("Content-Length: " + bodyBytes.Length + "\r\n");
               }

               headers.Append("Connection: close\r\n\r\n");

               byte[] headerBytes = Encoding.ASCII.GetBytes(headers.ToString());
               stream.Write(headerBytes, 0, headerBytes.Length);
               if (bodyBytes.Length > 0)
                  stream.Write(bodyBytes, 0, bodyBytes.Length);

               byte[] buffer = new byte[8192];
               int read;
               while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                  memory.Write(buffer, 0, read);

               byte[] all = memory.ToArray();
               var response = new Response();

               // The head is ASCII, so the separator's position in the decoded
               // head is its position in the bytes; the body is taken from the
               // bytes and never from the decoded string.
               int separator = IndexOfHeadEnd(all);
               string head = Encoding.ASCII.GetString(all, 0, separator >= 0 ? separator : all.Length);

               if (separator >= 0)
               {
                  response.BodyBytes = new byte[all.Length - separator - 4];
                  Array.Copy(all, separator + 4, response.BodyBytes, 0, response.BodyBytes.Length);
                  response.Body = Encoding.UTF8.GetString(response.BodyBytes);
               }

               string[] lines = head.Split(new[] { "\r\n" }, StringSplitOptions.None);
               if (lines.Length > 0)
               {
                  string[] parts = lines[0].Split(' ');
                  if (parts.Length >= 2)
                     int.TryParse(parts[1], out response.Status);
               }

               for (int i = 1; i < lines.Length; i++)
               {
                  int colon = lines[i].IndexOf(':');
                  if (colon > 0)
                     response.Headers[lines[i].Substring(0, colon).Trim()] = lines[i].Substring(colon + 1).Trim();
               }

               return response;
            }
         }
      }

      private static int IndexOfHeadEnd(byte[] bytes)
      {
         for (int i = 0; i + 3 < bytes.Length; i++)
         {
            if (bytes[i] == 13 && bytes[i + 1] == 10 && bytes[i + 2] == 13 && bytes[i + 3] == 10)
               return i;
         }
         return -1;
      }
   }
}
