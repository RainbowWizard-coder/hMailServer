// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{

   class IMAPCommandArgument;
   class IMAPSimpleCommandParser;
   class IMAPSortParser;

   class IMAPSearchCriteria
   {
   public:

      enum CriteriaType
      {
         CTUnknown = 0,
         CTHeader = 1,
         CTUID = 4,
         CTDeleted = 5,
         CTRecent = 8,
         CTUndeleted = 10,
         CTSeen = 11,
         CTUnseen = 12,
         CTText = 13,
         CTAll = 14,
         CTBody = 15,
         CTSentOn = 16,
         CTSentBefore = 17,
         CTSentSince = 18,
         CTSubject = 19,
         CTFrom = 20,
         CTTo = 21,
         CTCC = 22,
         CTSince = 23,
         
            
         CTAnswered = 24,
         CTBefore = 25,
         CTDraft = 26,
         CTFlagged = 27,
         CTLarger = 28,
         CTNew = 29,
         CTOld = 30,
         CTSmaller = 31,
         CTUnanswered = 32,
         CTUndraft = 33,
         CTUnflagged = 34,
         CTOn = 35,

         CTSequenceSet = 36,

         // RFC 7162 (CONDSTORE): MODSEQ search key (matches messages whose mod-sequence
         // is greater than or equal to the supplied value).
         CTModSeq = 37,

         CTCharset = 40,

         // RFC 3501 base search keys. No keyword can ever be stored - messageflags is a
         // fixed 8-bit bitmask with no room for client-defined flags and SELECT
         // advertises no \* in PERMANENTFLAGS - so KEYWORD matches nothing and
         // UNKEYWORD matches everything. That is exact rather than approximate, and it
         // is a great deal better than what these used to do, which was to fall through
         // as unrecognised words and leave the search matching every message.
         CTKeyword = 41,
         CTUnkeyword = 42,

         // RFC 5032 (WITHIN): relative-age keys measured against the internal date.
         CTOlder = 43,
         CTYounger = 44,

         // RFC 8514 (SAVEDATE): date keys measured against the save date - when
         // the message was saved into this mailbox - not the internal date.
         CTSavedBefore = 45,
         CTSavedOn = 46,
         CTSavedSince = 47,
         CTSavedateSupported = 48,

         // RFC 8474 (OBJECTID): match by the stable email id. THREADID exists
         // as a key so it parses, and matches nothing - this server has no
         // thread ids.
         CTEmailId = 49,
         CTThreadId = 50,

         // RFC 3501 6.4.4: BCC, a base search key that was never parsed - a
         // SEARCH BCC was answered "BAD Unrecognized search key".
         CTBCC = 51,

         CTSubCriteria = 100
      };

      IMAPSearchCriteria() : 
         uidlower_(-1),
         uidupper_(-1),
         positive_(true),
         is_or_(false),
         type_(CTUnknown) {};

      // What a criterion is made of - and so what makes two criteria the same
      // (IMAPSearchParser::RemoveRepeatedCriteria): a part added here is a part
      // RepeatedCriteria, in IMAPSearchParser.cpp, has to compare as well.
      long GetUIDLower() const {return uidlower_; }
      void SetUIDLower(long lNewVal) {uidlower_ = lNewVal; }

      long GetUIDUpper() const {return uidupper_; }
      void SetUIDUpper(long lNewVal) {uidupper_ = lNewVal; }

      const String &GetText() const {return text_; }
      void SetText(const String &sText) {text_ = sText; }

      bool GetPositive() const {return positive_; }
      void SetPositive(bool bNewVal) {positive_ = bNewVal; }

      bool GetIsOR() const {return is_or_; }
      void SetIsOR(bool bNewVal) {is_or_ = bNewVal; }

      void SetHeaderField(const String &sField) {header_field_ = sField;}
      const String &GetHeaderField() const {return header_field_;}

      CriteriaType GetType() const {return type_;}
      void SetType(CriteriaType newVal) {type_ = newVal; }

      static CriteriaType GetCriteriaTypeByName(const String &sName);

      std::vector<std::shared_ptr<IMAPSearchCriteria> > &GetSubCriterias() {return sub_criterias_;}
      const std::vector<std::shared_ptr<IMAPSearchCriteria> > &GetSubCriterias() const {return sub_criterias_;}

      // The elements of the set, held shared: every "$" of one SEARCH holds the
      // one copy of the saved result (IMAPSearchParser::ParseWord_), where each
      // used to hold a copy of its own - a hundred thousand "$" in a command
      // line, a hundred thousand copies of a saved result of thousands.
      void SetSequenceSet(std::vector<String> newVal) {sequence_set_ = std::make_shared<const std::vector<String> >(std::move(newVal));}
      void SetSequenceSet(const std::shared_ptr<const std::vector<String> > &shared) {sequence_set_ = shared;}
      const std::vector<String> &GetSequenceSet() const {return sequence_set_ ? *sequence_set_ : EmptySequenceSet_();}

   private:

      static bool IsSequenceSet_(const String &item);

      static const std::vector<String> &EmptySequenceSet_()
      {
         static const std::vector<String> empty;
         return empty;
      }

      long uidlower_;
      long uidupper_;
      String text_;
      bool positive_;
      CriteriaType type_;

      String header_field_;
      std::vector<std::shared_ptr<IMAPSearchCriteria> > sub_criterias_;
      std::shared_ptr<const std::vector<String> > sequence_set_;

      bool is_or_;
   };


   // Which command's grammar the criteria are wrapped in. An enum rather than the
   // bool it grew from, because THREAD made three of them and ParseCommand(pArg,
   // true) says nothing about which prefix "true" buys: SORT's prefix is a
   // parenthesised sort-key list then a charset, THREAD's is a bare algorithm atom
   // then a charset, and SEARCH has no prefix at all.
   enum IMAPSearchCommandMode
   {
      IMAPSearchModeSearch = 0,
      IMAPSearchModeSort = 1,
      IMAPSearchModeThread = 2
   };

   class IMAPSearchParser
   {
   public:
      IMAPSearchParser();
      virtual ~IMAPSearchParser();

      IMAPResult ParseCommand(std::shared_ptr<IMAPCommandArgument> pArgument, IMAPSearchCommandMode mode);

      // Of criteria named more than once side by side - among the keys of the
      // command, every one of which a message must match, and between the two of an
      // OR - the first stays where it stands and the others are left out, so that each
      // is asked of a message once. ParseCommand does it to every SEARCH, SORT and
      // THREAD it parses; IMAPSearchParser.cpp (RepeatedCriteria) says what the same
      // is.
      static void RemoveRepeatedCriteria(IMAPSearchCriteria &criteria);

      // RFC 5182 (SEARCHRES): the UIDs saved by an earlier SEARCH RETURN (SAVE), so the
      // "$" search key can be resolved during parsing. Must be set before ParseCommand.
      void SetSavedSearchResult(const std::vector<__int64> &uids) { saved_search_result_ = uids; }

      std::shared_ptr<IMAPSearchCriteria>  GetCriteria() {return result_criteria_;}
      std::shared_ptr<IMAPSortParser> GetSortParser() {return sort_parser_; }

      // The algorithm atom a THREAD command led with, exactly as the client sent
      // it. Stored rather than validated here: the parser's job is the grammar,
      // and naming an algorithm this server does not implement is the command's
      // BAD to give, with IMAPThread::ParseAlgorithm as the authority.
      String GetThreadAlgorithm() const { return thread_algorithm_; }

      String GetCharsetName() 
      {
         return charset_name_; 
      }

   private:

      bool IsValidCharset_(const String &charsetName);
      static IMAPResult BadCharsetResult_();
      bool NeedsDecoding_(IMAPSearchCriteria::CriteriaType criteriaType);
      String DecodeWordAccordingToCharset_(const String &inputValue);

      static bool IsNonZeroNumber_(const String &value);

      IMAPResult ParseSegment_(std::shared_ptr<IMAPSimpleCommandParser> pSimpleParser, int &currentWord, std::shared_ptr<IMAPSearchCriteria> pCriteria, int iRecursion);

      IMAPResult ParseWord_(std::shared_ptr<IMAPSimpleCommandParser> pSimpleParser, std::shared_ptr<IMAPSearchCriteria> pNewCriteria, int &iCurrentWord);

      std::shared_ptr<IMAPSortParser> sort_parser_;
      String thread_algorithm_;
      std::shared_ptr<IMAPSearchCriteria> result_criteria_;

      String charset_name_;
      std::vector<__int64> saved_search_result_;

      // The saved result as a set's elements, made for the first "$" of the
      // command and shared by every one after it.
      std::shared_ptr<const std::vector<String> > saved_set_;
   };

   // The criteria a SEARCH, SORT or THREAD is parsed into, with each criterion
   // named again beside itself left out (IMAPSearchParser::RemoveRepeatedCriteria):
   // commands with the tree each makes, generated trees against a removal that
   // compares every pair, and what they match, before and after, and the time.
   // Runs in ClassTester on Windows and imap-parse-tests on Linux.
   class IMAPSearchParserTester
   {
   public:
      void Test(int cases = 2000);
   };

}