// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class IMAPListLookup
   {
   public:

      IMAPListLookup();
      virtual ~IMAPListLookup();

      // maxItem is the value "*" stands for: the largest UID in the mailbox for a
      // UID set, or the number of messages for a message-sequence set. 64-bit,
      // because a UID is an unsigned 32-bit number: "UID 1:4294967295" was read
      // through an int, overflowed, and matched only the first message.
      static bool IsItemInList(const std::vector<String> &vecItems, __int64 item, __int64 maxItem);

      // The same set read once: each element as the run of numbers IsItemInList
      // reads it as, sorted, overlapping runs made one. IsItemInRuns then answers
      // what IsItemInList answers, by halving. A SEARCH asks for every message,
      // and IsItemInList reads the whole set each time - a set of a hundred
      // thousand elements, a client's, over every message of the mailbox.
      typedef std::vector<std::pair<__int64, __int64> > Runs;
      static Runs ToRuns(const std::vector<String> &vecItems, __int64 maxItem);
      static bool IsItemInRuns(const Runs &runs, __int64 item);

      // The same for a UID set already read into ranges (IMAPCommandUID's
      // ParseUidSet_): sorted, overlapping ones made one, so IsInUidRanges can
      // answer by halving whether any holds a UID.
      typedef std::vector<std::pair<unsigned int, unsigned int> > UidRanges;
      static UidRanges MergeUidRanges(UidRanges ranges);
      static bool IsInUidRanges(const UidRanges &merged, unsigned int uid);

      // A message set with "$" (RFC 5182, SEARCHRES) standing in for the saved
      // result - substitution, its elements joined by "," - where it stands as an
      // element of its own, once however often it is named: the set is a set.
      // False, and expanded untouched, when an element holds a "$" and something
      // else ("1:$", "$$"), which is no set. A "$" used to be replaced as text
      // wherever it stood, once per "$": a command line of a hundred thousand
      // copied the saved result a hundred thousand times, gigabytes for a saved
      // result of a few thousand messages.
      static bool ExpandSavedResult(const String &set, const String &substitution, String &expanded);

   private:

   };


}