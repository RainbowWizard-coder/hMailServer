// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommand.h"
#include "IMAPSearchParser.h"


namespace HM
{
   class IMAPSearchParser;
   class IMAPConnection;
   class MessageData;
   class MimeHeader;

   class IMAPCommandSEARCH : public IMAPCommand
   {
   public:
      IMAPCommandSEARCH(IMAPSearchCommandMode mode);
      virtual ~IMAPCommandSEARCH();

      void SetIsUID() { is_uid_ = true; }

      IMAPResult ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument);

   private:

      bool MatchesHeaderCriteria_(const String &fileName,std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);

      // RFC 3501 6.4.4: FROM, TO, CC and BCC match "the envelope structure's"
      // field - its addresses, parsed - not the header's raw text.
      bool MatchesAddressCriteria_(const String &fileName, std::shared_ptr<IMAPSearchCriteria> pCriteria, const String &sHeaderField);
      bool MatchesUIDCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSequenceSetCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria, int index);
      bool MatchesTEXTCriteria_(const String &fileName, std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesBODYCriteria_(const String &fileName, std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesONCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSENTONCriteria_(const String &fileName, std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSENTBEFORECriteria_(const String &fileName, std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSENTSINCECriteria_(const String &fileName, std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSINCECriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);

      // RFC 8514 (SAVEDATE): the same three date comparisons, measured against
      // the message's save date rather than its internal date.
      bool MatchesSAVEDBEFORECriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSAVEDONCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSAVEDSINCECriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      
      bool MatchesBEFORECriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesWITHINCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesLARGERCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);
      bool MatchesSMALLERCriteria_(std::shared_ptr<Message> pMessage, std::shared_ptr<IMAPSearchCriteria> pCriteria);

      String GetHeaderValue_(const String &fileName, std::shared_ptr<Message> pMessage, const String &sHeaderField);

      // Every occurrence of a header field, as the header has it (undecoded). A
      // field may be repeated, and a HEADER search matches any of them.
      void GetRawHeaderValues_(const String &fileName, const String &sHeaderField, std::vector<AnsiString> &rawValues);
      void EnsureMimeHeader_(const String &fileName);
      
      
      bool DoesMessageMatch_(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPSearchCriteria> pParentCriteria, const String &fileName, std::shared_ptr<Message> pMessage, int index);
      bool IsMessageRecent_(std::shared_ptr<IMAPConnection> pConnection, __int64 message_uid);

      void ResetSearchState_();
      bool BoundExceeded_();
      bool TimeBoundExceeded_();
      IMAPResult AbortSearch_(std::shared_ptr<IMAPConnection> pConnection, int messages_scanned);

      std::shared_ptr<MessageData> message_data_;
      std::shared_ptr<MimeHeader> mime_header_;

      bool is_sort_;
      bool is_thread_;
      bool is_uid_;

      // RFC 4731 (ESEARCH) result-option state, parsed from a "RETURN (...)" clause.
      bool is_esearch_;
      bool esearch_min_;
      bool esearch_max_;
      bool esearch_all_;
      bool esearch_count_;

      // RFC 5182 (SEARCHRES): set when the "SAVE" result option is present, in which case
      // the matched UIDs are stored on the connection for later reference via "$".
      bool esearch_save_;

      // RFC 7162 (CONDSTORE): set from the parsed command when it names a MODSEQ search key
      // anywhere - whether or not a message's match reached the key - so the SEARCH/ESEARCH
      // response can report the highest mod-sequence among the matched messages.
      bool modseq_search_;
      __int64 highest_modseq_;

      // What "*" stands for in a search key that carries a sequence set. Captured once
      // for the mailbox being searched, before the per-message matching starts.
      __int64 max_uid_;
      int message_count_;

      // Absolute ceiling on one search. search_start_tick_ is taken once at the top
      // of ExecuteCommand and never moved, and the two limits are read once there
      // too, so neither a configuration reload nor per-message progress can extend
      // the budget mid-search. A zero limit disables that half.
      unsigned __int64 search_start_tick_;
      unsigned __int64 bytes_examined_;
      unsigned __int64 max_bytes_examined_;
      int search_timeout_seconds_;

      // Which ceiling was passed, and by how much, for the application log entry.
      String bound_reason_;

      // Set when a ceiling was passed while one message was being matched, so the
      // match was given up part way: the search is abandoned, not answered. The
      // ceilings used to be asked only between messages, and one message met by a
      // hundred thousand TEXT keys - a client's command line - was read a hundred
      // thousand times before they were asked again.
      bool match_interrupted_ = false;

      // The sets of the UID and sequence-set keys, each read once into runs
      // (IMAPListLookup::ToRuns) for this search, by the set's address: the "$"
      // keys share one. Each used to be copied and read whole for every message.
      std::map<const std::vector<String> *, std::vector<std::pair<__int64, __int64> > > uid_runs_;
      std::map<const std::vector<String> *, std::vector<std::pair<__int64, __int64> > > sequence_runs_;

      // The rendered RFC 5256 tree for a THREAD command, built where SORT sorts and
      // consumed where the response is assembled. Cleared by ResetSearchState_,
      // because the handler is reused across commands on a connection.
      String thread_response_body_;
   };
}
