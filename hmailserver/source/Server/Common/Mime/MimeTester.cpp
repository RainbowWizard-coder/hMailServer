// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "MimeTester.h"
#include "Mime.h"
#include "MimeCode.h"
#include "MimeChar.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../Persistence/PersistentMessage.h"

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _DEBUG
   #define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
   #define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      bool TestFindStringEdgeCases()
      {
         const char* sample = "abc\r\ndef";
         const char* repeated = "abcabc";
         const char* beginning = "\r\nheader";
         const char* emptyNeedleSample = "payload";

         // Finds CRLF in the middle of the buffer.
         if (HM::FindString(sample, "\r\n", sample + strlen(sample)) != sample + 3)
            return false;

         // Returns the first occurrence when the haystack contains repeated matches.
         if (HM::FindString(repeated, "abc", repeated + strlen(repeated)) != repeated)
            return false;

         // Respects the provided start pointer and finds a later occurrence.
         if (HM::FindString(repeated + 1, "abc", repeated + strlen(repeated)) != repeated + 3)
            return false;

         // Finds a match located at the very beginning of the haystack.
         if (HM::FindString(beginning, "\r\n", beginning + strlen(beginning)) != beginning)
            return false;

         // Finds a match located at the very end of the haystack.
         if (HM::FindString(sample, "def", sample + strlen(sample)) != sample + 5)
            return false;

         // Returns the haystack start for an empty needle.
         if (HM::FindString(emptyNeedleSample, "", emptyNeedleSample + strlen(emptyNeedleSample)) != emptyNeedleSample)
            return false;

         // Returns NULL when searching past the only CRLF in the buffer.
         if (HM::FindString(sample + 5, "\r\n", sample + strlen(sample)) != NULL)
            return false;

         // Returns NULL when the allowed search range is too short to contain the needle.
         if (HM::FindString(sample + 3, "\r\n", sample + 4) != NULL)
            return false;

         // Returns NULL when the needle is longer than the haystack range.
         if (HM::FindString(sample, "abcdefghi", sample + 3) != NULL)
            return false;

         // Returns NULL when the needle does not exist in the haystack.
         if (HM::FindString(sample, "xyz", sample + strlen(sample)) != NULL)
            return false;

         // Returns NULL when the match would start exactly at haystackEnd.
         if (HM::FindString(sample, "\r\n", sample + 3) != NULL)
            return false;

         // Returns NULL when the haystack pointer is NULL.
         if (HM::FindString(NULL, "\r\n", sample + strlen(sample)) != NULL)
            return false;

         // Returns NULL when the needle pointer is NULL.
         if (HM::FindString(sample, NULL, sample + strlen(sample)) != NULL)
            return false;

         // Returns NULL when the haystack end pointer is NULL.
         if (HM::FindString(sample, "\r\n", NULL) != NULL)
            return false;

         // Returns NULL when haystackEnd points before haystack.
         if (HM::FindString(sample + 4, "\r\n", sample + 3) != NULL)
            return false;

         // Returns NULL for a closing MIME boundary at EOF with no trailing CRLF.
         // One array, so that the start and the end point into the same object:
         // two copies of the literal need not share storage, and in a Debug build
         // they do not.
         static const char closingBoundaryAtEof[] = "--------------9RmO0bL0Xu1K5PGiz2FQ63S5--";
         if (HM::FindString(closingBoundaryAtEof, "\r\n", closingBoundaryAtEof + sizeof(closingBoundaryAtEof) - 1) != NULL)
            return false;

         return true;
      }

      bool TestMultipartWithoutFinalCrlf()
      {
         const char* multipartWithoutFinalCrlf =
            "Content-Type: multipart/mixed; boundary=\"boundary42\"\r\n"
            "\r\n"
            "--boundary42\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hello\r\n"
            "--boundary42--";

         MimeBody message;
         size_t index = 0;
         bool part_loaded = false;
         size_t loaded = message.Load(multipartWithoutFinalCrlf, strlen(multipartWithoutFinalCrlf), index, part_loaded);

         if (!part_loaded)
            return false;

         if (loaded != strlen(multipartWithoutFinalCrlf))
            return false;

         if (message.GetPartCount() != 1)
            return false;

         return true;
      }

      bool TestMultipartWithFinalCrlf()
      {
         const char* multipartWithFinalCrlf =
            "Content-Type: multipart/mixed; boundary=\"boundary42\"\r\n"
            "\r\n"
            "--boundary42\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hello\r\n"
            "--boundary42--\r\n";

         MimeBody message;
         size_t index = 0;
         bool part_loaded = false;
         size_t loaded = message.Load(multipartWithFinalCrlf, strlen(multipartWithFinalCrlf), index, part_loaded);

         if (!part_loaded)
            return false;

         if (loaded != strlen(multipartWithFinalCrlf))
            return false;

         if (message.GetPartCount() != 1)
            return false;

         return true;
      }

      bool TestMultipartWithoutClosingBoundary()
      {
         const char* multipartWithoutClosingBoundary =
            "Content-Type: multipart/mixed; boundary=\"boundary42\"\r\n"
            "\r\n"
            "--boundary42\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hello\r\n";

         MimeBody message;
         size_t index = 0;
         bool part_loaded = false;
         size_t loaded = message.Load(multipartWithoutClosingBoundary, strlen(multipartWithoutClosingBoundary), index, part_loaded);

         if (!part_loaded)
            return false;

         if (loaded != strlen(multipartWithoutClosingBoundary))
            return false;

         if (message.GetPartCount() != 1)
            return false;

         return true;
      }

      bool TestMultipartWithClosingBoundaryMissingSeparator()
      {
         const char* multipartWithClosingBoundaryMissingSeparator =
            "Content-Type: multipart/mixed; boundary=\"boundary42\"\r\n"
            "\r\n"
            "--boundary42\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hello\r\n"
            "--boundary42--garbage";

         MimeBody message;
         size_t index = 0;
         bool part_loaded = false;
         size_t loaded = message.Load(multipartWithClosingBoundaryMissingSeparator, strlen(multipartWithClosingBoundaryMissingSeparator), index, part_loaded);

         if (!part_loaded)
            return false;

         if (loaded != strlen(multipartWithClosingBoundaryMissingSeparator))
            return false;

         if (message.GetPartCount() != 1)
            return false;

         return true;
      }

      AnsiString QPEncode(const char* input, bool addLineBreak = false)
      {
         MimeCodeQP coder;
         if (addLineBreak)
            coder.AddLineBreak(true);
         coder.SetInput(input, (int)strlen(input), true);
         AnsiString output;
         coder.GetOutput(output);
         return output;
      }

      bool TestQPEncodeEmpty()
      {
         return QPEncode("") == "";
      }

      bool TestQPEncodeSinglePrintableChar()
      {
         return QPEncode("a") == "a";
      }

      bool TestQPEncodeEqualsSign()
      {
         // '=' must always be quoted
         return QPEncode("=") == "=3D";
      }

      bool TestQPEncodeNonAscii()
      {
         // bytes outside printable ASCII range must be quoted
         return QPEncode("\x80") == "=80";
      }

      bool TestQPEncodeCRLF()
      {
         // hard line breaks must be preserved as-is
         return QPEncode("\r\n") == "\r\n";
      }

      bool TestQPEncodeBareLF()
      {
         // bare LF must be preserved (the encoder does not normalise line endings)
         return QPEncode("\n") == "\n";
      }

      bool TestQPEncodeTrailingSpace()
      {
         // space at end of input must be quoted per RFC 2045 §6.7
         return QPEncode("hello ") == "hello=20";
      }

      bool TestQPEncodeTrailingTab()
      {
         // tab at end of input must be quoted per RFC 2045 §6.7
         return QPEncode("hello\t") == "hello=09";
      }

      bool TestQPEncodeSpaceBeforeHardBreak()
      {
         // space immediately before \r\n must be quoted
         return QPEncode("hello \r\n") == "hello=20\r\n";
      }

      bool TestQPEncodeTabBeforeHardBreak()
      {
         // tab immediately before \r\n must be quoted
         return QPEncode("hello\t\r\n") == "hello=09\r\n";
      }

      bool TestQPEncodeSmtpDotQuoted()
      {
         // a lone '.' on its own line (\r\n.\r\n) must be quoted to avoid
         // being interpreted as the SMTP end-of-data marker
         return QPEncode("\r\n.\r\n") == "\r\n=2E\r\n";
      }

      bool TestQPEncodeDotNotQuotedMidLine()
      {
         // '.' in the middle of a line must not be quoted
         return QPEncode("a.b") == "a.b";
      }

      bool TestQPEncodeDotAtEndOfInputNotQuoted()
      {
         // '.' at end of input: pbData+2 would be out of bounds; must fall back to bCopy
         return QPEncode("a\r\n.") == "a\r\n.";
      }

      bool TestQPEncode75CharsNoSoftBreak()
      {
         // 75 regular chars followed by CRLF must not produce a soft line break
         AnsiString input(75, 'a');
         input += "\r\n";
         AnsiString output = QPEncode(input.c_str(), true);
         return output.find("=\r\n") == AnsiString::npos;
      }

      bool TestQPEncode76CharsTriggersSoftBreak()
      {
         // 76 regular chars: the 76th triggers a soft break (line limit is 75 content chars + '=')
         AnsiString input(76, 'a');
         AnsiString output = QPEncode(input.c_str(), true);
         return output.find("=\r\n") != AnsiString::npos;
      }

      bool TestQPEncodeNoTrailingWhitespaceBeforeSoftBreakHighCostNextChar()
      {
         // 73 'a' chars + two spaces + 'b': the first space is copied at nLineLen=73 (below the
         // quoting threshold), leaving nLineLen=74. The second space gets quoted (cost=3), firing
         // a soft break and leaving the first literal space as the last char before =\r\n.
         AnsiString input(73, 'a');
         input += "  b\r\n";

         MimeCodeQP coder;
         coder.AddLineBreak(true);
         coder.SetInput(input.c_str(), (int)input.size(), true);

         AnsiString output;
         coder.GetOutput(output);

         size_t softBreakPos = output.find("=\r\n");
         if (softBreakPos == AnsiString::npos)
            return false;

         char charBeforeSoftBreak = output[(int)(softBreakPos - 1)];
         if (charBeforeSoftBreak == ' ' || charBeforeSoftBreak == '\t')
            return false;

         return true;
      }

      bool TestQPEncodeNoTrailingWhitespaceBeforeSoftBreak()
      {
         // 74 'a' chars + space + 'b': the space lands at line position 75, then 'b' triggers
         // the soft break. Per RFC 2045 §6.7 rule 3, the space must be quoted (=20), not literal.
         AnsiString input(74, 'a');
         input += " b\r\n";

         MimeCodeQP coder;
         coder.AddLineBreak(true);
         coder.SetInput(input.c_str(), (int)input.size(), true);

         AnsiString output;
         coder.GetOutput(output);

         size_t softBreakPos = output.find("=\r\n");
         if (softBreakPos == AnsiString::npos)
            return false;

         char charBeforeSoftBreak = output[(int)(softBreakPos - 1)];
         if (charBeforeSoftBreak == ' ' || charBeforeSoftBreak == '\t')
            return false;

         return true;
      }

      bool TestMultipartWithPartBoundaryMissingCrlf()
      {
         const char* multipartWithPartBoundaryMissingCrlf =
            "Content-Type: multipart/mixed; boundary=\"boundary42\"\r\n"
            "\r\n"
            "--boundary42"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "hello\r\n"
            "--boundary42--";

         MimeBody message;
         size_t index = 0;
         bool part_loaded = false;
         size_t loaded = message.Load(multipartWithPartBoundaryMissingCrlf, strlen(multipartWithPartBoundaryMissingCrlf), index, part_loaded);

         if (!part_loaded)
            return false;

         if (loaded != strlen(multipartWithPartBoundaryMissingCrlf))
            return false;

         if (message.GetPartCount() != 0)
            return false;

         return true;
      }

      // GetParameter must not split on ';' inside a quoted value.
      bool TestGetParameterWithQuotedSemicolon()
      {
         MimeField field;
         const char* line = "Content-Disposition: attachment; filename=\"semi;colon.dll\"\r\n";
         field.Load(line, strlen(line), false);

         AnsiString value;
         if (!field.GetParameter("filename", value))
            return false;
         return value == "semi;colon.dll";
      }

      // A quoted value containing ';' must not consume the parameter that follows it.
      bool TestGetParameterNeighboringParamUnaffectedByQuotedSemicolon()
      {
         MimeField field;
         const char* line = "Content-Disposition: attachment; filename=\"semi;colon.dll\"; size=42\r\n";
         field.Load(line, strlen(line), false);

         AnsiString filename;
         if (!field.GetParameter("filename", filename))
            return false;
         if (filename != "semi;colon.dll")
            return false;

         AnsiString size;
         if (!field.GetParameter("size", size))
            return false;
         return size == "42";
      }

      // RemoveParameter must remove the named parameter while leaving others intact.
      bool TestRemoveParameterRemovesSimpleParam()
      {
         MimeField field;
         const char* line = "Content-Type: text/plain; charset=utf-8; name=\"test.txt\"\r\n";
         field.Load(line, strlen(line), false);

         field.RemoveParameter("name");

         AnsiString val;
         if (field.GetParameter("name", val))
            return false;

         AnsiString charset;
         if (!field.GetParameter("charset", charset))
            return false;
         return charset == "utf-8";
      }

      // RemoveParameter("filename") must remove RFC 2231 continuation parameters
      // (filename*0, filename*1) so that a subsequent SetParameter produces a clean
      // single value that GetParameter returns first.
      bool TestRemoveParameterRemovesRfc2231Continuations()
      {
         MimeField field;
         const char* line = "Content-Disposition: attachment; filename*0=\"long\"; filename*1=\"name.dll\"\r\n";
         field.Load(line, strlen(line), false);

         field.RemoveParameter("filename");
         field.SetParameter("filename", "new.dll");

         AnsiString val;
         if (!field.GetParameter("filename", val))
            return false;
         return val == "new.dll";
      }

      // SetFileName must remove RFC 2231 continuation filename parameters before installing
      // the new single filename= value.  With the old code the continuations were left
      // in place and shadowed the new value returned by GetParameter.
      bool TestSetFileNameReplacesRfc2231ContinuationFilename()
      {
         MimeHeader header;
         const char* headers =
            "Content-Type: application/octet-stream\r\n"
            "Content-Disposition: attachment; filename*0=\"oldpart0\"; filename*1=\"oldpart1\"\r\n"
            "\r\n";
         header.Load(headers, strlen(headers));

         header.SetFileName(L"replacement.dll");

         string result = header.GetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename());

         if (result == "oldpart0" || result == "oldpart1")
            return false;

         return result.find("replacement.dll") != string::npos;
      }

      // SetFileName must also replace a single RFC 2231 encoded filename*= parameter
      // rather than appending a new filename= after it.
      bool TestSetFileNameReplacesEncodedFilenameVariant()
      {
         MimeHeader header;
         const char* headers =
            "Content-Type: application/octet-stream\r\n"
            "Content-Disposition: attachment; filename*=UTF-8''old%20name.dll\r\n"
            "\r\n";
         header.Load(headers, strlen(headers));

         header.SetFileName(L"replacement.dll");

         string result = header.GetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename());

         if (result.find("old") != string::npos)
            return false;

         return result.find("replacement.dll") != string::npos;
      }

      // If there is no filename= on Content-Disposition, SetFileName falls back to
      // Content-Type name=. That path should also clear old RFC 2231-style name*
      // values before writing the new filename.
      bool TestSetFileNameReplacesContentTypeNameVariant()
      {
         MimeHeader header;
         const char* headers =
            "Content-Type: application/octet-stream; name*=UTF-8''old%20name.dll\r\n"
            "\r\n";
         header.Load(headers, strlen(headers));

         header.SetFileName(L"replacement.dll");

         string result = header.GetParameter(CMimeConst::ContentType(), CMimeConst::Name());

         if (result.find("old") != string::npos)
            return false;

         return result.find("replacement.dll") != string::npos;
      }

      // An RFC 2231 unquoted value like filename*=UTF-8''hello.dll must not be truncated
      // at the apostrophes.  The old code used IsToken which stops at non-token characters
      // such as apostrophe; the fix scans to ';' or end-of-string instead.
      bool TestRfc2231ApostropheInUnquotedValue()
      {
         MimeField field;
         const char* line = "Content-Disposition: attachment; filename*=UTF-8''hello.dll\r\n";
         field.Load(line, strlen(line), false);

         AnsiString val;
         if (!field.GetParameter("filename", val))
            return false;
         // GetParameter decodes the RFC 2231 value; the decoded result must contain the
         // actual filename.  With the old code the scan stopped at the first apostrophe
         // so only "UTF-8" was captured and "hello.dll" would be absent.
         return val.Find("hello.dll") >= 0;
      }

      // The shape SetBoundary writes: "__=_Part_Boundary_", three digits, "_", six
      // digits, ".", six digits - 35 characters, all of them bchars (RFC 2046
      // section 5.1.1).
      bool IsGeneratedBoundaryShape(const std::string &boundary)
      {
         const std::string prefix = "__=_Part_Boundary_";

         if (boundary.size() != 35 || boundary.compare(0, prefix.size(), prefix) != 0)
            return false;

         for (size_t i = prefix.size(); i < boundary.size(); i++)
         {
            const char c = boundary.at(i);

            if (i == 21)
            {
               if (c != '_')
                  return false;
            }
            else if (i == 28)
            {
               if (c != '.')
                  return false;
            }
            else if (c < '0' || c > '9')
               return false;
         }

         return true;
      }

      // Boundaries drawn at once on several threads, each thread drawing many
      // from one header object: every one different, and every one the shape and
      // the length the generator has always written. Against the rand() the
      // generator used until 25 September 2026 this fails at once - reseeded
      // from the time and the object's address on every call, one object drew
      // the same two numbers all second, so its tenth boundary repeated its first
      // - and the shared srand/rand state and part counter were raced by the
      // threads besides. A thread's exception is caught and counted here: one
      // escaping a std::thread would end the service, not fail this test.
      bool TestGeneratedBoundariesAreUniqueAcrossThreads()
      {
         const int threadCount = 8;
         int boundariesPerThread = 200;

         std::vector<std::vector<std::string> > drawn(threadCount);
         std::vector<int> failed(threadCount, 0);
         std::vector<std::thread> threads;

         for (int t = 0; t < threadCount; t++)
         {
            threads.push_back(std::thread([&drawn, &failed, t, boundariesPerThread]()
            {
               try
               {
                  MimeHeader header;

                  for (int i = 0; i < boundariesPerThread; i++)
                  {
                     header.SetBoundary(NULL);
                     drawn[t].push_back(header.GetBoundary());
                  }
               }
               catch (...)
               {
                  failed[t] = 1;
               }
            }));
         }

         for (size_t t = 0; t < threads.size(); t++)
            threads[t].join();

         std::set<std::string> seen;

         for (int t = 0; t < threadCount; t++)
         {
            if (failed[t] || drawn[t].size() != (size_t) boundariesPerThread)
               return false;

            for (size_t i = 0; i < drawn[t].size(); i++)
            {
               if (!IsGeneratedBoundaryShape(drawn[t][i]))
                  return false;

               if (!seen.insert(drawn[t][i]).second)
                  return false;
            }
         }

         return seen.size() == (size_t) (threadCount * boundariesPerThread);
      }

      AnsiString QPDecode(const char* input)
      {
         MimeCodeQP coder;
         coder.SetInput(input, (int)strlen(input), false);
         AnsiString output;
         coder.GetOutput(output);
         return output;
      }

      bool TestQPDecodeDeletesTrailingWhitespace()
      {
         // RFC 2045 6.7 rule 3: white space at the end of an encoded line is not data.
         return QPDecode("hello  \r\nbar\t\r\n") == "hello\r\nbar\r\n";
      }

      bool TestQPDecodeSoftBreakAfterWhitespace()
      {
         // Rule 5, with rule 3 applied first: "= " at the end of a line is a soft
         // break, and so is "=" followed by nothing but white space.
         return QPDecode("foo = \t\r\nbar\r\nfoo\t \t= \r\n=62\r\n") == "foo bar\r\nfoo\t \tb\r\n";
      }

      bool TestQPDecodeImaptestPart()
      {
         // The quoted-printable part of imaptest's fetch-binary-mime-qp test: 65
         // octets decoded, where this decoder used to produce 76.
         const char* encoded =
            "hello  \r\nbar=\r\n\r\nfoo\t=\r\nbar\r\nfoo\t \t= \r\n=62\r\nfoo = \t\r\nbar\r\n"
            "foo =\r\n=62\r\nfoo  \r\nbar=\r\n\r\nfoo_bar\r\n";
         AnsiString decoded = QPDecode(encoded);
         return decoded == "hello\r\nbar\r\nfoo\tbar\r\nfoo\t \tb\r\nfoo bar\r\nfoo b\r\nfoo\r\nbar\r\nfoo_bar\r\n" &&
                decoded.GetLength() == 65;
      }

      bool TestQPDecodeKeepsInvalidEscape()
      {
         // An "=" that starts no escape is data; it is not dropped with what follows.
         return QPDecode("a=zb=3D=4") == "a=zb==4";
      }

      bool TestHeaderWithoutFinalCrlf()
      {
         // A message stored exactly as an IMAP APPEND literal gave it need not end
         // with a line break. Its last field is still a field.
         MimeHeader header;
         const char* headers = "Subject: first\r\nDate: Wed, 15 Dec 2010 13:00:00 +0000";
         header.Load(headers, strlen(headers));

         const char *date = header.GetRawFieldValue("Date");
         if (!date || AnsiString(date) != "Wed, 15 Dec 2010 13:00:00 +0000")
            return false;

         const char *subject = header.GetRawFieldValue("Subject");
         if (!subject || AnsiString(subject) != "first")
            return false;

         // Written back, the field is a whole line again.
         AnsiString stored;
         header.Store(stored);
         return stored == "Subject: first\r\nDate: Wed, 15 Dec 2010 13:00:00 +0000\r\n\r\n";
      }

      bool TestUnterminatedTextWithoutFieldNameIsNoField()
      {
         MimeHeader header;
         const char* headers = "no colon here";
         header.Load(headers, strlen(headers));
         return header.GetFieldCount() == 0;
      }

      // FieldCodeBase::UnfoldField as 6.3.5 had it, kept to hold today's to the
      // same answers: the CRLFs from the last to the first, each found from the end
      // and cut out where it stood.
      void UnfoldFieldAsOf635_(string &strField)
      {
         for (;;)
         {
            string::size_type pos = strField.rfind("\r\n");
            if (pos == string::npos)
               break;

            strField.erase(pos, 2);

            int nSpaces = 0;
            while (CMimeChar::IsSpace((unsigned char) strField[pos + nSpaces]))
               nSpaces++;
            strField.replace(pos, nSpaces, " ");
         }
      }

      string UnfoldedNow_(const string &value)
      {
         string unfolded = value;
         FieldCodeBase::UnfoldField(unfolded);
         return unfolded;
      }

      class UnfoldRandom_
      {
      public:
         explicit UnfoldRandom_(unsigned long long seed) : state_(seed) {}

         int Below(int bound)
         {
            state_ ^= state_ >> 12;
            state_ ^= state_ << 25;
            state_ ^= state_ >> 27;
            return (int) ((state_ * 2685821657736338717ULL) % (unsigned long long) bound);
         }

      private:
         unsigned long long state_;
      };
   }

   void
   MimeTester::TestFieldUnfolding()
   {
      // What a CRLF and the white space around it become.
      if (UnfoldedNow_("a\r\n b") != "a b" ||
          UnfoldedNow_("a \r\n\tb") != "a  b" ||
          UnfoldedNow_("a\r\n\r\n b") != "a b" ||
          UnfoldedNow_("a\r\nb") != "a b" ||
          UnfoldedNow_("\r\n") != " " ||
          UnfoldedNow_("a\r\r\nb") != "a\r b" ||
          UnfoldedNow_("a\rb\nc") != "a\rb\nc" ||
          UnfoldedNow_(string("a\r\n\0b", 5)) != string("a \0b", 4) ||
          UnfoldedNow_("") != "")
         throw std::logic_error("A header field value was unfolded other than as a CRLF and the white space after it make one space.");

      // Generated values of the characters that matter - CR, LF, the other white
      // space, NUL, a byte past ASCII - against 6.3.5's code.
      static const char characters[] = { '\r', '\n', ' ', '\t', '\v', '\f', '\0', 'a', 'b', (char) 0xC3 };
      UnfoldRandom_ random(0x2545F4914F6CDD1DULL);
      for (int number = 0; number < 100000; number++)
      {
         string value;
         const int length = random.Below(25);
         for (int i = 0; i < length; i++)
            value += characters[random.Below((int) sizeof(characters))];

         string before = value;
         UnfoldFieldAsOf635_(before);

         if (UnfoldedNow_(value) != before)
            throw std::logic_error("A header field value was unfolded other than 6.3.5 unfolded it in case " + std::to_string(number));
      }

      // A value of 400,000 folds (1.6 MB), as an encoded word can decode to: one pass
      // over it, a few milliseconds. 6.3.5's took 31 s over it in the POSIX build.
      string folded = "a";
      folded.reserve(1600001);
      for (int i = 0; i < 400000; i++)
         folded += "\r\n a";

      const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
      const string unfolded = UnfoldedNow_(folded);
      const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

      string expected = "a";
      expected.reserve(800001);
      for (int i = 0; i < 400000; i++)
         expected += " a";

      if (unfolded != expected)
         throw std::logic_error("A header field value of 400,000 folds was not unfolded to its words.");

      if (milliseconds > HM_TEST_TIME_BOUND(2000))
         throw std::logic_error("A header field value of 400,000 folds took " + std::to_string((long long) milliseconds) + " ms to unfold");
   }

   MimeTester::MimeTester(void)
   {
   }

   MimeTester::~MimeTester(void)
   {
   }
   
   void
   MimeTester::Test()
   {
     if (!TestFindStringEdgeCases())
         HM_TEST_FAILED();

      if (!TestMultipartWithoutFinalCrlf())
         HM_TEST_FAILED();

      if (!TestMultipartWithFinalCrlf())
         HM_TEST_FAILED();

      if (!TestMultipartWithoutClosingBoundary())
         HM_TEST_FAILED();

      if (!TestMultipartWithClosingBoundaryMissingSeparator())
         HM_TEST_FAILED();

      if (!TestMultipartWithPartBoundaryMissingCrlf())
         HM_TEST_FAILED();

      if (!TestQPEncodeEmpty())
         HM_TEST_FAILED();

      if (!TestQPEncodeSinglePrintableChar())
         HM_TEST_FAILED();

      if (!TestQPEncodeEqualsSign())
         HM_TEST_FAILED();

      if (!TestQPEncodeNonAscii())
         HM_TEST_FAILED();

      if (!TestQPEncodeCRLF())
         HM_TEST_FAILED();

      if (!TestQPEncodeBareLF())
         HM_TEST_FAILED();

      if (!TestQPEncodeTrailingSpace())
         HM_TEST_FAILED();

      if (!TestQPEncodeTrailingTab())
         HM_TEST_FAILED();

      if (!TestQPEncodeSpaceBeforeHardBreak())
         HM_TEST_FAILED();

      if (!TestQPEncodeTabBeforeHardBreak())
         HM_TEST_FAILED();

      if (!TestQPEncodeSmtpDotQuoted())
         HM_TEST_FAILED();

      if (!TestQPEncodeDotNotQuotedMidLine())
         HM_TEST_FAILED();

      if (!TestQPEncodeDotAtEndOfInputNotQuoted())
         HM_TEST_FAILED();

      if (!TestQPEncode75CharsNoSoftBreak())
         HM_TEST_FAILED();

      if (!TestQPEncode76CharsTriggersSoftBreak())
         HM_TEST_FAILED();

      if (!TestQPEncodeNoTrailingWhitespaceBeforeSoftBreakHighCostNextChar())
         HM_TEST_FAILED();

      if (!TestQPEncodeNoTrailingWhitespaceBeforeSoftBreak())
         HM_TEST_FAILED();
      if (!TestGetParameterWithQuotedSemicolon())
         HM_TEST_FAILED();

      if (!TestGetParameterNeighboringParamUnaffectedByQuotedSemicolon())
         HM_TEST_FAILED();

      if (!TestRemoveParameterRemovesSimpleParam())
         HM_TEST_FAILED();

      if (!TestRemoveParameterRemovesRfc2231Continuations())
         HM_TEST_FAILED();

      if (!TestSetFileNameReplacesRfc2231ContinuationFilename())
         HM_TEST_FAILED();

      if (!TestSetFileNameReplacesEncodedFilenameVariant())
         HM_TEST_FAILED();

      if (!TestSetFileNameReplacesContentTypeNameVariant())
         HM_TEST_FAILED();

      if (!TestRfc2231ApostropheInUnquotedValue())
         HM_TEST_FAILED();

      if (!TestGeneratedBoundariesAreUniqueAcrossThreads())
         HM_TEST_FAILED();

      if (!TestQPDecodeDeletesTrailingWhitespace())
         HM_TEST_FAILED();

      if (!TestQPDecodeSoftBreakAfterWhitespace())
         HM_TEST_FAILED();

      if (!TestQPDecodeImaptestPart())
         HM_TEST_FAILED();

      if (!TestQPDecodeKeepsInvalidEscape())
         HM_TEST_FAILED();

      if (!TestHeaderWithoutFinalCrlf())
         HM_TEST_FAILED();

      if (!TestUnterminatedTextWithoutFieldNameIsNoField())
         HM_TEST_FAILED();

      TestFieldUnfolding();
   }
}
