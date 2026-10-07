// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./PersistentMessageIndex.h"
#include "./PersistentMessage.h"
#include "../BO/Message.h"
#include "../Application/IniFileSettings.h"
#include "../SQL/DatabaseSettings.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Long tokens are stored as overlapping MaxTermLength windows stepped by
      // this. The containment guarantee needs
      //    step <= MaxTermLength - MaxQueryNeedleLength + 1  (= 49)
      // and 40 leaves slack for the surrogate-preserving boundary trims below.
      const int ChunkStep = 40;

      inline bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
      inline bool IsLowSurrogate(wchar_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }
   }

   const wchar_t *
   PersistentMessageIndex::OverflowMarker()
   {
      // '\x01' cannot be produced by the tokeniser, so no content term can
      // collide with a marker.
      return L"\x01" L"fto";
   }

   const wchar_t *
   PersistentMessageIndex::CompleteMarker()
   {
      return L"\x01" L"ftc";
   }

   bool
   PersistentMessageIndex::IsTokenChar_(wchar_t c)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The tokeniser's character class: split ONLY on ASCII non-alphanumerics.
   // Everything above 127 stays inside tokens. That choice is what makes the
   // filter sound: it is locale-independent, and it guarantees that every
   // maximal ASCII-alphanumeric run of any text - which is all the searcher
   // ever queries on - lies inside a single token, because the query class
   // (IsAsciiAlnum_) is a subset of this one. The cost is that unspaced CJK
   // text and Unicode-punctuated prose form long merged tokens; the window
   // chunking absorbs those.
   //---------------------------------------------------------------------------()
   {
      if (c >= L'0' && c <= L'9') return true;
      if (c >= L'a' && c <= L'z') return true;
      if (c >= L'A' && c <= L'Z') return true;
      return c > 127;
   }

   bool
   PersistentMessageIndex::IsAsciiAlnum_(wchar_t c)
   {
      if (c >= L'0' && c <= L'9') return true;
      if (c >= L'a' && c <= L'z') return true;
      if (c >= L'A' && c <= L'Z') return true;
      return false;
   }

   void
   PersistentMessageIndex::TokenizeForIndex(const String &content, size_t maxDistinctTerms, std::set<String> &terms, bool &overflowed)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The distinct terms of one message's searchable text. The content MUST be
   // the exact string the scan searches (header + body + HTML body, in that
   // order, with nothing inserted between body and HTML body - BODY searches
   // their direct concatenation, so a token straddling that junction is real
   // and must be indexed as the merged token it is).
   //
   // Lowered with String::ToLower - the very function ContainsNoCase applies
   // to both sides during the scan - so the index's idea of case-insensitivity
   // is the scan's, character for character, and no exotic case pair can make
   // the index exclude a message the scan would match.
   //---------------------------------------------------------------------------()
   {
      overflowed = false;

      String lowered = content;
      lowered.ToLower();

      const wchar_t *data = lowered.c_str();
      const size_t length = (size_t) lowered.GetLength();

      size_t tokenStart = 0;
      bool inToken = false;

      for (size_t i = 0; i <= length; i++)
      {
         const bool isTokenChar = (i < length) && IsTokenChar_(data[i]);

         if (isTokenChar)
         {
            if (!inToken)
            {
               inToken = true;
               tokenStart = i;
            }
            continue;
         }

         if (!inToken)
            continue;

         inToken = false;
         const size_t tokenEnd = i;
         const size_t tokenLength = tokenEnd - tokenStart;

         // Dropping short tokens is sound because the searcher never uses a
         // needle shorter than IndexTimeMinTokenLength: a needle of length n
         // can only occur inside a token of length >= n.
         if (tokenLength < (size_t) IndexTimeMinTokenLength)
            continue;

         if (tokenLength <= (size_t) MaxTermLength)
         {
            terms.insert(lowered.Mid((int) tokenStart, (int) tokenLength));
         }
         else
         {
            // Overlapping windows: any substring of up to MaxQueryNeedleLength
            // characters of this token lies whole inside one window, so a LIKE
            // over the windows finds everything a LIKE over the token would.
            for (size_t windowStart = tokenStart; ; windowStart += ChunkStep)
            {
               size_t start = windowStart;
               size_t end = windowStart + (size_t) MaxTermLength;
               if (end > tokenEnd)
                  end = tokenEnd;

               // Never split a surrogate pair across a window edge: a half
               // pair is not a valid string, and some engines mangle or
               // reject it. The step of 40 leaves room for these one-unit
               // trims without breaking the containment guarantee.
               if (start > tokenStart && IsLowSurrogate(data[start]))
                  start++;
               if (end < tokenEnd && end > start && IsHighSurrogate(data[end - 1]))
                  end--;

               if (end > start && (end - start) >= (size_t) IndexTimeMinTokenLength)
                  terms.insert(lowered.Mid((int) start, (int) (end - start)));

               if (windowStart + (size_t) MaxTermLength >= tokenEnd)
                  break;

               if (terms.size() > maxDistinctTerms)
                  break;
            }
         }

         if (terms.size() > maxDistinctTerms)
         {
            // Too rich to index faithfully (a giant HTML message, an inline
            // base64 image). It gets the overflow marker instead of terms and
            // is scanned on every search - exactly today's behaviour.
            overflowed = true;
            terms.clear();
            return;
         }
      }
   }

   void
   PersistentMessageIndex::CreateQueryNeedles(const String &searchText, int minRunLength, std::vector<String> &needles)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The LIKE needles for one search text. Only maximal ASCII-alphanumeric
   // runs are used: an ASCII run of the search text must, in any message
   // containing the text, lie inside a single token (the query class is a
   // subset of the token class), so "no stored term contains the run" proves
   // "the message does not contain the text". Runs containing characters above
   // 127 are simply not used - skipping a run only widens the candidate set.
   //
   // Needles are clipped to MaxQueryNeedleLength (a prefix of a run is still a
   // substring of any text containing the run) so the window chunking of long
   // tokens stays sufficient. Longest runs first - they are the most
   // selective - and at most four, since each costs one scan of the account's
   // terms.
   //---------------------------------------------------------------------------()
   {
      needles.clear();

      if (minRunLength < IndexTimeMinTokenLength)
         minRunLength = IndexTimeMinTokenLength;

      String lowered = searchText;
      lowered.ToLower();

      const wchar_t *data = lowered.c_str();
      const size_t length = (size_t) lowered.GetLength();

      std::set<String> distinctNeedles;

      size_t runStart = 0;
      bool inRun = false;

      for (size_t i = 0; i <= length; i++)
      {
         const bool isRunChar = (i < length) && IsAsciiAlnum_(data[i]);

         if (isRunChar)
         {
            if (!inRun)
            {
               inRun = true;
               runStart = i;
            }
            continue;
         }

         if (!inRun)
            continue;

         inRun = false;
         const size_t runLength = i - runStart;

         if (runLength < (size_t) minRunLength)
            continue;

         size_t needleLength = runLength;
         if (needleLength > (size_t) MaxQueryNeedleLength)
            needleLength = (size_t) MaxQueryNeedleLength;

         distinctNeedles.insert(lowered.Mid((int) runStart, (int) needleLength));
      }

      needles.assign(distinctNeedles.begin(), distinctNeedles.end());

      std::sort(needles.begin(), needles.end(),
         [](const String &first, const String &second)
         {
            if (first.GetLength() != second.GetLength())
               return first.GetLength() > second.GetLength();
            return first < second;
         });

      if (needles.size() > 4)
         needles.resize(4);
   }

   bool
   PersistentMessageIndex::InsertTerm_(__int64 messageID, __int64 accountID, const String &term)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // One term row, written only while the message it hangs off still exists,
   // in the one statement. A message expunged between the indexer's listing
   // and this save used to draw the foreign key's refusal - on PostgreSQL and
   // on SQL Server Compact alike - which the DAL had logged as HM5032 before
   // the caller could see that the message was gone; the hosted Linux run of
   // 14 September 2026 failed a test that had just expunged the message it
   // reported. Selecting the ids from hm_messages leaves the key check only
   // the instant between that select and the insert, and a message that is
   // gone inserts nothing and succeeds, which is the truth of it. The term is
   // a literal: SQL Server Compact allows no parameter in a select list. The
   // account is the message's own, which is what the caller passed.
   //---------------------------------------------------------------------------()
   {
      (void) accountID;

      // N'...' on SQL Server and Compact. A bare '...' constant is varchar in the
      // database's code page there, so a character the code page lacks was stored as
      // '?' - under the default SQL_Latin1_General_CP1_CI_AS "привет" became "??????"
      // and could never be found again. MySQL and PostgreSQL keep the plain quote,
      // where N'' is not Unicode's spelling.
      const DatabaseSettings::SQLDBType databaseType = IniFileSettings::Instance()->GetDatabaseType();
      const bool unicodeLiteral = databaseType == DatabaseSettings::TypeMSSQLServer ||
                                  databaseType == DatabaseSettings::TypeMSSQLCompactEdition;

      String sql = _T("insert into hm_messageindexterms (mitmessageid, mitaccountid, mitterm) select messageid, messageaccountid, ");
      sql += unicodeLiteral ? _T("N'") : _T("'");
      sql += SQLStatement::Escape(term);
      sql += _T("' from hm_messages where messageid = @MESSAGEID");

      SQLCommand command(sql);
      command.AddParameter("@MESSAGEID", messageID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentMessageIndex::SaveTermsForMessage(__int64 messageID, __int64 accountID, const std::set<String> &terms, bool overflowed)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Replaces the message's index rows. The CompleteMarker goes in LAST: its
   // presence is the searcher's proof that every content term is committed, so
   // an interruption anywhere in here leaves a message that is merely scanned,
   // never one that is wrongly excluded. The delete first makes the whole
   // operation idempotent for the resumed backfill.
   //---------------------------------------------------------------------------()
   {
      if (!DeleteForMessage(messageID))
         return false;

      if (overflowed)
      {
         // An overflowed message is a candidate for every search, so content
         // terms for it would be rows nothing ever reads.
         if (!InsertTerm_(messageID, accountID, OverflowMarker()))
            return false;
      }
      else
      {
         for (const String &term : terms)
         {
            if (!InsertTerm_(messageID, accountID, term))
               return false;
         }
      }

      return InsertTerm_(messageID, accountID, CompleteMarker());
   }

   bool
   PersistentMessageIndex::DeleteForMessage(__int64 messageID)
   {
      SQLCommand command("delete from hm_messageindexterms where mitmessageid = @MESSAGEID");
      command.AddParameter("@MESSAGEID", messageID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentMessageIndex::DeleteOrphanedItems()
   {
      SQLCommand command("delete from hm_messageindexterms where not exists (select messageid from hm_messages where messageid = mitmessageid)");

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   PersistentMessageIndex::Clear()
   {
      SQLCommand deleteTerms("delete from hm_messageindexterms");
      Application::Instance()->GetDBManager()->Execute(deleteTerms);

      SQLCommand deleteState("delete from hm_messageindexstate");
      Application::Instance()->GetDBManager()->Execute(deleteState);
   }

   __int64
   PersistentMessageIndex::GetBackfillCursor()
   {
      SQLCommand command("select mishighwatermark from hm_messageindexstate where misaccountid = 0");

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return 0;

      return pRS->GetInt64Value("mishighwatermark");
   }

   bool
   PersistentMessageIndex::SetBackfillCursor(__int64 highestMessageID)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Read-then-insert-or-update is race-free here because the indexer thread
   // is the only writer of the misaccountid = 0 row.
   //---------------------------------------------------------------------------()
   {
      SQLCommand existsCommand("select count(*) as c from hm_messageindexstate where misaccountid = 0");

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(existsCommand);
      if (!pRS)
         return false;

      if (pRS->GetLongValue("c") == 0)
      {
         SQLCommand insertCommand("insert into hm_messageindexstate (misaccountid, mishighwatermark) values (0, @HWM)");
         insertCommand.AddParameter("@HWM", highestMessageID);
         return Application::Instance()->GetDBManager()->Execute(insertCommand);
      }

      SQLCommand updateCommand("update hm_messageindexstate set mishighwatermark = @HWM where misaccountid = 0");
      updateCommand.AddParameter("@HWM", highestMessageID);
      return Application::Instance()->GetDBManager()->Execute(updateCommand);
   }

   __int64
   PersistentMessageIndex::GetFirstMessageIdAbove(__int64 lowExclusive)
   {
      SQLCommand command("select min(messageid) as m from hm_messages where messagetype = 2 and messageid > @LOW");
      command.AddParameter("@LOW", lowExclusive);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return 0;

      return pRS->GetInt64Value("m");
   }

   bool
   PersistentMessageIndex::ReadIndexableMessages_(SQLCommand &command, int maxCount, std::vector<IndexableMessage> &result)
   {
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return false;

      while (!pRS->IsEOF())
      {
         if (maxCount > 0 && (int) result.size() >= maxCount)
            break;

         IndexableMessage info;
         info.MessageID = pRS->GetInt64Value("messageid");
         info.AccountID = pRS->GetLongValue("messageaccountid");

         String accountAddress = pRS->GetStringValue("accountaddress");
         String fileName = pRS->GetStringValue("messagefilename");

         // The same path construction the metadata indexer uses: a dummy
         // message carrying what GetFileName reads. An empty account address
         // (a public-folder message) selects the public-folder path.
         std::shared_ptr<Message> dummyMessage = std::shared_ptr<Message>(new Message);
         dummyMessage->SetID(info.MessageID);
         dummyMessage->SetAccountID(info.AccountID);
         dummyMessage->SetFolderID(pRS->GetLongValue("messagefolderid"));
         dummyMessage->SetPartialFileName(fileName);

         info.FileName = PersistentMessage::GetFileName(accountAddress, dummyMessage);

         result.push_back(info);
         pRS->MoveNext();
      }

      return true;
   }

   bool
   PersistentMessageIndex::GetMessagesInRange(__int64 lowExclusive, __int64 highInclusive, std::vector<IndexableMessage> &result)
   {
      SQLCommand command(
         "select messageid, messageaccountid, messagefolderid, messagefilename, accountaddress "
         "from hm_messages left join hm_accounts on hm_messages.messageaccountid = hm_accounts.accountid "
         "where hm_messages.messagetype = 2 and hm_messages.messageid > @LOW and hm_messages.messageid <= @HIGH");
      command.AddParameter("@LOW", lowExclusive);
      command.AddParameter("@HIGH", highInclusive);

      return ReadIndexableMessages_(command, 0, result);
   }

   bool
   PersistentMessageIndex::GetUnindexedMessages(__int64 highInclusive, int maxCount, std::vector<IndexableMessage> &result)
   {
      // Keyed on the missing CompleteMarker rather than on having no rows at
      // all, so a message whose indexing was interrupted partway - which has
      // rows but no marker, and is therefore scanned on every search - is
      // found and finished rather than left that way forever.
      SQLCommand command(
         "select messageid, messageaccountid, messagefolderid, messagefilename, accountaddress "
         "from hm_messages left join hm_accounts on hm_messages.messageaccountid = hm_accounts.accountid "
         "where hm_messages.messagetype = 2 and hm_messages.messageid <= @HIGH "
         "and not exists (select mitmessageid from hm_messageindexterms where mitmessageid = hm_messages.messageid and mitterm = @COMPLETE)");
      command.AddParameter("@HIGH", highInclusive);
      command.AddParameter("@COMPLETE", String(CompleteMarker()));

      return ReadIndexableMessages_(command, maxCount, result);
   }

   __int64
   PersistentMessageIndex::GetLatestDeliveredMessageId()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The backfill frontier. Deliberately max over DELIVERED messages only:
   // advancing the cursor past a still-queued row whose id later flips to
   // delivered would leave that message waiting for the reconciliation pass
   // instead of the next batch.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command("select max(messageid) as m from hm_messages where messagetype = 2");

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return 0;

      return pRS->GetInt64Value("m");
   }

   bool
   PersistentMessageIndex::GetMessagesThatCannotContain(const std::vector<__int64> &messageIds, const std::vector<String> &needles, std::set<__int64> &excluded)
   {
      // No needle, nothing the index can prove.
      if (needles.empty() || needles.size() > 4)
         return true;

      for (size_t first = 0; first < messageIds.size(); first += MaxMessagesPerLookup)
      {
         const size_t last = std::min(messageIds.size(), first + MaxMessagesPerLookup);

         // The ids are numbers of this server's own, written into the text as
         // CalendarStore::FindByUidAmong writes its; the markers and the
         // needles are parameters, added in the order they are written, which
         // is the order SQL Server and SQL Server Compact substitute them in.
         // The needles are ASCII-alphanumeric by construction
         // (CreateQueryNeedles), so they carry no LIKE metacharacter, and at
         // most four, so no name below is the start of another.
         String sql = _T("select mitmessageid, mitterm from hm_messageindexterms where mitmessageid in (");
         for (size_t i = first; i < last; i++)
         {
            if (i > first)
               sql += _T(",");
            sql += StringParser::IntToString(messageIds[i]);
         }
         sql += _T(") and (mitterm = @COMPLETE or mitterm = @OVERFLOW");

         static const char *const needleNames[] = { "@NEEDLEA", "@NEEDLEB", "@NEEDLEC", "@NEEDLED" };
         for (size_t n = 0; n < needles.size(); n++)
            sql += _T(" or mitterm like ") + String(needleNames[n]);
         sql += _T(")");

         SQLCommand command(sql);
         command.AddParameter("@COMPLETE", String(CompleteMarker()));
         command.AddParameter("@OVERFLOW", String(OverflowMarker()));
         for (size_t n = 0; n < needles.size(); n++)
            command.AddParameter(needleNames[n], "%" + needles[n] + "%");

         std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!pRS)
            return false;

         std::vector<std::pair<__int64, String> > rows;
         while (!pRS->IsEOF())
         {
            rows.push_back(std::make_pair(pRS->GetInt64Value("mitmessageid"), pRS->GetStringValue("mitterm")));
            pRS->MoveNext();
         }

         pRS.reset();

         DecideExclusions(rows, needles, excluded);
      }

      return true;
   }

   void
   PersistentMessageIndex::DecideExclusions(const std::vector<std::pair<__int64, String> > &rows, const std::vector<String> &needles, std::set<__int64> &excluded)
   {
      if (needles.empty() || needles.size() > 32)
         return;

      const String complete = CompleteMarker();
      const String overflow = OverflowMarker();

      // Per message: complete, overflowed, and which needles some term holds.
      struct State
      {
         bool complete = false;
         bool overflow = false;
         unsigned int found = 0;
      };

      std::map<__int64, State> states;
      for (const std::pair<__int64, String> &row : rows)
      {
         State &state = states[row.first];

         if (row.second == complete)
         {
            state.complete = true;
            continue;
         }

         if (row.second == overflow)
         {
            state.overflow = true;
            continue;
         }

         // The terms are stored lowered and the needles are lowered ASCII, so
         // an exact find is the scan's own test: a needle in the message lies
         // whole inside one stored term (the class comment). A row the
         // database matched on its collation alone holds no needle here.
         for (size_t n = 0; n < needles.size(); n++)
         {
            if (row.second.Find(needles[n]) >= 0)
               state.found |= 1u << n;
         }
      }

      const unsigned int every = needles.size() == 32 ? 0xFFFFFFFFu : ((1u << needles.size()) - 1);

      for (const std::pair<const __int64, State> &entry : states)
      {
         const State &state = entry.second;
         if (state.complete && !state.overflow && state.found != every)
            excluded.insert(entry.first);
      }
   }

   void
   PersistentMessageIndexTester::Test()
   {
      typedef std::vector<std::pair<__int64, String> > Rows;

      const String complete = PersistentMessageIndex::CompleteMarker();
      const String overflow = PersistentMessageIndex::OverflowMarker();

      std::vector<String> needles;
      needles.push_back(_T("quokka"));
      needles.push_back(_T("came"));

      Rows rows;
      // 1: complete, both needles inside terms - a candidate.
      rows.push_back(std::make_pair(1LL, String(_T("xquokkax"))));
      rows.push_back(std::make_pair(1LL, String(_T("became"))));
      rows.push_back(std::make_pair(1LL, complete));
      // 2: complete, one needle only - cannot contain the text.
      rows.push_back(std::make_pair(2LL, String(_T("quokka"))));
      rows.push_back(std::make_pair(2LL, complete));
      // 3: complete, no needle at all - cannot contain the text.
      rows.push_back(std::make_pair(3LL, complete));
      // 4: no complete marker - its terms may not all be in: read.
      rows.push_back(std::make_pair(4LL, String(_T("other"))));
      // 5: overflowed - always read, whatever else it has.
      rows.push_back(std::make_pair(5LL, overflow));
      rows.push_back(std::make_pair(5LL, complete));
      // 6: complete, but the terms the database answered match the needles on
      // its collation only - accents, case - and not as the scan finds them:
      // cannot contain the text, as the scan would say.
      rows.push_back(std::make_pair(6LL, String(_T("qu\x00f6kka"))));
      rows.push_back(std::make_pair(6LL, String(_T("CAME"))));
      rows.push_back(std::make_pair(6LL, complete));
      // 7: a marker spelt without its \x01, as a collation that ignores the
      // control character would match it: not complete, so read.
      rows.push_back(std::make_pair(7LL, String(_T("ftc"))));
      rows.push_back(std::make_pair(7LL, String(_T("quokka"))));
      // 8: complete, both needles in one term - a candidate.
      rows.push_back(std::make_pair(8LL, String(_T("quokkacame"))));
      rows.push_back(std::make_pair(8LL, complete));

      std::set<__int64> excluded;
      PersistentMessageIndex::DecideExclusions(rows, needles, excluded);

      std::set<__int64> expected;
      expected.insert(2);
      expected.insert(3);
      expected.insert(6);

      if (excluded != expected)
      {
         String found;
         for (__int64 id : excluded)
            found += StringParser::IntToString(id) + _T(" ");
         throw std::logic_error(("PersistentMessageIndex::DecideExclusions excluded " + AnsiString(found) + "where 2, 3 and 6 alone cannot contain the text.").c_str());
      }

      // No needle: nothing proved, whatever the rows say.
      std::set<__int64> none;
      PersistentMessageIndex::DecideExclusions(rows, std::vector<String>(), none);
      if (!none.empty())
         throw std::logic_error("PersistentMessageIndex::DecideExclusions excluded a message for a text with no needle.");

      // The needles of a text, and the soundness the decision rests on: a
      // needle of the text lies inside a term the tokeniser stores for any
      // content that holds the text.
      std::vector<String> made;
      PersistentMessageIndex::CreateQueryNeedles(_T("The Quokka-came, too"), 3, made);
      if (made.size() != 4 || made[0] != _T("quokka"))
         throw std::logic_error("PersistentMessageIndex::CreateQueryNeedles did not make the longest needle first.");

      std::set<String> terms;
      bool overflowed = false;
      PersistentMessageIndex::TokenizeForIndex(_T("Subject: x\r\n\r\nand THE QUOKKA-CAME too"), 1000, terms, overflowed);

      Rows stored;
      for (const String &term : terms)
         stored.push_back(std::make_pair(9LL, term));
      stored.push_back(std::make_pair(9LL, complete));

      std::set<__int64> wrongly;
      PersistentMessageIndex::DecideExclusions(stored, made, wrongly);
      if (!wrongly.empty())
         throw std::logic_error("PersistentMessageIndex::DecideExclusions excluded a message whose own terms hold the text.");
   }
}
