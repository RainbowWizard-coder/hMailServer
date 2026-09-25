// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // One element of an RFC 5322 address list, as IMAP presents it (RFC 3501
   // section 7.4.2): a mailbox, or the start or the end of a group. Every text
   // is the header's own, encoded-words included - an ENVELOPE carries the
   // header's text, and a search decodes what it compares.
   class IMAPAddress
   {
   public:
      enum Kind
      {
         KindMailbox,
         KindGroupStart,   // mailbox holds the group's name; the ENVELOPE's host is NIL
         KindGroupEnd      // everything NIL
      };

      Kind kind = KindMailbox;

      // The display name - the phrase before <...>, or, with no phrase, the
      // comment after a bare address ("user@domain (Real Name)"). Empty when
      // there is neither.
      AnsiString personal_name;

      // The obsolete source route of "<@a,@b:user@domain>", "@a,@b"; empty
      // when there is none.
      AnsiString route;

      // The local part, unquoted; the group's name for KindGroupStart.
      AnsiString mailbox;

      // The domain; empty when the address has none.
      AnsiString host;
   };

   // RFC 5322 address lists - From, Sender, Reply-To, To, Cc, Bcc - parsed the
   // way IMAP needs them: ENVELOPE's address structures, the FROM/TO/CC/BCC
   // search keys (RFC 3501 6.4.4, which search "the envelope structure's"
   // fields) and the SORT address keys (RFC 5256). The general-purpose
   // AddresslistParser is deliberately left alone: it serves delivery, DKIM
   // and DMARC with conventions of its own (the address stands in for a
   // missing name), which are wrong for IMAP but relied on there.
   //
   // Lenient, as a server reading other people's mail must be: groups,
   // comments, quoted strings, quoted pairs, source routes and domain literals
   // are understood; anything that is not an address is kept as well as it
   // can be rather than dropped.
   class IMAPAddressList
   {
   public:
      static std::vector<IMAPAddress> Parse(const AnsiString &rawValue);

      // The texts an address search key matches in, one per list element:
      // "Name <mailbox@host>" (or "mailbox@host" with no name) for each
      // mailbox, and each group's name. The name is decoded (RFC 2047).
      static std::vector<String> GetSearchTexts(const AnsiString &rawValue);

      // RFC 5256 section 3: the addr-mailbox of the first address - the local
      // part, or a group's name - as the FROM, TO and CC sort keys use it.
      // Empty when the list has no address.
      static AnsiString GetFirstMailbox(const AnsiString &rawValue);
   };
}
