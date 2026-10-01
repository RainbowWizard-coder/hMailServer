// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "IMAPAddressList.h"

#include "../Common/Mime/Mime.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      enum EnvelopeTokenKind
      {
         EnvelopeTokenAtom,
         EnvelopeTokenQuoted,
         EnvelopeTokenComment,
         EnvelopeTokenSpecial
      };

      struct EnvelopeToken
      {
         EnvelopeTokenKind kind = EnvelopeTokenAtom;
         std::string text;
      };

      // The RFC 5322 specials that give an address list its structure. "." is
      // not among them here: it belongs to the atom it is in (a dot-atom, or an
      // obsolete phrase such as "J. Doe").
      bool IsEnvelopeSpecial_(char c)
      {
         return c == '<' || c == '>' || c == '@' || c == ':' || c == ';' || c == ',';
      }

      bool IsEnvelopeSpace_(char c)
      {
         return c == ' ' || c == '\t' || c == '\r' || c == '\n';
      }

      bool IsSpecialToken_(const EnvelopeToken &token, char c)
      {
         return token.kind == EnvelopeTokenSpecial && token.text.size() == 1 && token.text[0] == c;
      }

      // RFC 5322 3.2.3: atext, and a dot-atom - atext runs joined by single dots.
      bool IsAtext_(char c)
      {
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return true;

         return std::string("!#$%&'*+-/=?^_`{|}~").find(c) != std::string::npos;
      }

      bool IsDotAtom_(const std::string &text)
      {
         if (text.empty() || text[0] == '.' || text[text.size() - 1] == '.')
            return false;

         for (size_t i = 0; i < text.size(); i++)
         {
            if (text[i] == '.')
            {
               if (text[i + 1] == '.')
                  return false;
               continue;
            }

            if (!IsAtext_(text[i]))
               return false;
         }

         return true;
      }

      std::string QuoteLocalPart_(const std::string &text)
      {
         std::string quoted = "\"";

         for (size_t i = 0; i < text.size(); i++)
         {
            if (text[i] == '"' || text[i] == '\\')
               quoted += '\\';
            quoted += text[i];
         }

         quoted += '"';
         return quoted;
      }

      std::vector<EnvelopeToken> TokenizeAddressList_(const std::string &value)
      {
         std::vector<EnvelopeToken> tokens;

         const size_t length = value.size();
         size_t i = 0;

         while (i < length)
         {
            const char c = value[i];

            if (IsEnvelopeSpace_(c))
            {
               i++;
               continue;
            }

            if (c == '"')
            {
               // A quoted string, its quoted pairs undone. One never closed runs to
               // the end of the value.
               EnvelopeToken token;
               token.kind = EnvelopeTokenQuoted;

               i++;
               while (i < length && value[i] != '"')
               {
                  if (value[i] == '\\' && i + 1 < length)
                     i++;

                  token.text += value[i];
                  i++;
               }

               if (i < length)
                  i++;

               tokens.push_back(token);
               continue;
            }

            if (c == '(')
            {
               // A comment, which may nest.
               EnvelopeToken token;
               token.kind = EnvelopeTokenComment;

               int depth = 1;
               i++;
               while (i < length)
               {
                  const char d = value[i];

                  if (d == '\\' && i + 1 < length)
                  {
                     token.text += value[i + 1];
                     i += 2;
                     continue;
                  }

                  if (d == '(')
                     depth++;
                  else if (d == ')')
                  {
                     depth--;

                     if (depth == 0)
                     {
                        i++;
                        break;
                     }
                  }

                  token.text += d;
                  i++;
               }

               tokens.push_back(token);
               continue;
            }

            if (IsEnvelopeSpecial_(c))
            {
               EnvelopeToken token;
               token.kind = EnvelopeTokenSpecial;
               token.text = std::string(1, c);
               tokens.push_back(token);

               i++;
               continue;
            }

            // An atom: up to white space, a special, a quote or a comment. A domain
            // literal's brackets are part of it.
            EnvelopeToken token;
            token.kind = EnvelopeTokenAtom;

            while (i < length && !IsEnvelopeSpace_(value[i]) && !IsEnvelopeSpecial_(value[i]) &&
                   value[i] != '"' && value[i] != '(')
            {
               token.text += value[i];
               i++;
            }

            tokens.push_back(token);
         }

         return tokens;
      }

      // A display name - or a group's name - from the tokens [first, last): its
      // words joined by single spaces, comments left out. An "@" in a name that
      // should have been quoted ("user@example.com <user@example.com>") stays
      // in it, attached to its neighbours.
      std::string PhraseOf_(const std::vector<EnvelopeToken> &tokens, size_t first, size_t last)
      {
         std::string phrase;
         bool glue = false;

         for (size_t i = first; i < last; i++)
         {
            const EnvelopeToken &token = tokens[i];

            if (token.kind == EnvelopeTokenComment)
               continue;

            if (token.kind == EnvelopeTokenSpecial)
            {
               if (!IsSpecialToken_(token, '@'))
                  continue;

               phrase += token.text;
               glue = true;
               continue;
            }

            if (!phrase.empty() && !glue)
               phrase += ' ';

            phrase += token.text;
            glue = false;
         }

         return phrase;
      }

      // addr-spec - local-part "@" domain - from the tokens [first, last),
      // comments skipped. Without an "@" the whole is the mailbox and there is
      // no host.
      void ParseAddrSpec_(const std::vector<EnvelopeToken> &tokens, size_t first, size_t last, IMAPAddress &address)
      {
         size_t at = last;
         for (size_t i = first; i < last; i++)
         {
            if (IsSpecialToken_(tokens[i], '@'))
               at = i;
         }

         if (at == last)
         {
            address.mailbox = PhraseOf_(tokens, first, last).c_str();
            return;
         }

         std::string mailbox;
         bool quoted = false;
         for (size_t i = first; i < at; i++)
         {
            if (tokens[i].kind == EnvelopeTokenAtom || tokens[i].kind == EnvelopeTokenQuoted)
               mailbox += tokens[i].text;

            if (tokens[i].kind == EnvelopeTokenQuoted)
               quoted = true;
         }

         // A quoted local part that is not a dot-atom - "john doe"@x - keeps its
         // quotes, its quote and backslash characters escaped again: without
         // them it is a different address, and one that does not parse.
         if (quoted && !IsDotAtom_(mailbox))
            mailbox = QuoteLocalPart_(mailbox);

         std::string host;
         for (size_t i = at + 1; i < last; i++)
         {
            if (tokens[i].kind == EnvelopeTokenAtom)
               host += tokens[i].text;
         }

         address.mailbox = mailbox.c_str();
         address.host = host.c_str();
      }

      // One list element that is not a group marker: a name-addr or an
      // addr-spec, from the tokens [first, last).
      void ParseMailbox_(const std::vector<EnvelopeToken> &tokens, size_t first, size_t last, std::vector<IMAPAddress> &result)
      {
         IMAPAddress address;
         address.kind = IMAPAddress::KindMailbox;

         // The first comment outside any angle brackets: the name when there is
         // no phrase, which is how "user@domain (Real Name)" names its mailbox.
         std::string comment;

         size_t open = last;
         for (size_t i = first; i < last; i++)
         {
            if (IsSpecialToken_(tokens[i], '<'))
            {
               open = i;
               break;
            }
         }

         if (open < last)
         {
            size_t close = last;
            for (size_t i = open + 1; i < last; i++)
            {
               if (IsSpecialToken_(tokens[i], '>'))
               {
                  close = i;
                  break;
               }
            }

            for (size_t i = first; i < last; i++)
            {
               // A comment inside the angle brackets is not a name. With no ">"
               // everything from the "<" on is inside them.
               const bool insideAngle = i >= open && (close == last || i <= close);
               if (insideAngle)
                  continue;

               if (tokens[i].kind == EnvelopeTokenComment && comment.empty())
                  comment = tokens[i].text;
            }

            const std::string phrase = PhraseOf_(tokens, first, open);

            size_t addrStart = open + 1;

            // obs-route (RFC 5322 4.4): "<@a,@b:user@domain>".
            if (addrStart < close && IsSpecialToken_(tokens[addrStart], '@'))
            {
               size_t colon = close;
               for (size_t i = addrStart; i < close; i++)
               {
                  if (IsSpecialToken_(tokens[i], ':'))
                  {
                     colon = i;
                     break;
                  }
               }

               if (colon < close)
               {
                  std::string route;
                  for (size_t i = addrStart; i < colon; i++)
                  {
                     if (tokens[i].kind != EnvelopeTokenComment)
                        route += tokens[i].text;
                  }

                  address.route = route.c_str();
                  addrStart = colon + 1;
               }
            }

            ParseAddrSpec_(tokens, addrStart, close, address);

            address.personal_name = phrase.empty() ? comment.c_str() : phrase.c_str();
         }
         else
         {
            for (size_t i = first; i < last; i++)
            {
               if (tokens[i].kind == EnvelopeTokenComment && comment.empty())
                  comment = tokens[i].text;
            }

            ParseAddrSpec_(tokens, first, last, address);

            address.personal_name = comment.c_str();
         }

         // An element with no address in it - a lone comment in an empty group,
         // "<>" - is not a mailbox.
         if (address.mailbox.IsEmpty() && address.host.IsEmpty())
            return;

         result.push_back(address);
      }
   }

   std::vector<IMAPAddress>
   IMAPAddressList::Parse(const AnsiString &rawValue)
   {
      std::vector<IMAPAddress> result;

      const std::string value = rawValue.c_str();
      const std::vector<EnvelopeToken> tokens = TokenizeAddressList_(value);
      const size_t count = tokens.size();

      bool inGroup = false;
      size_t i = 0;

      while (i < count)
      {
         // One element: up to a "," - or, inside a group, a ";" - that is not
         // inside angle brackets. A ":" outside them, outside a group, opens one.
         const size_t start = i;
         size_t end = i;
         size_t colon = count;
         bool inAngle = false;

         for (; end < count; end++)
         {
            const EnvelopeToken &token = tokens[end];

            if (token.kind != EnvelopeTokenSpecial)
               continue;

            if (IsSpecialToken_(token, '<'))
               inAngle = true;
            else if (IsSpecialToken_(token, '>'))
               inAngle = false;
            else if (inAngle)
               continue;
            else if (IsSpecialToken_(token, ','))
               break;
            else if (IsSpecialToken_(token, ';') && inGroup)
               break;
            else if (IsSpecialToken_(token, ':') && !inGroup)
            {
               colon = end;
               break;
            }
         }

         if (colon < count)
         {
            // RFC 3501 7.4.2: a group start is (NIL NIL "name" NIL).
            IMAPAddress group;
            group.kind = IMAPAddress::KindGroupStart;
            group.mailbox = PhraseOf_(tokens, start, colon).c_str();
            result.push_back(group);

            inGroup = true;
            i = colon + 1;
            continue;
         }

         if (end > start)
            ParseMailbox_(tokens, start, end, result);

         if (end < count)
         {
            if (IsSpecialToken_(tokens[end], ';'))
            {
               // ...and its end is (NIL NIL NIL NIL).
               IMAPAddress groupEnd;
               groupEnd.kind = IMAPAddress::KindGroupEnd;
               result.push_back(groupEnd);

               inGroup = false;
            }

            i = end + 1;
         }
         else
         {
            i = end;
         }
      }

      // A group never closed is closed here, so an ENVELOPE built from it still
      // pairs every start with an end.
      if (inGroup)
      {
         IMAPAddress groupEnd;
         groupEnd.kind = IMAPAddress::KindGroupEnd;
         result.push_back(groupEnd);
      }

      return result;
   }

   std::vector<String>
   IMAPAddressList::GetSearchTexts(const AnsiString &rawValue)
   {
      std::vector<String> texts;

      for (const IMAPAddress &address : Parse(rawValue))
      {
         if (address.kind == IMAPAddress::KindGroupEnd)
            continue;

         // A name in the header's own text may be an encoded-word; it is searched
         // decoded, as the rest of SEARCH searches header text.
         String name;
         if (!address.personal_name.IsEmpty())
            name = MimeHeader::GetUnicodeFieldValue("Subject", address.personal_name);

         if (address.kind == IMAPAddress::KindGroupStart)
         {
            String groupName = MimeHeader::GetUnicodeFieldValue("Subject", address.mailbox);

            if (!groupName.IsEmpty())
               texts.push_back(groupName);

            continue;
         }

         String mailbox = String(address.mailbox.c_str());
         if (!address.host.IsEmpty())
            mailbox += _T("@") + String(address.host.c_str());

         if (name.IsEmpty())
            texts.push_back(mailbox);
         else
            texts.push_back(name + _T(" <") + mailbox + _T(">"));
      }

      return texts;
   }

   AnsiString
   IMAPAddressList::GetFirstMailbox(const AnsiString &rawValue)
   {
      for (const IMAPAddress &address : Parse(rawValue))
      {
         if (address.kind == IMAPAddress::KindMailbox || address.kind == IMAPAddress::KindGroupStart)
            return address.mailbox;
      }

      return AnsiString();
   }
}
