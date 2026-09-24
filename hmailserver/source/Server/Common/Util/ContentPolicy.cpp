// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ContentPolicy.h"

#include "../BO/MessageData.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../BO/Attachments.h"
#include "../BO/Attachment.h"
#include "../BO/Domain.h"
#include "../BO/Route.h"
#include "../BO/Routes.h"
#include "../BO/RouteAddresses.h"
#include "../Cache/CacheContainer.h"
#include "../Application/Configuration.h"
#include "../../SMTP/SMTPConfiguration.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Unicode.h"
#include "../Rules/RuleGuard.h"

#include <cctype>
#include <cstring>
#include <vector>
#include <boost/regex.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How much readable text one message contributes. GetAttachmentText
      // bounds itself the same way and for the same reason: a message can carry
      // a very large text attachment, and the rule engine holds the result.
      const int MaximumReadableCharacters = 1024 * 1024;

      bool IsAsciiDigit(wchar_t c)
      {
         return c >= L'0' && c <= L'9';
      }

      bool IsAsciiLetter(wchar_t c)
      {
         return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
      }

      bool IsAsciiAlnum(wchar_t c)
      {
         return IsAsciiDigit(c) || IsAsciiLetter(c);
      }

      // The IBAN registry, as published by SWIFT: the country and the length
      // of that country's IBAN. A country not here does not have one, so a run
      // of characters beginning with its code is not asked the check-digit
      // question at all - which is what keeps "PO12" in a part number from
      // being tried at every length.
      struct IbanCountry
      {
         const char *code;
         int length;
      };

      const IbanCountry IbanCountries[] =
      {
         { "AD", 24 }, { "AE", 23 }, { "AL", 28 }, { "AT", 20 }, { "AZ", 28 }, { "BA", 20 }, { "BE", 16 },
         { "BG", 22 }, { "BH", 22 }, { "BI", 27 }, { "BR", 29 }, { "BY", 28 }, { "CH", 21 }, { "CR", 22 },
         { "CY", 28 }, { "CZ", 24 }, { "DE", 22 }, { "DJ", 27 }, { "DK", 18 }, { "DO", 28 }, { "EE", 20 },
         { "EG", 29 }, { "ES", 24 }, { "FI", 18 }, { "FK", 18 }, { "FO", 18 }, { "FR", 27 }, { "GB", 22 },
         { "GE", 22 }, { "GI", 23 }, { "GL", 18 }, { "GR", 27 }, { "GT", 28 }, { "HR", 21 }, { "HU", 28 },
         { "IE", 22 }, { "IL", 23 }, { "IQ", 23 }, { "IS", 26 }, { "IT", 27 }, { "JO", 30 }, { "KW", 30 },
         { "KZ", 20 }, { "LB", 28 }, { "LC", 32 }, { "LI", 21 }, { "LT", 20 }, { "LU", 20 }, { "LV", 21 },
         { "LY", 25 }, { "MC", 27 }, { "MD", 24 }, { "ME", 22 }, { "MK", 19 }, { "MN", 20 }, { "MR", 27 },
         { "MT", 31 }, { "MU", 30 }, { "NI", 28 }, { "NL", 18 }, { "NO", 15 }, { "OM", 23 }, { "PK", 24 },
         { "PL", 28 }, { "PS", 29 }, { "PT", 25 }, { "QA", 29 }, { "RO", 24 }, { "RS", 22 }, { "RU", 33 },
         { "SA", 24 }, { "SC", 31 }, { "SD", 18 }, { "SE", 24 }, { "SI", 19 }, { "SK", 24 }, { "SM", 27 },
         { "SO", 23 }, { "ST", 25 }, { "SV", 28 }, { "TL", 23 }, { "TN", 24 }, { "TR", 26 }, { "UA", 29 },
         { "VA", 22 }, { "VG", 24 }, { "XK", 20 }, { "YE", 30 }
      };

      bool StartsWithBytes(const AnsiString &bytes, const char *prefix, size_t length)
      {
         if ((size_t) bytes.GetLength() < length)
            return false;
         return memcmp(bytes.c_str(), prefix, length) == 0;
      }
   }

   // ---------------------------------------------------------------------
   // The readable text.
   // ---------------------------------------------------------------------

   String
   ContentPolicy::StripHtml(const String &html)
   {
      // Tags become a space, so "<td>4111</td><td>1111</td>" reads as two
      // groups with a space between them, which the card scan allows; the
      // entities a number is likely to be wrapped in are unwrapped. This is
      // not an HTML parser and does not claim to be one: it is enough to find
      // digits in a table.
      String text;
      text.reserve(html.GetLength());

      bool inTag = false;
      for (int i = 0; i < html.GetLength(); i++)
      {
         const wchar_t c = html[i];
         if (inTag)
         {
            if (c == L'>')
            {
               inTag = false;
               text += L' ';
            }
            continue;
         }
         if (c == L'<')
         {
            inTag = true;
            continue;
         }
         text += c;
      }

      text.Replace(_T("&nbsp;"), _T(" "));
      text.Replace(_T("&#160;"), _T(" "));
      text.Replace(_T("&amp;"), _T("&"));
      text.Replace(_T("&lt;"), _T("<"));
      text.Replace(_T("&gt;"), _T(">"));
      text.Replace(_T("&quot;"), _T("\""));

      // One space where several fell together. Two adjacent tags - the
      // "</td><td>" between a table's cells, a "</p>\n<p>" between paragraphs -
      // each leave their own space, and the card scan allows at most one
      // separator between two digits: a card written across four cells would
      // read as four short runs and be missed, which is exactly how a card gets
      // out of an office in a spreadsheet pasted into a message. Newlines and
      // tabs are spaces here too, since the text has no layout left to keep.
      String collapsed;
      collapsed.reserve(text.GetLength());
      bool lastWasSpace = false;
      for (int i = 0; i < text.GetLength(); i++)
      {
         const wchar_t c = text[i];
         const bool isSpace = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f' || c == L'\v';
         if (isSpace)
         {
            if (!lastWasSpace)
               collapsed += L' ';
            lastWasSpace = true;
            continue;
         }
         collapsed += c;
         lastWasSpace = false;
      }

      return collapsed;
   }

   String
   ContentPolicy::ReadableText(std::shared_ptr<MessageData> data)
   {
      String text;
      if (!data)
         return text;

      text = data->GetBody();

      String html = data->GetHTMLBody();
      if (!html.IsEmpty())
      {
         if (!text.IsEmpty())
            text += _T("\n");
         text += StripHtml(html);
      }

      if (text.GetLength() > MaximumReadableCharacters)
         return text.Left(MaximumReadableCharacters);

      // Every attachment whose bytes are text, whatever it is called and
      // whatever its Content-Type declares: a .csv sent as
      // application/octet-stream is still a list of numbers a person can read.
      std::shared_ptr<Attachments> attachments = data->GetAttachments();
      if (!attachments)
         return text;

      for (size_t i = 0; i < attachments->GetCount(); i++)
      {
         std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
         if (!attachment)
            continue;

         AnsiString bytes;
         if (!attachment->GetContent(bytes) || bytes.IsEmpty())
            continue;

         // A file that begins with a UTF-16 byte-order mark is read through
         // it; anything else is read as UTF-8, of which the RFC 2046 default
         // of US-ASCII is a subset, and a file in another single-byte encoding
         // still has its digits in ASCII.
         String attachmentText;
         if (!Utf16Text(bytes, attachmentText))
         {
            if (!LooksLikeText_(bytes))
               continue;
            if (!Unicode::MultiByteToWide(bytes, attachmentText))
               continue;
         }
         if (attachmentText.IsEmpty())
            continue;

         if (!text.IsEmpty())
            text += _T("\n");

         if (text.GetLength() + attachmentText.GetLength() > MaximumReadableCharacters)
         {
            text += attachmentText.Left(MaximumReadableCharacters - text.GetLength());
            return text;
         }
         text += attachmentText;
      }

      return text;
   }

   // ---------------------------------------------------------------------
   // The detectors.
   // ---------------------------------------------------------------------

   bool
   ContentPolicy::LuhnValid(const AnsiString &digits)
   {
      if (digits.IsEmpty())
         return false;

      int sum = 0;
      bool doubleIt = false;
      for (int i = digits.GetLength() - 1; i >= 0; i--)
      {
         const char c = digits[i];
         if (c < '0' || c > '9')
            return false;
         int digit = c - '0';
         if (doubleIt)
         {
            digit *= 2;
            if (digit > 9)
               digit -= 9;
         }
         sum += digit;
         doubleIt = !doubleIt;
      }
      return sum % 10 == 0;
   }

   bool
   ContentPolicy::IsCardNumber_(const AnsiString &digits)
   {
      // A run of one repeated digit passes Luhn (0000000000000000 does) and is
      // nobody's card.
      if (digits.GetLength() < 13 || digits.GetLength() > 19)
         return false;

      bool allSame = true;
      for (int k = 1; k < digits.GetLength(); k++)
      {
         if (digits[k] != digits[0])
         {
            allSame = false;
            break;
         }
      }

      return !allSame && LuhnValid(digits);
   }

   bool
   ContentPolicy::ContainsCardNumber(const String &text)
   {
      // A run of 13 to 19 digits, with at most one space or hyphen between
      // any two of them, bounded by anything else. A run longer than 19 is a
      // reference, a hash or a phone list, not a card written out, and is not
      // searched for a card inside it.
      //
      // A run that fails as a whole is tried again up to each of its
      // separators, longest first: "4111 1111 1111 1111 123" is a card and
      // then its security code, and the nineteen digits together fail Luhn
      // while the sixteen before the last space pass. The bound on the whole
      // run still holds, so a card followed by a four-digit group is twenty
      // digits and is left alone, and a solid run with no separator is only
      // ever tried whole - the prefixes of a reference number are not asked
      // whether one of them happens to be a card.
      AnsiString digits;
      std::vector<int> groupEnds;
      bool separatorPending = false;

      const int length = text.GetLength();
      for (int i = 0; i <= length; i++)
      {
         const wchar_t c = i < length ? text[i] : L'\0';

         if (IsAsciiDigit(c))
         {
            if (separatorPending)
               groupEnds.push_back(digits.GetLength());
            digits += (char) c;
            separatorPending = false;
            continue;
         }

         if ((c == L' ' || c == L'-') && !digits.IsEmpty() && !separatorPending)
         {
            separatorPending = true;
            continue;
         }

         // The run ended (a second separator in a row ends it too).
         if (digits.GetLength() >= 13 && digits.GetLength() <= 19)
         {
            if (IsCardNumber_(digits))
               return true;

            for (size_t k = groupEnds.size(); k > 0; k--)
            {
               if (IsCardNumber_(digits.Left(groupEnds[k - 1])))
                  return true;
            }
         }
         digits = "";
         groupEnds.clear();
         separatorPending = false;

         // The character that ended the run may itself begin the next one -
         // it cannot be a digit here, so it does not.
      }

      return false;
   }

   int
   ContentPolicy::IbanLength(char first, char second)
   {
      const char a = (char) toupper((unsigned char) first);
      const char b = (char) toupper((unsigned char) second);
      for (size_t i = 0; i < sizeof(IbanCountries) / sizeof(IbanCountries[0]); i++)
      {
         if (IbanCountries[i].code[0] == a && IbanCountries[i].code[1] == b)
            return IbanCountries[i].length;
      }
      return 0;
   }

   bool
   ContentPolicy::IbanValid(const AnsiString &iban)
   {
      // ISO 13616: move the first four characters to the end, replace each
      // letter with two digits (A=10 ... Z=35), and the number mod 97 is 1.
      // The country's registered length is the first test, so that a string
      // that happens to divide right at some other length is not an IBAN.
      if (iban.GetLength() < 5)
         return false;

      const int expected = IbanLength(iban[0], iban[1]);
      if (expected == 0 || iban.GetLength() != expected)
         return false;

      AnsiString rearranged = iban.Mid(4) + iban.Left(4);
      int remainder = 0;
      for (int i = 0; i < rearranged.GetLength(); i++)
      {
         const char c = (char) toupper((unsigned char) rearranged[i]);
         if (c >= '0' && c <= '9')
         {
            remainder = (remainder * 10 + (c - '0')) % 97;
         }
         else if (c >= 'A' && c <= 'Z')
         {
            const int value = c - 'A' + 10;
            remainder = (remainder * 100 + value) % 97;
         }
         else
            return false;
      }

      return remainder == 1;
   }

   bool
   ContentPolicy::ContainsIban(const String &text)
   {
      // Two letters and two digits at the start of a word whose country the
      // registry knows, then exactly that country's number of characters with
      // spaces allowed between them (the way a bank prints one, in fours),
      // followed by something that is not a letter or a digit.
      const int length = text.GetLength();
      for (int i = 0; i + 3 < length; i++)
      {
         if (i > 0 && IsAsciiAlnum(text[i - 1]))
            continue;
         if (!IsAsciiLetter(text[i]) || !IsAsciiLetter(text[i + 1]) || !IsAsciiDigit(text[i + 2]) || !IsAsciiDigit(text[i + 3]))
            continue;

         const int expected = IbanLength((char) text[i], (char) text[i + 1]);
         if (expected == 0)
            continue;

         AnsiString candidate;
         int position = i;
         while (position < length && candidate.GetLength() < expected)
         {
            const wchar_t c = text[position];
            if (IsAsciiAlnum(c))
               candidate += (char) c;
            else if (c == L' ' && !candidate.IsEmpty() && position + 1 < length && IsAsciiAlnum(text[position + 1]))
               ; // a space between groups
            else
               break;
            position++;
         }

         if (candidate.GetLength() != expected)
            continue;
         if (position < length && IsAsciiAlnum(text[position]))
            continue;

         if (IbanValid(candidate))
            return true;
      }

      return false;
   }

   bool
   ContentPolicy::MatchesAccountNumberPattern(const String &text, const String &pattern)
   {
      if (pattern.IsEmpty() || text.IsEmpty())
         return false;

      // A search, not a match: the number is somewhere in the text, which is
      // the question a policy asks. Through RuleGuard, as a rule's own regex
      // condition is, and not through a bare boost::regex_search: the text is
      // up to a megabyte of what a sender chose to write, and a pattern that
      // back-tracks hard over it is a cost any sender can make recur. The
      // breaker measures the search, reports a pattern that does not compile
      // (HM6821) apart from one that Boost abandons or that is over budget
      // (HM6822), and rests the pattern for a window either way.
      return RuleGuard::PolicyRegexMatches(pattern, text);
   }

   bool
   ContentPolicy::PatternCompiles(const String &pattern)
   {
      if (pattern.IsEmpty())
         return true;

      try
      {
         boost::wregex expression(pattern);
         return true;
      }
      catch (const std::runtime_error &)
      {
         return false;
      }
   }

   String
   ContentPolicy::Detectors(const String &text)
   {
      return Detectors(text, Configuration::Instance()->GetDlpAccountNumberPattern());
   }

   String
   ContentPolicy::Detectors(const String &text, const String &accountNumberPattern)
   {
      String found;
      if (ContainsCardNumber(text))
         found += _T("card_number");
      if (ContainsIban(text))
      {
         if (!found.IsEmpty())
            found += _T(";");
         found += _T("iban");
      }
      if (MatchesAccountNumberPattern(text, accountNumberPattern))
      {
         if (!found.IsEmpty())
            found += _T(";");
         found += _T("account_number");
      }
      return found;
   }

   // ---------------------------------------------------------------------
   // The attachment types.
   // ---------------------------------------------------------------------

   bool
   ContentPolicy::LooksLikeText_(const AnsiString &bytes)
   {
      // No NUL in the first 512 bytes, and nine in ten of them printable or
      // white space. UTF-8 multi-byte sequences are bytes over 127 and count
      // as printable, so a text in any language passes; a compressed or
      // encrypted file does not.
      const int sample = bytes.GetLength() < 512 ? bytes.GetLength() : 512;
      if (sample == 0)
         return false;

      int printable = 0;
      for (int i = 0; i < sample; i++)
      {
         const unsigned char c = (unsigned char) bytes[i];
         if (c == 0)
            return false;
         if (c >= 32 || c == '\t' || c == '\r' || c == '\n' || c == '\f')
            printable++;
      }

      return printable * 10 >= sample * 9;
   }

   bool
   ContentPolicy::Utf16Text(const AnsiString &bytes, String &text)
   {
      // FF FE is little-endian, FE FF big-endian. Without a mark the bytes
      // are not decoded here: a UTF-16 file that has none is binary to the
      // byte test, and guessing an encoding from a NUL in every other byte
      // would also match a good many files that are not text at all.
      if (bytes.GetLength() < 2)
         return false;

      const unsigned char first = (unsigned char) bytes[0];
      const unsigned char second = (unsigned char) bytes[1];
      const bool littleEndian = first == 0xFF && second == 0xFE;
      const bool bigEndian = first == 0xFE && second == 0xFF;
      if (!littleEndian && !bigEndian)
         return false;

      // The code units after the mark; a trailing odd byte is not one.
      const size_t count = ((size_t) bytes.GetLength() - 2) & ~(size_t) 1;
      const unsigned char *units = (const unsigned char *) bytes.c_str() + 2;

      if (littleEndian)
      {
         text = String(Unicode::FromUtf16Le(units, count));
         return true;
      }

      // Big-endian is the same units the other way round, and FromUtf16Le
      // joins surrogate pairs where a wchar_t can hold them (Unicode.h).
      std::string swapped;
      swapped.reserve(count);
      for (size_t i = 0; i + 1 < count; i += 2)
      {
         swapped.push_back((char) units[i + 1]);
         swapped.push_back((char) units[i]);
      }
      text = String(Unicode::FromUtf16Le((const unsigned char *) swapped.data(), swapped.size()));
      return true;
   }

   AnsiString
   ContentPolicy::TypeOfBytes(const AnsiString &bytes)
   {
      if (bytes.IsEmpty())
         return "empty";

      // A UTF-16 byte-order mark says text before the byte test can say
      // binary of the NULs that follow it.
      if (StartsWithBytes(bytes, "\xff\xfe", 2) || StartsWithBytes(bytes, "\xfe\xff", 2))
         return "text";

      if (StartsWithBytes(bytes, "MZ", 2))
         return "executable";
      if (StartsWithBytes(bytes, "\x7f" "ELF", 4))
         return "elf";
      if (StartsWithBytes(bytes, "%PDF", 4))
         return "pdf";
      if (StartsWithBytes(bytes, "PK\x03\x04", 4))
      {
         // The first local file header: its name length at offset 26 and its
         // name at offset 30. An OOXML document lists [Content_Types].xml
         // first and a jar lists META-INF/; anything else is a zip, which is
         // also what an encrypted one is - the first bytes say nothing about
         // what is inside it.
         if (bytes.GetLength() >= 30)
         {
            const unsigned int nameLength = (unsigned char) bytes[26] | ((unsigned char) bytes[27] << 8);
            if (nameLength > 0 && (size_t) bytes.GetLength() >= 30 + nameLength)
            {
               AnsiString name = bytes.Mid(30, nameLength);
               if (name == "[Content_Types].xml")
                  return "office_xml";
               if (name.StartsWith("META-INF/"))
                  return "jar";
            }
         }
         return "zip";
      }
      if (StartsWithBytes(bytes, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8))
         return "office";
      if (StartsWithBytes(bytes, "Rar!\x1a\x07", 6))
         return "rar";
      if (StartsWithBytes(bytes, "7z\xbc\xaf\x27\x1c", 6))
         return "7z";
      if (StartsWithBytes(bytes, "\x1f\x8b", 2))
         return "gzip";
      if (StartsWithBytes(bytes, "\x89PNG", 4))
         return "png";
      if (StartsWithBytes(bytes, "\xff\xd8\xff", 3))
         return "jpeg";
      if (StartsWithBytes(bytes, "GIF87a", 6) || StartsWithBytes(bytes, "GIF89a", 6))
         return "gif";
      if (StartsWithBytes(bytes, "{\\rtf", 5))
         return "rtf";
      if (StartsWithBytes(bytes, "#!", 2))
         return "script";
      if (LooksLikeText_(bytes))
         return "text";
      return "binary";
   }

   String
   ContentPolicy::AttachmentTypes(std::shared_ptr<MessageData> data)
   {
      String types;
      if (!data)
         return types;

      std::shared_ptr<Attachments> attachments = data->GetAttachments();
      if (!attachments)
         return types;

      for (size_t i = 0; i < attachments->GetCount(); i++)
      {
         std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
         if (!attachment)
            continue;

         AnsiString bytes;
         if (!attachment->GetContent(bytes))
            bytes = "";

         if (!types.IsEmpty())
            types += _T(";");
         types += String(TypeOfBytes(bytes));
      }

      return types;
   }

   // ---------------------------------------------------------------------
   // The external recipients.
   // ---------------------------------------------------------------------

   String
   ContentPolicy::ExternalRecipients(std::shared_ptr<Message> message)
   {
      // A recipient this server does not host: not resolved to a local name
      // at RCPT, not a local account, not in a domain this server has, and
      // not reached through a route that treats its recipients as local. The
      // first flag is set when the message is accepted and not kept in the
      // database, so a message on its second delivery attempt is judged by
      // the last three, which are - the route question is the one
      // RecipientParser asks at RCPT, asked again here for that reason.
      String external;
      if (!message)
         return external;

      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return external;

      // owned by this call: the recipients of the message its caller holds - a session's, a delivery's or a task's own
      for (std::shared_ptr<MessageRecipient> recipient : recipients->GetVector())
      {
         if (!recipient)
            continue;
         if (recipient->GetIsLocalName() || recipient->GetLocalAccountID() > 0)
            continue;

         const String address = recipient->GetAddress();
         if (address.IsEmpty())
            continue;

         const String recipientDomain = StringParser::ExtractDomain(address);
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(recipientDomain);
         if (domain)
            continue;

         std::shared_ptr<Route> route = Configuration::Instance()->GetSMTPConfiguration()->GetRoutes()->GetItemByNameWithWildcardMatch(recipientDomain);
         if (route && route->GetTreatRecipientAsLocalDomain() && (route->ToAllAddresses() || route->GetAddresses()->GetItemByName(address)))
            continue;

         if (!external.IsEmpty())
            external += _T(";");
         external += address;
      }

      return external;
   }

   // ---------------------------------------------------------------------
   // The tester.
   // ---------------------------------------------------------------------

   void
   ContentPolicyTester::Test()
   {
      TestLuhn_();
      TestIban_();
      TestDetectors_();
      TestTypes_();
   }

   void
   ContentPolicyTester::TestLuhn_()
   {
      if (!ContentPolicy::LuhnValid("4111111111111111"))
         throw std::logic_error("ContentPolicy::LuhnValid rejected the published Visa test number.");
      if (!ContentPolicy::LuhnValid("5500000000000004"))
         throw std::logic_error("ContentPolicy::LuhnValid rejected the published Mastercard test number.");
      if (!ContentPolicy::LuhnValid("378282246310005"))
         throw std::logic_error("ContentPolicy::LuhnValid rejected a fifteen-digit number that passes.");
      if (ContentPolicy::LuhnValid("4111111111111112"))
         throw std::logic_error("ContentPolicy::LuhnValid accepted a number one digit off.");
      if (ContentPolicy::LuhnValid("41111111111111a1"))
         throw std::logic_error("ContentPolicy::LuhnValid accepted a letter.");
      if (ContentPolicy::LuhnValid(""))
         throw std::logic_error("ContentPolicy::LuhnValid accepted nothing.");
   }

   void
   ContentPolicyTester::TestIban_()
   {
      if (!ContentPolicy::IbanValid("GB82WEST12345698765432"))
         throw std::logic_error("ContentPolicy::IbanValid rejected the registry's example.");
      if (!ContentPolicy::IbanValid("DE89370400440532013000"))
         throw std::logic_error("ContentPolicy::IbanValid rejected a German example.");
      if (ContentPolicy::IbanValid("GB82WEST12345698765433"))
         throw std::logic_error("ContentPolicy::IbanValid accepted a number one digit off.");
      if (ContentPolicy::IbanValid("GB82WEST1234569876543"))
         throw std::logic_error("ContentPolicy::IbanValid accepted a number one character short.");
      if (ContentPolicy::IbanValid("ZZ82WEST12345698765432"))
         throw std::logic_error("ContentPolicy::IbanValid accepted a country the registry has not.");
      if (ContentPolicy::IbanLength('g', 'b') != 22 || ContentPolicy::IbanLength('N', 'O') != 15 || ContentPolicy::IbanLength('Z', 'Z') != 0)
         throw std::logic_error("ContentPolicy::IbanLength does not read the registry.");
   }

   void
   ContentPolicyTester::TestDetectors_()
   {
      if (!ContentPolicy::ContainsCardNumber(_T("Pay with 4111 1111 1111 1111 please")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card written in fours.");
      if (!ContentPolicy::ContainsCardNumber(_T("card: 4111-1111-1111-1111.")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card written with hyphens.");
      if (!ContentPolicy::ContainsCardNumber(_T("card:4111111111111111")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card written without separators.");
      if (ContentPolicy::ContainsCardNumber(_T("Call +44 20 7946 0958 or 020 7946 0958")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber saw a card in a phone number.");
      if (ContentPolicy::ContainsCardNumber(_T("Order 0000 0000 0000 0000 confirmed")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber saw a card in a run of one digit.");
      if (ContentPolicy::ContainsCardNumber(_T("Hash 41111111111111114111111111111111")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber saw a card inside a longer run.");
      if (ContentPolicy::ContainsCardNumber(_T("Ref 4111 1111 1111 1112")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber saw a card in a number that fails Luhn.");
      if (!ContentPolicy::ContainsCardNumber(_T("Card 4111 1111 1111 1111 123, expires 12/28")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card followed by its security code.");
      if (!ContentPolicy::ContainsCardNumber(_T("Card 4111-1111-1111-1111-123")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card followed by its security code with hyphens.");
      if (ContentPolicy::ContainsCardNumber(_T("Ref 4111111111111111123")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber tried the prefixes of a solid run.");
      if (ContentPolicy::ContainsCardNumber(_T("Card 4111 1111 1111 1111 1234")))
         throw std::logic_error("ContentPolicy::ContainsCardNumber searched inside a run longer than 19 digits.");

      if (!ContentPolicy::ContainsIban(_T("Send it to GB82 WEST 1234 5698 7654 32 by Friday.")))
         throw std::logic_error("ContentPolicy::ContainsIban missed an IBAN printed in fours.");
      if (!ContentPolicy::ContainsIban(_T("IBAN:DE89370400440532013000")))
         throw std::logic_error("ContentPolicy::ContainsIban missed an IBAN written solid.");
      if (ContentPolicy::ContainsIban(_T("Part GB82WEST12345698765432X ships Monday")))
         throw std::logic_error("ContentPolicy::ContainsIban saw an IBAN inside a longer word.");
      if (ContentPolicy::ContainsIban(_T("The PO12 3456 7890 1234 5678 90 order")))
         throw std::logic_error("ContentPolicy::ContainsIban saw an IBAN in a code of an unknown country.");
      if (ContentPolicy::ContainsIban(_T("GB82 WEST 1234 5698 7654 33")))
         throw std::logic_error("ContentPolicy::ContainsIban accepted an IBAN with a wrong check.");

      if (!ContentPolicy::MatchesAccountNumberPattern(_T("Account 12-34-56 78901234 today"), _T("\\b\\d{2}-\\d{2}-\\d{2} \\d{8}\\b")))
         throw std::logic_error("ContentPolicy::MatchesAccountNumberPattern did not search the text.");
      if (ContentPolicy::MatchesAccountNumberPattern(_T("Account 12-34-56 78901234 today"), _T("")))
         throw std::logic_error("ContentPolicy::MatchesAccountNumberPattern matched with no pattern.");
      // The compile question on its own, and not MatchesAccountNumberPattern
      // with a pattern that does not compile: that path reports HM6821 and
      // rests the pattern, and a start-up test must not write an error line.
      if (ContentPolicy::PatternCompiles(_T("(")))
         throw std::logic_error("ContentPolicy::PatternCompiles accepted a pattern that does not compile.");
      if (!ContentPolicy::PatternCompiles(_T("\\bACC-\\d{6}\\b")) || !ContentPolicy::PatternCompiles(_T("")))
         throw std::logic_error("ContentPolicy::PatternCompiles refused a pattern that compiles.");

      const String all = ContentPolicy::Detectors(_T("4111 1111 1111 1111 and GB82 WEST 1234 5698 7654 32 and ACC-777"), _T("ACC-\\d{3}"));
      if (all != _T("card_number;iban;account_number"))
         throw std::logic_error("ContentPolicy::Detectors did not name all three in order.");
      if (!ContentPolicy::Detectors(_T("Lunch tomorrow?"), _T("ACC-\\d{3}")).IsEmpty())
         throw std::logic_error("ContentPolicy::Detectors found something in nothing.");

      const String stripped = ContentPolicy::StripHtml(_T("<table><tr><td>4111</td><td>1111</td><td>1111</td><td>1111</td></tr></table>"));
      if (!ContentPolicy::ContainsCardNumber(stripped))
         throw std::logic_error("ContentPolicy::StripHtml did not leave a card in a table findable.");
      if (stripped.Find(_T("  ")) >= 0)
         throw std::logic_error("ContentPolicy::StripHtml left two spaces where two tags met.");
      // Every tag and every newline between two words collapses to the one
      // space that keeps them apart, so a paragraph break neither runs two
      // words together nor leaves a gap the card scan would trip on.
      if (!ContentPolicy::StripHtml(_T("<p>one</p>\r\n<p>two</p>")).Contains(_T("one two")))
         throw std::logic_error("ContentPolicy::StripHtml did not put one space between two paragraphs.");
   }

   void
   ContentPolicyTester::TestTypes_()
   {
      if (ContentPolicy::TypeOfBytes(AnsiString("MZ\x90\x00\x03", 5)) != "executable")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see an executable.");
      if (ContentPolicy::TypeOfBytes("%PDF-1.7\n%") != "pdf")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see a PDF.");
      if (ContentPolicy::TypeOfBytes("Dear all,\r\nthe minutes are attached.\r\n") != "text")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see text.");
      if (ContentPolicy::TypeOfBytes("") != "empty")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see an empty file.");
      if (ContentPolicy::TypeOfBytes("#!/bin/sh\nrm -rf /\n") != "script")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see a script.");
      if (ContentPolicy::TypeOfBytes(AnsiString("\x00\x01\x02\x03\x04\x05", 6)) != "binary")
         throw std::logic_error("ContentPolicy::TypeOfBytes called bytes text.");

      // A zip whose first entry is [Content_Types].xml is an Office document;
      // one whose first entry is META-INF/ is a jar; another is a zip.
      AnsiString zip("PK\x03\x04", 4);
      zip += AnsiString(std::string(22, '\0').c_str(), 22);
      AnsiString office = zip;
      office += AnsiString("\x13\x00", 2);
      office += AnsiString("\x00\x00", 2);
      office += "[Content_Types].xml";
      if (ContentPolicy::TypeOfBytes(office) != "office_xml")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see an Office document in a zip.");
      AnsiString jar = zip;
      jar += AnsiString("\x14\x00", 2);
      jar += AnsiString("\x00\x00", 2);
      jar += "META-INF/MANIFEST.MF";
      if (ContentPolicy::TypeOfBytes(jar) != "jar")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see a jar.");
      AnsiString plain = zip;
      plain += AnsiString("\x05\x00", 2);
      plain += AnsiString("\x00\x00", 2);
      plain += "a.txt";
      if (ContentPolicy::TypeOfBytes(plain) != "zip")
         throw std::logic_error("ContentPolicy::TypeOfBytes did not see a zip.");

      // A UTF-16 text attachment, Excel's "Unicode Text": the mark says what
      // it is, and the text is read through the mark either way round. Without
      // a mark the bytes are not decoded as UTF-16.
      const char *card = "Card 4111 1111 1111 1111 today";
      AnsiString littleEndian("\xff\xfe", 2);
      AnsiString bigEndian("\xfe\xff", 2);
      for (const char *p = card; *p; p++)
      {
         littleEndian += AnsiString(p, 1);
         littleEndian += AnsiString("\x00", 1);
         bigEndian += AnsiString("\x00", 1);
         bigEndian += AnsiString(p, 1);
      }
      String decoded;
      if (!ContentPolicy::Utf16Text(littleEndian, decoded) || decoded != String(card))
         throw std::logic_error("ContentPolicy::Utf16Text did not read a little-endian file through its mark.");
      if (!ContentPolicy::ContainsCardNumber(decoded))
         throw std::logic_error("ContentPolicy::ContainsCardNumber missed a card in a decoded UTF-16 file.");
      String decodedBig;
      if (!ContentPolicy::Utf16Text(bigEndian, decodedBig) || decodedBig != String(card))
         throw std::logic_error("ContentPolicy::Utf16Text did not read a big-endian file through its mark.");
      if (ContentPolicy::TypeOfBytes(littleEndian) != "text" || ContentPolicy::TypeOfBytes(bigEndian) != "text")
         throw std::logic_error("ContentPolicy::TypeOfBytes called a marked UTF-16 file binary.");
      String none;
      if (ContentPolicy::Utf16Text(AnsiString(card), none))
         throw std::logic_error("ContentPolicy::Utf16Text decoded a file with no mark.");
   }
}
