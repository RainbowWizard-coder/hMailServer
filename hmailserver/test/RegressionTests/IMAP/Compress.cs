// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;
using hMailServer;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    RFC 4978, COMPRESS=DEFLATE. After "COMPRESS DEFLATE" and its OK, both directions
   ///    of the connection are raw DEFLATE: the server inflates commands and literals,
   ///    deflates responses with a flush at the end of each, and the protocol above is
   ///    unchanged.
   /// </summary>
   [TestFixture]
   public class Compress : TestFixtureBase
   {
      private Account _account;

      [SetUp]
      public void CreateAccount()
      {
         _account = SingletonProvider<TestSetup>.Instance.AddAccount(_domain, "compress@example.test", "test");
      }

      [Test]
      public void TheCapabilityIsAdvertisedUntilCompressionIsOn()
      {
         using (var client = new DeflateImapClient())
         {
            StringAssert.DoesNotContain("COMPRESS=DEFLATE", client.Command("A1 CAPABILITY"), "Not offered before authentication, where it is not taken.");
            client.Login(_account.Address, "test");
            StringAssert.Contains("COMPRESS=DEFLATE", client.Command("A2 CAPABILITY"), "Offered on a plain connection once signed in.");
            client.Compress();
            string capability = client.Command("A3 CAPABILITY");
            StringAssert.Contains("* CAPABILITY IMAP4", capability, "The response arrived inflated.");
            StringAssert.DoesNotContain("COMPRESS=DEFLATE", capability, "RFC 4978: not a capability of a connection that already compresses.");
         }
      }

      [Test]
      public void CommandsAndResponsesRunCompressedAfterTheOk()
      {
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "compressed subject", "compressed body line one\r\nline two\r\n");
         Pop3ClientSimulator.AssertMessageCount(_account.Address, "test", 1);

         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            client.Compress();

            string select = client.Command("A3 SELECT INBOX");
            StringAssert.Contains("* 1 EXISTS", select);
            StringAssert.Contains("A3 OK", select);

            string fetch = client.Command("A4 FETCH 1 (FLAGS BODY[HEADER.FIELDS (SUBJECT)])");
            StringAssert.Contains("compressed subject", fetch, "The subject header came back inflated.");
            StringAssert.Contains("A4 OK", fetch);

            StringAssert.Contains("A5 OK", client.Command("A5 NOOP"));

            string logout = client.Command("A6 LOGOUT");
            StringAssert.Contains("* BYE", logout);
            StringAssert.Contains("A6 OK", logout);
         }
      }

      [Test]
      public void ALargeResponseRoundTripsInflated()
      {
         // A body well past one DEFLATE block and one socket segment, with markers at
         // both ends, so a lost or duplicated block shows.
         var body = new StringBuilder("BEGIN-MARKER\r\n");
         var random = new Random(2026);
         for (int i = 0; i < 4000; i++)
         {
            var line = new char[60];
            for (int j = 0; j < line.Length; j++)
               line[j] = (char) ('a' + random.Next(26));
            body.Append(new string(line)).Append("\r\n");
         }
         body.Append("END-MARKER\r\n");
         string text = body.ToString();
         SmtpClientSimulator.StaticSend("sender@example.test", _account.Address, "large", text);
         Pop3ClientSimulator.AssertMessageCount(_account.Address, "test", 1);

         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            client.Compress();
            StringAssert.Contains("A3 OK", client.Command("A3 SELECT INBOX"));

            string fetch = client.Command("A4 FETCH 1 BODY[TEXT]");
            StringAssert.Contains("A4 OK", fetch);
            StringAssert.Contains("BEGIN-MARKER", fetch);
            StringAssert.Contains("END-MARKER", fetch);
            int begin = fetch.IndexOf("BEGIN-MARKER", StringComparison.Ordinal);
            int end = fetch.IndexOf("END-MARKER\r\n", StringComparison.Ordinal) + "END-MARKER\r\n".Length;
            Assert.AreEqual(text, fetch.Substring(begin, end - begin), "The body came back byte for byte through the inflater.");
         }
      }

      [Test]
      public void AnAppendLiteralIsInflatedOnTheWayIn()
      {
         string message = "From: sender@example.com\r\nTo: compress@example.test\r\nSubject: appended under compression\r\n\r\nThe literal travelled as DEFLATE.\r\n";

         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            client.Compress();

            string continuation = client.Command("A3 APPEND INBOX {" + message.Length + "}", "+");
            StringAssert.StartsWith("+", continuation, "The server asks for the literal.");
            string appended = client.Command(message, "A3");
            StringAssert.Contains("A3 OK", appended);

            StringAssert.Contains("* 1 EXISTS", client.Command("A4 SELECT INBOX"));
            string fetch = client.Command("A5 FETCH 1 BODY[]");
            StringAssert.Contains("Subject: appended under compression", fetch);
            StringAssert.Contains("The literal travelled as DEFLATE.", fetch);
            StringAssert.Contains("A5 OK", fetch);
         }
      }

      [Test]
      public void AnAppendLiteralThatInflatesPastTheReceiveBufferArrivesWhole()
      {
         // A run of one letter, sent as a dynamic-Huffman DEFLATE block of 258-octet
         // matches at two bits each: about a thousand plain octets for every
         // compressed one. One socket read of a few hundred compressed octets then
         // inflates past the connection's 250,000-octet receive buffer, and the
         // tail past it was dropped without a word - the literal arrived short and
         // the server went on reading the command's last line as message octets.
         const string header = "From: sender@example.com\r\nTo: compress@example.test\r\nSubject: inflated past the buffer\r\n\r\n";
         const int matches = 5000;
         int run = 1 + 258 * matches;
         int size = header.Length + run + 2;

         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            client.Compress();

            string continuation = client.Command("A3 APPEND INBOX {" + size + "}", "+");
            StringAssert.StartsWith("+", continuation, "The server asks for the literal.");

            client.SendText(header);
            client.SendDeflate(DeflateImapClient.RunOfTheLetterA(matches));
            // The literal's own last CRLF, then the command's.
            string appended = client.Command("\r\n", "A3");
            StringAssert.Contains("A3 OK", appended, "The whole literal must arrive and be stored: " + appended);

            StringAssert.Contains("* 1 EXISTS", client.Command("A4 SELECT INBOX"));
            string fetch = client.Command("A5 FETCH 1 (RFC822.SIZE BODY.PEEK[]<" + (size - 12) + ".12>)");
            StringAssert.Contains("RFC822.SIZE " + size, fetch, "The stored message is the size of the literal: " + fetch);
            StringAssert.Contains("AAAAAAAAAA\r\n", fetch, "The message ends with the run and its line break: " + fetch);
            StringAssert.Contains("A5 OK", fetch);
         }
      }

      [Test]
      public void ASecondCompressIsRefusedWithCompressionActive()
      {
         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            client.Compress();
            string again = client.Command("A3 COMPRESS DEFLATE");
            StringAssert.Contains("A3 NO [COMPRESSIONACTIVE]", again);
            StringAssert.Contains("A4 OK", client.Command("A4 NOOP"), "And the connection carries on, compressed.");
         }
      }

      [Test]
      public void AnAlgorithmOtherThanDeflateIsBad()
      {
         using (var client = new DeflateImapClient())
         {
            client.Login(_account.Address, "test");
            StringAssert.Contains("A2 BAD", client.Command("A2 COMPRESS LZMA"));
            StringAssert.Contains("A3 BAD", client.Command("A3 COMPRESS"));
            StringAssert.Contains("A4 OK", client.Command("A4 NOOP"), "Still plain, still working.");
         }
      }

      [Test]
      public void CompressIsRefusedBeforeLogin()
      {
         // Taken once signed in only: before authentication it put the zlib
         // inflater within reach of anyone who could connect, for no client
         // that needs it there. The connection stays plain and usable.
         using (var client = new DeflateImapClient())
         {
            StringAssert.Contains("A1 NO", client.Command("A1 COMPRESS DEFLATE"));
            StringAssert.Contains("A2 OK", client.Command("A2 LOGIN " + _account.Address + " test"));
            client.Compress("A3");
            StringAssert.Contains("A4 OK", client.Command("A4 SELECT INBOX"));
         }
      }

      [Test]
      public void StartTlsIsRefusedOnceCompressed()
      {
         using (var client = new DeflateImapClient())
         {
            if (!client.Command("A1 CAPABILITY").Contains("STARTTLS"))
               Assert.Ignore("STARTTLS is not offered on the plain IMAP port in this environment.");
            client.Login(_account.Address, "test");
            client.Compress("A3");
            StringAssert.Contains("A4 BAD", client.Command("A4 STARTTLS"), "RFC 4978 section 3: no TLS under compression.");
         }
      }

      [Test]
      public void CompressionCanBeSwitchedOff()
      {
         IniFileSetting.Write("IMAPCompressionEnabled", "0");
         _application.Reinitialize();
         try
         {
            using (var client = new DeflateImapClient())
            {
               StringAssert.DoesNotContain("COMPRESS=DEFLATE", client.Command("A1 CAPABILITY"));
               StringAssert.Contains("A2 BAD", client.Command("A2 COMPRESS DEFLATE"));
            }
         }
         finally
         {
            IniFileSetting.Write("IMAPCompressionEnabled", "1");
            _application.Reinitialize();
         }
      }

      /// <summary>
      ///    An IMAP client on port 143 that can switch to DEFLATE. Plain reads come off
      ///    the socket directly; once compression is on, a background thread accumulates
      ///    the raw compressed bytes and every read re-inflates the whole run from the
      ///    start with a fresh DeflateStream over a MemoryStream. That is deliberate:
      ///    .NET Framework's DeflateStream never returns output at a sync-flush boundary
      ///    on a live socket - it reads on for the next block and blocks - but it does
      ///    return everything when the underlying stream ends, and replaying from byte
      ///    zero rebuilds the LZ77 window the server's persistent deflater relies on
      ///    across flushes. O(n^2), which is nothing for a test. Writes are stored
      ///    DEFLATE blocks (RFC 1951 section 3.2.4: valid, uncompressed).
      /// </summary>
      private sealed class DeflateImapClient : IDisposable
      {
         private readonly TcpClient _client = new TcpClient();
         private readonly NetworkStream _network;
         private volatile bool _compressed;
         private volatile bool _closed;
         private volatile Exception _readerError;
         private Thread _readerThread;
         private readonly object _lock = new object();
         private readonly List<byte> _rawCompressed = new List<byte>();
         private int _consumed;   // decoded characters already returned by ReadUntil

         public DeflateImapClient()
         {
            _client.ReceiveTimeout = 30000;
            _client.SendTimeout = 30000;
            _client.Connect(TestPorts.HostAddress, TestPorts.Actual(143));
            _network = _client.GetStream();
            StringAssert.Contains("* OK", ReadUntil("* OK"));
         }

         public void Login(string address, string password)
         {
            StringAssert.Contains("A2 OK", Command("A2 LOGIN " + address + " " + password));
         }

         public void Compress(string tag = "A2")
         {
            // The OK is the last plain line; only after it is read does the raw-byte
            // collector start, so no compressed byte is ever read as plain.
            StringAssert.Contains(tag + " OK DEFLATE active", Command(tag + " COMPRESS DEFLATE"));
            _compressed = true;
            _readerThread = new Thread(CollectRaw) {IsBackground = true, Name = "DeflateImapClient"};
            _readerThread.Start();
         }

         private void CollectRaw()
         {
            var buffer = new byte[16384];
            try
            {
               while (!_closed)
               {
                  int read = _network.Read(buffer, 0, buffer.Length);
                  if (read <= 0)
                     break;
                  lock (_lock)
                     for (int i = 0; i < read; i++)
                        _rawCompressed.Add(buffer[i]);
               }
            }
            catch (Exception ex) when (RecordUnlessClosed(ex))
            {
               // Every exception, deliberately: nothing may leave a background thread,
               // where an unhandled one ends the whole run instead of one test. The
               // filter takes all of them and does the deciding, so that _closed is read
               // exactly once - two clauses, one filtered on the flag and one on its
               // negation, would read it twice, and Dispose setting it between those two
               // reads would leave the exception unhandled here.
               Trace.WriteLine("DeflateImapClient: the reader stopped on " + ex.GetType().Name + ": " + ex.Message);
            }
         }

         // The reader's catch filter, true for every exception. The single read of _closed
         // is what says which kind this is: after Dispose it is the socket closed under the
         // read this thread was blocked in, which is how the reader is meant to end; before
         // it, it is a real failure, and ReadUntil fails the test with it.
         private bool RecordUnlessClosed(Exception ex)
         {
            if (!_closed)
               _readerError = ex;
            return true;
         }

         // The whole compressed run inflated from the start, as an ISO-8859-1 string.
         private string InflateAll()
         {
            byte[] raw;
            lock (_lock)
               raw = _rawCompressed.ToArray();
            if (raw.Length == 0)
               return "";
            using (var input = new MemoryStream(raw))
            using (var deflate = new DeflateStream(input, CompressionMode.Decompress))
            using (var output = new MemoryStream())
            {
               deflate.CopyTo(output);
               return Encoding.GetEncoding("ISO-8859-1").GetString(output.ToArray());
            }
         }

         // Sends a line and returns everything up to and including the line that
         // starts with the tag (the command's own tag unless another is given).
         public string Command(string line, string tag = null)
         {
            tag = tag ?? line.Substring(0, line.IndexOf(' '));
            Send(line + "\r\n");
            return ReadUntil("\r\n" + tag + " ", tag + " ");
         }

         private void Send(string text)
         {
            byte[] bytes = Encoding.ASCII.GetBytes(text);
            if (!_compressed)
            {
               _network.Write(bytes, 0, bytes.Length);
               return;
            }
            byte[] stored = StoredDeflate(bytes);
            _network.Write(stored, 0, stored.Length);
         }

         // Text without a line break of its own, as stored blocks.
         public void SendText(string text)
         {
            Send(text);
         }

         // DEFLATE bytes built by the test, as they are. They must leave the stream
         // on a byte boundary, as RunOfTheLetterA does.
         public void SendDeflate(byte[] deflate)
         {
            _network.Write(deflate, 0, deflate.Length);
         }

         // One non-final dynamic-Huffman block (RFC 1951 section 3.2.7) that
         // inflates to "A" followed by 'matches' copies of a 258-octet match at
         // distance 1, then an empty stored block to come back to a byte boundary.
         // Literal/length code: 285 (length 258) '0', 'A' '10', end of block '11';
         // distance code 0 '0'; the code-length code: 18 '0', 1 '10', 2 '11'.
         public static byte[] RunOfTheLetterA(int matches)
         {
            var bits = new BitWriter();
            bits.Write(0, 1);          // BFINAL: more blocks follow
            bits.Write(2, 2);          // BTYPE 10: dynamic Huffman
            bits.Write(286 - 257, 5);  // HLIT: literal/length codes 0-285
            bits.Write(1 - 1, 5);      // HDIST: distance code 0
            int[] order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1};
            bits.Write(order.Length - 4, 4);
            foreach (int symbol in order)
               bits.Write(symbol == 18 ? 1 : (symbol == 1 || symbol == 2) ? 2 : 0, 3);

            // The 286 literal/length code lengths and the one distance code length.
            bits.Code(0, 1); bits.Write(65 - 11, 7);     // 0-64 unused
            bits.Code(3, 2);                             // 'A' (65): 2
            bits.Code(0, 1); bits.Write(138 - 11, 7);    // 66-203 unused
            bits.Code(0, 1); bits.Write(52 - 11, 7);     // 204-255 unused
            bits.Code(3, 2);                             // end of block (256): 2
            bits.Code(0, 1); bits.Write(28 - 11, 7);     // 257-284 unused
            bits.Code(2, 2);                             // 285: 1
            bits.Code(2, 2);                             // distance 0: 1

            bits.Code(2, 2);                             // 'A'
            for (int i = 0; i < matches; i++)
            {
               bits.Code(0, 1);                          // length 258
               bits.Code(0, 1);                          // distance 1
            }
            bits.Code(3, 2);                             // end of block

            // An empty stored block: its header, the pad to the byte, LEN 0, NLEN.
            bits.Write(0, 3);
            bits.Align();
            bits.Bytes.AddRange(new byte[] {0x00, 0x00, 0xFF, 0xFF});
            return bits.Bytes.ToArray();
         }

         private sealed class BitWriter
         {
            public readonly List<byte> Bytes = new List<byte>();
            private int _current;
            private int _count;

            // A header field or extra bits: least significant bit first.
            public void Write(int value, int length)
            {
               for (int i = 0; i < length; i++)
                  Bit((value >> i) & 1);
            }

            // A Huffman code: most significant bit first.
            public void Code(int code, int length)
            {
               for (int i = length - 1; i >= 0; i--)
                  Bit((code >> i) & 1);
            }

            public void Align()
            {
               if (_count > 0)
               {
                  Bytes.Add((byte) _current);
                  _current = 0;
                  _count = 0;
               }
            }

            private void Bit(int bit)
            {
               _current |= bit << _count;
               _count++;
               if (_count == 8)
                  Align();
            }
         }

         private string ReadUntil(params string[] markers)
         {
            var plainBuffer = new byte[16384];
            var plain = new StringBuilder();
            DateTime deadline = DateTime.Now.AddSeconds(30);
            while (true)
            {
               // What has arrived since the last line was returned: the tail of the
               // inflated run when compressed, the plain buffer before that.
               string available = _compressed ? InflateAll().Substring(_consumed) : plain.ToString();

               foreach (string marker in markers)
               {
                  int at = available.StartsWith(marker, StringComparison.Ordinal) ? 0 : available.IndexOf(marker, StringComparison.Ordinal);
                  if (at < 0)
                     continue;
                  int lineEnd = available.IndexOf("\r\n", at + marker.Length, StringComparison.Ordinal);
                  if (lineEnd < 0)
                     continue;
                  string result = available.Substring(0, lineEnd + 2);
                  if (_compressed)
                     _consumed += lineEnd + 2;
                  else
                     plain.Remove(0, lineEnd + 2);
                  return result;
               }

               if (_readerError != null)
                  Assert.Fail("The reader failed: " + _readerError);
               if (DateTime.Now > deadline)
                  Assert.Fail("No line with " + string.Join(" or ", markers) + " within 30 seconds. Received so far:\n" + available);

               if (_compressed)
               {
                  Thread.Sleep(20);
                  continue;
               }

               int n = _network.Read(plainBuffer, 0, plainBuffer.Length);
               if (n <= 0)
                  Assert.Fail("The server closed the connection. Received so far:\n" + plain);
               plain.Append(Encoding.GetEncoding("ISO-8859-1").GetString(plainBuffer, 0, n));
            }
         }

         // RFC 1951 stored blocks: a zero header byte (not final, type 00), the length
         // and its complement little-endian, the bytes. An empty payload still emits
         // one empty block, which is a valid flush.
         private static byte[] StoredDeflate(byte[] data)
         {
            using (var stream = new MemoryStream())
            {
               int offset = 0;
               do
               {
                  int length = Math.Min(65535, data.Length - offset);
                  int complement = ~length & 0xFFFF;
                  stream.WriteByte(0x00);
                  stream.WriteByte((byte) (length & 0xFF));
                  stream.WriteByte((byte) (length >> 8));
                  stream.WriteByte((byte) (complement & 0xFF));
                  stream.WriteByte((byte) (complement >> 8));
                  if (length > 0)
                     stream.Write(data, offset, length);
                  offset += length;
               } while (offset < data.Length);
               return stream.ToArray();
            }
         }

         public void Dispose()
         {
            _closed = true;
            try
            {
               _client.Close();
            }
            catch (Exception ex) when (ex is IOException || ex is SocketException || ex is ObjectDisposedException)
            {
               // The reader thread is blocked in a read on this socket, and closing it is
               // how that read is ended; a close that throws is that race, not a failure
               // of the test. The Join below still waits for the reader to leave.
               Trace.WriteLine("DeflateImapClient: closing the socket threw " + ex.GetType().Name + ": " + ex.Message);
            }
            if (_readerThread != null)
               _readerThread.Join(2000);
         }
      }
   }
}
