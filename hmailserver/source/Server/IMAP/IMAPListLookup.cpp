// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "IMAPListLookup.h"
#include "../Common/Util/Parsing/StringParser.h"

#include <algorithm>
#include <iterator>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   IMAPListLookup::IMAPListLookup()
   {

   }

   IMAPListLookup::~IMAPListLookup()
   {

   }
   bool
   IMAPListLookup::IsItemInList(const std::vector<String> &vecItems, __int64 item, __int64 maxItem)
   {
      for(const String &sCur : vecItems)
      {
         int lColonPos = sCur.Find(_T(":"));

         if (lColonPos >= 0)
         {
            String sFirstPart = sCur.Mid(0, lColonPos);
            String sSecondPart = sCur.Mid(lColonPos + 1);

            // RFC 3501: "*" is the largest value in use - on EITHER side of the
            // colon - and a range is valid in either order, so "*:1" is the whole
            // mailbox and "3:1" is the same set as "1:3".
            __int64 lower = sFirstPart == _T("*") ? maxItem : _ttoi64(sFirstPart);
            __int64 upper = sSecondPart == _T("*") ? maxItem : _ttoi64(sSecondPart);

            if (upper < lower)
               std::swap(lower, upper);

            if (item >= lower && item <= upper)
               return true;
         }
         else
         {
            __int64 foundItem = sCur == _T("*") ? maxItem : _ttoi64(sCur);

            if (foundItem == item)
               return true;
         }
      }

      return false;

   }

   IMAPListLookup::Runs
   IMAPListLookup::ToRuns(const std::vector<String> &vecItems, __int64 maxItem)
   {
      Runs runs;
      runs.reserve(vecItems.size());

      // Each element as the loop above reads it.
      for (const String &sCur : vecItems)
      {
         int lColonPos = sCur.Find(_T(":"));

         if (lColonPos >= 0)
         {
            String sFirstPart = sCur.Mid(0, lColonPos);
            String sSecondPart = sCur.Mid(lColonPos + 1);

            __int64 lower = sFirstPart == _T("*") ? maxItem : _ttoi64(sFirstPart);
            __int64 upper = sSecondPart == _T("*") ? maxItem : _ttoi64(sSecondPart);

            if (upper < lower)
               std::swap(lower, upper);

            runs.push_back(std::make_pair(lower, upper));
         }
         else
         {
            const __int64 foundItem = sCur == _T("*") ? maxItem : _ttoi64(sCur);
            runs.push_back(std::make_pair(foundItem, foundItem));
         }
      }

      std::sort(runs.begin(), runs.end());

      // Overlapping runs made one, so that the only run that can hold an item is
      // the last that starts at or before it.
      Runs merged;
      for (const std::pair<__int64, __int64> &run : runs)
      {
         if (!merged.empty() && run.first <= merged.back().second)
         {
            if (run.second > merged.back().second)
               merged.back().second = run.second;
            continue;
         }

         merged.push_back(run);
      }

      return merged;
   }

   bool
   IMAPListLookup::IsItemInRuns(const Runs &runs, __int64 item)
   {
      auto after = std::upper_bound(runs.begin(), runs.end(), item,
         [](__int64 value, const std::pair<__int64, __int64> &run) { return value < run.first; });

      if (after == runs.begin())
         return false;

      return item <= (*std::prev(after)).second;
   }

   IMAPListLookup::UidRanges
   IMAPListLookup::MergeUidRanges(UidRanges ranges)
   {
      std::sort(ranges.begin(), ranges.end());

      UidRanges merged;
      for (const std::pair<unsigned int, unsigned int> &range : ranges)
      {
         if (!merged.empty() && range.first <= merged.back().second)
         {
            if (range.second > merged.back().second)
               merged.back().second = range.second;
            continue;
         }

         merged.push_back(range);
      }

      return merged;
   }

   bool
   IMAPListLookup::IsInUidRanges(const UidRanges &merged, unsigned int uid)
   {
      auto after = std::upper_bound(merged.begin(), merged.end(), uid,
         [](unsigned int value, const std::pair<unsigned int, unsigned int> &range) { return value < range.first; });

      if (after == merged.begin())
         return false;

      return uid <= (*std::prev(after)).second;
   }

   bool
   IMAPListLookup::ExpandSavedResult(const String &set, const String &substitution, String &expanded)
   {
      std::vector<String> elements = StringParser::SplitString(set, _T(","));
      std::vector<String> result;
      bool substituted = false;

      for (const String &element : elements)
      {
         if (element.Find(_T("$")) < 0)
         {
            result.push_back(element);
            continue;
         }

         if (element != _T("$"))
            return false;

         if (substituted)
            continue;

         result.push_back(substitution);
         substituted = true;
      }

      expanded = StringParser::JoinVector(result, _T(","));
      return true;
   }

}
