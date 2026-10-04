// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "InputCostTester.h"

#include "ICalendar.h"
#include "Imip.h"
#include "JsonDocument.h"
#include "Parsing/StringParser.h"
#include "../Mime/Mime.h"
#include "../Mime/MimeChar.h"
#include "../Mime/MimeCode.h"
#include "../AntiSpam/DKIM/Canonicalization.h"
#include "../Sieve/SieveEvaluator.h"
#include "../../IMAP/FolderListCreator.h"
#include "../../SMTP/SPF/SPFAddress.h"
#include "../../SMTP/SPF/SPFMacroExpander.h"
#include "../../SMTP/SPF/SPFTestLookup.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // xorshift64*: the same cases on every run and every platform.
      struct Random_
      {
         unsigned long long state;

         explicit Random_(unsigned long long seed) : state(seed) {}

         unsigned long long Next()
         {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            return state * 2685821657736338717ULL;
         }

         size_t Below(size_t n) { return n == 0 ? 0 : (size_t) (Next() % n); }

         std::string Text(const char *alphabet, size_t alphabetSize, size_t maxLength)
         {
            std::string text;
            const size_t length = Below(maxLength + 1);
            for (size_t i = 0; i < length; i++)
               text += alphabet[Below(alphabetSize)];
            return text;
         }
      };

      void Fail_(const std::string &what)
      {
         throw std::logic_error(what);
      }

      // Milliseconds f takes; a failure if over the bound.
      void Within_(const char *what, double boundMilliseconds, const std::function<void()> &f)
      {
         const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         f();
         const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
         const double bound = HM_TEST_TIME_BOUND(boundMilliseconds);
         if (milliseconds > bound)
            Fail_(std::string(what) + " took " + std::to_string((long long) milliseconds) + " ms, over the " + std::to_string((long long) bound) + " ms a single pass is held to");
      }

      std::string Repeat_(const std::string &unit, size_t times)
      {
         std::string text;
         text.reserve(unit.size() * times);
         for (size_t i = 0; i < times; i++)
            text += unit;
         return text;
      }

      // ---------------------------------------------------------------------------
      // 6.3.5's code, kept to hold the new code to what it made.
      // ---------------------------------------------------------------------------

      // MimeEncodedWord::Decode, as 6.3.5 had it.
      void OldDecodeEncodedWords_(const std::string &input, AnsiString &output, std::string &charset)
      {
         charset.clear();
         const char *start = input.data();
         const char *pbData = start;
         const char *pbEnd = pbData + input.size();

         while (pbData < pbEnd)
         {
            const char *pszHeaderEnd = pbData;
            const char *pszCodeEnd = pbEnd;
            int nCoding = 0, nCodeLen = (int) (pbEnd - pbData);
            if (pbEnd - pbData >= 2 && pbData[0] == '=' && pbData[1] == '?')
            {
               const size_t charsetSearchLength = static_cast<size_t>(pbEnd - (pbData + 2));
               pszHeaderEnd = static_cast<const char *>(::memchr(pbData + 2, '?', charsetSearchLength));

               if (pszHeaderEnd != NULL && pszHeaderEnd + 3 < pbEnd && pszHeaderEnd[2] == '?')
               {
                  nCoding = tolower(pszHeaderEnd[1]);
                  pszHeaderEnd += 3;

                  int iDataLength = (int) (pbEnd - pszHeaderEnd);

                  pszCodeEnd = StringParser::Search(pszHeaderEnd, iDataLength, "?=");
                  if (!pszCodeEnd || pszCodeEnd >= pbEnd)
                     pszCodeEnd = pbEnd;
                  nCodeLen = (int) (pszCodeEnd - pszHeaderEnd);
                  pszCodeEnd += 2;
                  if (charset.empty())
                     charset.assign(pbData + 2, pszHeaderEnd - pbData - 5);
               }
            }

            if (nCoding == 'b')
            {
               MimeCodeBase64 base64;
               base64.SetInput(pszHeaderEnd, nCodeLen, false);
               base64.GetOutput(output);
            }
            else if (nCoding == 'q')
            {
               MimeCodeQ qp;
               qp.SetInput(pszHeaderEnd, nCodeLen, false);
               qp.GetOutput(output);
            }
            else
            {
               int iDataLength = (int) (pbEnd - pbData);

               pszCodeEnd = StringParser::Search(pbData + 1, iDataLength - 1, "=?");
               if (!pszCodeEnd || pszCodeEnd >= pbEnd)
                  pszCodeEnd = pbEnd;
               else if (pbData > start)
               {
                  const char *pszSpace = pbData;
                  while (CMimeChar::IsSpace((unsigned char) *pszSpace))
                     pszSpace++;
                  if (pszSpace == pszCodeEnd)
                     pbData = pszCodeEnd;
               }

               int nDecoded = (int) (pszCodeEnd - pbData);
               output.append((char *) pbData, nDecoded);
            }

            pbData = pszCodeEnd;
         }
      }

      // MessageData::DeleteField, as 6.3.5 had it.
      void OldDeleteFields_(MimeHeader &header, const AnsiString &name)
      {
         MimeField *pField = header.GetField(name);
         while (pField)
         {
            header.DeleteField(pField);
            pField = header.GetField(name);
         }
      }

      // SimpleCanonicalization::CanonicalizeHeader, as 6.3.5 had it, without
      // the signature field (which is unchanged).
      AnsiString OldSimpleHeader_(const AnsiString &header, const std::vector<AnsiString> &fieldsToInclude, AnsiString &fieldList)
      {
         std::vector<AnsiString> formattedHeaderLines;

         AnsiString result;
         std::vector<AnsiString> headerLines = StringParser::SplitString(header, "\r\n");

         AnsiString foldedLines;
         for (size_t i = headerLines.size(); i > 0; i--)
         {
            AnsiString line = headerLines[i - 1];

            if (line.StartsWith(" ") || line.StartsWith("\t"))
               foldedLines = line + "\r\n" + foldedLines;
            else
            {
               int colonPos = line.Find(":");
               if (colonPos < 0)
               {
                  foldedLines = "";
                  continue;
               }

               AnsiString entireHeaderField = line + "\r\n" + foldedLines;
               formattedHeaderLines.push_back(entireHeaderField);
               foldedLines = "";
            }
         }

         for (AnsiString fieldToInclude : fieldsToInclude)
         {
            fieldToInclude.Trim();

            auto iter = formattedHeaderLines.begin();
            auto iterEnd = formattedHeaderLines.end();

            for (; iter != iterEnd; iter++)
            {
               AnsiString headerLine = (*iter);

               int colonPos = headerLine.Find(":");
               AnsiString headerName = headerLine.Mid(0, colonPos);

               if (headerName.CompareNoCase(fieldToInclude) == 0)
               {
                  result += headerLine;

                  if (!fieldList.IsEmpty())
                     fieldList += ":";

                  fieldList += headerName;

                  formattedHeaderLines.erase(iter);
                  break;
               }
            }
         }

         return result;
      }

      // RelaxedCanonicalization::CanonicalizeHeader, as 6.3.5 had it, without
      // the signature field.
      AnsiString OldRelaxedHeader_(const AnsiString &header, const std::vector<AnsiString> &fieldsToInclude, AnsiString &fieldList)
      {
         RelaxedCanonicalization relaxed;

         MimeHeader mimeHeader;
         mimeHeader.Load(header, header.GetLength(), true);

         std::vector<MimeField> fields = mimeHeader.Fields();

         String result;
         for (AnsiString headerField : fieldsToInclude)
         {
            headerField.Trim();

            String value;
            for (size_t i = fields.size(); i > 0; i--)
            {
               size_t fieldIndex = i - 1;
               MimeField field = fields[fieldIndex];

               if (headerField.CompareNoCase(field.GetName()) == 0)
               {
                  fields.erase(fields.begin() + fieldIndex);

                  value = field.GetValue();
                  if (value.GetLength() == 0)
                     value += "\r\n";
                  break;
               }
            }

            if (value.GetLength() > 0)
            {
               String relaxedFieldValue;
               String relaxedHeaderName = relaxed.CanonicalizeHeaderName(headerField);
               relaxedFieldValue = relaxed.CanonicalizeHeaderValue(value);

               result += relaxedHeaderName + ":" + relaxedFieldValue + "\r\n";

               if (!fieldList.IsEmpty())
                  fieldList += ":";

               fieldList += headerField;
            }
         }

         return result;
      }

      // SieveEvaluator::WildcardMatchWithCaptures_, as 6.3.5 had it.
      bool OldSieveMatches_(const String &pattern, const String &value, bool caseSensitive, std::vector<String> &captures)
      {
         String p = pattern;
         String v = value;

         if (!caseSensitive)
         {
            p.ToLower();
            v.ToLower();
         }

         std::vector<std::pair<int, int>> spans;

         std::function<bool(int, int)> matchFrom = [&](int pi, int vi) -> bool
         {
            while (pi < p.GetLength())
            {
               wchar_t pc = p[pi];

               if (pc == L'\\' && pi + 1 < p.GetLength())
               {
                  if (vi >= v.GetLength() || v[vi] != p[pi + 1])
                     return false;
                  pi += 2;
                  vi += 1;
                  continue;
               }

               if (pc == L'?')
               {
                  if (vi >= v.GetLength())
                     return false;
                  spans.push_back(std::make_pair(vi, vi + 1));
                  if (matchFrom(pi + 1, vi + 1))
                     return true;
                  spans.pop_back();
                  return false;
               }

               if (pc == L'*')
               {
                  for (int take = v.GetLength() - vi; take >= 0; take--)
                  {
                     spans.push_back(std::make_pair(vi, vi + take));
                     if (matchFrom(pi + 1, vi + take))
                        return true;
                     spans.pop_back();
                  }
                  return false;
               }

               if (vi >= v.GetLength() || v[vi] != pc)
                  return false;

               pi++;
               vi++;
            }

            return vi == v.GetLength();
         };

         spans.clear();
         if (!matchFrom(0, 0))
            return false;

         captures.clear();
         captures.push_back(value);
         for (const std::pair<int, int> &span : spans)
            captures.push_back(value.Mid(span.first, span.second - span.first));

         return true;
      }

      // SPFMacroExpander's cut, as 6.3.5 had it.
      AnsiString OldSpfCut_(AnsiString result)
      {
         if (!result.IsEmpty() && result[result.GetLength() - 1] == '.')
            result = result.Left(result.GetLength() - 1);

         while (result.GetLength() > 253)
         {
            int dot = result.Find('.');

            if (dot < 0)
               break;

            result = result.Mid(dot + 1);
         }

         return result;
      }

      // MimeField::FindParameter and RemoveParameter, as 6.3.5 had them, over
      // the value itself.
      bool OldFindParameter_(const std::string &value_, const char *pszAttr, int &nPos, int &nSize)
      {
         const char *pszParms = ::strchr(value_.data(), ';');

         int nAttrSize = (int) ::strlen(pszAttr);

         while (pszParms != NULL)
         {
            while (CMimeChar::IsSpace((unsigned char) *pszParms) || *pszParms == ';')
               pszParms++;

            const char *pszName = pszParms;
            pszParms = ::strchr(pszParms, '=');
            if (!pszParms)
               break;

            pszParms++;
            while (*pszParms == ' ' || *pszParms == '\t')
               pszParms++;

            int nActualAttrNameSize = (int) (pszParms - pszName - 1);

            if (*pszParms == 0)
               return false;

            const char *pszParmEnd = NULL;
            if (*pszParms == '"')
               pszParmEnd = ::strchr(pszParms + 1, '"');
            if (!pszParmEnd)
            {
               pszParmEnd = pszParms;
               while (*pszParmEnd && *pszParmEnd != ';')
                  pszParmEnd++;
            }
            else
               pszParmEnd++;

            if (!::_memicmp(pszAttr, pszName, nAttrSize) &&
               (CMimeChar::IsSpace((unsigned char) pszName[nActualAttrNameSize]) || pszName[nActualAttrNameSize] == '='))
            {
               nPos = (int) (pszParms - value_.data());
               nSize = (int) (pszParmEnd - pszParms);
               return true;
            }

            pszParms = pszParmEnd;
         }
         return false;
      }

      // False where 6.3.5 never ended: the match's value empty and followed by
      // ';', which it removed nothing of and found again, for ever.
      bool OldRemoveParameter_(std::string &value_, const char *pszAttr)
      {
         int nPos, nSize;
         while (OldFindParameter_(value_, pszAttr, nPos, nSize))
         {
            std::string::size_type segStart = value_.rfind(';', nPos);
            if (segStart == std::string::npos)
               break;

            size_t segEnd = nPos + nSize;
            if (segEnd == segStart)
               return false;

            value_.erase(segStart, segEnd - segStart);
         }

         return true;
      }

      // The value of a field, embedded NULs and all.
      std::string WholeValue_(const MimeField &field)
      {
         const size_t length = (size_t) field.GetLength() - ::strlen(field.GetName()) - 4;
         return std::string(field.GetValue(), length);
      }

      std::string NewRemoveParameter_(const std::string &value, const char *attribute)
      {
         MimeField field;
         const std::string line = "X: " + value + "\r\n";
         field.Load(line.data(), line.size(), false);
         field.RemoveParameter(attribute);
         return WholeValue_(field);
      }

      std::string Describe_(const std::string &text)
      {
         std::string out;
         for (unsigned char c : text)
         {
            if (c >= 32 && c < 127 && c != '\\')
               out += (char) c;
            else
            {
               char escaped[8];
               (void) ::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
               out += escaped;
            }
         }
         return out;
      }
   }

   void
   InputCostTester::Test()
   {
      TestEncodedWords();
      TestFieldDeletion();
      TestDkimHeaderSelection();
      TestSieveMatches();
      TestSpfTruncation();
      TestParameterRemoval();
      TestListLines();
      TestJsonMembers();
      TestCalendarCancel();
   }

   void
   InputCostTester::TestCalendarCancel()
   {
      // A weekly series of a stranger's with 200,000 more dates (3.4 MB),
      // cancelled from its second week on. The dates were cut off the front of
      // the list one at a time, the whole RDATE copied for each. (A rule's
      // parts were cut the same way, but a rule of more parts than RFC 5545
      // names is refused before it is read there.)
      std::string dates;
      for (int i = 0; i < 200000; i++)
         dates += (i ? "," : "") + std::string(i % 2 ? "20261102T090000Z" : "20261201T090000Z");
      const std::string rule = "FREQ=WEEKLY;COUNT=4";

      const AnsiString head = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//InputCostTester//EN\r\n";
      const AnsiString people = "ORGANIZER:mailto:olga@partner.example\r\nATTENDEE;PARTSTAT=ACCEPTED:mailto:guest@example.com\r\n";
      const AnsiString uid = "UID:series-0001@example.com\r\n";
      const AnsiString stored = head + "BEGIN:VEVENT\r\n" + uid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\n"
         "DTEND:20261102T100000Z\r\nRRULE:" + AnsiString(rule.c_str()) + "\r\nRDATE:" + AnsiString(dates.c_str()) + "\r\nSUMMARY:Weekly\r\nSEQUENCE:0\r\n" +
         people + "END:VEVENT\r\nEND:VCALENDAR\r\n";
      const AnsiString cancel = head + "METHOD:CANCEL\r\nBEGIN:VEVENT\r\n" + uid + "RECURRENCE-ID;RANGE=THISANDFUTURE:20261109T090000Z\r\n"
         "DTSTAMP:20260929T080000Z\r\nSEQUENCE:1\r\nSTATUS:CANCELLED\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n";

      ICalComponent tree;
      AnsiString problem;
      if (!ICalendar::Parse(stored, tree, problem))
         Fail_(std::string("The stored series did not parse: ") + problem.c_str());

      ImipMessage message;
      if (!Imip::Read(cancel, message, problem))
         Fail_(std::string("The CANCEL did not parse: ") + problem.c_str());

      Within_("A CANCEL from an occurrence on over an RDATE of 200,000 dates", 2000, [&]()
      {
         size_t refused = 0;
         const auto organiser = [](const AnsiString &address) { return address == "olga@partner.example"; };
         const auto nobody = [](const AnsiString &, const AnsiString &) { return false; };
         if (!Imip::ApplyToStored(tree, message, "guest@example.com", organiser, nobody, nobody, nobody, refused))
            Fail_("The organiser's CANCEL from an occurrence on was not applied");
      });

      const ICalProperty *rdate = tree.children.empty() ? nullptr : tree.children[0].Property("RDATE");
      if (!rdate || rdate->value.GetLength() != 100000 * 17 - 1)
         Fail_("The CANCEL did not keep exactly the 100,000 dates before it");

      // The same over dates in a time zone of the event's own, under the 1 MB
      // a message is read for a calendar within: 20,000 dates, 1,500 overrides
      // and a VTIMEZONE whose observance lists 25,000 more. Each date and each
      // override's RECURRENCE-ID had the zone built afresh, the observance's
      // whole list read each time though only 500 of it are kept.
      std::string zonedDates;
      for (int i = 0; i < 20000; i++)
         zonedDates += (i ? "," : "") + std::string(i % 2 ? "20261102T100000" : "20261201T100000");
      std::string transitions;
      for (int i = 0; i < 25000; i++)
         transitions += (i ? "," : "") + std::string("19800101T000000");
      std::string overrides;
      for (int i = 0; i < 1500; i++)
         overrides += "BEGIN:VEVENT\r\nUID:series-0002@example.com\r\nRECURRENCE-ID;TZID=Test/Zone:20261102T100000\r\n"
            "DTSTAMP:20260928T080000Z\r\nDTSTART;TZID=Test/Zone:20261102T100000\r\nEND:VEVENT\r\n";

      const AnsiString zone = "BEGIN:VTIMEZONE\r\nTZID:Test/Zone\r\n"
         "BEGIN:STANDARD\r\nDTSTART:19701025T030000\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\n"
         "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\nRDATE:" + AnsiString(transitions.c_str()) + "\r\nEND:STANDARD\r\n"
         "BEGIN:DAYLIGHT\r\nDTSTART:19700329T020000\r\nTZOFFSETFROM:+0100\r\nTZOFFSETTO:+0200\r\n"
         "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\nEND:DAYLIGHT\r\nEND:VTIMEZONE\r\n";
      const AnsiString zonedUid = "UID:series-0002@example.com\r\n";
      const AnsiString zonedStored = head + zone + "BEGIN:VEVENT\r\n" + zonedUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\n"
         "DTEND:20261102T100000Z\r\nRRULE:" + AnsiString(rule.c_str()) + "\r\nRDATE;TZID=Test/Zone:" + AnsiString(zonedDates.c_str()) +
         "\r\nSUMMARY:Weekly\r\nSEQUENCE:0\r\n" + people + "END:VEVENT\r\n" + AnsiString(overrides.c_str()) + "END:VCALENDAR\r\n";
      const AnsiString zonedCancel = head + "METHOD:CANCEL\r\nBEGIN:VEVENT\r\n" + zonedUid + "RECURRENCE-ID;RANGE=THISANDFUTURE:20261109T090000Z\r\n"
         "DTSTAMP:20260929T080000Z\r\nSEQUENCE:1\r\nSTATUS:CANCELLED\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n";

      if (zonedStored.GetLength() >= 1024 * 1024)
         Fail_("The zoned series is not under the 1 MB a message is read for a calendar within");

      ICalComponent zonedTree;
      if (!ICalendar::Parse(zonedStored, zonedTree, problem))
         Fail_(std::string("The zoned series did not parse: ") + problem.c_str());

      ImipMessage zonedMessage;
      if (!Imip::Read(zonedCancel, zonedMessage, problem))
         Fail_(std::string("The zoned CANCEL did not parse: ") + problem.c_str());

      Within_("A CANCEL from an occurrence on over 20,000 dates and 1,500 overrides in a zone of 25,000 transitions", 2000, [&]()
      {
         size_t refused = 0;
         const auto organiser = [](const AnsiString &address) { return address == "olga@partner.example"; };
         const auto nobody = [](const AnsiString &, const AnsiString &) { return false; };
         if (!Imip::ApplyToStored(zonedTree, zonedMessage, "guest@example.com", organiser, nobody, nobody, nobody, refused))
            Fail_("The organiser's CANCEL from an occurrence on was not applied to the zoned series");
      });

      const ICalComponent *zonedMaster = nullptr;
      size_t zonedOverrides = 0;
      for (const ICalComponent &child : zonedTree.children)
      {
         if (child.name != "VEVENT")
            continue;
         if (child.Property("RECURRENCE-ID"))
            zonedOverrides++;
         else
            zonedMaster = &child;
      }

      const ICalProperty *zonedRdate = zonedMaster ? zonedMaster->Property("RDATE") : nullptr;
      if (!zonedRdate || zonedRdate->value.GetLength() != 10000 * 16 - 1 || zonedOverrides != 1500)
         Fail_("The zoned CANCEL did not keep exactly the 10,000 dates and the 1,500 overrides before it");

      // A CANCEL of 1,990 single occurrences of a daily series whose DTSTART
      // is in a zone of 60,000 transitions (the series 0.9 MB, the CANCEL
      // 0.5 MB): the DTSTART was placed again, the zone built afresh and its
      // whole list read, for each EXDATE written.
      std::string manyTransitions;
      for (int i = 0; i < 60000; i++)
         manyTransitions += (i ? "," : "") + std::string("19800101T000000");
      const AnsiString flatZone = "BEGIN:VTIMEZONE\r\nTZID:Flat/Zone\r\n"
         "BEGIN:STANDARD\r\nDTSTART:19701025T030000\r\nTZOFFSETFROM:+0100\r\nTZOFFSETTO:+0100\r\n"
         "RRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\nRDATE:" + AnsiString(manyTransitions.c_str()) + "\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n";
      const AnsiString dailyUid = "UID:series-0003@example.com\r\n";
      const AnsiString daily = head + flatZone + "BEGIN:VEVENT\r\n" + dailyUid + "DTSTAMP:20260928T080000Z\r\n"
         "DTSTART;TZID=Flat/Zone:20261102T100000\r\nDTEND;TZID=Flat/Zone:20261102T110000\r\nRRULE:FREQ=DAILY;COUNT=4000\r\n"
         "SUMMARY:Daily\r\nSEQUENCE:0\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n";

      __int64 firstDay = 0;
      if (!ICalendar::ParseUtc("20261102T090000Z", firstDay))
         Fail_("The first day of the daily series did not read");
      AnsiString singles;
      for (int i = 0; i < 1990; i++)
         singles += AnsiString("BEGIN:VEVENT\r\n") + dailyUid + "RECURRENCE-ID:" + ICalendar::FormatUtc(firstDay + (__int64) (2 * i + 1) * 86400) +
            "\r\nDTSTAMP:20260929T080000Z\r\nSEQUENCE:1\r\nSTATUS:CANCELLED\r\n" + people + "END:VEVENT\r\n";
      const AnsiString singlesCancel = head + "METHOD:CANCEL\r\n" + singles + "END:VCALENDAR\r\n";

      if (daily.GetLength() >= 1024 * 1024 || singlesCancel.GetLength() >= 1024 * 1024)
         Fail_("The daily series or its CANCEL is not under the 1 MB a message is read for a calendar within");

      ICalComponent dailyTree;
      if (!ICalendar::Parse(daily, dailyTree, problem))
         Fail_(std::string("The daily series did not parse: ") + problem.c_str());

      ImipMessage singlesMessage;
      if (!Imip::Read(singlesCancel, singlesMessage, problem))
         Fail_(std::string("The CANCEL of 1,990 occurrences did not parse: ") + problem.c_str());

      Within_("A CANCEL of 1,990 occurrences of a series whose DTSTART is in a zone of 60,000 transitions", 2000, [&]()
      {
         size_t refused = 0;
         const auto organiser = [](const AnsiString &address) { return address == "olga@partner.example"; };
         const auto nobody = [](const AnsiString &, const AnsiString &) { return false; };
         if (!Imip::ApplyToStored(dailyTree, singlesMessage, "guest@example.com", organiser, nobody, nobody, nobody, refused))
            Fail_("The organiser's CANCEL of 1,990 occurrences was not applied");
      });

      size_t exdates = 0;
      for (const ICalComponent &child : dailyTree.children)
      {
         if (child.name != "VEVENT")
            continue;
         for (const ICalProperty &property : child.properties)
         {
            if (property.name == "EXDATE")
               exdates++;
         }
      }
      if (exdates != 1990)
         Fail_("The CANCEL of 1,990 occurrences did not leave each out with an EXDATE: " + std::to_string(exdates));
   }

   void
   InputCostTester::TestEncodedWords()
   {
      static const char alphabet[] = "=?=?abqBQxX_ 3D41\t\r\n\x80";
      Random_ random(0x5eed0001ULL);

      for (int number = 0; number < 200000; number++)
      {
         const std::string input = random.Text(alphabet, sizeof(alphabet) - 1, 40);

         AnsiString expected;
         std::string expectedCharset;
         OldDecodeEncodedWords_(input, expected, expectedCharset);

         MimeEncodedWord coder;
         coder.SetInput(input.data(), input.size(), false);
         AnsiString actual;
         coder.GetOutput(actual);

         if (actual != expected || std::string(coder.GetCharset()) != expectedCharset)
            Fail_("An encoded word was decoded other than 6.3.5 decoded it: " + Describe_(input) + " gave " + Describe_(actual) + ", not " + Describe_(expected));
      }

      // 160,000 words of an encoding other than B or Q, and no "?=" anywhere:
      // 6.3.5 searched each to the end, 101 s in the POSIX build.
      const std::string value = Repeat_("=?a?x? ", 160000);
      Within_("Decoding 160,000 encoded words of an unknown encoding", 2000, [&]()
      {
         String decoded = MimeHeader::GetUnicodeFieldValue("Subject", value.c_str());
         if (decoded.GetLength() < (int) value.size() / 2)
            Fail_("A value of unknown encoded words was not kept as text");
      });
   }

   void
   InputCostTester::TestFieldDeletion()
   {
      static const char *names[] = { "A", "a", "X-Spam", "x-spam", "X-Spam2", "B" };
      Random_ random(0x5eed0002ULL);

      for (int number = 0; number < 20000; number++)
      {
         std::string text;
         const size_t count = random.Below(12);
         for (size_t i = 0; i < count; i++)
            text += std::string(names[random.Below(6)]) + ": " + std::to_string(i) + "\r\n";
         text += "\r\n";

         const char *doomed = names[random.Below(6)];

         MimeHeader expected;
         expected.Load(text.data(), text.size(), true);
         OldDeleteFields_(expected, doomed);

         MimeHeader actual;
         actual.Load(text.data(), text.size(), true);
         actual.DeleteFields(doomed);

         if (expected.Store() != actual.Store())
            Fail_("DeleteFields left other fields than 6.3.5's loop left: " + Describe_(text));

         // RemoveFieldsAt against erasing back to front, as Sieve's deleteheader did.
         MimeHeader erased;
         erased.Load(text.data(), text.size(), true);
         MimeHeader removed;
         removed.Load(text.data(), text.size(), true);

         std::vector<size_t> positions;
         for (size_t i = 0; i < erased.Fields().size(); i++)
         {
            if (random.Below(2) == 0)
               positions.push_back(i);
         }

         for (auto it = positions.rbegin(); it != positions.rend(); ++it)
            erased.Fields().erase(erased.Fields().begin() + *it);
         MimeHeader::RemoveFieldsAt(removed.Fields(), positions);

         if (erased.Store() != removed.Store())
            Fail_("RemoveFieldsAt left other fields than erasing them one by one: " + Describe_(text));
      }

      // 80,000 fields of one name: 18.6 s for 6.3.5's loop.
      const std::string many = "Subject: s\r\n" + Repeat_("X-Spam: yes\r\n", 80000) + "\r\n";
      Within_("Deleting 80,000 fields of one name", 2000, [&]()
      {
         MimeHeader header;
         header.Load(many.data(), many.size(), true);
         if (header.DeleteFields("X-Spam") != 80000 || header.GetFieldCount() != 1)
            Fail_("DeleteFields did not delete the 80,000 fields of a name");
      });

      // 80,000 of 160,000 fields, every other one: 18.8 s erased back to front.
      const std::string pairs = Repeat_("X-Spam: yes\r\nX-Keep: no\r\n", 80000) + "\r\n";
      Within_("Removing every other one of 160,000 fields", 2000, [&]()
      {
         MimeHeader header;
         header.Load(pairs.data(), pairs.size(), true);
         std::vector<size_t> positions;
         for (size_t i = 0; i < header.Fields().size(); i += 2)
            positions.push_back(i);
         MimeHeader::RemoveFieldsAt(header.Fields(), positions);
         if (header.GetFieldCount() != 80000 || AnsiString(header.Fields()[0].GetName()) != "X-Keep")
            Fail_("RemoveFieldsAt did not remove every other field");
      });
   }

   void
   InputCostTester::TestDkimHeaderSelection()
   {
      static const char *lines[] = { "From: a", "from:b", "FROM :c", "To: d", "X-A: e", "x-a:f", " folded", "\tfolded2", "no colon", "", "Subject:", ": empty name", "X-\xc3\xa9: g" };
      static const char *names[] = { "from", "FROM", " to ", "x-a", "X-A", "subject", "", "nope", "x-\xc3\xa9", "X-\xc3\x89" };
      Random_ random(0x5eed0003ULL);

      SimpleCanonicalization simple;
      RelaxedCanonicalization relaxed;
      const std::pair<AnsiString, AnsiString> noSignature;

      for (int number = 0; number < 50000; number++)
      {
         std::string text;
         const size_t count = random.Below(10);
         for (size_t i = 0; i < count; i++)
            text += std::string(lines[random.Below(sizeof(lines) / sizeof(lines[0]))]) + "\r\n";
         if (random.Below(2) == 0)
            text += "\r\n";

         std::vector<AnsiString> include;
         const size_t includeCount = random.Below(8);
         for (size_t i = 0; i < includeCount; i++)
            include.push_back(names[random.Below(sizeof(names) / sizeof(names[0]))]);

         const AnsiString header = text.c_str();

         AnsiString expectedList, actualList;
         const AnsiString expected = OldSimpleHeader_(header, include, expectedList);
         const AnsiString actual = simple.CanonicalizeHeader(header, noSignature, include, actualList);
         if (expected != actual || expectedList != actualList)
            Fail_("The simple header canonicalisation chose other fields than 6.3.5's: " + Describe_(text));

         AnsiString expectedRelaxedList, actualRelaxedList;
         const AnsiString expectedRelaxed = OldRelaxedHeader_(header, include, expectedRelaxedList);
         const AnsiString actualRelaxed = relaxed.CanonicalizeHeader(header, noSignature, include, actualRelaxedList);
         if (expectedRelaxed != actualRelaxed || expectedRelaxedList != actualRelaxedList)
            Fail_("The relaxed header canonicalisation chose other fields than 6.3.5's: " + Describe_(text));
      }

      // One field of 640,000 continuation lines: 6.3.5 put each in front of
      // the ones after it (0.9 s for 160,000, a square).
      const std::string foldedText = "X: a\r\n" + Repeat_(" b\r\n", 640000) + "From: f\r\n\r\n";
      const AnsiString folded = foldedText.c_str();
      Within_("The simple canonicalisation of a field of 640,000 lines", 2000, [&]()
      {
         AnsiString list;
         if (simple.CanonicalizeHeader(folded, noSignature, std::vector<AnsiString>{ "x", "from" }, list).GetLength() < 2560000)
            Fail_("A field of 640,000 lines was not kept whole");
      });

      // h= naming 40,000 fields the header lacks, over 40,000 fields: 17.8 s
      // (simple) and 19.3 s (relaxed) for 6.3.5.
      const std::string wideText = Repeat_("X: a\r\n", 40000) + "From: f\r\n\r\n";
      const AnsiString wide = wideText.c_str();
      std::vector<AnsiString> absent(40000, "nope");
      absent.push_back("from");
      Within_("The simple canonicalisation of 40,000 names over 40,000 fields", 2000, [&]()
      {
         AnsiString list;
         simple.CanonicalizeHeader(wide, noSignature, absent, list);
         if (list != "From")
            Fail_("The simple canonicalisation did not find From among 40,000 fields");
      });
      Within_("The relaxed canonicalisation of 40,000 names over 40,000 fields", 2000, [&]()
      {
         AnsiString list;
         relaxed.CanonicalizeHeader(wide, noSignature, absent, list);
         if (list != "from")
            Fail_("The relaxed canonicalisation did not find from among 40,000 fields");
      });
   }

   void
   InputCostTester::TestSieveMatches()
   {
      static const char patternAlphabet[] = "ab*?\\*A";
      static const char valueAlphabet[] = "abA*?\\";
      Random_ random(0x5eed0004ULL);

      for (int number = 0; number < 200000; number++)
      {
         const String pattern = random.Text(patternAlphabet, sizeof(patternAlphabet) - 1, 8).c_str();
         const String value = random.Text(valueAlphabet, sizeof(valueAlphabet) - 1, 10).c_str();
         const bool caseSensitive = random.Below(2) == 0;

         std::vector<String> expectedCaptures, actualCaptures;
         const bool expected = OldSieveMatches_(pattern, value, caseSensitive, expectedCaptures);
         const bool actual = SieveEvaluator::MatchesWithCaptures(pattern, value, caseSensitive, actualCaptures);

         if (expected != actual || (expected && expectedCaptures != actualCaptures))
            Fail_("Sieve's :matches answered other than 6.3.5's for the pattern " + std::string(AnsiString(pattern).c_str()) + " and the value " + std::string(AnsiString(value).c_str()));
      }

      // A subject of "free" 64,000 times under "*free*money*": 1.3 s for a
      // quarter of it in 6.3.5, and four times as long for each doubling.
      const String free = Repeat_("free", 64000).c_str();
      Within_("Sieve's :matches \"*free*money*\" over 256 KB", 2000, [&]()
      {
         std::vector<String> captures;
         if (SieveEvaluator::MatchesWithCaptures(_T("*free*money*"), free, false, captures))
            Fail_("\"*free*money*\" matched a value without money in it");
         if (!SieveEvaluator::MatchesWithCaptures(_T("*free*free*"), free, false, captures) || captures.size() != 4 ||
             captures[1].GetLength() != 255992 || captures[2].GetLength() != 0 || captures[3].GetLength() != 0)
            Fail_("\"*free*free*\" did not take the longest first");
      });

      // "*a*a*a*b" over 800 characters: 44 s in 6.3.5.
      const String as = Repeat_("a", 800).c_str();
      Within_("Sieve's :matches \"*a*a*a*b\" over 800 characters", 2000, [&]()
      {
         std::vector<String> captures;
         if (SieveEvaluator::MatchesWithCaptures(_T("*a*a*a*b"), as, false, captures))
            Fail_("\"*a*a*a*b\" matched a value with no b");
      });
   }

   void
   InputCostTester::TestSpfTruncation()
   {
      static const char *pieces[] = { "ab", ".", "%{s}", "x", "-", "." };
      Random_ random(0x5eed0005ULL);

      std::string localPart;
      while (localPart.size() < 60)
         localPart += "a.";
      localPart += "b";
      const std::string sender = localPart + "@c.example";

      auto lookup = std::make_shared<SPFTestLookup>();
      SPFAddress client;
      SPFAddress::TryParse("192.0.2.3", client);

      for (int number = 0; number < 20000; number++)
      {
         std::string spec, literal;
         const size_t count = 1 + random.Below(40);
         for (size_t i = 0; i < count; i++)
         {
            const std::string piece = pieces[random.Below(sizeof(pieces) / sizeof(pieces[0]))];
            spec += piece;
            literal += piece == "%{s}" ? sender : piece;
         }

         SPFMacroExpander expander(lookup, client, sender.c_str(), "mx.example.org");
         AnsiString actual;
         if (!expander.ExpandDomainSpec(spec.c_str(), "c.example", actual))
            continue;   // not a domain-spec; neither version expands it

         const AnsiString expected = OldSpfCut_(literal.c_str());
         if (expected != actual)
            Fail_("SPF cut a long name other than 6.3.5 cut it: " + spec);
      }

      // 4,000 macros of a 250-character sender: 4.9 s for half as many in 6.3.5.
      std::string longLocal;
      while (longLocal.size() < 240)
         longLocal += "a.";
      longLocal += "a";
      const std::string spec = Repeat_("%{s}", 4000) + ".example";
      Within_("SPF's cut of a domain-spec of 4,000 %{s}", 2000, [&]()
      {
         SPFMacroExpander expander(lookup, client, (longLocal + "@b.example").c_str(), "mx.example.org");
         AnsiString expanded;
         if (!expander.ExpandDomainSpec(spec.c_str(), "b.example", expanded) || expanded.GetLength() > 253)
            Fail_("A domain-spec of 4,000 %{s} was not cut to 253 characters");
      });
   }

   void
   InputCostTester::TestParameterRemoval()
   {
      static const char alphabet[] = ";;==\"  \tnaNAx*0z";
      Random_ random(0x5eed0006ULL);
      int hung = 0;

      // Short values, and longer ones that see many removals each, so that the
      // step made again after each removal meets every kind of scan stopped
      // before, at and after the removal's start.
      for (int number = 0; number < 300000; number++)
      {
         std::string value = "v" + random.Text(alphabet, sizeof(alphabet) - 1, number < 200000 ? 30 : 90);
         if (random.Below(20) == 0 && value.size() > 1)
            value[1 + random.Below(value.size() - 1)] = '\0';

         std::string expected = value;
         const bool ended = OldRemoveParameter_(expected, "na");

         const std::string actual = NewRemoveParameter_(value, "na");
         if (ended)
         {
            if (expected != actual)
               Fail_("RemoveParameter left " + Describe_(actual) + " where 6.3.5 left " + Describe_(expected) + ", of " + Describe_(value));
            continue;
         }

         // Where 6.3.5 never ended, nothing it would remove is left.
         hung++;
         int nPos, nSize;
         if (OldFindParameter_(actual, "na", nPos, nSize) && actual.rfind(';', nPos) != std::string::npos)
            Fail_("RemoveParameter left a parameter it removes in " + Describe_(actual) + ", of " + Describe_(value));
      }

      if (hung == 0)
         Fail_("No generated case had an empty parameter value before a ';', which 6.3.5 never finished removing");

      // The shape the attachment blocker met: 6.3.5 never returned.
      if (NewRemoveParameter_("attachment; filename=a.bat; filename=; size=1", "filename") != "attachment; size=1")
         Fail_("RemoveParameter did not remove an empty filename before a ';'");

      // 400,000 continuations of a filename: 78 ms for 40,000 in 6.3.5, a
      // square.
      std::string many = "attachment";
      for (int i = 0; i < 400000; i++)
         many += "; filename*" + std::to_string(i) + "=a";
      many += "; size=1";
      Within_("Removing 400,000 continuations of a parameter", 2000, [&]()
      {
         if (NewRemoveParameter_(many, "filename") != "attachment; size=1")
            Fail_("RemoveParameter did not remove 400,000 continuations of a filename");
      });

      // One step of the walk made again after each of 200,000 removals, its
      // name 1,000,000 characters long before the first of them: read again in
      // full each time, as the first one-walk version did, a square.
      const std::string longName = "filename" + std::string(1000000, 'a');
      std::string repeated = "attachment; " + longName;
      for (int i = 0; i < 200000; i++)
         repeated += ";x=1";
      repeated += "; size=1";
      Within_("Removing 200,000 parameters after a name of 1,000,000 characters", 2000, [&]()
      {
         if (NewRemoveParameter_(repeated, "filename") != "attachment; " + longName)
            Fail_("RemoveParameter did not remove 200,000 parameters after a long name");
      });
   }

   void
   InputCostTester::TestListLines()
   {
      Random_ random(0x5eed0007ULL);

      for (int number = 0; number < 20000; number++)
      {
         std::unordered_set<std::wstring> seen;
         std::vector<String> seenList;
         std::vector<String> expected, actual;

         for (int round = 0; round < 3; round++)
         {
            String listing;
            const size_t count = random.Below(8);
            for (size_t i = 0; i < count; i++)
            {
               const size_t kind = random.Below(6);
               listing += kind == 0 ? String() : String(_T("* LIST () \".\" \"F")) + String(std::to_string(kind).c_str()) + _T("\"");
               listing += _T("\r\n");
            }

            for (const String &line : StringParser::SplitString(listing, _T("\r\n")))
            {
               if (line.IsEmpty())
                  continue;
               if (std::find(seenList.begin(), seenList.end(), line) != seenList.end())
                  continue;
               seenList.push_back(line);
               expected.push_back(line);
            }

            FolderListCreator::TakeNewLines(listing, seen, actual);
         }

         if (expected != actual)
            Fail_("LIST's lines were taken other than 6.3.5 took them");
      }

      // 100,000 folders: 1.4 s for 40,000 in 6.3.5, a square.
      String listing;
      for (int i = 0; i < 100000; i++)
         listing += String(_T("* LIST (\\HasNoChildren) \".\" \"INBOX.folder")) + String(std::to_string(i).c_str()) + _T("\"\r\n");
      Within_("LIST's lines of 100,000 folders", 2000, [&]()
      {
         std::unordered_set<std::wstring> seen;
         std::vector<String> lines;
         FolderListCreator::TakeNewLines(listing, seen, lines);
         FolderListCreator::TakeNewLines(listing, seen, lines);
         if (lines.size() != 100000)
            Fail_("LIST's lines of 100,000 folders were not each taken once");
      });
   }

   void
   InputCostTester::TestJsonMembers()
   {
      static const char *keys[] = { "a", "b", "c", "A", "\\u0061", "" };
      Random_ random(0x5eed0008ULL);

      for (int number = 0; number < 20000; number++)
      {
         std::string text = "{";
         std::vector<std::string> expectedNames;
         std::vector<int> expectedValues;
         const size_t count = random.Below(8);
         for (size_t i = 0; i < count; i++)
         {
            const std::string key = keys[random.Below(sizeof(keys) / sizeof(keys[0]))];
            const std::string decoded = key == "\\u0061" ? "a" : key;
            if (i > 0)
               text += ",";
            text += "\"" + key + "\":" + std::to_string(i);

            if (std::find(expectedNames.begin(), expectedNames.end(), decoded) == expectedNames.end())
            {
               expectedNames.push_back(decoded);
               expectedValues.push_back((int) i);
            }
         }
         text += "}";

         JsonValue document;
         std::string error;
         if (!JsonValue::Parse(text, document, error) || !document.IsObject())
            Fail_("A JSON object did not parse: " + text);

         const auto &members = document.Members();
         if (members.size() != expectedNames.size())
            Fail_("A JSON object kept other members than the first of each name: " + text);

         for (size_t i = 0; i < members.size(); i++)
         {
            if (members[i].first != expectedNames[i] || members[i].second.AsInt() != expectedValues[i])
               Fail_("A JSON object kept other members than the first of each name: " + text);
         }
      }

      // 256,000 members: 2.4 s for 64,000 in 6.3.5, a square.
      std::string wide = "{";
      for (int i = 0; i < 256000; i++)
         wide += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":" + std::to_string(i);
      wide += "}";
      Within_("A JSON object of 256,000 members", 2000, [&]()
      {
         JsonValue document;
         std::string error;
         if (!JsonValue::Parse(wide, document, error) || document.Members().size() != 256000)
            Fail_("A JSON object of 256,000 members did not parse whole");
      });
   }
}
