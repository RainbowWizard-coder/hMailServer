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

      // A user's name as the User: header carries it. spamd reads the header's
      // bytes as they are and refuses a request whose User: holds a byte below
      // 0x20 (spamd's got_user_header, "User header contains control chars");
      // what the name means - a Unix user, a virtual-config directory's %u, an SQL
      // username - is its own. An address the server holds as its bytes widened
      // (Unicode::IsWidenedUtf8, as SMTP gives it) goes as those bytes; any other
      // name as UTF-8 on POSIX, and on Windows one byte a character when every
      // character is U+00FF or below, as Unicode::ToANSI wrote it up to 6.3.7,
      // and UTF-8 when one is above. False, with nothing written, for a name with
      // a control character or half of a surrogate pair. Up to 6.3.7 every caller
      // used ToANSI, which gave nothing for a name with a character outside ASCII
      // on Linux and above U+00FF on Windows, and the request then went without
      // the header, as spamd's default user, without a word.
      static bool UserBytes(const String &user, AnsiString &bytes);

      // UserBytes for a request, said in the error log (HM5514) when a user is
      // named and cannot be sent: the request then goes without the header, which
      // spamd reads as its own default user. Empty for an empty user.
      static AnsiString UserHeaderValue(const String &user);

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

   // SpamAssassinClient::UserBytes: the bytes spamd's User: header is given for a
   // name, an address held decoded and held as its bytes widened alike, and a
   // control character refused. Throws std::logic_error with a sentence.
   // unicode-tests runs it on Linux, ClassTester inside the Windows service. No
   // spamd, configuration or network.
   class SpamAssassinUserTester
   {
   public:
      void Test();
   };
}