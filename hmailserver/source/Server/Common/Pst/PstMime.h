// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A message read from a PST, written as the Internet message it stands for
// ([MS-OXCMAIL]). A PST keeps properties, not the message as it arrived, so this
// is a rebuild, and it is DETERMINISTIC: the same message gives the same bytes
// on every run - every boundary is made from the store and the message's NID,
// and no header carries the time it was made - because a job that is
// interrupted and resumed recognises what it filed before by those bytes.
//
// The headers are the message's transport headers where it kept them - what
// arrived, with MIME-Version and every Content-* field left out and written
// again for the body built here - and otherwise built from the properties:
// From (the "sent representing" sender), Sender when it differs, To, Cc and -
// only where the recipient table has one - Bcc, Subject, Date (the submit time,
// else the delivery time, else the creation time, in UTC), Message-ID,
// In-Reply-To and References; an Exchange sender or recipient by the SMTP
// address the PST keeps beside it. Text that is not ASCII is RFC 2047-encoded,
// and a CR or LF inside a property never starts a header.
//
// The body: text/plain from PR_BODY; text/html from PR_HTML, its bytes as they
// are and its charset named from PR_INTERNET_CPID, or else the HTML an RTF body
// carries (\fromhtml1); an RTF body that is neither HTML nor text is kept as
// body.rtf beside the text. multipart/alternative, multipart/related (the
// attachments with a Content-ID, under an HTML body) and multipart/mixed each
// appear only where they are needed. An attachment by value is a part, named
// by its long file name cut to its last component and RFC 2231-encoded; an
// embedded message is message/rfc822, rebuilt the same way. S/MIME is kept as it
// was: a clear-signed message's signed entity byte for byte, an opaque one as
// application/pkcs7-mime.

#pragma once

#include "PstStore.h"

namespace HM
{
   class PstMime
   {
   public:
      // The message as RFC 5322 bytes, CRLF throughout. stem goes into every
      // boundary; what the rebuild could not keep is added to warnings.
      static std::string Build(const PstMessage &message, const std::string &stem, std::vector<PstWarning> &warnings);

      // The pieces, public for the tester.

      // A header's text with every CR, LF and other control character made a
      // space.
      static std::string HeaderSafe(const std::string &utf8);

      // RFC 2047 encoded words (B, UTF-8), each at most 75 characters, split at
      // character boundaries and joined by a folding CRLF SP.
      static std::string EncodedWords(const std::string &utf8);

      // A display name as a phrase: as it is, quoted, or encoded.
      static std::string Phrase(const std::string &utf8);

      // name <address>, or the address alone; with no usable address, the name
      // as an empty group ("Name:;"), which says who without inventing where.
      static std::string Mailbox(const std::string &name, const std::string &address);

      static std::string QuotedPrintable(const std::string &bytes);
      static std::string Base64(const std::string &bytes);

      // Line endings made CRLF: a lone CR or LF becomes CRLF.
      static std::string Crlf(const std::string &text);

      // "Fri, 01 Mar 2024 09:00:00 +0000".
      static std::string DateText(int64_t unixSeconds);

      // An attachment's name as a file name: its last path component, with no
      // control characters, "attachment" when nothing is left.
      static std::string CleanFileName(const std::string &utf8);

      // parameter="name" for ASCII, parameter*=UTF-8''percent-encoded otherwise.
      static std::string FileNameParameter(const std::string &parameter, const std::string &utf8);

      // Transport headers with MIME-Version and the Content-* fields left out,
      // CRLF throughout, ending in CRLF; nothing after the first empty line.
      static std::string WithoutContentFields(const std::string &headers);
   };
}
