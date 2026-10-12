// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Text;
using NUnit.Framework;
using NUnit.Framework.Legacy;
using RegressionTests.Shared;

namespace RegressionTests.IMAP
{
   /// <summary>
   ///    The octets a FETCH hands out for the parts of a message are the octets the
   ///    message is stored with. Each part's BODY[N.MIME] is its header as written -
   ///    folds, spacing, 8-bit bytes and line breaks as they are - and BODY[N] its
   ///    body, a multipart's with its preamble, its boundary lines and its epilogue,
   ///    and with no closing boundary where the message has none; so that the two
   ///    together are the part, where BODY[] has it. BODY[N.MIME] was each field
   ///    written out again, unfolded, and BODY[N] of a multipart its parts written
   ///    out again, boundaries spelt afresh, the epilogue dropped and a missing
   ///    closing boundary added.
   ///
   ///    And the sizes agree: RFC822.SIZE, BODY[], BODY[HEADER] with BODY[TEXT], each
   ///    BODYSTRUCTURE size and line count and the octets of the part it describes.
   ///    A BODYSTRUCTURE is read here as RFC 3501 7.4.2 writes it, strictly: one of
   ///    messages nested in messages past its depth, which was a message with
   ///    nothing where its body goes, and one of a multipart with no parts, which
   ///    was a multipart with no body, are ones a client can read; and BODY, "a form
   ///    of BODYSTRUCTURE without extension data", carries none.
   ///
   ///    Everything is sent and read as octets, one character to a byte, so that
   ///    what is compared is exactly what the server sent.
   /// </summary>
   [TestFixture]
   public class FetchPartBytes : TestFixtureBase
   {
      private const string Password = "test";

      // One character to an octet, both ways.
      private static readonly Encoding Octets = Encoding.GetEncoding(28591);

      // ------------------------------------------------------------------ messages

      // A part as the message was written: its number, its MIME header and its body.
      private sealed class WrittenPart
      {
         public string Number;
         public string Header;
         public string Body;
         public bool Multipart;

         // For a message/rfc822 part, the header and the body of the message it holds.
         public string InnerHeader;
         public string InnerBody;
      }

      private sealed class WrittenMessage
      {
         public string Text;
         public readonly List<WrittenPart> Parts = new List<WrittenPart>();

         public WrittenPart Part(string number, string header, string body)
         {
            var part = new WrittenPart { Number = number, Header = header, Body = body };
            Parts.Add(part);
            return part;
         }
      }

      // Multiparts in multiparts with preambles and epilogues, a message holding
      // another multipart, folded fields, fields with no space after the colon,
      // 8-bit bytes in a header and in a body.
      private static WrittenMessage NestedMessage()
      {
         var message = new WrittenMessage();

         string p1Header = "Content-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: 8bit\r\nContent-Description:caf\x00E9\r\n\r\n";
         string p1Body = "caf\x00E9 cr\x00E8me \x00E0 la carte\r\n";
         message.Part("1", p1Header, p1Body);

         string p21Header = "Content-Type:text/plain\r\n\r\n";
         string p21Body = "plain";
         string p22Header = "Content-Type: text/html; charset=\"utf-8\"\r\n\r\n";
         string p22Body = "<p>caf\x00C3\x00A9</p>\r\n";
         string p2Header = "Content-Type: multipart/alternative;\r\n\tboundary=\"inner\"\r\nX-Folded: one\r\n  two\r\nX-No-Space:value\r\n\r\n";
         string p2Body = "inner preamble\r\n" +
            "--inner\r\n" + p21Header + p21Body +
            "\r\n--inner\r\n" + p22Header + p22Body +
            "\r\n--inner--\r\ninner epilogue\r\n";
         message.Part("2", p2Header, p2Body).Multipart = true;
         message.Part("2.1", p21Header, p21Body);
         message.Part("2.2", p22Header, p22Body);

         string p31Header = "Content-Type: text/plain\r\n\r\n";
         string p31Body = "deep one\r\n";
         string p321Header = "Content-Type: application/octet-stream\r\nContent-Transfer-Encoding: base64\r\n\r\n";
         string p321Body = "aGVsbG8gd29ybGQ=\r\n";
         string p32Header = "Content-Type: multipart/related; boundary=\"deeper\"\r\n\r\n";
         string p32Body = "--deeper\r\n" + p321Header + p321Body + "\r\n--deeper--\r\ndeeper epilogue\r\n";
         string innerHeader = "Subject: inner message\r\nFrom: inner@example.test\r\nContent-Type: multipart/mixed; boundary=\"deep\"\r\n\r\n";
         string innerBody = "--deep\r\n" + p31Header + p31Body +
            "\r\n--deep\r\n" + p32Header + p32Body +
            "\r\n--deep--\r\ndeep epilogue\r\n";
         string p3Header = "Content-Type: message/rfc822\r\nContent-Disposition: attachment;\r\n filename=\"forwarded.eml\"\r\n\r\n";
         var p3 = message.Part("3", p3Header, innerHeader + innerBody);
         p3.InnerHeader = innerHeader;
         p3.InnerBody = innerBody;
         message.Part("3.1", p31Header, p31Body);
         message.Part("3.2", p32Header, p32Body).Multipart = true;
         message.Part("3.2.1", p321Header, p321Body);

         message.Text =
            "From: sender@example.test\r\nTo: parts@example.test\r\nSubject: nested\r\nMIME-Version: 1.0\r\n" +
            "Content-Type: multipart/mixed;\r\n\tboundary=\"outer\"\r\n\r\n" +
            "outer preamble\r\n" +
            "--outer\r\n" + p1Header + p1Body +
            "\r\n--outer\r\n" + p2Header + p2Body +
            "\r\n--outer\r\n" + p3Header + innerHeader + innerBody +
            "\r\n--outer--\r\nouter epilogue\r\n";

         return message;
      }

      // Neither the outer multipart nor the one in it is closed: each last part
      // runs to the end of the message.
      private static WrittenMessage UnclosedMessage()
      {
         var message = new WrittenMessage();

         string p1Header = "Content-Type: text/plain\r\n\r\n";
         string p1Body = "one\r\n";
         message.Part("1", p1Header, p1Body);

         string p21Header = "Content-Type: text/plain\r\n\r\n";
         string p21Body = "two";
         string p22Header = "Content-Type: text/html\r\n\r\n";
         string p22Body = "<b>two</b>\r\n";
         string p2Header = "Content-Type: multipart/alternative; boundary=\"n\"\r\n\r\n";
         string p2Body = "--n\r\n" + p21Header + p21Body + "\r\n--n\r\n" + p22Header + p22Body;
         message.Part("2", p2Header, p2Body).Multipart = true;
         message.Part("2.1", p21Header, p21Body);
         message.Part("2.2", p22Header, p22Body);

         message.Text =
            "From: sender@example.test\r\nSubject: unclosed\r\nContent-Type: multipart/mixed; boundary=\"m\"\r\n\r\n" +
            "--m\r\n" + p1Header + p1Body +
            "\r\n--m\r\n" + p2Header + p2Body;

         return message;
      }

      // Bare line feeds in a body, in a header field and in an encapsulated
      // message's body; a body with no line break at its end; octets above 0x7F;
      // a closing boundary with nothing after it.
      private static WrittenMessage QuirkyMessage()
      {
         var message = new WrittenMessage();

         string p1Header = "Content-Type: text/plain\r\nX-Bare-LF: one\ntwo\r\n\r\n";
         string p1Body = "bare\nline\nfeeds\n\nand a CRLF\r\nlast line without one";
         message.Part("1", p1Header, p1Body);

         string p2Header = "Content-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: 8bit\r\nX-Eight: \x00E9t\x00E9\r\n\r\n";
         string p2Body = "\x00E9t\x00E9 \x00FF\x0080\x0081\r\n";
         message.Part("2", p2Header, p2Body);

         string innerHeader = "Subject: inner\r\n\r\n";
         string innerBody = "an inner body with\nbare line feeds\n";
         string p3Header = "Content-Type: message/rfc822\r\n\r\n";
         var p3 = message.Part("3", p3Header, innerHeader + innerBody);
         p3.InnerHeader = innerHeader;
         p3.InnerBody = innerBody;
         message.Part("3.1", innerHeader, innerBody);

         message.Text =
            "From: sender@example.test\r\nSubject: quirks\r\nContent-Type: multipart/mixed; boundary=\"q\"\r\n\r\n" +
            "--q\r\n" + p1Header + p1Body +
            "\r\n--q\r\n" + p2Header + p2Body +
            "\r\n--q\r\n" + p3Header + innerHeader + innerBody +
            "\r\n--q--";

         return message;
      }

      // ------------------------------------------------------------------ a client of octets

      private sealed class OctetClient : IDisposable
      {
         private readonly TcpClient _client;
         private readonly Stream _stream;
         private readonly byte[] _buffer = new byte[65536];
         private int _buffered;
         private int _read;
         private int _tag;

         public OctetClient(string address)
         {
            _client = new TcpClient(TestPorts.HostAddress.AddressFamily);
            _client.Connect(TestPorts.HostAddress, TestPorts.Actual(TestPorts.Imap));
            _client.ReceiveTimeout = 30000;
            _stream = _client.GetStream();

            StringAssert.StartsWith("* OK", ReadLine(), "The server's greeting.");
            StringAssert.Contains(" OK", Command("LOGIN " + address + " " + Password), "LOGIN");
         }

         public void Dispose()
         {
            _client.Close();
         }

         private int ReadByte()
         {
            if (_read == _buffered)
            {
               _buffered = _stream.Read(_buffer, 0, _buffer.Length);
               _read = 0;

               if (_buffered <= 0)
                  throw new IOException("The server closed the connection.");
            }

            return _buffer[_read++];
         }

         private string ReadLine()
         {
            var line = new StringBuilder();
            for (;;)
            {
               char c = (char) ReadByte();
               line.Append(c);
               if (c == '\n' && line.Length >= 2 && line[line.Length - 2] == '\r')
                  return line.ToString();
            }
         }

         private string ReadOctets(int count)
         {
            var octets = new StringBuilder(count);
            for (int i = 0; i < count; i++)
               octets.Append((char) ReadByte());
            return octets.ToString();
         }

         private void Write(string text)
         {
            byte[] bytes = Octets.GetBytes(text);
            _stream.Write(bytes, 0, bytes.Length);
         }

         // A literal's length when the line announces one - "{n}" or "~{n}" at its end.
         private static int AnnouncedLiteral(string line)
         {
            if (!line.EndsWith("}\r\n", StringComparison.Ordinal))
               return -1;

            int open = line.LastIndexOf('{');
            if (open < 0)
               return -1;

            int length;
            return int.TryParse(line.Substring(open + 1, line.Length - open - 4), NumberStyles.None, CultureInfo.InvariantCulture, out length) ? length : -1;
         }

         // Sends a command and reads its whole answer, literals and all, up to and
         // with its tagged line.
         public string Command(string command)
         {
            string tag = "C" + (++_tag);
            Write(tag + " " + command + "\r\n");
            return ReadAnswer(tag);
         }

         private string ReadAnswer(string tag)
         {
            var answer = new StringBuilder();
            bool lineStart = true;

            for (;;)
            {
               string line = ReadLine();
               answer.Append(line);

               if (lineStart && line.StartsWith(tag + " ", StringComparison.Ordinal))
                  return answer.ToString();

               int literal = AnnouncedLiteral(line);
               if (literal >= 0)
               {
                  answer.Append(ReadOctets(literal));
                  lineStart = false;
               }
               else
                  lineStart = true;
            }
         }

         public void Append(string folder, string message)
         {
            string tag = "C" + (++_tag);
            Write(tag + " APPEND " + folder + " {" + Octets.GetByteCount(message) + "}\r\n");
            StringAssert.StartsWith("+", ReadLine(), "The APPEND's continuation.");
            Write(message + "\r\n");

            string answer = ReadAnswer(tag);
            StringAssert.Contains(tag + " OK", answer, "The APPEND: " + answer);
         }
      }

      // ------------------------------------------------------------------ reading answers

      // The octets of the literal that follows item in a FETCH answer.
      private static string Literal(string answer, string item)
      {
         foreach (string open in new[] { " {", " ~{" })
         {
            int at = answer.IndexOf(item + open, StringComparison.Ordinal);
            if (at < 0)
               continue;

            int start = at + item.Length + open.Length;
            int close = answer.IndexOf('}', start);
            int length = int.Parse(answer.Substring(start, close - start), CultureInfo.InvariantCulture);
            ClassicAssert.AreEqual("\r\n", answer.Substring(close + 1, 2), "A literal is followed by CRLF: " + answer);
            return answer.Substring(close + 3, length);
         }

         // An empty section is answered "".
         if (answer.IndexOf(item + " \"\"", StringComparison.Ordinal) >= 0)
            return "";

         Assert.Fail("No " + item + " in the answer: " + answer);
         return null;
      }

      private static long Number(string answer, string item)
      {
         int at = answer.IndexOf(item + " ", StringComparison.Ordinal);
         ClassicAssert.IsTrue(at >= 0, "No " + item + " in the answer: " + answer);

         int start = at + item.Length + 1;
         int end = start;
         while (end < answer.Length && char.IsDigit(answer[end]))
            end++;

         return long.Parse(answer.Substring(start, end - start), CultureInfo.InvariantCulture);
      }

      // The tagged line of an answer - its last - says OK.
      private static void AssertCompleted(string answer, string what)
      {
         string lines = answer.EndsWith("\r\n", StringComparison.Ordinal) ? answer.Substring(0, answer.Length - 2) : answer;
         int lastBreak = lines.LastIndexOf("\r\n", StringComparison.Ordinal);
         string tagged = lastBreak < 0 ? lines : lines.Substring(lastBreak + 2);

         ClassicAssert.IsTrue(tagged.Contains(" OK"), what + " was not completed: " + tagged);
      }

      private static string Section(OctetClient client, string section)
      {
         string answer = client.Command("FETCH 1 (BODY.PEEK[" + section + "])");
         AssertCompleted(answer, "FETCH BODY.PEEK[" + section + "]");
         return Literal(answer, "BODY[" + section + "]");
      }

      // Where two strings of octets part, for a failure that says where.
      private static string Difference(string got, string expected)
      {
         int at = 0;
         while (at < got.Length && at < expected.Length && got[at] == expected[at])
            at++;

         return got.Length + " octets where " + expected.Length + " were written, differing from octet " + at +
            ": \"" + Escaped(got, at) + "\" where \"" + Escaped(expected, at) + "\"";
      }

      private static string Escaped(string text, int at)
      {
         var escaped = new StringBuilder();
         for (int i = at; i < text.Length && i < at + 40; i++)
         {
            char c = text[i];
            if (c == '\r')
               escaped.Append("\\r");
            else if (c == '\n')
               escaped.Append("\\n");
            else if (c < 0x20 || c > 0x7E)
               escaped.Append("\\x" + ((int) c).ToString("X2", CultureInfo.InvariantCulture));
            else
               escaped.Append(c);
         }
         return escaped.ToString();
      }

      private static void AssertOctets(string expected, string got, string what)
      {
         if (got != expected)
            Assert.Fail(what + ": " + Difference(got, expected));
      }

      // RFC 3501 7.4.2's body-fld-lines as the server counts them: a line is what
      // a line feed ends, and a last line without one still counts.
      private static long Lines(string octets)
      {
         if (octets.Length == 0)
            return 0;

         long lines = 0;
         foreach (char c in octets)
         {
            if (c == '\n')
               lines++;
         }

         if (octets[octets.Length - 1] != '\n')
            lines++;

         return lines;
      }

      // ------------------------------------------------------------------ BODYSTRUCTURE, strictly

      // A parenthesised list's items: a nested list, a string, or an atom (NIL
      // is null).
      private sealed class Structure
      {
         private readonly string _text;
         private int _at;

         private Structure(string text, int at)
         {
            _text = text;
            _at = at;
         }

         // The list that follows item in a FETCH answer.
         public static List<object> After(string answer, string item)
         {
            int at = answer.IndexOf(item + " (", StringComparison.Ordinal);
            ClassicAssert.IsTrue(at >= 0, "No " + item + " in the answer: " + answer);

            var structure = new Structure(answer, at + item.Length + 1);
            return structure.List();
         }

         private List<object> List()
         {
            Expect('(');
            var items = new List<object>();

            if (Peek() == ')')
               throw new FormatException("An empty list at " + _at + ": " + Around());

            for (;;)
            {
               items.Add(Item());

               char next = Peek();
               if (next == ')')
               {
                  _at++;
                  return items;
               }

               // Items are one space apart - but a body follows a body with none.
               if (next == ' ')
               {
                  _at++;
                  if (Peek() == ' ' || Peek() == ')')
                     throw new FormatException("An empty item at " + _at + ": " + Around());
               }
               else if (next != '(')
                  throw new FormatException("\"" + next + "\" where a space, \"(\" or \")\" belongs at " + _at + ": " + Around());
            }
         }

         private object Item()
         {
            char c = Peek();
            if (c == '(')
               return List();

            if (c == '"')
            {
               _at++;
               var value = new StringBuilder();
               for (;;)
               {
                  char next = _text[_at++];
                  if (next == '\\')
                     value.Append(_text[_at++]);
                  else if (next == '"')
                     return value.ToString();
                  else if (next == '\r' || next == '\n')
                     throw new FormatException("A line break in a quoted string at " + _at + ": " + Around());
                  else
                     value.Append(next);
               }
            }

            int start = _at;
            while (_at < _text.Length && _text[_at] != ' ' && _text[_at] != ')' && _text[_at] != '(')
               _at++;

            string atom = _text.Substring(start, _at - start);
            if (atom.Length == 0)
               throw new FormatException("Nothing where an item belongs at " + start + ": " + Around());

            return atom == "NIL" ? null : (object) new Atom(atom);
         }

         private char Peek()
         {
            if (_at >= _text.Length)
               throw new FormatException("The structure ends before its last parenthesis.");
            return _text[_at];
         }

         private void Expect(char c)
         {
            if (Peek() != c)
               throw new FormatException("\"" + Peek() + "\" where \"" + c + "\" belongs at " + _at + ": " + Around());
            _at++;
         }

         private string Around()
         {
            int from = Math.Max(0, _at - 60);
            return _text.Substring(from, Math.Min(_text.Length, _at + 60) - from);
         }
      }

      private sealed class Atom
      {
         public readonly string Text;
         public Atom(string text) { Text = text; }
      }

      // What a structure says of one part.
      private sealed class DescribedPart
      {
         public string Number;
         public string Type;
         public long Size;
         public long Lines = -1;
      }

      private static string Join(string prefix, int index)
      {
         return prefix.Length == 0 ? index.ToString(CultureInfo.InvariantCulture) : prefix + "." + index.ToString(CultureInfo.InvariantCulture);
      }

      private static long NumberOf(object item, string what)
      {
         var atom = item as Atom;
         long number;
         if (atom == null || !long.TryParse(atom.Text, NumberStyles.None, CultureInfo.InvariantCulture, out number))
            throw new FormatException(what + " is not a number");
         return number;
      }

      private static string StringOf(object item, string what)
      {
         var text = item as string;
         if (text == null)
            throw new FormatException(what + " is not a string");
         return text;
      }

      // Reads a body as RFC 3501 7.4.2 has it - body-type-mpart, or body-type-1part
      // of body-type-msg, body-type-text or body-type-basic - and lists every part
      // that is not a multipart under the number a client fetches it by. root says
      // the body is a message's own: its one part, when it is not a multipart, is
      // number.1.
      private static void Describe(List<object> body, string number, bool root, bool extensible, List<DescribedPart> parts)
      {
         if (body.Count > 0 && body[0] is List<object>)
         {
            int index = 0;
            while (index < body.Count && body[index] is List<object>)
            {
               Describe((List<object>) body[index], Join(number, index + 1), false, extensible, parts);
               index++;
            }

            if (index >= body.Count)
               throw new FormatException("A multipart with no subtype");
            StringOf(body[index], "A multipart's subtype");

            int extension = body.Count - index - 1;
            if (!extensible && extension != 0)
               throw new FormatException("BODY carries a multipart's extension data");
            return;
         }

         string own = root ? Join(number, 1) : number;

         if (body.Count < 7)
            throw new FormatException("A part of " + body.Count + " items, short of the seven of body-fields");

         string type = StringOf(body[0], "A part's type").ToUpperInvariant();
         string subtype = StringOf(body[1], "A part's subtype").ToUpperInvariant();
         if (body[2] != null && !(body[2] is List<object>))
            throw new FormatException("A part's parameters are not a list or NIL");
         StringOf(body[5], "A part's encoding");

         var described = new DescribedPart { Number = own, Type = type + "/" + subtype, Size = NumberOf(body[6], "A part's size") };
         int next = 7;

         if (type == "MESSAGE" && subtype == "RFC822")
         {
            if (body.Count < 10)
               throw new FormatException("A message/rfc822 part without its envelope, body and lines");

            var envelope = body[7] as List<object>;
            if (envelope == null || envelope.Count != 10)
               throw new FormatException("A message/rfc822 part's envelope is not ten items");

            var inner = body[8] as List<object>;
            if (inner == null)
               throw new FormatException("A message/rfc822 part's body is not a body");

            described.Lines = NumberOf(body[9], "A message/rfc822 part's lines");
            parts.Add(described);

            Describe(inner, own, true, extensible, parts);
            next = 10;
         }
         else
         {
            if (type == "TEXT")
            {
               if (body.Count < 8)
                  throw new FormatException("A text part without its lines");
               described.Lines = NumberOf(body[7], "A text part's lines");
               next = 8;
            }

            parts.Add(described);
         }

         if (!extensible && body.Count > next)
            throw new FormatException("BODY carries extension data for part " + own + ": " + (body.Count - next) + " items");
      }

      private static List<DescribedPart> Described(OctetClient client, bool extensible)
      {
         string item = extensible ? "BODYSTRUCTURE" : "BODY";
         string answer = client.Command("FETCH 1 (" + item + ")");

         var parts = new List<DescribedPart>();
         try
         {
            Describe(Structure.After(answer, item), "", true, extensible, parts);
         }
         catch (FormatException unreadable)
         {
            Assert.Fail("The " + item + " is not one a client can read: " + unreadable.Message + "\r\n" + answer);
         }

         return parts;
      }

      // ------------------------------------------------------------------ the tests

      private OctetClient AppendAndSelect(string address, string message)
      {
         SingletonProvider<TestSetup>.Instance.AddAccount(_domain, address, Password);

         var client = new OctetClient(address);
         client.Append("INBOX", message);
         StringAssert.Contains(" OK", client.Command("SELECT INBOX"));
         return client;
      }

      // Every part's MIME header and body, and the sections made of them, against
      // what was written - and the parts found in BODY[] where they were written.
      private static void AssertPartsAreAsWritten(OctetClient client, WrittenMessage message)
      {
         string whole = Section(client, "");
         AssertOctets(message.Text, whole, "BODY[]");

         foreach (WrittenPart part in message.Parts)
         {
            string mime = Section(client, part.Number + ".MIME");
            string body = Section(client, part.Number);

            AssertOctets(part.Header, mime, "BODY[" + part.Number + ".MIME]");
            AssertOctets(part.Body, body, "BODY[" + part.Number + "]");

            ClassicAssert.IsTrue(whole.IndexOf(mime + body, StringComparison.Ordinal) >= 0,
               "BODY[" + part.Number + ".MIME] and BODY[" + part.Number + "] together are not a run of BODY[].");

            if (part.InnerHeader != null)
            {
               AssertOctets(part.InnerHeader, Section(client, part.Number + ".HEADER"), "BODY[" + part.Number + ".HEADER]");
               AssertOctets(part.InnerBody, Section(client, part.Number + ".TEXT"), "BODY[" + part.Number + ".TEXT]");
            }
            else
            {
               // Not a message: its MIME header and its body, as they always were.
               AssertOctets(part.Header, Section(client, part.Number + ".HEADER"), "BODY[" + part.Number + ".HEADER]");
               AssertOctets(part.Body, Section(client, part.Number + ".TEXT"), "BODY[" + part.Number + ".TEXT]");
            }

            if (part.Multipart)
            {
               // A multipart has no transfer encoding: BINARY is its body.
               string binary = client.Command("FETCH 1 (BINARY.PEEK[" + part.Number + "] BINARY.SIZE[" + part.Number + "])");
               AssertOctets(part.Body, Literal(binary, "BINARY[" + part.Number + "]"), "BINARY[" + part.Number + "]");
               ClassicAssert.AreEqual(part.Body.Length, Number(binary, "BINARY.SIZE[" + part.Number + "]"), "BINARY.SIZE[" + part.Number + "]");

               // A partial range is cut from the same octets.
               int start = Math.Min(5, part.Body.Length);
               string partial = client.Command("FETCH 1 (BODY.PEEK[" + part.Number + "]<5.20>)");
               AssertOctets(part.Body.Substring(start, Math.Min(20, part.Body.Length - start)), Literal(partial, "BODY[" + part.Number + "]<5>"), "BODY[" + part.Number + "]<5.20>");
            }
         }
      }

      // RFC822.SIZE, BODY[], BODY[HEADER] and BODY[TEXT] agree; and each part
      // BODYSTRUCTURE describes has the size and the lines of the octets BODY[N]
      // hands out for it.
      private static void AssertSizesAgree(OctetClient client)
      {
         string whole = Section(client, "");
         string header = Section(client, "HEADER");
         string text = Section(client, "TEXT");

         ClassicAssert.AreEqual(whole.Length, Number(client.Command("FETCH 1 (RFC822.SIZE)"), "RFC822.SIZE"), "RFC822.SIZE and BODY[]");
         AssertOctets(whole, header + text, "BODY[HEADER] followed by BODY[TEXT]");

         List<DescribedPart> parts = Described(client, true);
         ClassicAssert.IsTrue(parts.Count > 0, "The BODYSTRUCTURE describes no part.");

         foreach (DescribedPart part in parts)
         {
            string body = Section(client, part.Number);

            ClassicAssert.AreEqual(body.Length, part.Size,
               "BODYSTRUCTURE's size of part " + part.Number + " (" + part.Type + ") and the octets of BODY[" + part.Number + "]");

            if (part.Lines >= 0)
               ClassicAssert.AreEqual(Lines(body), part.Lines,
                  "BODYSTRUCTURE's lines of part " + part.Number + " (" + part.Type + ") and the lines of BODY[" + part.Number + "]");
         }

         // BODY says the same of every part, without extension data.
         List<DescribedPart> plain = Described(client, false);
         ClassicAssert.AreEqual(parts.Count, plain.Count, "BODY and BODYSTRUCTURE describe as many parts.");
      }

      [Test]
      [Description("Each part's BODY[N.MIME] and BODY[N] - a multipart's with its preamble, boundaries and epilogue - are the stored octets, in multiparts nested in multiparts and in a message.")]
      public void NestedPartsAreTheStoredOctets()
      {
         WrittenMessage message = NestedMessage();

         using (OctetClient client = AppendAndSelect("nestedparts@example.test", message.Text))
            AssertPartsAreAsWritten(client, message);
      }

      [Test]
      [Description("Multiparts with no closing boundary: each last part runs to the end, and BODY[N] adds no closing boundary.")]
      public void PartsOfMultipartsWithNoClosingBoundaryAreTheStoredOctets()
      {
         WrittenMessage message = UnclosedMessage();

         using (OctetClient client = AppendAndSelect("unclosedparts@example.test", message.Text))
            AssertPartsAreAsWritten(client, message);
      }

      [Test]
      [Description("Bare line feeds in a header field and in bodies, 8-bit octets and a body with no final line break are handed out as stored.")]
      public void PartsWithBareLineFeedsAndEightBitOctetsAreTheStoredOctets()
      {
         WrittenMessage message = QuirkyMessage();

         using (OctetClient client = AppendAndSelect("quirkyparts@example.test", message.Text))
            AssertPartsAreAsWritten(client, message);
      }

      [Test]
      [Description("RFC822.SIZE, BODY[], HEADER with TEXT, and every BODYSTRUCTURE size and line count agree with the octets FETCH hands out.")]
      public void SizesAgreeWithTheOctetsHandedOut()
      {
         var messages = new[]
         {
            NestedMessage().Text,
            UnclosedMessage().Text,
            QuirkyMessage().Text,
            "From: sender@example.test\r\nSubject: single\r\nContent-Type: text/plain\r\n\r\none\r\ntwo\nthree",
            "From: sender@example.test\r\nSubject: header only\r\n\r\n"
         };

         for (int i = 0; i < messages.Length; i++)
         {
            using (OctetClient client = AppendAndSelect("sizes" + i + "@example.test", messages[i]))
               AssertSizesAgree(client);
         }
      }

      [Test]
      [Description("The BODYSTRUCTURE and BODY of messages nested in messages 120 deep are ones a client can read, a part too deep to describe given as octets of its own size.")]
      public void StructureOfMessagesNestedPastItsDepthIsReadable()
      {
         var text = new StringBuilder("From: sender@example.test\r\nSubject: nested deep\r\n");
         for (int i = 0; i < 120; i++)
            text.Append("Content-Type: message/rfc822\r\n\r\n");
         text.Append("Subject: innermost\r\n\r\ninnermost\r\n");

         using (OctetClient client = AppendAndSelect("nesteddeep@example.test", text.ToString()))
         {
            List<DescribedPart> parts = Described(client, true);
            Described(client, false);

            DescribedPart octets = parts.Find(part => part.Type == "APPLICATION/OCTET-STREAM");
            ClassicAssert.IsNotNull(octets, "Past the depth a structure describes, a message is described as a part of octets.");
            ClassicAssert.AreEqual(Section(client, octets.Number).Length, octets.Size, "Its size is that of the octets BODY[" + octets.Number + "] hands out.");

            foreach (DescribedPart part in parts)
               ClassicAssert.AreEqual(Section(client, part.Number).Length, part.Size, "BODYSTRUCTURE's size of part " + part.Number);
         }
      }

      // Multiparts in multiparts, levels deep, each holding the next as its one
      // part, and a text part in the innermost.
      private static string NestedMultiparts(int levels)
      {
         string entity = "Content-Type: text/plain\r\n\r\ninnermost\r\n";
         for (int level = 1; level <= levels; level++)
         {
            string boundary = "N" + level.ToString(CultureInfo.InvariantCulture);
            entity = "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n" +
               "--" + boundary + "\r\n" + entity + "\r\n--" + boundary + "--\r\n";
         }

         return "From: sender@example.test\r\nSubject: nested multiparts\r\n" + entity;
      }

      [Test]
      [Description("A multipart with no parts - no boundary named, a boundary its body never holds, one nested past the depth the parse descends - is described as the one part FETCH serves it as, in a structure a client can read.")]
      public void MultipartWithNoPartsIsDescribedAsOnePart()
      {
         var messages = new[]
         {
            "From: sender@example.test\r\nSubject: no boundary\r\nContent-Type: multipart/mixed\r\n\r\nno boundary is named\r\n--x\r\n",
            "From: sender@example.test\r\nSubject: boundary absent\r\nContent-Type: multipart/alternative; boundary=\"absent\"\r\n\r\na body without it\r\n",
            NestedMultiparts(25)
         };

         for (int i = 0; i < messages.Length; i++)
         {
            using (OctetClient client = AppendAndSelect("noparts" + i + "@example.test", messages[i]))
            {
               List<DescribedPart> parts = Described(client, true);
               Described(client, false);

               DescribedPart octets = parts.Find(part => part.Type == "APPLICATION/OCTET-STREAM");
               ClassicAssert.IsNotNull(octets, "A multipart with no parts is described as a part of octets.");

               foreach (DescribedPart part in parts)
                  ClassicAssert.AreEqual(Section(client, part.Number).Length, part.Size, "BODYSTRUCTURE's size of part " + part.Number + " (" + part.Type + ")");
            }
         }
      }

      [Test]
      [Description("BODY carries no extension data - an attachment's disposition and file name - which BODYSTRUCTURE does.")]
      public void BodyLeavesOutTheExtensionDataOfAnAttachment()
      {
         string message =
            "From: sender@example.test\r\nSubject: attachment\r\nContent-Type: multipart/mixed; boundary=\"a\"\r\n\r\n" +
            "--a\r\nContent-Type: text/plain\r\n\r\nSee the file.\r\n" +
            "--a\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"figures.bin\"\r\nContent-Transfer-Encoding: base64\r\n\r\nAAAA\r\n" +
            "--a\r\nContent-Type: message/rfc822\r\nContent-Disposition: attachment\r\n\r\nSubject: forwarded\r\n\r\nforwarded\r\n" +
            "--a--\r\n";

         using (OctetClient client = AppendAndSelect("bodyextension@example.test", message))
         {
            string structure = client.Command("FETCH 1 (BODYSTRUCTURE)");
            StringAssert.Contains("(\"attachment\" (\"FILENAME\" \"figures.bin\"))", structure, structure);

            string body = client.Command("FETCH 1 (BODY)");
            ClassicAssert.IsFalse(body.Contains("FILENAME") || body.Contains("\"attachment\""),
               "BODY carried an attachment's disposition, which is extension data: " + body);

            Described(client, true);
            Described(client, false);
         }
      }
   }
}
