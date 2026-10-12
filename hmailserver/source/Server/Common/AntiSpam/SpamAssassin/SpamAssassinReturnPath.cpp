// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamAssassinReturnPath.h"

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char ReturnPathName[] = "return-path";
      const size_t ReturnPathNameLength = sizeof(ReturnPathName) - 1;

      char LowerAscii(char character)
      {
         return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
      }
   }

   ReturnPathFieldFilter::ReturnPathFieldFilter() :
      in_header_(true),
      removing_field_(false),
      line_state_(LineUndecided),
      removed_bytes_(0),
      removed_fields_(0)
   {

   }

   ReturnPathFieldFilter::Verdict
   ReturnPathFieldFilter::Judge_() const
   {
      // held_ is the start of a line of the header block, never empty here.
      const size_t length = held_.size();
      const char first = held_[0];

      // The empty line: the header block ends with it.
      if (first == '\n')
         return VerdictEndOfHeader;

      if (first == '\r')
      {
         if (length == 1)
            return VerdictNotYet;

         if (held_[1] == '\n')
            return VerdictEndOfHeader;

         // A line that starts with a bare CR is no field of any name.
         return VerdictKeepField;
      }

      // A continuation line belongs to the field above it.
      if (first == ' ' || first == '\t')
         return removing_field_ ? VerdictRemoveContinuation : VerdictKeepContinuation;

      // The name, in any case.
      for (size_t i = 0; i < length && i < ReturnPathNameLength; i++)
      {
         if (LowerAscii(held_[i]) != ReturnPathName[i])
            return VerdictKeepField;
      }

      if (length <= ReturnPathNameLength)
         return VerdictNotYet;

      // Then spaces or tabs, and the colon.
      for (size_t i = ReturnPathNameLength; i < length; i++)
      {
         const char character = held_[i];

         if (character == ':')
            return VerdictRemoveField;

         if (character != ' ' && character != '\t')
            return VerdictKeepField;
      }

      if (length > MaxHeldLine)
         return VerdictKeepField;

      return VerdictNotYet;
   }

   void
   ReturnPathFieldFilter::Decide_(std::string &output)
   {
      const Verdict verdict = Judge_();

      if (verdict == VerdictNotYet)
         return;

      // The deciding byte may have been the line's LF ("Return\n" is decided by
      // it), and then the next byte starts another line.
      const bool lineEnded = held_[held_.size() - 1] == '\n';

      switch (verdict)
      {
      case VerdictEndOfHeader:
         output.append(held_);
         in_header_ = false;
         removing_field_ = false;
         break;

      case VerdictKeepField:
      case VerdictKeepContinuation:
         if (verdict == VerdictKeepField)
            removing_field_ = false;

         output.append(held_);
         line_state_ = lineEnded ? LineUndecided : LineKept;
         break;

      case VerdictRemoveField:
      case VerdictRemoveContinuation:
         if (verdict == VerdictRemoveField)
         {
            removing_field_ = true;
            removed_fields_++;
         }

         removed_bytes_ += held_.size();
         line_state_ = lineEnded ? LineUndecided : LineRemoved;
         break;

      case VerdictNotYet:
         break;
      }

      held_.clear();
   }

   void
   ReturnPathFieldFilter::Feed(const char *data, size_t length, std::string &output)
   {
      size_t position = 0;

      while (position < length)
      {
         if (!in_header_)
         {
            output.append(data + position, length - position);
            return;
         }

         if (line_state_ == LineUndecided)
         {
            held_.push_back(data[position]);
            position++;

            Decide_(output);
            continue;
         }

         // The rest of a line already judged, to its LF or to the end of these bytes.
         const char *start = data + position;
         const char *lineFeed = static_cast<const char *>(memchr(start, '\n', length - position));
         const size_t take = lineFeed ? static_cast<size_t>(lineFeed - start) + 1 : length - position;

         if (line_state_ == LineKept)
            output.append(start, take);
         else
            removed_bytes_ += take;

         position += take;

         if (lineFeed)
            line_state_ = LineUndecided;
      }
   }

   void
   ReturnPathFieldFilter::Finish(std::string &output)
   {
      output.append(held_);
      held_.clear();
   }

   std::string
   SpamAssassinReturnPath::RequestLine(const std::string &returnPath)
   {
      std::string line = "Return-Path: ";

      for (char character : returnPath)
      {
         if (character != '\r' && character != '\n')
            line.push_back(character);
      }

      line.append("\r\n");
      return line;
   }

   void
   SpamAssassinReturnPathTester::Expect_(const std::string &what, const std::string &input, const std::string &expected, int expectedFields)
   {
      // In one piece.
      std::string whole;
      {
         ReturnPathFieldFilter filter;
         filter.Feed(input.data(), input.size(), whole);
         filter.Finish(whole);

         if (whole != expected)
            throw std::logic_error("ReturnPathFieldFilter, " + what + ": passed [" + whole + "], expected [" + expected + "].");

         if (filter.GetRemovedBytes() != input.size() - expected.size())
            throw std::logic_error("ReturnPathFieldFilter, " + what + ": counted " + std::to_string(filter.GetRemovedBytes()) +
               " bytes left out, " + std::to_string(input.size() - expected.size()) + " were.");

         if (filter.GetRemovedFields() != expectedFields)
            throw std::logic_error("ReturnPathFieldFilter, " + what + ": counted " + std::to_string(filter.GetRemovedFields()) +
               " fields left out, expected " + std::to_string(expectedFields) + ".");
      }

      // In two pieces, split everywhere - an empty first or last piece included.
      for (size_t split = 0; split <= input.size(); split++)
      {
         ReturnPathFieldFilter filter;
         std::string passed;
         filter.Feed(input.data(), split, passed);
         filter.Feed(input.data() + split, input.size() - split, passed);
         filter.Finish(passed);

         if (passed != expected)
            throw std::logic_error("ReturnPathFieldFilter, " + what + ", split after byte " + std::to_string(split) +
               ": passed [" + passed + "], expected [" + expected + "].");
      }

      // A byte at a time.
      {
         ReturnPathFieldFilter filter;
         std::string passed;
         for (size_t i = 0; i < input.size(); i++)
            filter.Feed(input.data() + i, 1, passed);
         filter.Finish(passed);

         if (passed != expected)
            throw std::logic_error("ReturnPathFieldFilter, " + what + ", a byte at a time: passed [" + passed + "], expected [" + expected + "].");

         if (filter.GetRemovedFields() != expectedFields)
            throw std::logic_error("ReturnPathFieldFilter, " + what + ", a byte at a time: counted " + std::to_string(filter.GetRemovedFields()) + " fields.");
      }
   }

   void
   SpamAssassinReturnPathTester::Test()
   {
      const std::string body = "\r\nBody line.\r\nReturn-Path: <in-the-body@example.com>\r\n";

      // Nothing to leave out.
      Expect_("no Return-Path", "From: a@example.com\r\nSubject: s\r\n" + body, "From: a@example.com\r\nSubject: s\r\n" + body, 0);
      Expect_("empty stream", "", "", 0);
      Expect_("no header block", body, body, 0);

      // The field where it may be.
      Expect_("first", "Return-Path: <x@example.com>\r\nFrom: a@example.com\r\n" + body, "From: a@example.com\r\n" + body, 1);
      Expect_("in the middle", "From: a@example.com\r\nReturn-Path: <x@example.com>\r\nSubject: s\r\n" + body, "From: a@example.com\r\nSubject: s\r\n" + body, 1);
      Expect_("last", "From: a@example.com\r\nReturn-Path: <x@example.com>\r\n" + body, "From: a@example.com\r\n" + body, 1);
      Expect_("twice", "Return-Path: <x@example.com>\r\nReceived: from a\r\nReturn-Path: <y@example.com>\r\nFrom: a@example.com\r\n" + body,
         "Received: from a\r\nFrom: a@example.com\r\n" + body, 2);
      Expect_("the only field", "Return-Path: <>\r\n" + body, body, 1);

      // Folded: the continuation lines go with it, and a continuation of another
      // field stays.
      Expect_("folded", "Return-Path:\r\n <x@example.com>\r\n\t(comment)\r\nReceived: from a\r\n by b\r\n" + body,
         "Received: from a\r\n by b\r\n" + body, 1);

      // The name in any case, and with white space before the colon.
      Expect_("lower case", "return-path: <x@example.com>\r\nFrom: a\r\n" + body, "From: a\r\n" + body, 1);
      Expect_("upper case", "RETURN-PATH:<x@example.com>\r\nFrom: a\r\n" + body, "From: a\r\n" + body, 1);
      Expect_("white space before the colon", "Return-Path \t : <x@example.com>\r\nFrom: a\r\n" + body, "From: a\r\n" + body, 1);

      // Names that only begin or end like it, and one that is cut short.
      const std::string others =
         "Return-Paths: <x@example.com>\r\n"
         "Return-Path-Id: 1\r\n"
         "X-Return-Path: <x@example.com>\r\n"
         "Return-Pat: <x@example.com>\r\n"
         "Return-Path x: <x@example.com>\r\n"
         "Return-Path\r\n"
         "Return\r\n";
      Expect_("names that are not it", others + body, others + body, 0);

      // Bare LF lines, and an empty line of a bare LF.
      Expect_("LF lines", "Return-Path: <x@example.com>\nFrom: a\nReturn-Path: <y@example.com>\n \tfolded\nSubject: s\n\nReturn-Path: <body>\n",
         "From: a\nSubject: s\n\nReturn-Path: <body>\n", 2);

      // A line its own LF decides - one that only begins like the name - and the
      // line after it judged afresh.
      Expect_("a line decided by its LF", "From: a\nReturn\nReturn-Path: <x@example.com>\nReturn-Path\nReturn-Path: <y@example.com>\nSubject: s\n\n",
         "From: a\nReturn\nReturn-Path\nSubject: s\n\n", 2);

      // Nothing after the header block is looked at: not the body, and not the
      // header of an attached message.
      const std::string attached =
         "From: a\r\n"
         "Content-Type: multipart/mixed; boundary=\"b\"\r\n"
         "\r\n"
         "--b\r\n"
         "Content-Type: message/rfc822\r\n"
         "\r\n"
         "Return-Path: <inner@example.com>\r\n"
         "From: inner\r\n"
         "\r\n"
         "inner body\r\n"
         "--b--\r\n"
         "epilogue\r\n";
      Expect_("an attached message", "Return-Path: <x@example.com>\r\n" + attached, attached, 1);

      // A header block with no empty line after it: the stream ends inside it.
      Expect_("no empty line", "From: a\r\nReturn-Path: <x@example.com>\r\nSubject: s\r\n", "From: a\r\nSubject: s\r\n", 1);
      Expect_("cut off in the name", "From: a\r\nReturn-Pa", "From: a\r\nReturn-Pa", 0);
      Expect_("cut off before the colon", "From: a\r\nReturn-Path  ", "From: a\r\nReturn-Path  ", 0);
      Expect_("cut off in the value", "From: a\r\nReturn-Path: <x@exa", "From: a\r\n", 1);
      Expect_("cut off at a CR", "From: a\r\n\r", "From: a\r\n\r", 0);

      // A line that starts with a bare CR is not the end of the header block.
      Expect_("a bare CR", "From: a\r\n\rX\r\nReturn-Path: <x@example.com>\r\n" + body, "From: a\r\n\rX\r\n" + body, 1);

      // A continuation line at the very top belongs to no field and stays.
      Expect_("a continuation first", " folded\r\nReturn-Path: <x@example.com>\r\n" + body, " folded\r\n" + body, 1);

      // A line past RFC 5322's limit still undecided is not taken for the field.
      {
         const std::string spaces(ReturnPathFieldFilter::MaxHeldLine, ' ');
         const std::string longLine = "Return-Path" + spaces + ": <x@example.com>\r\n";
         Expect_("a line too long to judge", longLine + body, longLine + body, 0);
      }

      // The line sent ahead of the message.
      if (SpamAssassinReturnPath::RequestLine("<x@example.com>") != "Return-Path: <x@example.com>\r\n")
         throw std::logic_error("SpamAssassinReturnPath::RequestLine did not write the field and CR LF.");

      if (SpamAssassinReturnPath::RequestLine("<>") != "Return-Path: <>\r\n")
         throw std::logic_error("SpamAssassinReturnPath::RequestLine did not write the null sender.");

      if (SpamAssassinReturnPath::RequestLine("<x@example.com>\r\nX-Injected: 1") != "Return-Path: <x@example.com>X-Injected: 1\r\n")
         throw std::logic_error("SpamAssassinReturnPath::RequestLine let a line break through.");

      // What the client does with it: the line, then the message without its own
      // Return-Path fields; and the reply without the line. The request's length
      // is the line's and the message's less what was left out.
      {
         const std::string message = "Return-Path: <forged@example.net>\r\nFrom: a\r\n" + body;
         const std::string line = SpamAssassinReturnPath::RequestLine("<sender@example.com>");

         ReturnPathFieldFilter request;
         std::string sent = line;
         request.Feed(message.data(), message.size(), sent);
         request.Finish(sent);

         if (sent != line + "From: a\r\n" + body || sent.size() != line.size() + message.size() - request.GetRemovedBytes())
            throw std::logic_error("ReturnPathFieldFilter: the request was not the line and the message without its own Return-Path.");

         const std::string reply = line + "X-Spam-Status: No, score=0.0\r\n" + sent.substr(line.size());

         ReturnPathFieldFilter response;
         std::string stored;
         response.Feed(reply.data(), reply.size(), stored);
         response.Finish(stored);

         if (stored != "X-Spam-Status: No, score=0.0\r\nFrom: a\r\n" + body)
            throw std::logic_error("ReturnPathFieldFilter: the reply kept the line sent ahead of the message, or lost something else.");

         if (!response.GetHeaderEnded())
            throw std::logic_error("ReturnPathFieldFilter: the header block of the reply did not end at its empty line.");
      }
   }
}
