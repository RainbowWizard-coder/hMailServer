// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../../TCPIP/TCPConnection.h"

namespace HM
{
   class File;
   class ReturnPathFieldFilter;

   class SpamAssassinClient : public TCPConnection
   {
   public:
      SpamAssassinClient(const String &sFile,
         boost::asio::io_context& io_context,
         boost::asio::ssl::context& context,
         std::shared_ptr<Event> disconnected,
         std::shared_ptr<bool> testCompleted);
      ~SpamAssassinClient(void);

      // The User: header of the PROCESS request - the user whose preferences spamd
      // applies to this scan. Empty sends no header. Set before Connect.
      void SetUser(const AnsiString &user) { user_ = user; }

      // TELL instead of PROCESS: the message is a lesson - spam or ham - for spamd's
      // local Bayes store, and the reply is an acknowledgement rather than the
      // message back. Set before Connect. See SpamAssassinLearner.
      void SetLearning(bool spam) { learning_ = true; learn_spam_ = spam; }

      // The Return-Path a PROCESS request carries ahead of the message - "<sender>",
      // or "<>" for the null sender - in place of any the message has; and the
      // Return-Path fields of spamd's reply are left out of the result that replaces
      // the message. Empty sends the file as it is and keeps the reply whole. Set
      // before Connect. See SpamAssassinReturnPath.h.
      void SetReturnPath(const AnsiString &returnPath) { return_path_ = returnPath; }

      virtual void ParseData(const AnsiString &Request);
      virtual void ParseData(std::shared_ptr<ByteBuffer> pBuf);

      

      
   protected:

      virtual void OnCouldNotConnect(const AnsiString &sErrorDescription);
      virtual void OnReadError(int errorCode);
      virtual void OnConnected();
      virtual void OnHandshakeCompleted() {};
      virtual void OnHandshakeFailed() {};
      virtual AnsiString GetCommandSeparator() const;
      virtual AnsiString GetSessionProtocolName() const { return "SpamAssassin"; }
      virtual void OnConnectionTimeout();
      virtual void OnExcessiveDataReceived();

   private:

      void Cleanup_();
      void FinishTesting_();
      void AbortResponse_();
      int ParseFirstBuffer_(std::shared_ptr<ByteBuffer> pBuffer) const;
      bool ReadRequestBody_(std::vector<std::shared_ptr<ByteBuffer> > &body, __int64 &bodySize);
      void SendRequestBody_(const std::vector<std::shared_ptr<ByteBuffer> > &body);

      String command_buffer_;

      String message_file_;
      AnsiString user_;
      AnsiString return_path_;
      bool learning_ = false;
      bool learn_spam_ = false;
      AnsiString learn_reply_;
      __int64 spam_dsize_;      // Content-length spamd reported; < 0 until a valid header is parsed
      std::shared_ptr<File> result_;

      // The Return-Path fields of the reply, left out on the way to result_; set
      // when the request carried a Return-Path (return_path_).
      std::shared_ptr<ReturnPathFieldFilter> response_filter_;

      // The bytes of the reply's message spamd has sent, counted against its
      // Content-length (spam_dsize_), whatever of them was left out of result_.
      __int64 total_result_bytes_received_;

      // Shared with SpamTestSpamAssassin::RunTest rather than a reference to its
      // stack, so that if RunTest stops waiting (its bounded wait elapsed against
      // a spamd that never answers) and returns, this still-live connection can
      // set the flag without writing through a dangling reference.
      std::shared_ptr<bool> test_completed_;

      __int64 total_result_bytes_written_;
  };
}