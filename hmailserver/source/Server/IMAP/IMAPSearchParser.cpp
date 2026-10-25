// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "IMAPSearchParser.h"
#include "IMAPSimpleCommandParser.h"
#include "IMAPCommand.h"
#include "IMAPSortParser.h"

#include "../Common/Util/RegularExpression.h"
#include "../Common/Util/Unicode.h"
#include "IMAPListLookup.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      //---------------------------------------------------------------------------()
      // Criteria named again beside themselves, left out when the command is parsed.
      //
      // Each criterion of a SEARCH is asked of every message of the mailbox, so a key
      // named a hundred thousand times was asked a hundred thousand times of each:
      // "SEARCH $ $ $ ...", the saved result named as often as one command line holds
      // it, was ten million questions over a hundred messages, and held the search -
      // and the thread it runs on - for the whole of IMAPSearchTimeout on one of the
      // CI's Linux runners, where the answer is the saved result. SORT and THREAD take
      // their criteria from this parser too. A message matches a criterion named twice
      // exactly as it matches it named once - among the command's own, every one of
      // which it must match, and between the two of an OR, either of which it may - so
      // the first of each stays where it stands and the others are left out: the
      // answer is the same, and each is asked of a message once.
      //
      // The same is the same as the parser made it, and nothing looser: the same key -
      // in whatever case the client wrote its name, SEEN and seen being one key to the
      // grammar and to ParseWord_ - the same NOT, the same value character for
      // character, case included, the same header field name likewise, the same set
      // element for element, and for an OR the same two in the same order. Nothing is
      // merged that is the same only by a rule of the search's own: SUBJECT foo and
      // SUBJECT FOO, which it matches alike, stay two, as do HEADER X-A v and HEADER
      // x-a v, KEYWORD a and KEYWORD A, UID 1:3 and UID 1,2,3, LARGER 10 and LARGER
      // 010, and OR A B and OR B A. Each key's matcher has its own idea of alike -
      // ContainsNoCase lowers with the global locale, CompareNoCase with the classic
      // one, dates and numbers are read by functions of their own - and a merge made
      // by any other idea could one day answer otherwise than the keys it merged; a
      // client that names one value in many spellings is a client that names many
      // values, which IMAPSearchTimeout bounds as it bounded these. Nor is anything
      // rewritten: OR A B beside A stays, and an OR of two the same keeps its one as
      // an OR. A NOT is part of the key it stands before (ParseWord_), so NOT A and A
      // are two, and NOT A twice is one.
      //
      // A parenthesised list is no criterion of its own here: ParseCommand reads the
      // keys of a list in the list's place, so "($ $) (($) $)" is four "$" of the
      // command, made one.
      //
      // Each criterion is given a number, the same for criteria that are the same - a
      // key's from its parts, an OR's from the numbers of the two it keeps - so the
      // tree is read once, however deep, and nothing is copied. Two sets are the same
      // when they are one set - the hundred thousand "$" hold one (ParseWord_) - and
      // are otherwise compared element by element. The numbers are an ordered set's,
      // which no command can make collide: a criterion the same as one before it
      // costs a look in that set and a number in an array - a "$" or a flag the same
      // as the last of them found again not even the look - and a new one a node of
      // it and two numbers.
      //---------------------------------------------------------------------------()
      class RepeatedCriteria
      {
      public:
         RepeatedCriteria() :
            numbers_(PartsLess(listed_)),
            next_number_(0),
            lists_read_(0),
            recent_set_(nullptr),
            recent_kind_(0),
            recent_lower_(0),
            recent_upper_(0),
            recent_number_(0)
         {
         }

         RepeatedCriteria(const RepeatedCriteria &) = delete;
         RepeatedCriteria &operator=(const RepeatedCriteria &) = delete;

         // Leaves the first of each of the list's criteria, in its place, and the
         // numbers of those it leaves in listed_, count of them from first.
         void RemoveFrom(IMAPSearchCriteria &list, unsigned int &first, unsigned int &count)
         {
            std::vector<std::shared_ptr<IMAPSearchCriteria> > &criterias = list.GetSubCriterias();
            const size_t size = criterias.size();

            // The number of each first - an OR's after its own two, which list theirs
            // after these - so that no list under this one marks a number while
            // this one chooses.
            first = static_cast<unsigned int>(listed_.size());
            listed_.resize(listed_.size() + size);
            for (size_t i = 0; i < size; i++)
            {
               const unsigned int number = NumberOf_(*criterias[i]);
               listed_[first + i] = number;
            }

            // Then the first of each number stays, moved up behind the one kept
            // before it, and the rest are let go of. Nothing here allocates or
            // throws, so the list is either as it was or done.
            const unsigned int mark = ++lists_read_;
            count = 0;
            for (size_t i = 0; i < size; i++)
            {
               const unsigned int number = listed_[first + i];
               if (last_kept_in_[number] == mark)
               {
                  criterias[i].reset();
                  continue;
               }

               last_kept_in_[number] = mark;
               listed_[first + count] = number;
               if (count != i)
                  criterias[count] = std::move(criterias[i]);
               count++;
            }

            criterias.erase(criterias.begin() + count, criterias.end());
         }

      private:

         // A criterion's parts: those it holds, its key with its NOT and OR as one
         // number (kind), so that most comparisons need not read the criterion,
         // and where in listed_ the numbers of the criteria it keeps of its own
         // (an OR's) are. Its number goes with them but is none of them.
         struct Parts
         {
            Parts() :
               criteria(nullptr),
               kind(0),
               first(0),
               count(0),
               number(0)
            {
            }

            const IMAPSearchCriteria *criteria;
            unsigned int kind;
            unsigned int first;
            unsigned int count;
            unsigned int number;
         };

         // Character for character: std::wstring's own comparison, which neither
         // ignores case nor stops at a NUL.
         static int CompareText_(const String &a, const String &b)
         {
            return static_cast<const std::wstring &>(a).compare(static_cast<const std::wstring &>(b));
         }

         // A set: the same as itself, and as another set its elements, each
         // character for character, in their order.
         static int CompareSets_(const std::vector<String> &a, const std::vector<String> &b)
         {
            if (&a == &b)
               return 0;

            const size_t common = std::min(a.size(), b.size());
            for (size_t i = 0; i < common; i++)
            {
               const int element = CompareText_(a[i], b[i]);
               if (element != 0)
                  return element;
            }

            if (a.size() == b.size())
               return 0;

            return a.size() < b.size() ? -1 : 1;
         }

         struct PartsLess
         {
            explicit PartsLess(const std::vector<unsigned int> &listed) :
               listed_(&listed)
            {
            }

            bool operator()(const Parts &a, const Parts &b) const
            {
               if (a.kind != b.kind)
                  return a.kind < b.kind;

               const int kept = CompareKept_(a, b);
               if (kept != 0)
                  return kept < 0;

               const IMAPSearchCriteria &x = *a.criteria;
               const IMAPSearchCriteria &y = *b.criteria;

               if (x.GetUIDLower() != y.GetUIDLower())
                  return x.GetUIDLower() < y.GetUIDLower();
               if (x.GetUIDUpper() != y.GetUIDUpper())
                  return x.GetUIDUpper() < y.GetUIDUpper();

               const int set = CompareSets_(x.GetSequenceSet(), y.GetSequenceSet());
               if (set != 0)
                  return set < 0;

               const int field = CompareText_(x.GetHeaderField(), y.GetHeaderField());
               if (field != 0)
                  return field < 0;

               return CompareText_(x.GetText(), y.GetText()) < 0;
            }

         private:

            // What two ORs keep, number by number.
            int CompareKept_(const Parts &a, const Parts &b) const
            {
               const std::vector<unsigned int> &listed = *listed_;

               const unsigned int common = std::min(a.count, b.count);
               for (unsigned int i = 0; i < common; i++)
               {
                  const unsigned int x = listed[a.first + i];
                  const unsigned int y = listed[b.first + i];
                  if (x != y)
                     return x < y ? -1 : 1;
               }

               if (a.count == b.count)
                  return 0;

               return a.count < b.count ? -1 : 1;
            }

            // RepeatedCriteria's listed_, which holds what each OR keeps.
            const std::vector<unsigned int> *listed_;
         };

         unsigned int NumberOf_(IMAPSearchCriteria &criteria)
         {
            Parts parts;
            parts.criteria = &criteria;
            parts.kind = static_cast<unsigned int>(criteria.GetType()) * 4 +
                         (criteria.GetPositive() ? 2 : 0) + (criteria.GetIsOR() ? 1 : 0);

            // A key of nothing but its set the same as the last such key found
            // again, set and all: its number without a look.
            const bool bare = criteria.GetSubCriterias().empty() && criteria.GetText().IsEmpty() &&
                              criteria.GetHeaderField().IsEmpty();
            if (bare && &criteria.GetSequenceSet() == recent_set_ && parts.kind == recent_kind_ &&
                criteria.GetUIDLower() == recent_lower_ && criteria.GetUIDUpper() == recent_upper_)
               return recent_number_;

            // An OR's two first, so that it is compared by what is left of them.
            const size_t listed = listed_.size();
            const unsigned int numbered = next_number_;
            if (!criteria.GetSubCriterias().empty())
               RemoveFrom(criteria, parts.first, parts.count);

            const std::set<Parts, PartsLess>::iterator known = numbers_.lower_bound(parts);
            if (known != numbers_.end() && !numbers_.key_comp()(parts, *known))
            {
               // Remembered when it holds the very set of the first of its number,
               // which is never let go of: the next "$" is this "$".
               if (bare && &criteria.GetSequenceSet() == &known->criteria->GetSequenceSet())
               {
                  recent_set_ = &criteria.GetSequenceSet();
                  recent_kind_ = parts.kind;
                  recent_lower_ = criteria.GetUIDLower();
                  recent_upper_ = criteria.GetUIDUpper();
                  recent_number_ = known->number;
               }

               // An OR made again of nothing new: what it listed, and the lists
               // under it, are read no more, and no number's parts are among it.
               if (next_number_ == numbered && listed_.size() != listed)
                  listed_.resize(listed);

               return known->number;
            }

            parts.number = next_number_;
            last_kept_in_.push_back(0);
            numbers_.insert(known, parts);
            return next_number_++;
         }

         // The numbers of each list's criteria, in the list's order, and over them,
         // once the list is done, the numbers of those it kept.
         std::vector<unsigned int> listed_;

         // Each number, by the parts of the first criterion that had it, which is
         // not let go of while the set is read: a criterion left out of a list comes
         // after one of its number in that list, and what is under an OR left out is
         // of the numbers of what is under the OR of its number before it, numbered
         // before - none of it is the first of its number.
         std::set<Parts, PartsLess> numbers_;
         unsigned int next_number_;

         // For each number, the mark of the last list that kept a criterion of it;
         // each list is marked by a number of its own as it chooses.
         std::vector<unsigned int> last_kept_in_;
         unsigned int lists_read_;

         // The last key of nothing but a set found again holding the very set of the
         // first of its number - a "$", whose set every "$" holds, or a flag, whose
         // empty set every flag holds - and that number.
         const std::vector<String> *recent_set_;
         unsigned int recent_kind_;
         long recent_lower_;
         long recent_upper_;
         unsigned int recent_number_;
      };
   }

   void
   IMAPSearchParser::RemoveRepeatedCriteria(IMAPSearchCriteria &criteria)
   {
      unsigned int first = 0;
      unsigned int count = 0;

      RepeatedCriteria repeated;
      repeated.RemoveFrom(criteria, first, count);
   }

   IMAPSearchCriteria::CriteriaType
   IMAPSearchCriteria::GetCriteriaTypeByName(const String &sName)
   {
      String sTmp = sName;
      sTmp.MakeLower();
      
      if (sTmp.Compare(_T("")) == 0)
         return CTUnknown;
      else if (sTmp.Compare(_T("charset")) == 0)
         return CTCharset;
      else if (sTmp == _T("on"))
         return CTOn;
      else if (sTmp == _T("header"))
         return CTHeader;
      else if (sTmp == _T("text"))
         return CTText;
      else if (sTmp == _T("body"))
         return CTBody;
      else if (sTmp == _T("subject"))
         return CTSubject;
      else if (sTmp == _T("from"))
         return CTFrom;
      else if (sTmp == _T("cc"))
         return CTCC;
      else if (sTmp == _T("bcc"))
         return CTBCC;
      else if (sTmp == _T("to"))
         return CTTo;
      else if (sTmp == _T("senton"))
         return CTSentOn;
      else if (sTmp == _T("sentbefore"))
         return CTSentBefore;
      else if (sTmp == _T("sentsince"))
         return CTSentSince;
      else if (sTmp == _T("savedbefore"))
         return CTSavedBefore;
      else if (sTmp == _T("savedon"))
         return CTSavedOn;
      else if (sTmp == _T("savedsince"))
         return CTSavedSince;
      else if (sTmp == _T("savedatesupported"))
         return CTSavedateSupported;
      else if (sTmp == _T("emailid"))
         return CTEmailId;
      else if (sTmp == _T("threadid"))
         return CTThreadId;
      else if (sTmp == _T("since"))
         return CTSince;
      else if (sTmp == _T("deleted"))
         return CTDeleted;
      else if (sTmp == _T("recent"))
         return CTRecent;
      else if (sTmp == _T("seen"))
         return CTSeen;
      else if (sTmp == _T("unseen"))
         return CTUnseen;
      else if (sTmp == _T("undeleted"))
         return CTUndeleted;
      else if (sTmp == _T("uid"))
         return CTUID;
      else if (sTmp == _T("answered"))
         return CTAnswered;
      else if (sTmp == _T("before"))
         return CTBefore;
      else if (sTmp == _T("draft"))
         return CTDraft;
      else if (sTmp == _T("flagged"))
         return CTFlagged;
      else if (sTmp == _T("larger"))
         return CTLarger;
      else if (sTmp == _T("new"))
         return CTNew;
      else if (sTmp == _T("old"))
         return CTOld;
      else if (sTmp == _T("smaller"))
         return CTSmaller;
      else if (sTmp == _T("modseq"))
         return CTModSeq;
      else if (sTmp == _T("unanswered"))
         return CTUnanswered;
      else if (sTmp == _T("undraft"))
         return CTUndraft;
      else if (sTmp == _T("unflagged"))
         return CTUnflagged;
      else if (sTmp == _T("all"))
         return CTAll;
      else if (sTmp == _T("keyword"))
         return CTKeyword;
      else if (sTmp == _T("unkeyword"))
         return CTUnkeyword;
      // RFC 5032 (WITHIN).
      else if (sTmp == _T("older"))
         return CTOlder;
      else if (sTmp == _T("younger"))
         return CTYounger;
      else if (IsSequenceSet_(sTmp))
         return CTSequenceSet;

      return CTUnknown;
   }

   bool 
   IMAPSearchCriteria::IsSequenceSet_(const String &item)
   {
      const String sequenceSetRegex = "[0-9:,*]*";

      return RegularExpression::TestExactMatch(sequenceSetRegex, item);
   }



   IMAPSearchParser::IMAPSearchParser()
   {


   }

   IMAPSearchParser::~IMAPSearchParser()
   {

   }

   IMAPResult
   IMAPSearchParser::ParseCommand(std::shared_ptr<IMAPCommandArgument> pArgument, IMAPSearchCommandMode mode)
   {
      // Replace literals in the command.
      std::shared_ptr<IMAPSimpleCommandParser> pSimpleParser = std::shared_ptr<IMAPSimpleCommandParser> (new IMAPSimpleCommandParser);

      if (mode == IMAPSearchModeSort || mode == IMAPSearchModeThread)
      {
         pSimpleParser->Parse(pArgument);
         pSimpleParser->UnliteralData();

         // Checked before Word(0) is touched, not after. The parser produces NO words at
         // all when the command fails validation - which unbalanced parentheses outside a
         // quoted string do - and Word() used to index the vector directly, so "SORT ("
         // read parsed_words_[0] on an empty vector. One line on an authenticated
         // connection, and the fault it raises is swallowed by the /EHa catch(...) in
         // TCPConnection::AsyncReadCompleted, which logs it as error 5136, drops the
         // session and rethrows for a minidump. Word() is bounds-checked now as well;
         // this ordering is what makes the client's answer the right one.
         if (pSimpleParser->WordCount() < 2)
         {
            return IMAPResult(IMAPResult::ResultBad, "SearchCharacter set must be specified.");
         }

         if (mode == IMAPSearchModeSort)
         {
            std::shared_ptr<IMAPSimpleWord> pSort = pSimpleParser->Word(0);
            if (pSort->Paranthezied())
            {
               sort_parser_ = std::shared_ptr<IMAPSortParser>(new IMAPSortParser);
               sort_parser_->Parse(pSort->Value());
            }
         }
         else
         {
            // RFC 5256: THREAD leads with a bare algorithm atom, not a
            // parenthesised list - "THREAD REFERENCES UTF-8 ALL". A parenthesised
            // word here is a malformed command, and it is refused rather than
            // unwrapped because unwrapping would make this server accept syntax no
            // other server accepts, which a client author then ships against.
            std::shared_ptr<IMAPSimpleWord> pAlgorithm = pSimpleParser->Word(0);

            // Clammerized is the literal form ("{10}\r\nREFERENCES"). Refused with
            // the others not only because it is absurd for an atom, but because the
            // prefix trim below cuts the algorithm's own text off the command
            // string - a literal leaves "{10}" standing there instead, and the
            // criteria parser would then eat a brace token as a search key.
            if (pAlgorithm->Paranthezied() || pAlgorithm->Quoted() || pAlgorithm->Clammerized() ||
                pAlgorithm->Value().IsEmpty())
               return IMAPResult(IMAPResult::ResultBad, "Thread algorithm must be specified.");

            thread_algorithm_ = pAlgorithm->Value();
         }

         charset_name_ = pSimpleParser->Word(1)->Value();
         if (!IsValidCharset_(charset_name_))
            return BadCharsetResult_();

         // Trim away the SORT/THREAD prefix of the expression, since only the
         // search criteria matter below.
         String tempString = pArgument->Command();

         if (mode == IMAPSearchModeSort)
         {
            if (tempString.Find(_T(")")) > 0)
               tempString = tempString.Mid(tempString.Find(_T(")"))+2);
         }
         else
         {
            // The algorithm atom is the first word of the remaining command string
            // and cannot be quoted or a literal (both were refused above), so
            // cutting its own length off the front is exact.
            tempString.TrimLeft();

            if (tempString.Mid(0, thread_algorithm_.GetLength()).CompareNoCase(thread_algorithm_) == 0)
               tempString = tempString.Mid(thread_algorithm_.GetLength());
         }

         // The charset has already been taken into charset_name_ above, and it is not a
         // search key, so it has to come out of the string the criteria parser sees.
         // It used to be left in and quietly discarded further down as an unrecognised
         // word - which stopped being viable the moment an unrecognised word became an
         // error, because "SORT (DATE) US-ASCII ALL" would then fail on "US-ASCII".
         tempString.TrimLeft();

         if (!tempString.IsEmpty() && tempString.GetAt(0) == '"')
         {
            // Quoted charset: drop everything up to and including the closing quote.
            int closingQuote = tempString.Find(_T("\""), 1);
            if (closingQuote > 0)
               tempString = tempString.Mid(closingQuote + 1);
         }
         else if (!tempString.IsEmpty() && tempString.GetAt(0) == '{')
         {
            // Charset sent as a literal. What stands in the command is the "{n}" token,
            // while the value above came from the literal data. Both have to go, and
            // together: leaving the token behind makes the criteria parser read "UTF-8"
            // as a search key, and dropping only the token shifts every remaining literal
            // by one so the wrong data is paired with the wrong word.
            int closingBrace = tempString.Find(_T("}"));
            if (closingBrace > 0)
            {
               tempString = tempString.Mid(closingBrace + 1);

               std::vector<String> remainingLiterals = pArgument->Literals();
               if (!remainingLiterals.empty())
               {
                  remainingLiterals.erase(remainingLiterals.begin());
                  pArgument->Literals(remainingLiterals);
               }
            }
         }
         else if (!charset_name_.IsEmpty() &&
                  tempString.Mid(0, charset_name_.GetLength()).CompareNoCase(charset_name_) == 0)
         {
            tempString = tempString.Mid(charset_name_.GetLength());
         }

         tempString.TrimLeft();

         pArgument->Command(tempString);
      }

      /* 
         Remove all parenthesis outside of strings. Some IMAP
         clients sends parenthesis, some doesn't. We remove
         them here to prevent the behaviors from differing.

         It should be safe to do this, since
            Criteria1 OR (Criteria2 Criteria 3)
         means the same as
            Criteria1 OR Criteria2 Criteria 3
      */

      String resultString;
      const String inputString = pArgument->Command();
      bool insideString = false;
      for (int i = 0; i < inputString.GetLength(); i++)
      {
         wchar_t curChar = inputString.GetAt(i);

         switch (curChar)
         {
         case '"':
            insideString = !insideString;
            break;
         case '(':
         case ')':
            if (!insideString)
               continue;
         }

         resultString += curChar;
      }

      // Replace literals in the command.
      pSimpleParser = std::shared_ptr<IMAPSimpleCommandParser> (new IMAPSimpleCommandParser);
      pArgument->Command(resultString);

      pSimpleParser->Parse(pArgument);
      pSimpleParser->UnliteralData();

      std::shared_ptr<IMAPSearchCriteria> pCriteria = std::shared_ptr<IMAPSearchCriteria> (new IMAPSearchCriteria);

      int currentWord = 0;
      IMAPResult result = ParseSegment_(pSimpleParser, currentWord, pCriteria, 0);
      if (result.GetResult() != IMAPResult::ResultOK)
         return result;

      // Each criterion asked of a message once, however often it was named beside
      // itself (RepeatedCriteria, above). Never all of them: the first of each stays.
      RemoveRepeatedCriteria(*pCriteria);

      // RFC 3501: a SEARCH must carry at least one search key. Nothing at all here used
      // to be treated as "no restrictions", and DoesMessageMatch_ answers true for an
      // empty criteria list - so a command whose keys were all dropped, or a bare
      // "SEARCH", reported every message in the mailbox as a match. A client that acts
      // on a result set in bulk (Thunderbird's Search Messages window offering Delete,
      // an archiver moving "what the search returned") would then act on the whole
      // mailbox. Refusing the command is the only answer the protocol lets a client
      // detect.
      if (pCriteria->GetSubCriterias().empty())
         return IMAPResult(IMAPResult::ResultBad, "No search criteria specified.");

      result_criteria_ = pCriteria;

      return IMAPResult();
   }

   IMAPResult
   IMAPSearchParser::ParseSegment_(std::shared_ptr<IMAPSimpleCommandParser> pSimpleParser, int &currentWord, std::shared_ptr<IMAPSearchCriteria> pCriteria, int iRecursion)
   {
      iRecursion++;
      if (iRecursion > 50)
      {
         return IMAPResult(IMAPResult::ResultNo, "Search failed due to excessive search expression recursion.");
      }
      
      int originalCriteriaCount = (int) pCriteria->GetSubCriterias().size();
      for (; currentWord < (int) pSimpleParser->WordCount(); currentWord++)
      {
         std::shared_ptr<IMAPSimpleWord> pWord = pSimpleParser->Word(currentWord);
         String sCurCommand = pWord->Value().ToUpper();

         if (sCurCommand == _T("OR"))
         {
            // We have a sub argument.
            std::shared_ptr<IMAPSearchCriteria> pSubCriteria = std::shared_ptr<IMAPSearchCriteria> (new IMAPSearchCriteria());

            pSubCriteria->SetType(IMAPSearchCriteria::CTSubCriteria);
            pSubCriteria->SetIsOR(true);

            currentWord++;
            IMAPResult result = ParseSegment_(pSimpleParser, currentWord, pSubCriteria, iRecursion);
            if (result.GetResult() != IMAPResult::ResultOK)
               return result;
            
            pCriteria->GetSubCriterias().push_back(pSubCriteria);

            if (iRecursion > 1)
            {
               // This is a sub criteria. We only add two words here.
               if (originalCriteriaCount + 2 == pCriteria->GetSubCriterias().size())
               {
                  return IMAPResult();
               }
            }

            continue;
         }
 
         std::shared_ptr<IMAPSearchCriteria> pNewCriteria = std::shared_ptr<IMAPSearchCriteria> (new IMAPSearchCriteria);
         IMAPResult result = ParseWord_(pSimpleParser, pNewCriteria, currentWord );
         if (result.GetResult() != IMAPResult::ResultOK)
            return result;
         
         if (pNewCriteria->GetType() != IMAPSearchCriteria::CTUnknown)
            pCriteria->GetSubCriterias().push_back(pNewCriteria);

         if (iRecursion > 1)
         {
            // This is a sub criteria. We only add two words here.
            if (originalCriteriaCount + 2 == pCriteria->GetSubCriterias().size())
            {
               return IMAPResult();
            }
         }
      }

      return IMAPResult();
   }

   IMAPResult 
   IMAPSearchParser::ParseWord_(std::shared_ptr<IMAPSimpleCommandParser> pSimpleParser, std::shared_ptr<IMAPSearchCriteria> pNewCriteria, int &iCurrentWord)
   {
      String sCurCommand = pSimpleParser->Word(iCurrentWord)->Value();

      // Compared case-insensitively because IMAP keywords are case-insensitive. This was
      // an exact match against "NOT", so a client sending "not deleted" had the NOT
      // silently discarded as an unrecognised word and got the messages that ARE
      // deleted - the exact opposite of what it asked for.
      if (sCurCommand.CompareNoCase(_T("NOT")) == 0)
      {
         pNewCriteria->SetPositive(false);
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. NOT used but no search criteria specified.");

         std::shared_ptr<IMAPSimpleWord> pWord = pSimpleParser->Word(iCurrentWord);
         sCurCommand = pWord->Value().ToUpper();
      }
      else
         pNewCriteria->SetPositive(true);

      // RFC 5182 (SEARCHRES): "$" stands for the result saved by an earlier
      // SEARCH RETURN (SAVE), and RFC 5182 section 2.1 uses it as a search key
      // ("SEARCH $ SMALLER 4096") as well as in FETCH/STORE/COPY. The saved result is a
      // UID list, so it is matched as a UID set - the same set of messages whether this
      // is a SEARCH or a UID SEARCH. An empty saved result becomes the set "0", which
      // no message matches, so the search returns nothing rather than everything.
      if (sCurCommand == _T("$"))
      {
         // Made once for the command and shared by every "$" in it.
         if (!saved_set_)
         {
            std::vector<String> savedSet;

            for (__int64 savedUid : saved_search_result_)
            {
               String uidText;
               uidText.Format(_T("%I64d"), savedUid);
               savedSet.push_back(uidText);
            }

            if (savedSet.empty())
               savedSet.push_back(_T("0"));

            saved_set_ = std::make_shared<const std::vector<String> >(std::move(savedSet));
         }

         pNewCriteria->SetSequenceSet(saved_set_);
         pNewCriteria->SetType(IMAPSearchCriteria::CTUID);

         return IMAPResult();
      }

      IMAPSearchCriteria::CriteriaType ct = IMAPSearchCriteria::GetCriteriaTypeByName(sCurCommand);

      if (ct == IMAPSearchCriteria::CTHeader)
      {
         // The client want's to search for a 
         // header field.
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. No header specified in header-search.");

         String sHeaderField = pSimpleParser->Word(iCurrentWord)->Value();            

         // Go to the header value.
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. No value specified in header-search.");

         String sHeaderValue = pSimpleParser->Word(iCurrentWord)->Value();

         sHeaderValue = DecodeWordAccordingToCharset_(sHeaderValue);

         pNewCriteria->SetHeaderField(sHeaderField);
         pNewCriteria->SetText(sHeaderValue);
         pNewCriteria->SetType(IMAPSearchCriteria::CTHeader);         

      }
      else if (ct == IMAPSearchCriteria::CTText ||
         ct == IMAPSearchCriteria::CTSentOn ||
         ct == IMAPSearchCriteria::CTSentBefore ||
         ct == IMAPSearchCriteria::CTSentSince ||
         ct == IMAPSearchCriteria::CTSavedBefore ||
         ct == IMAPSearchCriteria::CTSavedOn ||
         ct == IMAPSearchCriteria::CTSavedSince ||
         ct == IMAPSearchCriteria::CTEmailId ||
         ct == IMAPSearchCriteria::CTThreadId ||
         ct == IMAPSearchCriteria::CTBody ||
         ct == IMAPSearchCriteria::CTSubject ||
         ct == IMAPSearchCriteria::CTTo ||
         ct == IMAPSearchCriteria::CTFrom ||
         ct == IMAPSearchCriteria::CTCC ||
         ct == IMAPSearchCriteria::CTBCC ||
         ct == IMAPSearchCriteria::CTSince || 
         ct == IMAPSearchCriteria::CTBefore ||
         ct == IMAPSearchCriteria::CTLarger ||
         ct == IMAPSearchCriteria::CTSmaller ||
         ct == IMAPSearchCriteria::CTModSeq ||
         ct == IMAPSearchCriteria::CTOn)
      {
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. Missing value.");

         // RFC 7162 3.1.5: MODSEQ may name a metadata item before the value -
         // MODSEQ "/flags/\\seen" all 620162338 - and the value is the LAST of
         // the three. It used to be taken from the first, the quoted entry name,
         // which reads as 0 and matched every message, while "all" and the
         // number were parsed as the search keys ALL and a sequence set - so the
         // search answered for message 620162338 alone. This server keeps one
         // mod-sequence per message, not one per flag, so the entry name and its
         // type are passed over and the message's own mod-sequence compared,
         // which is the RFC's permitted approximation (a message's mod-sequence
         // is never lower than that of any of its metadata items).
         if (ct == IMAPSearchCriteria::CTModSeq && pSimpleParser->Word(iCurrentWord)->Quoted())
         {
            iCurrentWord += 2;

            if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
               return IMAPResult(IMAPResult::ResultBad, "Syntax error. MODSEQ entry without a value.");
         }

         std::shared_ptr<IMAPSimpleWord> pWord = pSimpleParser->Word(iCurrentWord);

         if (pWord)
         {
            String searchValue = pWord->Value();

            if (NeedsDecoding_(ct))
               searchValue = DecodeWordAccordingToCharset_(searchValue);

            pNewCriteria->SetText(searchValue);
            pNewCriteria->SetType(ct);
         }

      }
      else if (ct == IMAPSearchCriteria::CTOlder ||
         ct == IMAPSearchCriteria::CTYounger)
      {
         // RFC 5032 (WITHIN): the interval is an nz-number of seconds. Validated here
         // rather than left to the matcher, because a non-numeric or zero interval would
         // otherwise silently become "0 seconds ago" and match either everything or
         // nothing depending on which key it was.
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. Missing interval.");

         String interval = pSimpleParser->Word(iCurrentWord)->Value();

         if (!IsNonZeroNumber_(interval))
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. The OLDER and YOUNGER intervals must be a positive number of seconds.");

         pNewCriteria->SetText(interval);
         pNewCriteria->SetType(ct);
      }
      else if (ct == IMAPSearchCriteria::CTKeyword ||
         ct == IMAPSearchCriteria::CTUnkeyword)
      {
         // The keyword name is the next word.
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. Missing keyword name.");

         pNewCriteria->SetText(pSimpleParser->Word(iCurrentWord)->Value());
         pNewCriteria->SetType(ct);
      }
      else if (ct == IMAPSearchCriteria::CTUID)
      {
         // Check which UID's we should send back.
         //

         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. UID parameters missing.");

         std::shared_ptr<IMAPSimpleWord> pWord = pSimpleParser->Word(iCurrentWord);
         if (pWord)
         {
            String sTemp = pWord->Value();
            std::vector<String> vecSplit = StringParser::SplitString(sTemp, ",");
            pNewCriteria->SetSequenceSet(vecSplit);
            pNewCriteria->SetType(ct);

         }            
      }
      else if (ct == IMAPSearchCriteria::CTDeleted ||
         ct == IMAPSearchCriteria::CTUndeleted ||
         ct == IMAPSearchCriteria::CTSeen ||
         ct == IMAPSearchCriteria::CTUnseen ||
         ct == IMAPSearchCriteria::CTRecent ||
         ct == IMAPSearchCriteria::CTAnswered ||
         ct == IMAPSearchCriteria::CTDraft ||
         ct == IMAPSearchCriteria::CTFlagged ||
         ct == IMAPSearchCriteria::CTNew ||
         ct == IMAPSearchCriteria::CTOld ||
         ct == IMAPSearchCriteria::CTUnanswered ||
         ct == IMAPSearchCriteria::CTUndraft ||
         ct == IMAPSearchCriteria::CTUnflagged ||
         ct == IMAPSearchCriteria::CTSavedateSupported ||
         ct == IMAPSearchCriteria::CTAll)
      {
         pNewCriteria->SetType(ct);
      }
      else if (ct == IMAPSearchCriteria::CTSequenceSet)
      {
         pNewCriteria->SetType(ct);
         pNewCriteria->SetSequenceSet(StringParser::SplitString(sCurCommand, ","));
      }
      else if (ct == IMAPSearchCriteria::CTCharset)
      {
         iCurrentWord++;

         if (iCurrentWord > (int) pSimpleParser->WordCount() - 1)
            return IMAPResult(IMAPResult::ResultBad, "Syntax error. Missing charset name.");

         std::shared_ptr<IMAPSimpleWord> pWord = pSimpleParser->Word(iCurrentWord);

         if (pWord)
         {
            String charsetName = pWord->Value();

            if (!IsValidCharset_(charsetName))
               return BadCharsetResult_();

            charset_name_ = charsetName;
         }
      }
      else if (!sCurCommand.IsEmpty())
      {
         // An unrecognised search key used to be dropped on the floor here, which is the
         // worst of the three possible answers. The key the client relied on to narrow
         // the search was simply not applied, and a command whose keys were ALL
         // unrecognised - "SEARCH KEYWORD $Label1" before KEYWORD existed, or anything
         // from an extension this server does not implement - left an empty criteria list
         // that matches every message in the mailbox. RFC 3501 requires BAD for a key the
         // server does not know, and BAD is also the only answer a client can detect: a
         // "* SEARCH" line is indistinguishable from a correct one.
         //
         // The IsEmpty() guard is deliberate. The word splitter emits an empty word for
         // each run of two or more spaces, and clients do send those ("SEARCH FLAGGED  "),
         // so an empty word is whitespace rather than a key and stays ignored.
         return IMAPResult(IMAPResult::ResultBad,
            Formatter::Format("Unrecognized search key: {0}", sCurCommand));
      }

      return IMAPResult();
   }

   bool
   IMAPSearchParser::IsNonZeroNumber_(const String &value)
   {
      if (value.IsEmpty())
         return false;

      for (int i = 0; i < value.GetLength(); i++)
      {
         TCHAR ch = value.GetAt(i);
         if (ch < _T('0') || ch > _T('9'))
            return false;
      }

      return _ttoi64(value) > 0;
   }

   IMAPResult
   IMAPSearchParser::BadCharsetResult_()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // RFC 3501 section 7.1: the BADCHARSET response code may name the charsets the server
   // does support, and every response code is followed by human-readable text.
   //
   // This used to be the bare "[BADCHARSET]" with nothing after it, which is not a
   // well-formed resp-text - text is not optional in the grammar - and tells the client
   // nothing it can act on. The list is the part that helps: a client whose first choice
   // is refused can reissue the same search in a charset that will work, instead of
   // reporting the search as broken.
   //---------------------------------------------------------------------------()
   {
      return IMAPResult(IMAPResult::ResultNo,
         "[BADCHARSET (US-ASCII UTF-8 ISO-8859-1)] The specified charset is not supported.");
   }

   bool
   IMAPSearchParser::IsValidCharset_(const String &charsetName)
   {
      if (charsetName.CompareNoCase(_T("UTF-8")) == 0 ||
          charsetName.CompareNoCase(_T("US-ASCII")) == 0 ||
          charsetName.CompareNoCase(_T("ISO-8859-1")) == 0)
          return true;
      else
          return false;
   }

   bool 
   IMAPSearchParser::NeedsDecoding_(IMAPSearchCriteria::CriteriaType criteriaType)
   {
      switch (criteriaType)
      {
      case IMAPSearchCriteria::CTHeader:
      case IMAPSearchCriteria::CTText:
      case IMAPSearchCriteria::CTBody:
      case IMAPSearchCriteria::CTSubject:
      case IMAPSearchCriteria::CTFrom:
      case IMAPSearchCriteria::CTTo:
      case IMAPSearchCriteria::CTCC:
      case IMAPSearchCriteria::CTBCC:
         return true;
      default:
         return false;
      }
   }

   String
   IMAPSearchParser::DecodeWordAccordingToCharset_(const String &inputValue)
   {
      if (charset_name_.CompareNoCase(_T("UTF-8")) == 0)
      {
         String resultValue;
         if (Unicode::MultiByteToWide(inputValue, resultValue))
            return resultValue;
         else
            return inputValue;
      }
      else
         return inputValue;
   }

   namespace
   {
      // xorshift64*: the same cases on every run and every platform.
      unsigned long long NextSearchRandom_(unsigned long long &state)
      {
         state ^= state >> 12;
         state ^= state << 25;
         state ^= state >> 27;
         return state * 2685821657736338717ULL;
      }

      std::string Narrow_(const String &text)
      {
         return std::string(AnsiString(text).c_str());
      }

      bool SameText_(const String &a, const String &b)
      {
         return static_cast<const std::wstring &>(a).compare(static_cast<const std::wstring &>(b)) == 0;
      }

      String KeyName_(const IMAPSearchCriteria &criteria)
      {
         switch (criteria.GetType())
         {
         case IMAPSearchCriteria::CTUID: return _T("UID");
         case IMAPSearchCriteria::CTSequenceSet: return _T("SET");
         case IMAPSearchCriteria::CTSeen: return _T("SEEN");
         case IMAPSearchCriteria::CTUnseen: return _T("UNSEEN");
         case IMAPSearchCriteria::CTDeleted: return _T("DELETED");
         case IMAPSearchCriteria::CTFlagged: return _T("FLAGGED");
         case IMAPSearchCriteria::CTAll: return _T("ALL");
         case IMAPSearchCriteria::CTSubject: return _T("SUBJECT");
         case IMAPSearchCriteria::CTHeader: return _T("HEADER");
         case IMAPSearchCriteria::CTText: return _T("TEXT");
         case IMAPSearchCriteria::CTLarger: return _T("LARGER");
         case IMAPSearchCriteria::CTSmaller: return _T("SMALLER");
         case IMAPSearchCriteria::CTModSeq: return _T("MODSEQ");
         case IMAPSearchCriteria::CTKeyword: return _T("KEYWORD");
         case IMAPSearchCriteria::CTSubCriteria: return criteria.GetIsOR() ? _T("OR") : _T("AND");
         default: break;
         }

         String name;
         name.Format(_T("T%d"), (int) criteria.GetType());
         return name;
      }

      String DescribeList_(const IMAPSearchCriteria &list);

      // A criterion written out: NOT before its key, its header field in brackets,
      // its text quoted, its set, and what an OR holds in parentheses.
      String Describe_(const IMAPSearchCriteria &criteria)
      {
         String text = criteria.GetPositive() ? _T("") : _T("NOT ");
         text += KeyName_(criteria);

         if (!criteria.GetHeaderField().IsEmpty())
            text += _T(" [") + criteria.GetHeaderField() + _T("]");

         if (!criteria.GetText().IsEmpty())
            text += _T(" \"") + criteria.GetText() + _T("\"");

         if (!criteria.GetSequenceSet().empty())
            text += _T(" ") + StringParser::JoinVector(criteria.GetSequenceSet(), _T(","));

         if (!criteria.GetSubCriterias().empty())
            text += _T(" (") + DescribeList_(criteria) + _T(")");

         return text;
      }

      // A list's criteria written out one after another: the command's, or an OR's.
      String DescribeList_(const IMAPSearchCriteria &list)
      {
         String text;
         for (const std::shared_ptr<IMAPSearchCriteria> &criteria : list.GetSubCriterias())
         {
            if (!text.IsEmpty())
               text += _T(" ");
            text += Describe_(*criteria);
         }
         return text;
      }

      // The criteria the parser makes of a command, or a logic_error saying why it
      // refused it.
      std::shared_ptr<IMAPSearchCriteria> ParseForTest_(const String &command, IMAPSearchCommandMode mode,
                                                        const std::vector<__int64> &saved,
                                                        const std::vector<String> &literals = std::vector<String>())
      {
         std::shared_ptr<IMAPCommandArgument> argument = std::shared_ptr<IMAPCommandArgument>(new IMAPCommandArgument);
         argument->Command(command);
         for (const String &literal : literals)
            argument->AddLiteral(literal);

         IMAPSearchParser parser;
         parser.SetSavedSearchResult(saved);

         const IMAPResult result = parser.ParseCommand(argument, mode);
         if (result.GetResult() != IMAPResult::ResultOK || !parser.GetCriteria())
            throw std::logic_error("The search parser refused a command of its tester: " + Narrow_(command.Left(200)));

         return parser.GetCriteria();
      }

      // Whether two criteria are made of the same parts, compared one by one and
      // nothing by number: an independent account of the same.
      bool Same_(const IMAPSearchCriteria &a, const IMAPSearchCriteria &b)
      {
         if (a.GetType() != b.GetType() || a.GetPositive() != b.GetPositive() || a.GetIsOR() != b.GetIsOR() ||
             a.GetUIDLower() != b.GetUIDLower() || a.GetUIDUpper() != b.GetUIDUpper() ||
             !SameText_(a.GetHeaderField(), b.GetHeaderField()) || !SameText_(a.GetText(), b.GetText()))
            return false;

         const std::vector<String> &setA = a.GetSequenceSet();
         const std::vector<String> &setB = b.GetSequenceSet();
         if (setA.size() != setB.size())
            return false;
         for (size_t i = 0; i < setA.size(); i++)
         {
            if (!SameText_(setA[i], setB[i]))
               return false;
         }

         const std::vector<std::shared_ptr<IMAPSearchCriteria> > &subA = a.GetSubCriterias();
         const std::vector<std::shared_ptr<IMAPSearchCriteria> > &subB = b.GetSubCriterias();
         if (subA.size() != subB.size())
            return false;
         for (size_t i = 0; i < subA.size(); i++)
         {
            if (!Same_(*subA[i], *subB[i]))
               return false;
         }

         return true;
      }

      // The removal done the slow way: each criterion against every one kept before
      // it, after its own have been done the same way.
      void RemoveRepeatedSlowly_(IMAPSearchCriteria &list)
      {
         std::vector<std::shared_ptr<IMAPSearchCriteria> > kept;

         for (const std::shared_ptr<IMAPSearchCriteria> &criteria : list.GetSubCriterias())
         {
            RemoveRepeatedSlowly_(*criteria);

            bool again = false;
            for (const std::shared_ptr<IMAPSearchCriteria> &earlier : kept)
               again = again || Same_(*earlier, *criteria);

            if (!again)
               kept.push_back(criteria);
         }

         list.GetSubCriterias().swap(kept);
      }

      // A copy of a tree, made of new criteria with new sets but for the saved
      // result, which the copy holds as the parser's "$" do: the one set, shared.
      std::shared_ptr<IMAPSearchCriteria> Copy_(const IMAPSearchCriteria &criteria, const std::shared_ptr<const std::vector<String> > &saved)
      {
         std::shared_ptr<IMAPSearchCriteria> copy = std::shared_ptr<IMAPSearchCriteria>(new IMAPSearchCriteria);
         copy->SetType(criteria.GetType());
         copy->SetPositive(criteria.GetPositive());
         copy->SetIsOR(criteria.GetIsOR());
         copy->SetUIDLower(criteria.GetUIDLower());
         copy->SetUIDUpper(criteria.GetUIDUpper());
         copy->SetHeaderField(criteria.GetHeaderField());
         copy->SetText(criteria.GetText());

         if (&criteria.GetSequenceSet() == saved.get())
            copy->SetSequenceSet(saved);
         else if (!criteria.GetSequenceSet().empty())
            copy->SetSequenceSet(std::vector<String>(criteria.GetSequenceSet()));

         for (const std::shared_ptr<IMAPSearchCriteria> &sub : criteria.GetSubCriterias())
            copy->GetSubCriterias().push_back(Copy_(*sub, saved));

         return copy;
      }

      // A message of the tester's mailbox, with what the keys it makes ask.
      struct TestMessage
      {
         TestMessage() :
            seen(false),
            deleted(false),
            flagged(false),
            uid(0),
            sequence(0),
            size(0)
         {
         }

         bool seen;
         bool deleted;
         bool flagged;
         __int64 uid;
         __int64 sequence;
         int size;
         String subject;
         String field;
         String value;
      };

      // The keys the generator makes, matched as IMAPCommandSEARCH::DoesMessageMatch_
      // matches them: a NOT turns the answer.
      bool MatchesKey_(const IMAPSearchCriteria &criteria, const TestMessage &message, __int64 maxUid, __int64 count)
      {
         bool has = false;

         switch (criteria.GetType())
         {
         case IMAPSearchCriteria::CTSeen: has = message.seen; break;
         case IMAPSearchCriteria::CTUnseen: has = !message.seen; break;
         case IMAPSearchCriteria::CTDeleted: has = message.deleted; break;
         case IMAPSearchCriteria::CTFlagged: has = message.flagged; break;
         case IMAPSearchCriteria::CTAll: has = true; break;
         case IMAPSearchCriteria::CTUID: has = IMAPListLookup::IsItemInList(criteria.GetSequenceSet(), message.uid, maxUid); break;
         case IMAPSearchCriteria::CTSequenceSet: has = IMAPListLookup::IsItemInList(criteria.GetSequenceSet(), message.sequence, count); break;
         case IMAPSearchCriteria::CTLarger: has = message.size > _ttoi(criteria.GetText().c_str()); break;
         case IMAPSearchCriteria::CTSmaller: has = message.size < _ttoi(criteria.GetText().c_str()); break;
         case IMAPSearchCriteria::CTSubject: has = message.subject.ContainsNoCase(criteria.GetText()); break;
         case IMAPSearchCriteria::CTHeader:
            has = !message.field.IsEmpty() && message.field.CompareNoCase(criteria.GetHeaderField()) == 0 &&
                  message.value.ContainsNoCase(criteria.GetText());
            break;
         default:
            throw std::logic_error("The search parser's tester made a key it cannot match");
         }

         return has == criteria.GetPositive();
      }

      // A list - the command's, or an OR's - as DoesMessageMatch_ walks it.
      bool MatchesList_(const IMAPSearchCriteria &list, const TestMessage &message, __int64 maxUid, __int64 count)
      {
         bool matching = true;

         for (const std::shared_ptr<IMAPSearchCriteria> &criteria : list.GetSubCriterias())
         {
            matching = criteria->GetType() == IMAPSearchCriteria::CTSubCriteria ?
               MatchesList_(*criteria, message, maxUid, count) :
               MatchesKey_(*criteria, message, maxUid, count);

            if (list.GetIsOR() ? matching : !matching)
               return matching;
         }

         return matching;
      }

      // One of the generator's keys, from a vocabulary small enough that it names
      // the same key again often, each value in two spellings - and a subject that
      // is empty, a key of nothing but its name like a flag.
      std::shared_ptr<IMAPSearchCriteria> MakeKey_(unsigned long long &state, const std::shared_ptr<const std::vector<String> > &saved)
      {
         static const wchar_t *const elements[] = { L"1", L"2:4", L"3:*", L"*", L"5:2", L"7", L"1:3" };
         static const wchar_t *const subjects[] = { L"a", L"A", L"ab", L"b", L"" };
         static const wchar_t *const sizes[] = { L"10", L"010", L"30", L"1000" };
         static const wchar_t *const fields[] = { L"X-A", L"x-a", L"X-B" };
         static const wchar_t *const values[] = { L"v", L"V", L"w" };

         std::shared_ptr<IMAPSearchCriteria> criteria = std::shared_ptr<IMAPSearchCriteria>(new IMAPSearchCriteria);
         criteria->SetPositive(NextSearchRandom_(state) % 4 != 0);

         std::vector<String> set;
         set.push_back(elements[NextSearchRandom_(state) % 7]);
         if (NextSearchRandom_(state) % 3 == 0)
            set.push_back(elements[NextSearchRandom_(state) % 7]);

         switch (NextSearchRandom_(state) % 11)
         {
         case 0: criteria->SetType(IMAPSearchCriteria::CTSeen); break;
         case 1: criteria->SetType(IMAPSearchCriteria::CTUnseen); break;
         case 2: criteria->SetType(IMAPSearchCriteria::CTDeleted); break;
         case 3: criteria->SetType(IMAPSearchCriteria::CTFlagged); break;
         case 4: criteria->SetType(IMAPSearchCriteria::CTAll); break;
         case 5: criteria->SetType(IMAPSearchCriteria::CTUID); criteria->SetSequenceSet(saved); break;
         case 6: criteria->SetType(IMAPSearchCriteria::CTUID); criteria->SetSequenceSet(set); break;
         case 7: criteria->SetType(IMAPSearchCriteria::CTSequenceSet); criteria->SetSequenceSet(set); break;
         case 8:
            criteria->SetType(NextSearchRandom_(state) % 2 == 0 ? IMAPSearchCriteria::CTLarger : IMAPSearchCriteria::CTSmaller);
            criteria->SetText(sizes[NextSearchRandom_(state) % 4]);
            break;
         case 9:
            criteria->SetType(IMAPSearchCriteria::CTSubject);
            criteria->SetText(subjects[NextSearchRandom_(state) % 5]);
            break;
         default:
            criteria->SetType(IMAPSearchCriteria::CTHeader);
            criteria->SetHeaderField(fields[NextSearchRandom_(state) % 3]);
            criteria->SetText(values[NextSearchRandom_(state) % 3]);
            break;
         }

         return criteria;
      }

      // A criterion of the generator's: one made before, again, a key, or an OR of
      // two such - nested no deeper than depth.
      std::shared_ptr<IMAPSearchCriteria> MakeCriterion_(unsigned long long &state, int depth,
                                                         const std::shared_ptr<const std::vector<String> > &saved,
                                                         std::vector<std::shared_ptr<IMAPSearchCriteria> > &made)
      {
         std::shared_ptr<IMAPSearchCriteria> criteria;
         const unsigned long long choice = NextSearchRandom_(state) % 10;

         if (choice < 3 && !made.empty())
            criteria = Copy_(*made[NextSearchRandom_(state) % made.size()], saved);
         else if (choice < 6 && depth > 0)
         {
            criteria = std::shared_ptr<IMAPSearchCriteria>(new IMAPSearchCriteria);
            criteria->SetType(IMAPSearchCriteria::CTSubCriteria);
            criteria->SetIsOR(true);
            criteria->GetSubCriterias().push_back(MakeCriterion_(state, depth - 1, saved, made));
            criteria->GetSubCriterias().push_back(NextSearchRandom_(state) % 3 == 0 ?
               Copy_(*criteria->GetSubCriterias().front(), saved) : MakeCriterion_(state, depth - 1, saved, made));
         }
         else
            criteria = MakeKey_(state, saved);

         made.push_back(criteria);
         return criteria;
      }

      // A key's name in a case of the generator's choosing: the grammar's names
      // are the same in any case.
      String AnyCase_(unsigned long long &state, const String &name)
      {
         String spelt = name;
         for (int i = 0; i < spelt.GetLength(); i++)
         {
            if (NextSearchRandom_(state) % 2 == 0)
               spelt.SetAt(i, (wchar_t) std::towlower((wint_t) spelt.GetAt(i)));
         }
         return spelt;
      }

      // A criterion as a client writes it: "$" for the saved result.
      String Write_(unsigned long long &state, const IMAPSearchCriteria &criteria, const std::shared_ptr<const std::vector<String> > &saved)
      {
         String text = criteria.GetPositive() ? String() : AnyCase_(state, _T("NOT")) + _T(" ");
         const String set = StringParser::JoinVector(criteria.GetSequenceSet(), _T(","));

         switch (criteria.GetType())
         {
         case IMAPSearchCriteria::CTSubCriteria:
            text += AnyCase_(state, _T("OR"));
            for (const std::shared_ptr<IMAPSearchCriteria> &sub : criteria.GetSubCriterias())
               text += _T(" ") + Write_(state, *sub, saved);
            return text;
         case IMAPSearchCriteria::CTUID:
            if (&criteria.GetSequenceSet() == saved.get())
               return text + _T("$");
            return text + AnyCase_(state, _T("UID")) + _T(" ") + set;
         case IMAPSearchCriteria::CTSequenceSet:
            return text + set;
         case IMAPSearchCriteria::CTHeader:
            return text + AnyCase_(state, _T("HEADER")) + _T(" ") + criteria.GetHeaderField() + _T(" ") + criteria.GetText();
         case IMAPSearchCriteria::CTLarger:
         case IMAPSearchCriteria::CTSmaller:
         case IMAPSearchCriteria::CTSubject:
            return text + AnyCase_(state, KeyName_(criteria)) + _T(" ") +
                   (criteria.GetText().IsEmpty() ? String(_T("\"\"")) : criteria.GetText());
         default:
            return text + AnyCase_(state, KeyName_(criteria));
         }
      }
   }

   void
   IMAPSearchParserTester::Test(int cases)
   {
      const std::vector<__int64> saved = { 2, 4, 6 };

      // What the parser makes of commands: each criterion once, in the place it was
      // first named, and nothing merged that is not the same.
      {
         struct Case
         {
            IMAPSearchCommandMode mode;
            const wchar_t *command;
            const wchar_t *literal;
            const wchar_t *made;
         };

         static const Case commands[] =
         {
            // The same key, named again: by "$", in another case, with its NOT.
            { IMAPSearchModeSearch, L"$ $ $", nullptr, L"UID 2,4,6" },
            { IMAPSearchModeSearch, L"$", nullptr, L"UID 2,4,6" },
            { IMAPSearchModeSearch, L"SEEN seen Seen sEEN", nullptr, L"SEEN" },
            { IMAPSearchModeSearch, L"NOT SEEN NOT SEEN not seen", nullptr, L"NOT SEEN" },
            { IMAPSearchModeSearch, L"NOT $ $ NOT $", nullptr, L"NOT UID 2,4,6 UID 2,4,6" },
            { IMAPSearchModeSearch, L"DELETED $ SEEN $ DELETED SEEN", nullptr, L"DELETED UID 2,4,6 SEEN" },
            { IMAPSearchModeSearch, L"FLAGGED  FLAGGED", nullptr, L"FLAGGED" },
            { IMAPSearchModeSearch, L"CHARSET UTF-8 SUBJECT x SUBJECT x", nullptr, L"SUBJECT \"x\"" },
            { IMAPSearchModeSearch, L"subject foo SUBJECT foo", nullptr, L"SUBJECT \"foo\"" },
            { IMAPSearchModeSearch, L"MODSEQ 5 MODSEQ 5 MODSEQ 6", nullptr, L"MODSEQ \"5\" MODSEQ \"6\"" },
            { IMAPSearchModeSearch, L"UID 1:5 UID 1:5 UID 5:1 1:5 1:5", nullptr, L"UID 1:5 UID 5:1 SET 1:5" },

            // A set the same as the saved result's is the same set; one that
            // begins as it does, or goes on beyond it, is not.
            { IMAPSearchModeSearch, L"$ UID 2,4,6 UID 2,4 UID 2,4,6,8 $ UID 2,4", nullptr, L"UID 2,4,6 UID 2,4 UID 2,4,6,8" },

            // The last "$" or flag found again knows the next of its key, its NOT
            // and its very set at once, and nothing else: not another NOT, another
            // set or a key with a value.
            { IMAPSearchModeSearch, L"$ $ NOT $ $", nullptr, L"UID 2,4,6 NOT UID 2,4,6" },
            { IMAPSearchModeSearch, L"$ $ UID 9 $", nullptr, L"UID 2,4,6 UID 9" },
            { IMAPSearchModeSearch, L"SUBJECT \"\" SUBJECT \"\" SUBJECT x", nullptr, L"SUBJECT SUBJECT \"x\"" },

            // The same value as a literal, an atom and a quoted string.
            { IMAPSearchModeSearch, L"TEXT {3} TEXT abc TEXT \"abc\"", L"abc", L"TEXT \"abc\"" },

            // Not the same: a NOT, a value's case, a field name's case, another
            // spelling of the same set or number.
            { IMAPSearchModeSearch, L"SEEN NOT SEEN", nullptr, L"SEEN NOT SEEN" },
            { IMAPSearchModeSearch, L"SUBJECT foo SUBJECT FOO subject foo", nullptr, L"SUBJECT \"foo\" SUBJECT \"FOO\"" },
            { IMAPSearchModeSearch, L"HEADER X-A v HEADER x-a v HEADER X-A v HEADER X-A V", nullptr, L"HEADER [X-A] \"v\" HEADER [x-a] \"v\" HEADER [X-A] \"V\"" },
            { IMAPSearchModeSearch, L"KEYWORD a KEYWORD A keyword a", nullptr, L"KEYWORD \"a\" KEYWORD \"A\"" },
            { IMAPSearchModeSearch, L"UID 1,2,3 UID 1:3", nullptr, L"UID 1,2,3 UID 1:3" },
            { IMAPSearchModeSearch, L"LARGER 10 LARGER 010 LARGER 10", nullptr, L"LARGER \"10\" LARGER \"010\"" },

            // An OR keeps its structure: the same two made one, each OR otherwise
            // as it was, the same OR beside it made one, none folded into the list.
            { IMAPSearchModeSearch, L"OR $ $", nullptr, L"OR (UID 2,4,6)" },
            { IMAPSearchModeSearch, L"OR SEEN DELETED OR SEEN DELETED OR DELETED SEEN", nullptr, L"OR (SEEN DELETED) OR (DELETED SEEN)" },
            { IMAPSearchModeSearch, L"OR SEEN DELETED SEEN", nullptr, L"OR (SEEN DELETED) SEEN" },
            { IMAPSearchModeSearch, L"SEEN OR SEEN SEEN", nullptr, L"SEEN OR (SEEN)" },
            { IMAPSearchModeSearch, L"OR OR $ $ OR $ $ ALL", nullptr, L"OR (OR (UID 2,4,6)) ALL" },
            { IMAPSearchModeSearch, L"OR NOT SEEN NOT SEEN", nullptr, L"OR (NOT SEEN)" },

            // A key the command kept, named again in an OR and then after it: what
            // the OR keeps of its own leaves the command's choice as it was.
            { IMAPSearchModeSearch, L"$ OR $ SEEN $", nullptr, L"UID 2,4,6 OR (UID 2,4,6 SEEN)" },
            { IMAPSearchModeSearch, L"SEEN OR SEEN DELETED SEEN OR SEEN DELETED", nullptr, L"SEEN OR (SEEN DELETED)" },

            // Lists, nested: their keys stand in their place.
            { IMAPSearchModeSearch, L"($ $) (($) $) $", nullptr, L"UID 2,4,6" },
            { IMAPSearchModeSearch, L"(SEEN (DELETED SEEN)) DELETED ((SEEN))", nullptr, L"SEEN DELETED" },
            { IMAPSearchModeSearch, L"OR (SEEN) (SEEN)", nullptr, L"OR (SEEN)" },

            // SORT and THREAD, whose criteria are parsed alike.
            { IMAPSearchModeSort, L"(DATE) UTF-8 $ $ SEEN SEEN", nullptr, L"UID 2,4,6 SEEN" },
            { IMAPSearchModeThread, L"REFERENCES UTF-8 ALL ALL all", nullptr, L"ALL" },
         };

         for (const Case &one : commands)
         {
            std::vector<String> literals;
            if (one.literal)
               literals.push_back(one.literal);

            const String made = DescribeList_(*ParseForTest_(one.command, one.mode, saved, literals));
            if (!SameText_(made, one.made))
               throw std::logic_error("The search parser made \"" + Narrow_(made) + "\" of \"" + Narrow_(one.command) +
                  "\", where \"" + Narrow_(one.made) + "\" was expected");
         }

         // An empty saved result is the set no message is in, once.
         if (!SameText_(DescribeList_(*ParseForTest_(_T("$ $"), IMAPSearchModeSearch, std::vector<__int64>())), _T("UID 0")))
            HM_TEST_FAILED();
      }

      // The time: a hundred thousand "$" over a saved result of 5,000 - one key, the
      // one set - and a hundred thousand keys each other than the rest, all kept...
      {
         std::vector<__int64> many;
         for (__int64 uid = 1; uid <= 5000; uid++)
            many.push_back(uid * 2);

         String command;
         for (int i = 0; i < 100000; i++)
            command += i == 0 ? _T("$") : _T(" $");

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         std::shared_ptr<IMAPSearchCriteria> criteria = ParseForTest_(command, IMAPSearchModeSearch, many);
         double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (criteria->GetSubCriterias().size() != 1 || criteria->GetSubCriterias().front()->GetSequenceSet().size() != 5000)
            throw std::logic_error("A hundred thousand \"$\" were parsed into " + std::to_string((long long) criteria->GetSubCriterias().size()) + " criteria, not one");

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("A hundred thousand \"$\" took " + std::to_string((long long) milliseconds) + " ms to parse");

         command.Empty();
         for (int i = 1; i <= 100000; i++)
         {
            String key;
            key.Format(i == 1 ? _T("UID %d") : _T(" UID %d"), i);
            command += key;
         }

         started = std::chrono::steady_clock::now();
         criteria = ParseForTest_(command, IMAPSearchModeSearch, saved);
         milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (criteria->GetSubCriterias().size() != 100000 ||
             !SameText_(criteria->GetSubCriterias().back()->GetSequenceSet().front(), _T("100000")))
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(4000))
            throw std::logic_error("A hundred thousand different UID keys took " + std::to_string((long long) milliseconds) + " ms to parse");
      }

      // ...two thousand ORs nested forty deep, each made of "$" alone - the first
      // left, made what the "$" make of it - and four TEXT literals of a million
      // characters, two of them the same and two other only in their last.
      {
         String chain;
         for (int i = 0; i < 40; i++)
            chain += _T("OR $ ");
         chain += _T("$");

         String command;
         for (int i = 0; i < 2000; i++)
         {
            if (i > 0)
               command += _T(" ");
            command += chain;
         }

         std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         std::shared_ptr<IMAPSearchCriteria> criteria = ParseForTest_(command, IMAPSearchModeSearch, saved);
         double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (criteria->GetSubCriterias().size() != 1)
            HM_TEST_FAILED();

         // Each OR keeps its "$" and the OR under it; the deepest, of two "$", keeps one.
         int depth = 0;
         std::shared_ptr<IMAPSearchCriteria> level = criteria->GetSubCriterias().front();
         while (level->GetType() == IMAPSearchCriteria::CTSubCriteria)
         {
            depth++;
            const size_t expected = depth == 40 ? 1 : 2;
            if (level->GetSubCriterias().size() != expected || level->GetSubCriterias().front()->GetType() != IMAPSearchCriteria::CTUID)
               HM_TEST_FAILED();
            level = level->GetSubCriterias().back();
         }

         if (depth != 40)
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(4000))
            throw std::logic_error("Two thousand ORs nested forty deep took " + std::to_string((long long) milliseconds) + " ms to parse");

         const String long_text(1000000, L'q');
         String other = long_text;
         other.SetAt(other.GetLength() - 1, L'r');

         std::vector<String> literals;
         literals.push_back(long_text);
         literals.push_back(other);
         literals.push_back(long_text);
         literals.push_back(String(other));

         started = std::chrono::steady_clock::now();
         criteria = ParseForTest_(_T("TEXT {1000000} TEXT {1000000} TEXT {1000000} TEXT {1000000}"), IMAPSearchModeSearch, saved, literals);
         milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

         if (criteria->GetSubCriterias().size() != 2 ||
             !SameText_(criteria->GetSubCriterias().front()->GetText(), long_text) ||
             !SameText_(criteria->GetSubCriterias().back()->GetText(), other))
            HM_TEST_FAILED();

         if (milliseconds > HM_TEST_TIME_BOUND(2000))
            throw std::logic_error("Four TEXT literals of a million characters took " + std::to_string((long long) milliseconds) + " ms to parse");
      }

      // Then generated trees, with keys named again at every level, against the
      // removal done the slow way, and what they match before and after over a
      // generated mailbox; and the same trees written as a client writes them -
      // names in any case, lists around runs of the command's own keys - parsed.
      unsigned long long state = 0x3C6EF372FE94F82BULL;

      for (int number = 0; number < cases; number++)
      {
         // The mailbox, and the result its "$" stands for.
         std::vector<TestMessage> messages;
         std::vector<__int64> savedUids;
         const int count = (int) (NextSearchRandom_(state) % 9);
         __int64 uid = 0;
         for (int i = 0; i < count; i++)
         {
            static const wchar_t *const subjects[] = { L"a", L"AB", L"b", L"x" };
            static const wchar_t *const fields[] = { L"", L"X-A", L"x-a" };
            static const wchar_t *const values[] = { L"v", L"W", L"" };
            static const int sizes[] = { 5, 10, 25, 1000, 2000 };

            uid += 1 + (__int64) (NextSearchRandom_(state) % 3);

            TestMessage message;
            message.seen = NextSearchRandom_(state) % 2 == 0;
            message.deleted = NextSearchRandom_(state) % 3 == 0;
            message.flagged = NextSearchRandom_(state) % 2 == 0;
            message.uid = uid;
            message.sequence = i + 1;
            message.size = sizes[NextSearchRandom_(state) % 5];
            message.subject = subjects[NextSearchRandom_(state) % 4];
            message.field = fields[NextSearchRandom_(state) % 3];
            message.value = values[NextSearchRandom_(state) % 3];
            messages.push_back(message);

            if (NextSearchRandom_(state) % 2 == 0)
               savedUids.push_back(uid);
         }

         // The saved result as the parser holds it for "$" (ParseWord_).
         std::vector<String> savedElements;
         for (__int64 savedUid : savedUids)
         {
            String element;
            element.Format(_T("%I64d"), savedUid);
            savedElements.push_back(element);
         }
         if (savedElements.empty())
            savedElements.push_back(_T("0"));
         const std::shared_ptr<const std::vector<String> > savedSet = std::make_shared<const std::vector<String> >(savedElements);

         // The command's criteria.
         IMAPSearchCriteria tree;
         std::vector<std::shared_ptr<IMAPSearchCriteria> > made;
         const int keys = 1 + (int) (NextSearchRandom_(state) % 10);
         for (int i = 0; i < keys; i++)
            tree.GetSubCriterias().push_back(MakeCriterion_(state, 3, savedSet, made));

         std::shared_ptr<IMAPSearchCriteria> removed = Copy_(tree, savedSet);
         IMAPSearchParser::RemoveRepeatedCriteria(*removed);

         std::shared_ptr<IMAPSearchCriteria> slowly = Copy_(tree, savedSet);
         RemoveRepeatedSlowly_(*slowly);

         if (!Same_(*removed, *slowly))
            throw std::logic_error("Repeated criteria were removed other than one by one in case " + std::to_string(number) +
               ": \"" + Narrow_(DescribeList_(tree)) + "\" was made \"" + Narrow_(DescribeList_(*removed)) +
               "\", one by one \"" + Narrow_(DescribeList_(*slowly)) + "\"");

         const __int64 maxUid = messages.empty() ? 0 : messages.back().uid;
         for (const TestMessage &message : messages)
         {
            if (MatchesList_(tree, message, maxUid, count) != MatchesList_(*removed, message, maxUid, count))
               throw std::logic_error("Repeated criteria removed changed what a message matches in case " + std::to_string(number));
         }

         // As a client writes it, with lists around runs of the command's own keys,
         // some within others.
         String command;
         int open = 0;
         const std::vector<std::shared_ptr<IMAPSearchCriteria> > &criterias = tree.GetSubCriterias();
         for (size_t i = 0; i < criterias.size(); i++)
         {
            if (!command.IsEmpty())
               command += _T(" ");

            while (NextSearchRandom_(state) % 4 == 0)
            {
               command += _T("(");
               open++;
            }

            command += Write_(state, *criterias[i], savedSet);

            while (open > 0 && NextSearchRandom_(state) % 3 == 0)
            {
               command += _T(")");
               open--;
            }
         }
         while (open-- > 0)
            command += _T(")");

         const std::shared_ptr<IMAPSearchCriteria> parsed = ParseForTest_(command, IMAPSearchModeSearch, savedUids);
         if (!Same_(*parsed, *slowly))
            throw std::logic_error("The search parser made \"" + Narrow_(DescribeList_(*parsed)) + "\" of \"" + Narrow_(command) +
               "\" in case " + std::to_string(number) + ", where \"" + Narrow_(DescribeList_(*slowly)) + "\" was expected");
      }
   }
}