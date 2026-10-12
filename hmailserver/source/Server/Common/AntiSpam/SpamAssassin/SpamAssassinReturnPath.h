// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Return-Path a SpamAssassin scan is sent, and the bytes that keep it out of
// the stored message.
//
// SpamAssassin reads the envelope sender from a Return-Path field at the top of
// the message - its SPF checks and a number of its stock rules need it - and a
// message being accepted has none yet: the Return-Path is written by the final
// delivery (RFC 5321 section 4.4). So the scan sends one, built from the envelope
// sender, as the first line of the message in the PROCESS request, with the
// request's Content-length counting it, and nowhere else. A message collected from
// another server's mailbox has no envelope sender here - the server guesses one
// from its From field - but that server's final delivery recorded the real one, in
// the message's own topmost Return-Path; the scan of such a message sends that
// field's path instead, when it reads as one (TopmostPath). The stored message is
// never written for it:
//
//   - The message's own Return-Path fields, if it arrived with any, are left out
//     of the request, so that spamd sees one envelope sender and it is the
//     server's (RFC 5321 lets a final delivery remove them). A scan that fails
//     leaves them in the stored message, which it does not touch; a completed
//     scan's result, which replaces the message, does not carry them - as the
//     message rewritten after the scan did not before 6.3.7.
//   - spamd's reply, when the scan completes, is the message as SpamAssassin left
//     it, and it becomes the stored message (SpamAssassinClient::FinishTesting_).
//     The Return-Path fields of the reply's own header block - the one sent, which
//     SpamAssassin keeps above its X-Spam fields - are left out of it byte for byte
//     on the way to the disk, and every other byte is written as spamd sent it.
//     Until 6.3.7 the line was written into the stored file before the scan and
//     taken out afterwards by parsing the whole message and writing it again,
//     which changed bytes the scan never touched (a multipart message's epilogue
//     and boundary lines), and every way the scan could fail - spamd down, a
//     timeout, a reply cut short - left the line in the message, under the one the
//     delivery then wrote.
//
// A field is a Return-Path field when its line starts with the name, in any case,
// and a colon follows it, with only spaces or tabs between (the obsolete syntax of
// RFC 5322 section 4.5, obs-return in section 4.5.7); its continuation lines -
// those that start with a space or a tab - go with it. The header block ends at
// the first empty line, CR LF or a bare LF; nothing after it is looked at, so a
// Return-Path in the body, or in the header of an attached message, stays where it
// is.

#pragma once

#include <string>

namespace HM
{
   // The Return-Path fields of a message's own header block, left out of its bytes
   // as they pass, whatever the pieces they arrive in. Every byte that is not part
   // of one passes unchanged and in order; the empty line that ends the header block
   // passes too. A line it cannot judge yet - the start of one that may be a
   // Return-Path field - is held until it can, and Finish hands over whatever is
   // held when the stream ends.
   class ReturnPathFieldFilter
   {
   public:
      ReturnPathFieldFilter();

      // Appends to output the bytes of data that pass.
      void Feed(const char *data, size_t length, std::string &output);

      // The end of the stream: a line still held is not a Return-Path field (its
      // colon never came), and passes.
      void Finish(std::string &output);

      // The bytes and the Return-Path fields left out so far.
      size_t GetRemovedBytes() const { return removed_bytes_; }
      int GetRemovedFields() const { return removed_fields_; }

      // Whether the empty line that ends the header block has passed. From there
      // on every byte passes, and nothing is held.
      bool GetHeaderEnded() const { return !in_header_; }

      // The longest a line is held to be judged: RFC 5322's limit on a line. A
      // line still undecided past it is not taken for a Return-Path field.
      static const size_t MaxHeldLine = 998;

   private:

      enum LineState
      {
         LineUndecided,
         LineKept,
         LineRemoved
      };

      enum Verdict
      {
         VerdictNotYet,
         VerdictEndOfHeader,
         VerdictKeepField,
         VerdictKeepContinuation,
         VerdictRemoveField,
         VerdictRemoveContinuation
      };

      Verdict Judge_() const;
      void Decide_(std::string &output);

      bool in_header_;
      bool removing_field_;
      LineState line_state_;
      std::string held_;
      size_t removed_bytes_;
      int removed_fields_;
   };

   class SpamAssassinReturnPath
   {
   public:
      // The line sent to spamd ahead of the message: "Return-Path: " and the value,
      // and CR LF. A CR or an LF in the value is left out - it would end the line
      // and make what follows a field of its own.
      static std::string RequestLine(const std::string &returnPath);

      // The path of a message's topmost Return-Path field - the first of its header
      // block, as ReturnPathFieldFilter takes one - written as RequestLine is given
      // one: "<local-part@domain>", or "<>" for the null path. For a message
      // collected from another server's mailbox, that is the field the other
      // server's final delivery wrote, and its path the envelope sender the message
      // was delivered with there - where that server wrote none, it is one the
      // message carried when it arrived there, a path its sender chose no less
      // freely than the From field the guess is read from; for any other message
      // it is whatever the message arrived with, so only the collection asks for
      // it. False, and path left as it was, when the header block has no
      // Return-Path field, when its field runs past MaxPathField bytes, or when its
      // value does not read as a path: RFC 5322's path (section 3.6.7), with an
      // obsolete route (section 4.4) or as a bare address, white space and
      // comments around it; the address a dot-atom or a quoted string, "@", and a
      // dot-atom or a domain literal, with no white space or comment inside it; and
      // no control character. Only the topmost field is read: a later one was
      // written before it, further from the mailbox. message may be the whole
      // message or its header block.
      static bool TopmostPath(const std::string &message, std::string &path);

      static const size_t MaxPathField = 4096;
   };

   // ReturnPathFieldFilter and RequestLine held to the edges: a Return-Path field
   // first, in the middle, last, twice and folded; the name in any case and with
   // white space before its colon; names that only begin like it (Return-Paths,
   // Return-Path-Id) and that end like it (X-Return-Path); CR LF and bare LF lines;
   // the header block's end, a Return-Path after it and in an attached message; a
   // message with no header block and one with no empty line; a line cut off by
   // the end of the stream; and every message fed in every split into two pieces
   // and one byte at a time, which must pass exactly what one piece passes. And
   // TopmostPath: the paths a Return-Path holds in the field (a plain, null,
   // obsolete-routed and bare address, comments, folding, a quoted local part, a
   // domain literal, UTF-8), the topmost of two and below Received fields, and
   // the values that are no path. Throws std::logic_error naming the first thing
   // that is wrong. No file, network, configuration or database: ClassTester runs
   // it inside the service, and public-suffix-tests on Linux.
   class SpamAssassinReturnPathTester
   {
   public:
      void Test();

   private:
      void Expect_(const std::string &what, const std::string &input, const std::string &expected, int expectedFields);
      void ExpectPath_(const std::string &what, const std::string &message, const std::string &expected);
      void TestTopmostPath_();
   };
}
