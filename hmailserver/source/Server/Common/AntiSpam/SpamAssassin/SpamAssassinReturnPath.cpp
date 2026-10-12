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

      // RFC 5322's atext (section 3.2.3), and any byte of a UTF-8 sequence (RFC 6532).
      bool IsAtext(char character)
      {
         const unsigned char value = static_cast<unsigned char>(character);

         if (value >= 0x80)
            return true;

         if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))
            return true;

         switch (value)
         {
         case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
         case '-': case '/': case '=': case '?': case '^': case '_': case '`': case '{':
         case '|': case '}': case '~':
            return true;
         default:
            return false;
         }
      }

      // The path in the unfolded value of a Return-Path field, read from left to
      // right (SpamAssassinReturnPath::TopmostPath). Every method that answers false
      // has found something that is not a path; where it stopped does not matter
      // then, since the whole value is given up.
      class PathReader
      {
      public:
         explicit PathReader(const std::string &value) :
            value_(value),
            position_(0)
         {

         }

         bool AtEnd() const { return position_ >= value_.size(); }

         bool Next(char character) const { return !AtEnd() && value_[position_] == character; }

         bool Take(char character)
         {
            if (!Next(character))
               return false;

            position_++;
            return true;
         }

         // White space and comments (RFC 5322 section 3.2.2): a comment may hold
         // comments and quoted pairs. False for a comment that is never closed.
         bool SkipCfws()
         {
            while (!AtEnd())
            {
               const char character = value_[position_];

               if (character == ' ' || character == '\t')
               {
                  position_++;
                  continue;
               }

               if (character != '(')
                  return true;

               int depth = 0;
               while (!AtEnd())
               {
                  const char inComment = value_[position_++];

                  if (inComment == '\\')
                  {
                     if (AtEnd())
                        return false;

                     position_++;
                  }
                  else if (inComment == '(')
                     depth++;
                  else if (inComment == ')' && --depth == 0)
                     break;
               }

               if (depth != 0)
                  return false;
            }

            return true;
         }

         // dot-atom-text: atext, and dots between, never first, last or twice.
         bool DotAtom(std::string &address)
         {
            const size_t start = position_;

            for (;;)
            {
               const size_t atom = position_;
               while (!AtEnd() && IsAtext(value_[position_]))
                  position_++;

               if (position_ == atom)
                  return false;

               if (!Take('.'))
                  break;
            }

            address.append(value_, start, position_ - start);
            return true;
         }

         // A quoted string, its quotes and quoted pairs kept as they are.
         bool QuotedString(std::string &address)
         {
            const size_t start = position_;

            if (!Take('"'))
               return false;

            while (!AtEnd())
            {
               const char character = value_[position_++];

               if (character == '"')
               {
                  address.append(value_, start, position_ - start);
                  return true;
               }

               if (character == '\\')
               {
                  if (AtEnd())
                     return false;

                  position_++;
               }
            }

            return false;
         }

         // A domain literal: "[", then no "[", "]", "\" or white space, then "]".
         bool DomainLiteral(std::string &address)
         {
            const size_t start = position_;

            if (!Take('['))
               return false;

            while (!AtEnd())
            {
               const char character = value_[position_++];

               if (character == ']')
               {
                  address.append(value_, start, position_ - start);
                  return true;
               }

               if (character == '[' || character == '\\' || character == ' ' || character == '\t')
                  return false;
            }

            return false;
         }

         bool Domain(std::string &address)
         {
            return Next('[') ? DomainLiteral(address) : DotAtom(address);
         }

         // addr-spec, with nothing between its parts.
         bool AddrSpec(std::string &address)
         {
            std::string read;

            if (!(Next('"') ? QuotedString(read) : DotAtom(read)))
               return false;

            if (!Take('@'))
               return false;

            read.push_back('@');

            if (!Domain(read))
               return false;

            address = read;
            return true;
         }

         // An obsolete route ahead of the address (RFC 5322 section 4.4,
         // obs-route): domains each after an "@", commas between, and a colon. The
         // domains are not kept: the path is the address.
         bool SkipRoute()
         {
            bool domains = false;

            for (;;)
            {
               if (!SkipCfws())
                  return false;

               if (Take(','))
                  continue;

               if (!Take('@'))
                  break;

               std::string ignored;
               if (!Domain(ignored))
                  return false;

               domains = true;
            }

            return domains && Take(':');
         }

      private:
         const std::string &value_;
         size_t position_;
      };
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

   bool
   SpamAssassinReturnPath::TopmostPath(const std::string &message, std::string &path)
   {
      // The topmost field's bytes, taken as the filter leaves Return-Path fields out
      // of a request: the message is fed to it a line at a time, every line is
      // decided by its LF at the latest, and a line the filter leaves out belongs
      // to a Return-Path field - the first one, until a line passes, a second field
      // starts or the header block ends.
      ReturnPathFieldFilter filter;
      std::string field;
      std::string passed;

      size_t position = 0;
      while (position < message.size() && !filter.GetHeaderEnded())
      {
         const size_t lineFeed = message.find('\n', position);
         const size_t end = lineFeed == std::string::npos ? message.size() : lineFeed + 1;

         const int fieldsBefore = filter.GetRemovedFields();
         const size_t removedBefore = filter.GetRemovedBytes();

         passed.clear();
         filter.Feed(message.data() + position, end - position, passed);

         if (filter.GetRemovedBytes() > removedBefore)
         {
            if (fieldsBefore > 0 && filter.GetRemovedFields() > fieldsBefore)
               break;

            field.append(message, position, end - position);

            if (field.size() > MaxPathField)
               return false;
         }
         else if (!field.empty())
            break;

         position = end;
      }

      if (field.empty())
         return false;

      // The value, unfolded (RFC 5322 section 2.2.3): what follows the colon, the
      // line breaks taken out. Any other control character - a bare CR among them -
      // is no part of a path.
      std::string value;
      for (size_t i = field.find(':') + 1; i < field.size(); i++)
      {
         const char character = field[i];

         if (character == '\n')
            continue;

         if (character == '\r' && i + 1 < field.size() && field[i + 1] == '\n')
            continue;

         const unsigned char code = static_cast<unsigned char>(character);
         if ((code < 0x20 && character != '\t') || code == 0x7f)
            return false;

         value.push_back(character);
      }

      // path = angle-addr / ([CFWS] "<" [CFWS] ">" [CFWS]), the obsolete route
      // in an angle-addr, or a bare address.
      PathReader reader(value);
      std::string address;

      if (!reader.SkipCfws())
         return false;

      if (reader.Take('<'))
      {
         if (!reader.SkipCfws())
            return false;

         const bool routed = reader.Next('@') || reader.Next(',');
         if (routed && (!reader.SkipRoute() || !reader.SkipCfws()))
            return false;

         if (routed || !reader.Next('>'))
         {
            if (!reader.AddrSpec(address) || !reader.SkipCfws())
               return false;
         }

         if (!reader.Take('>'))
            return false;
      }
      else if (!reader.AddrSpec(address))
         return false;

      if (!reader.SkipCfws() || !reader.AtEnd())
         return false;

      path = "<" + address + ">";
      return true;
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

      TestTopmostPath_();
   }

   void
   SpamAssassinReturnPathTester::ExpectPath_(const std::string &what, const std::string &message, const std::string &expected)
   {
      // Empty expected: no path. path starts with something TopmostPath must
      // either replace or leave alone.
      std::string path = "unchanged";
      const bool found = SpamAssassinReturnPath::TopmostPath(message, path);

      if (expected.empty())
      {
         if (found || path != "unchanged")
            throw std::logic_error("SpamAssassinReturnPath::TopmostPath, " + what + ": read [" + path + "], expected no path.");

         return;
      }

      if (!found || path != expected)
         throw std::logic_error("SpamAssassinReturnPath::TopmostPath, " + what + ": read " + (found ? "[" + path + "]" : std::string("no path")) +
            ", expected [" + expected + "].");
   }

   void
   SpamAssassinReturnPathTester::TestTopmostPath_()
   {
      const std::string rest = "From: a@example.com\r\nSubject: s\r\n\r\nBody.\r\n";

      // None.
      ExpectPath_("no Return-Path", rest, "");
      ExpectPath_("an empty message", "", "");
      ExpectPath_("a Return-Path in the body only", "From: a@example.com\r\n\r\nReturn-Path: <body@example.com>\r\n", "");
      ExpectPath_("names that are not it", "X-Return-Path: <x@example.com>\r\nReturn-Paths: <y@example.com>\r\nReturn-Path-Id: 1\r\n" + rest, "");

      // The forms a path takes.
      ExpectPath_("first", "Return-Path: <x@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("the null path", "Return-Path: <>\r\n" + rest, "<>");
      ExpectPath_("the null path, with white space", "Return-Path:  < >  \r\n" + rest, "<>");
      ExpectPath_("a bare address", "Return-Path: x@example.com\r\n" + rest, "<x@example.com>");
      ExpectPath_("an obsolete route", "Return-Path: <@relay.example.net,@hop.example.net:x@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("an obsolete route, white space after it", "Return-Path: <@relay.example.net: x@example.com >\r\n" + rest, "<x@example.com>");
      ExpectPath_("comments", "Return-Path: (bounce (nested) \\) handler) <x@example.com> (via the list)\r\n" + rest, "<x@example.com>");
      ExpectPath_("no space after the colon", "Return-Path:<x@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("folded", "Return-Path:\r\n <x@example.com>\r\n\t(folded)\r\n" + rest, "<x@example.com>");
      ExpectPath_("the name in lower case, white space before the colon", "return-path \t: <x@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("bare LF lines", "Return-Path: <x@example.com>\nFrom: a\n\n", "<x@example.com>");
      ExpectPath_("a quoted local part", "Return-Path: <\"x y\\\"z\"@example.com>\r\n" + rest, "<\"x y\\\"z\"@example.com>");
      ExpectPath_("a domain literal", "Return-Path: <x@[192.0.2.1]>\r\n" + rest, "<x@[192.0.2.1]>");
      ExpectPath_("UTF-8", "Return-Path: <j\xC3\xB8rn@\xC3\xA6x.example>\r\n" + rest, "<j\xC3\xB8rn@\xC3\xA6x.example>");
      ExpectPath_("the stream ends after the value", "Return-Path: <x@example.com>", "<x@example.com>");

      // The topmost one: in Gmail's layout, under its own Received fields and over
      // the ones the message came in with; and the first of two.
      ExpectPath_("under Received fields",
         "X-hMailServer-ExternalAccount: collected\r\n"
         "Delivered-To: someone@gmail.example\r\n"
         "Received: by 2002:a05:6000:1a8c with SMTP id x1;\r\n"
         "        Mon, 12 Oct 2026 10:00:00 -0700 (PDT)\r\n"
         "X-Received: by 2002:a05:6000:1a8c with SMTP id y2; Mon, 12 Oct 2026 10:00:00 -0700 (PDT)\r\n"
         "Return-Path: <bounce@lists.example.org>\r\n"
         "Received: from mail.lists.example.org by mx.gmail.example with ESMTPS id z3\r\n"
         "Return-Path: <upstream@example.net>\r\n" + rest,
         "<bounce@lists.example.org>");
      ExpectPath_("the first of two", "Return-Path: <x@example.com>\r\nReturn-Path: <y@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("the first of two, folded", "Return-Path:\r\n <x@example.com>\r\nReturn-Path: <y@example.com>\r\n" + rest, "<x@example.com>");
      ExpectPath_("the first of two, a continuation of another field first", " folded\r\nFrom: a\r\nReturn-Path: <x@example.com>\r\nReturn-Path: <y@example.com>\r\n" + rest,
         "<x@example.com>");

      // Values that are no path: not the topmost field's either, though a later
      // one holds one.
      ExpectPath_("a word", "Return-Path: bounce\r\n" + rest, "");
      ExpectPath_("an empty value", "Return-Path: \r\n" + rest, "");
      ExpectPath_("a display name", "Return-Path: Bounces <x@example.com>\r\n" + rest, "");
      ExpectPath_("an angle bracket not closed", "Return-Path: <x@example.com\r\n" + rest, "");
      ExpectPath_("a second angle bracket", "Return-Path: <<x@example.com>>\r\n" + rest, "");
      ExpectPath_("something after it", "Return-Path: <x@example.com> x\r\n" + rest, "");
      ExpectPath_("white space inside the address", "Return-Path: <x @example.com>\r\n" + rest, "");
      ExpectPath_("no domain", "Return-Path: <x@>\r\n" + rest, "");
      ExpectPath_("no local part", "Return-Path: <@example.com>\r\n" + rest, "");
      ExpectPath_("a route and no address", "Return-Path: <@relay.example.net:>\r\n" + rest, "");
      ExpectPath_("a route with no domain", "Return-Path: <,:x@example.com>\r\n" + rest, "");
      ExpectPath_("white space in a domain literal", "Return-Path: <x@[192.0.2.1 ]>\r\n" + rest, "");
      ExpectPath_("two at signs", "Return-Path: <x@@example.com>\r\n" + rest, "");
      ExpectPath_("a dot last in the local part", "Return-Path: <x.@example.com>\r\n" + rest, "");
      ExpectPath_("a comment not closed", "Return-Path: (bounce <x@example.com>\r\n" + rest, "");
      ExpectPath_("a comment not closed after the path", "Return-Path: <x@example.com> (bounce\r\n" + rest, "");
      ExpectPath_("a quoted string not closed", "Return-Path: <\"x@example.com>\r\n" + rest, "");
      ExpectPath_("a bare CR in the value", "Return-Path: <x@example.com>\rX-Injected: 1\r\n" + rest, "");
      ExpectPath_("a NUL in the value", std::string("Return-Path: <x@exa") + '\0' + "mple.com>\r\n" + rest, "");
      ExpectPath_("a control character in a quoted local part", "Return-Path: <\"x\x01y\"@example.com>\r\n" + rest, "");
      ExpectPath_("a bare CR in a comment", "Return-Path: (bounce\rX-Injected: 1) <x@example.com>\r\n" + rest, "");
      ExpectPath_("the stream ends inside the value", "Return-Path: <x@exa", "");
      ExpectPath_("the topmost no path, a later one a path", "Return-Path: bounce\r\nReturn-Path: <y@example.com>\r\n" + rest, "");

      // Fields the filter does not take for a Return-Path field, and one too long.
      {
         const std::string spaces(ReturnPathFieldFilter::MaxHeldLine, ' ');
         ExpectPath_("a line too long to judge", "Return-Path" + spaces + ": <x@example.com>\r\n" + rest, "");

         const std::string comment(SpamAssassinReturnPath::MaxPathField, 'c');
         ExpectPath_("a field too long", "Return-Path: (" + comment + ") <x@example.com>\r\n" + rest, "");
      }

      // What the scan of a collected message is sent.
      std::string path;
      if (!SpamAssassinReturnPath::TopmostPath("Return-Path: (list) <x@example.com>\r\n" + rest, path) ||
          SpamAssassinReturnPath::RequestLine(path) != "Return-Path: <x@example.com>\r\n")
         throw std::logic_error("SpamAssassinReturnPath: the line sent for a collected message's topmost Return-Path is not its path.");
   }
}
