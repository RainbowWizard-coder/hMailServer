// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommand.h"
#include "../Common/Util/ByteBuffer.h"

namespace HM
{
   class Account;
   class ByteBuffer;
   class IMAPFolder;
   class Domain;

   class IMAPCommandAppend : public IMAPCommand
   {
   public:
      IMAPCommandAppend();
      virtual ~IMAPCommandAppend();

      virtual IMAPResult ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument);
      void ParseBinary(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<ByteBuffer> pBuf);

      // RFC 8508 (REPLACE): the next REPLACE this handler executes names its
      // target by UID rather than by message sequence number. Set by the UID
      // command handler, consumed by the one ExecuteCommand that follows.
      void SetReplaceUidMode(bool value) { replace_uid_mode_ = value; }

      // The largest APPEND this account will be allowed, in bytes - the number
      // CAPABILITY advertises as APPENDLIMIT= and STATUS reports (RFC 7889).
      // Never zero: an "unlimited" configuration still has the hard 2 GB
      // ceiling, and advertising 0 would mean "no APPEND accepted at all".
      static __int64 GetEffectiveAppendLimitBytes(std::shared_ptr<const Account> pAccount);

      // What an APPEND the process stopped in the middle of left behind: the
      // message file of a literal received but never given a row. Every such
      // file has a marker in the temp folder's IMAPAppend directory from the
      // moment the literal's file is named until its row is saved or the file
      // removed; a marker no command of this process holds belongs to a
      // process that ended - a crash, a kill, a power cut - and its file, when
      // no row names it, is removed with it. Run by Application::StartServers,
      // before any listener is up.
      static void SweepAbandonedAppends();


   private:

      // RFC 3502 (MULTIAPPEND): one received-but-unsaved message. Database rows
      // are created only when the whole command has succeeded, because a
      // multi-message APPEND is atomic - all stored or none.
      struct PendingMessage
      {
         std::shared_ptr<Message> message;
         String fileName;

         // Its marker (see SweepAbandonedAppends), or empty.
         String markerFile;
      };

      // The command stays in binary mode from the first literal to the final
      // CRLF; between literals the bytes are the rest of the command line.
      enum ReceiveState
      {
         ReceivingLiteral,
         ReceivingContinuationLine
      };

      void CompleteCurrentMessage_(std::shared_ptr<IMAPConnection> pConnection);
      void ParseContinuationLine_(std::shared_ptr<IMAPConnection> pConnection, const AnsiString &line);
      void TerminateWithProtocolError_(std::shared_ptr<IMAPConnection> pConnection, const String &responseAfterTag);
      IMAPResult ValidateAndPrepareMessage_(std::shared_ptr<IMAPConnection> pConnection, __int64 declaredSize);
      void FinalizeCommand_(std::shared_ptr<IMAPConnection> pConnection);
      void FailCommand_(const String &response);

      // A refusal of the first message, once its literal size is known. For a
      // synchronizing literal the refusal is simply the answer - the client is
      // waiting for a continuation and sends nothing more. For a {n+} literal the
      // octets are already on their way, so they are consumed, stored nowhere,
      // and the refusal is sent when the command's line ends (RFC 7888 section
      // 4); otherwise they would be read as command lines.
      IMAPResult RefuseWithLiteral_(std::shared_ptr<IMAPConnection> pConnection, const IMAPResult &refusal, bool nonSynchronizingLiteral, __int64 declaredSize);
      void CleanupPendingMessages_();

      // The most of a literal that is read only to be discarded, and the
      // TOOBIG close for one declared larger (true when the connection was
      // closed; ParseBinary then reads nothing more).
      static __int64 RefusedLiteralLimitBytes_(std::shared_ptr<const Account> pAccount);
      bool CloseOnOversizedDiscard_(std::shared_ptr<IMAPConnection> pConnection, __int64 declaredSize);

      bool TruncateBuffer_(const std::shared_ptr<IMAPConnection> pConnection );
      bool WriteData_(const std::shared_ptr<IMAPConnection> pConnection, const BYTE *pBuf, size_t WriteLen);
      void KillCurrentMessage_();

      // The markers of SweepAbandonedAppends. WriteAppendMarker_ answers the
      // marker's path, or empty when it could not be written - the APPEND then
      // goes on as it always did, unguarded against a crash rather than refused.
      static String AppendMarkerDirectory_();
      static String WriteAppendMarker_(const String &messageFile, bool placed, const String &existingMarker);
      static void RemoveAppendMarker_(String &markerFile);
      // One marker of SweepAbandonedAppends; whatever it throws is caught there.
      static void SweepAbandonedAppend_(const String &directory, const String &dataDirectory, const String &markerName);

      static bool StripUtf8LiteralWrapper_(String &command);

      int GetMaxMessageSize_(std::shared_ptr<const Domain> pDomain, std::shared_ptr<const Account> pAccount);

      String current_tag_;
      String flags_to_set_;
      String create_time_to_set_;
      size_t bytes_left_to_receive_;

      String message_file_name_;

      // The marker of the message being received (see SweepAbandonedAppends).
      String current_marker_;

      // Set when any part of the literal could not be written to disk. The
      // literal is still consumed to keep the parser in step, but the command
      // must then fail instead of reporting OK for a message that was lost.
      bool write_failed_ = false;

      ReceiveState receive_state_ = ReceivingLiteral;

      // Bytes of the between-literals command line received so far.
      AnsiString continuation_line_;

      // When the command has already failed (a write error, an oversized later
      // message), the remaining literals are consumed to keep the protocol in
      // step but discarded, and this holds the response to send at the end.
      bool command_failed_ = false;
      String failure_response_;

      // Set when CloseOnOversizedDiscard_ has closed the connection.
      bool connection_closing_ = false;

      // RFC 8508 (REPLACE): the message the command replaces. Removed from the
      // selected mailbox only after the replacement is safely stored - a failed
      // append leaves it untouched.
      bool replace_mode_ = false;
      bool replace_uid_mode_ = false;
      std::shared_ptr<Message> replace_target_;

      // RFC 6855 section 4: the message was sent as UTF8 (~{n}) and the ")"
      // that closes the wrapper follows its octets on the wire.
      bool utf8_wrapped_literal_ = false;

      std::vector<PendingMessage> pending_messages_;

      // Everything received in binary mode and not yet consumed: the literal's
      // octets, the command line after it and - once the command's final CRLF
      // has gone by - whatever the client pipelined behind the command, which
      // ParseBinary hands back to the connection to be read as command text.
      ByteBuffer append_buffer_;
      std::shared_ptr<IMAPFolder> destination_folder_;
      std::shared_ptr<Message> current_message_;


   };

}
