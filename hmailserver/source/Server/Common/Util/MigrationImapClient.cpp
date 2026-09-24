// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See MigrationImapClient.h.

#include "StdAfx.h"
#include "MigrationImapClient.h"

#include "File.h"
#include "Parsing/StringParser.h"
#include "Strings/Formatter.h"
#include "../TCPIP/CertificateVerifier.h"
#include "../TCPIP/SslContextInitializer.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef std::chrono::steady_clock Clock;

      // A reply line longer than this is not IMAP a migration can use: the
      // longest legitimate one is a UID SEARCH of a very large mailbox, and
      // sixteen megabytes holds some two million UIDs.
      const size_t MaxLineBytes = 16 * 1024 * 1024;

      // A literal that is not a message body - a mailbox name, most often -
      // is kept in the reply's text up to this size and read past beyond it.
      const __int64 MaxInlineLiteral = 1024 * 1024;

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }

      AnsiString Upper(const AnsiString &value)
      {
         AnsiString result = value;
         result.MakeUpper();
         return result;
      }

      String Words(const char *text)
      {
         return String(AnsiString(text));
      }

      String Reason(const boost::system::error_code &code)
      {
         const std::string text = code.message() + " [" + code.category().name() + ":" + std::to_string(code.value()) + "]";
         return String(AnsiString(text.c_str()));
      }
   }

   //---------------------------------------------------------------------------
   // The connection: a socket, and TLS over it once it is asked for.
   //---------------------------------------------------------------------------

   class MigrationImapClient::Transport
   {
   public:
      Transport(std::function<bool()> stopRequested, bool &stopped) :
         ssl_context_(boost::asio::ssl::context::tls_client),
         stop_requested_(stopRequested),
         stopped_(stopped),
         tls_(false),
         timeout_seconds_(60)
      {
         // The mail client's context: the protocol floor every context in this
         // server has, the machine's roots, and the [Settings] TLS toggles the
         // mail client follows (SslContextInitializer::InitClient).
         ssl_context_.set_options(HM_TLS_CONTEXT_FLOOR);
         ssl_context_.set_default_verify_paths();
         SslContextInitializer::InitClient(ssl_context_, true);

         stream_.reset(new boost::asio::ssl::stream<boost::asio::ip::tcp::socket>(io_, ssl_context_));
      }

      ~Transport()
      {
         Close();
      }

      bool Connect(const AnsiString &host, int port, int timeoutSeconds, String &error)
      {
         timeout_seconds_ = timeoutSeconds > 0 ? timeoutSeconds : 60;

         std::vector<boost::asio::ip::tcp::endpoint> endpoints;

         boost::system::error_code notLiteral;
         const boost::asio::ip::address literal = boost::asio::ip::make_address(std::string(host.c_str()), notLiteral);
         if (!notLiteral)
         {
            endpoints.push_back(boost::asio::ip::tcp::endpoint(literal, (unsigned short) port));
         }
         else
         {
            // A name is resolved synchronously: the resolver cannot be abandoned
            // any more than an asynchronous one could (its thread is joined when
            // the context goes), and the job's thread is its own.
            boost::asio::ip::tcp::resolver resolver(io_);
            boost::system::error_code resolveError;
            boost::asio::ip::tcp::resolver::results_type results = resolver.resolve(std::string(host.c_str()), std::to_string(port), resolveError);
            if (resolveError)
            {
               error = Formatter::Format(_T("{0} does not resolve: {1}"), String(host), Reason(resolveError));
               return false;
            }

            for (const boost::asio::ip::tcp::resolver::results_type::value_type &entry : results)
               endpoints.push_back(entry.endpoint());
         }

         if (endpoints.empty())
         {
            error = Formatter::Format(_T("{0} does not resolve."), String(host));
            return false;
         }

         bool finished = false;
         boost::system::error_code result;
         boost::asio::async_connect(stream_->next_layer(), endpoints,
            [&finished, &result](const boost::system::error_code &ec, const boost::asio::ip::tcp::endpoint &)
            {
               result = ec;
               finished = true;
            });

         if (!Await_(finished, "no connection was made", error))
            return false;

         if (result)
         {
            error = Formatter::Format(_T("The connection to {0}:{1} failed: {2}"), String(host), port, Reason(result));
            return false;
         }

         return true;
      }

      bool Handshake(const AnsiString &host, ConnectionSecurity security, String &error)
      {
         stream_->set_verify_mode(boost::asio::ssl::verify_peer);
         stream_->set_verify_callback(CertificateVerifier(0, security, String(host)));

         // The server's name, when it has one: RFC 6066 has no place for an
         // address in server_name.
         boost::system::error_code notLiteral;
         boost::asio::ip::make_address(std::string(host.c_str()), notLiteral);
         if (notLiteral && !SSL_set_tlsext_host_name(stream_->native_handle(), host.c_str()))
         {
            error = _T("The TLS server name could not be set.");
            return false;
         }

         // Nothing the server sent before the handshake is part of what follows.
         buffer_.clear();

         bool finished = false;
         boost::system::error_code result;
         stream_->async_handshake(boost::asio::ssl::stream_base::client,
            [&finished, &result](const boost::system::error_code &ec)
            {
               result = ec;
               finished = true;
            });

         if (!Await_(finished, "the TLS handshake did not finish", error))
            return false;

         if (result)
         {
            error = Formatter::Format(_T("The TLS handshake with {0} failed: {1}"), String(host), Reason(result));
            return false;
         }

         tls_ = true;
         return true;
      }

      bool Write(const AnsiString &data, String &error)
      {
         bool finished = false;
         boost::system::error_code result;
         auto handler = [&finished, &result](const boost::system::error_code &ec, std::size_t)
         {
            result = ec;
            finished = true;
         };

         if (tls_)
            boost::asio::async_write(*stream_, boost::asio::buffer(data.c_str(), data.size()), handler);
         else
            boost::asio::async_write(stream_->next_layer(), boost::asio::buffer(data.c_str(), data.size()), handler);

         if (!Await_(finished, "the command was not sent", error))
            return false;

         if (result)
         {
            error = _T("The connection failed while sending: ") + Reason(result);
            return false;
         }

         return true;
      }

      bool ReadLine(AnsiString &line, String &error)
      {
         for (;;)
         {
            const size_t end = buffer_.find("\r\n");
            if (end != std::string::npos)
            {
               line = AnsiString(buffer_.substr(0, end).c_str());
               buffer_.erase(0, end + 2);
               return true;
            }

            if (buffer_.size() > MaxLineBytes)
            {
               error = _T("The server sent a line longer than this client reads.");
               return false;
            }

            if (!ReadSome_(error))
               return false;
         }
      }

      // Exactly length octets: to the file, into memory, or - with neither -
      // read past.
      bool ReadExact(__int64 length, File *file, std::string *memory, String &error)
      {
         __int64 remaining = length;

         while (remaining > 0)
         {
            if (buffer_.empty() && !ReadSome_(error))
               return false;

            const size_t take = (size_t) std::min<__int64>(remaining, (__int64) buffer_.size());

            if (file && !file->Write((const unsigned char *) buffer_.data(), take))
            {
               error = _T("The message file could not be written.");
               return false;
            }

            if (memory)
               memory->append(buffer_.data(), take);

            buffer_.erase(0, take);
            remaining -= (__int64) take;
         }

         return true;
      }

      void Close()
      {
         boost::system::error_code ignored;
         if (stream_)
         {
            stream_->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            stream_->next_layer().close(ignored);
         }
      }

   private:
      // Runs the one operation just started until it finishes, the deadline
      // passes, or the job is told to stop - checked every quarter second.
      bool Await_(bool &finished, const char *step, String &error)
      {
         const Clock::time_point deadline = Clock::now() + std::chrono::seconds(timeout_seconds_);

         for (;;)
         {
            io_.restart();

            Clock::time_point slice = Clock::now() + std::chrono::milliseconds(250);
            if (slice > deadline)
               slice = deadline;

            io_.run_until(slice);

            if (finished)
               return true;

            if (stop_requested_ && stop_requested_())
            {
               Abandon_();
               stopped_ = true;
               error = _T("The job was told to stop.");
               return false;
            }

            if (Clock::now() >= deadline)
            {
               Abandon_();
               error = Words(step) + _T(" in the time allowed.");
               return false;
            }
         }
      }

      // Closing the socket completes the pending operation with an error; the
      // context is then run until that handler has, so nothing is left
      // referring to the caller's frame.
      void Abandon_()
      {
         boost::system::error_code ignored;
         stream_->next_layer().close(ignored);
         io_.restart();
         io_.run();
      }

      bool ReadSome_(String &error)
      {
         char chunk[16384];
         bool finished = false;
         size_t got = 0;
         boost::system::error_code result;
         auto handler = [&finished, &got, &result](const boost::system::error_code &ec, std::size_t transferred)
         {
            result = ec;
            got = transferred;
            finished = true;
         };

         if (tls_)
            stream_->async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), handler);
         else
            stream_->next_layer().async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), handler);

         if (!Await_(finished, "the server did not answer", error))
            return false;

         if (got > 0)
            buffer_.append(chunk, got);

         if (result && got == 0)
         {
            error = result == boost::asio::error::eof
               ? String(_T("The server closed the connection."))
               : _T("The connection failed: ") + Reason(result);
            return false;
         }

         return true;
      }

      boost::asio::io_context io_;
      boost::asio::ssl::context ssl_context_;
      std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> > stream_;
      std::function<bool()> stop_requested_;
      bool &stopped_;
      bool tls_;
      int timeout_seconds_;
      std::string buffer_;
   };

   //---------------------------------------------------------------------------
   // The client.
   //---------------------------------------------------------------------------

   MigrationImapClient::MigrationImapClient(std::function<bool()> stopRequested) :
      stop_requested_(stopRequested),
      tag_(0),
      stopped_(false)
   {
      transport_.reset(new Transport(stopRequested, stopped_));
   }

   MigrationImapClient::~MigrationImapClient()
   {
      try
      {
         transport_.reset();
      }
      catch (...)
      {
         // A destructor lets nothing escape.
      }
   }

   AnsiString
   MigrationImapClient::NextTag_()
   {
      tag_++;
      current_tag_.Format("M%d", tag_);
      return current_tag_;
   }

   bool
   MigrationImapClient::Connect(const AnsiString &host, int port, ConnectionSecurity security, int timeoutSeconds, String &error)
   {
      if (!transport_->Connect(host, port, timeoutSeconds, error))
         return false;

      if (security == CSSSL && !transport_->Handshake(host, security, error))
         return false;

      AnsiString greeting;
      if (!transport_->ReadLine(greeting, error))
         return false;

      last_line_ = greeting;

      if (!greeting.StartsWith("* OK") && !greeting.StartsWith("* PREAUTH"))
      {
         // Not what it said: the host and port are the migration's, and a
         // banner read back into last_error would let a credential confined to
         // a domain read what any service on the network answers.
         error = Formatter::Format(_T("The server at {0}:{1} did not answer with an IMAP greeting."), String(host), port);
         return false;
      }

      if (security == CSSTARTTLSOptional || security == CSSTARTTLSRequired)
      {
         std::vector<AnsiString> untagged;
         AnsiString status;
         if (!Command_("STARTTLS", untagged, status, error))
            return false;

         if (status == "OK")
            return transport_->Handshake(host, security, error);

         if (security == CSSTARTTLSRequired)
         {
            error = _T("The server refused STARTTLS, and the migration requires it: ") + String(last_line_.Mid(0, 200));
            return false;
         }
      }

      return true;
   }

   bool
   MigrationImapClient::Login(const AnsiString &user, const AnsiString &password, bool &refused, String &error)
   {
      refused = false;

      std::vector<AnsiString> untagged;
      AnsiString status;

      if (NeedsLiteral(user) || NeedsLiteral(password))
      {
         std::vector<AnsiString> pieces;
         pieces.push_back("LOGIN ");
         pieces.push_back(" ");
         pieces.push_back("");
         std::vector<AnsiString> literals;
         literals.push_back(user);
         literals.push_back(password);

         if (!CommandWithLiterals_(pieces, literals, untagged, status, error))
            return false;
      }
      else if (!Command_("LOGIN " + Quote(user) + " " + Quote(password), untagged, status, error))
      {
         return false;
      }

      if (status != "OK")
      {
         // The credentials refused, which no retry mends; or the source unable
         // to log anybody on just now, which a retry does.
         refused = IsCredentialRefusal(last_line_);
         error = (refused ? String(_T("The source refused the logon: ")) : String(_T("The source could not log the job on just now: "))) +
                 String(last_line_.Mid(0, 300));
         return false;
      }

      return true;
   }

   AnsiString
   MigrationImapClient::XOAuth2InitialResponse(const AnsiString &user, const AnsiString &token)
   {
      // user=<address>^Aauth=Bearer <token>^A^A, base64 - the blob the
      // fetcher sends (IMAPClientConnection::SendLogin_).
      String blob = String(user) + _T("\x01") + _T("auth=Bearer ") + String(token) + _T("\x01\x01");
      blob = _T("user=") + blob;
      String encoded;
      StringParser::Base64Encode(blob, encoded);
      return AnsiString(encoded);
   }

   bool
   MigrationImapClient::LoginXOAuth2(const AnsiString &user, const AnsiString &token, bool &refused, String &error)
   {
      refused = false;

      const AnsiString tag = NextTag_();
      if (!transport_->Write(tag + " AUTHENTICATE XOAUTH2 " + XOAuth2InitialResponse(user, token) + "\r\n", error))
         return false;

      for (;;)
      {
         AnsiString line;
         if (!ReadReply_(line, nullptr, 0, nullptr, error))
            return false;

         last_line_ = line;

         // A continuation after the initial response is the server saying the
         // token was refused, with the reason in base64; an empty line lets it
         // finish with the tagged NO.
         if (line.StartsWith("+"))
         {
            if (!transport_->Write("\r\n", error))
               return false;
            continue;
         }

         if (line.StartsWith(tag + " "))
         {
            if (Upper(line.Mid(tag.GetLength() + 1, 2)) == "OK")
               return true;

            refused = IsCredentialRefusal(line);
            error = (refused ? String(_T("The source refused the token: ")) : String(_T("The source could not log the job on just now: "))) +
                    String(line.Mid(0, 300));
            return false;
         }
      }
   }

   bool
   MigrationImapClient::Namespace(std::vector<AnsiString> &otherPrefixes, String &error)
   {
      otherPrefixes.clear();

      std::vector<AnsiString> untagged;
      AnsiString status;
      if (!Command_("NAMESPACE", untagged, status, error))
         return false;

      // Not offered: nothing is known to be somebody else's, and LIST is
      // taken as it comes.
      if (status != "OK")
         return true;

      for (const AnsiString &line : untagged)
      {
         if (Upper(line).StartsWith("* NAMESPACE "))
            ParseNamespace(line, otherPrefixes);
      }

      return true;
   }

   bool
   MigrationImapClient::List(std::vector<Mailbox> &mailboxes, String &error)
   {
      mailboxes.clear();

      std::vector<AnsiString> untagged;
      AnsiString status;
      if (!Command_("LIST \"\" \"*\"", untagged, status, error))
         return false;

      if (status != "OK")
      {
         error = _T("The source refused LIST: ") + String(last_line_.Mid(0, 300));
         return false;
      }

      for (const AnsiString &line : untagged)
      {
         Mailbox mailbox;
         if (ParseListLine(line, mailbox))
            mailboxes.push_back(mailbox);
      }

      return true;
   }

   bool
   MigrationImapClient::Status(const AnsiString &mailbox, __int64 &messages, String &error)
   {
      messages = 0;

      std::vector<AnsiString> untagged;
      AnsiString status;
      if (!Command_("STATUS " + Quote(mailbox) + " (MESSAGES)", untagged, status, error))
         return false;

      if (status != "OK")
         return true;   // counted as none; EXAMINE says whether it opens

      for (const AnsiString &line : untagged)
      {
         if (!line.StartsWith("* STATUS "))
            continue;

         // The mailbox name comes first and may itself hold the word, so the
         // last occurrence is the item.
         const int at = Upper(line).ReverseFind("MESSAGES ");
         if (at >= 0)
            messages = _atoi64(line.Mid(at + 9).c_str());
      }

      return true;
   }

   bool
   MigrationImapClient::Examine(const AnsiString &mailbox, __int64 &uidValidity, __int64 &exists, bool &refused, bool &transient, String &error)
   {
      uidValidity = 0;
      exists = 0;
      refused = false;
      transient = false;

      std::vector<AnsiString> untagged;
      AnsiString status;
      if (!Command_("EXAMINE " + Quote(mailbox), untagged, status, error))
         return false;

      if (status != "OK")
      {
         refused = true;
         transient = IsTransientRefusal(last_line_);
         error = _T("The source would not open the folder: ") + String(last_line_.Mid(0, 300));
         return false;
      }

      for (const AnsiString &line : untagged)
      {
         const int at = line.Find("[UIDVALIDITY ");
         if (at >= 0)
            uidValidity = _atoi64(line.Mid(at + 13).c_str());

         if (line.StartsWith("* ") && Upper(line).EndsWith(" EXISTS"))
            exists = _atoi64(line.Mid(2).c_str());
      }

      return true;
   }

   bool
   MigrationImapClient::UidsFrom(unsigned int from, std::vector<unsigned int> &uids, String &error)
   {
      uids.clear();

      AnsiString command;
      command.Format("UID SEARCH UID %u:*", from == 0 ? 1u : from);

      std::vector<AnsiString> untagged;
      AnsiString status;
      if (!Command_(command, untagged, status, error))
         return false;

      if (status != "OK")
      {
         error = _T("The source refused UID SEARCH: ") + String(last_line_.Mid(0, 300));
         return false;
      }

      for (const AnsiString &line : untagged)
      {
         if (!Upper(line).StartsWith("* SEARCH"))
            continue;

         std::vector<AnsiString> parts = StringParser::SplitString(line.Mid(8), " ");
         for (AnsiString part : parts)
         {
            part.Trim();
            if (part.IsEmpty())
               continue;

            const __int64 uid = _atoi64(part.c_str());
            if (uid > 0 && uid <= 0xFFFFFFFF && (unsigned int) uid >= from)
               uids.push_back((unsigned int) uid);
         }
      }

      std::sort(uids.begin(), uids.end());
      uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
      return true;
   }

   bool
   MigrationImapClient::FetchToFile(unsigned int uid, const String &fileName, __int64 maxBytes, Fetched &fetched, bool &gone, bool &refused, String &error)
   {
      fetched = Fetched();
      gone = false;
      refused = false;

      AnsiString command;
      command.Format("UID FETCH %u (UID FLAGS INTERNALDATE BODY.PEEK[])", uid);

      const AnsiString tag = NextTag_();
      if (!transport_->Write(tag + " " + command + "\r\n", error))
         return false;

      bool answered = false;
      for (;;)
      {
         Fetched candidate;
         AnsiString text;
         if (!ReadReply_(text, &fileName, maxBytes, &candidate, error))
            return false;

         last_line_ = text;

         if (text.StartsWith(tag + " "))
         {
            if (Upper(text.Mid(tag.GetLength() + 1, 2)) != "OK")
            {
               // Read to its tagged end, so the connection is in step for the
               // next command: the message, not the session, was refused.
               refused = true;
               error = _T("The source refused UID FETCH: ") + String(text.Mid(0, 300));
               return false;
            }
            break;
         }

         // The FETCH of this message - another message's flag update may
         // arrive among the replies, and is not it.
         if (text.StartsWith("* ") && Upper(text).Find(" FETCH ") > 0)
         {
            ParseFetchItems(text, candidate);
            if (candidate.body || candidate.tooLarge)
            {
               fetched = candidate;
               answered = true;
            }
         }
      }

      if (!answered)
         gone = true;

      return true;
   }

   void
   MigrationImapClient::Logout()
   {
      if (!stopped_)
      {
         std::vector<AnsiString> untagged;
         AnsiString status;
         String ignored;
         Command_("LOGOUT", untagged, status, ignored);
      }

      transport_->Close();
   }

   bool
   MigrationImapClient::Command_(const AnsiString &command, std::vector<AnsiString> &untagged, AnsiString &status, String &error)
   {
      untagged.clear();
      status = "";

      const AnsiString tag = NextTag_();
      if (!transport_->Write(tag + " " + command + "\r\n", error))
         return false;

      for (;;)
      {
         AnsiString text;
         if (!ReadReply_(text, nullptr, 0, nullptr, error))
            return false;

         last_line_ = text;

         if (text.StartsWith(tag + " "))
         {
            AnsiString rest = text.Mid(tag.GetLength() + 1);
            const int space = rest.Find(" ");
            status = Upper(space >= 0 ? rest.Mid(0, space) : rest);
            return true;
         }

         if (text.StartsWith("*"))
            untagged.push_back(text);

         // A continuation here was not asked for; the command did not take a
         // literal. It is read past and the tagged answer waited for.
      }
   }

   bool
   MigrationImapClient::CommandWithLiterals_(const std::vector<AnsiString> &pieces, const std::vector<AnsiString> &literals,
                                             std::vector<AnsiString> &untagged, AnsiString &status, String &error)
   {
      untagged.clear();
      status = "";

      const AnsiString tag = NextTag_();
      AnsiString sending = tag + " " + pieces[0];

      for (size_t i = 0; i < literals.size(); i++)
      {
         AnsiString announce;
         announce.Format("{%d}\r\n", (int) literals[i].GetLength());
         if (!transport_->Write(sending + announce, error))
            return false;

         // The server has to say go on before the octets; a tagged answer
         // instead is its refusal.
         for (;;)
         {
            AnsiString text;
            if (!ReadReply_(text, nullptr, 0, nullptr, error))
               return false;

            last_line_ = text;

            if (text.StartsWith("+"))
               break;

            if (text.StartsWith(tag + " "))
            {
               AnsiString rest = text.Mid(tag.GetLength() + 1);
               const int space = rest.Find(" ");
               status = Upper(space >= 0 ? rest.Mid(0, space) : rest);
               return true;
            }

            if (text.StartsWith("*"))
               untagged.push_back(text);
         }

         sending = literals[i] + (i + 1 < pieces.size() ? pieces[i + 1] : AnsiString());
      }

      if (!transport_->Write(sending + "\r\n", error))
         return false;

      for (;;)
      {
         AnsiString text;
         if (!ReadReply_(text, nullptr, 0, nullptr, error))
            return false;

         last_line_ = text;

         if (text.StartsWith(tag + " "))
         {
            AnsiString rest = text.Mid(tag.GetLength() + 1);
            const int space = rest.Find(" ");
            status = Upper(space >= 0 ? rest.Mid(0, space) : rest);
            return true;
         }

         if (text.StartsWith("*"))
            untagged.push_back(text);
      }
   }

   bool
   MigrationImapClient::ReadReply_(AnsiString &text, const String *bodyFile, __int64 maxBody, Fetched *fetched, String &error)
   {
      text = "";

      for (;;)
      {
         AnsiString line;
         if (!transport_->ReadLine(line, error))
            return false;

         __int64 length = 0;
         AnsiString before;
         if (!ParseLiteralLength(line, length, before))
         {
            text += line;
            return true;
         }

         text += before;

         // The body of the message a FETCH is for: what precedes the literal
         // names it as BODY[] (or BODY[]<0>, which some servers answer a
         // BODY.PEEK[] with).
         AnsiString trimmed = Upper(before);
         trimmed.TrimRight();
         const bool isBody = bodyFile && fetched &&
                             (trimmed.EndsWith("BODY[]") || trimmed.EndsWith("BODY[]<0>") || trimmed.EndsWith("RFC822"));

         if (isBody)
         {
            if (maxBody > 0 && length > maxBody)
            {
               if (!transport_->ReadExact(length, nullptr, nullptr, error))
                  return false;
               fetched->tooLarge = true;
            }
            else
            {
               File file;
               if (!file.Open(*bodyFile, File::OTCreate))
               {
                  // Still read, so the connection stays in step for the next
                  // command, and then refused.
                  transport_->ReadExact(length, nullptr, nullptr, error);
                  error = _T("The message file could not be created: ") + *bodyFile;
                  return false;
               }

               const bool written = transport_->ReadExact(length, &file, nullptr, error);
               file.Close();
               if (!written)
                  return false;

               fetched->body = true;
               fetched->bytes = length;
            }

            text += "{BODY}";
            continue;
         }

         if (length <= MaxInlineLiteral)
         {
            std::string value;
            if (!transport_->ReadExact(length, nullptr, &value, error))
               return false;
            text += Quote(AnsiString(value.c_str()));
         }
         else
         {
            if (!transport_->ReadExact(length, nullptr, nullptr, error))
               return false;
            text += "\"\"";
         }
      }
   }

   //---------------------------------------------------------------------------
   // Parsing.
   //---------------------------------------------------------------------------

   AnsiString
   MigrationImapClient::Quote(const AnsiString &value)
   {
      AnsiString quoted = value;
      quoted.Replace("\\", "\\\\");
      quoted.Replace("\"", "\\\"");
      return "\"" + quoted + "\"";
   }

   bool
   MigrationImapClient::NeedsLiteral(const AnsiString &value)
   {
      // A quoted string holds seven-bit text without CR or LF (RFC 3501
      // section 4.3); anything else goes as a literal.
      for (int i = 0; i < value.GetLength(); i++)
      {
         const unsigned char c = (unsigned char) value.GetAt(i);
         if (c < 0x20 || c > 0x7e)
            return true;
      }
      return false;
   }

   bool
   MigrationImapClient::ParseLiteralLength(const AnsiString &line, __int64 &length, AnsiString &before)
   {
      // "... {1234}" or LITERAL+'s "... {1234+}" at the end of the line.
      if (!line.EndsWith("}"))
         return false;

      const int open = line.ReverseFind('{');
      if (open < 0)
         return false;

      AnsiString digits = line.Mid(open + 1, line.GetLength() - open - 2);
      if (digits.EndsWith("+"))
         digits = digits.Mid(0, digits.GetLength() - 1);

      if (digits.IsEmpty() || digits.GetLength() > 12)
         return false;

      for (int i = 0; i < digits.GetLength(); i++)
      {
         if (digits.GetAt(i) < '0' || digits.GetAt(i) > '9')
            return false;
      }

      length = _atoi64(digits.c_str());
      before = line.Mid(0, open);
      return true;
   }

   bool
   MigrationImapClient::ParseListLine(const AnsiString &line, Mailbox &mailbox)
   {
      // * LIST (\HasNoChildren \Sent) "/" "Sent Items"
      // * LIST (\Noselect \HasChildren) "." Archive
      // * LIST () NIL INBOX
      if (!Upper(line).StartsWith("* LIST "))
         return false;

      const int open = line.Find("(");
      const int close = open >= 0 ? line.Find(")", open) : -1;
      if (open < 0 || close < 0)
         return false;

      mailbox.attributes = line.Mid(open + 1, close - open - 1);
      const AnsiString upperAttributes = Upper(mailbox.attributes);
      mailbox.selectable = upperAttributes.Find("\\NOSELECT") < 0 && upperAttributes.Find("\\NONEXISTENT") < 0;

      AnsiString rest = line.Mid(close + 1);
      rest.TrimLeft();

      if (rest.StartsWith("\""))
      {
         // A quoted delimiter: one character, or an escaped one.
         int end = 1;
         if (rest.GetLength() > 1 && rest.GetAt(1) == '\\')
            end = 3;
         else
            end = 2;

         if (rest.GetLength() <= end || rest.GetAt(end) != '"')
            return false;

         mailbox.delimiter = rest.Mid(end - 1, 1);
         rest = rest.Mid(end + 1);
      }
      else if (Upper(rest).StartsWith("NIL"))
      {
         mailbox.delimiter = "";
         rest = rest.Mid(3);
      }
      else
      {
         return false;
      }

      rest.Trim();
      if (rest.IsEmpty())
         return false;

      if (rest.StartsWith("\""))
      {
         if (rest.GetLength() < 2 || !rest.EndsWith("\""))
            return false;

         AnsiString inner = rest.Mid(1, rest.GetLength() - 2);
         AnsiString name;
         for (int i = 0; i < inner.GetLength(); i++)
         {
            char c = inner.GetAt(i);
            if (c == '\\' && i + 1 < inner.GetLength())
            {
               i++;
               c = inner.GetAt(i);
            }
            name += c;
         }
         mailbox.name = name;
      }
      else
      {
         mailbox.name = rest;
      }

      return !mailbox.name.IsEmpty();
   }

   void
   MigrationImapClient::ParseFetchItems(const AnsiString &text, Fetched &fetched)
   {
      // * 12 FETCH (UID 34 FLAGS (\Seen $Forwarded) INTERNALDATE "17-Jul-1996 02:44:25 -0700" BODY[] {BODY})
      // - the items in any order the server likes, before the body or after it.
      const AnsiString upper = Upper(text);

      int at = upper.Find("(UID ");
      if (at < 0)
         at = upper.Find(" UID ");
      if (at >= 0)
         fetched.uid = (unsigned int) _atoi64(text.Mid(at + 5).c_str());

      const int flagsAt = upper.Find("FLAGS (");
      if (flagsAt >= 0)
      {
         const int close = text.Find(")", flagsAt);
         if (close > flagsAt)
            fetched.flags = text.Mid(flagsAt + 7, close - flagsAt - 7);
      }

      const int dateAt = upper.Find("INTERNALDATE \"");
      if (dateAt >= 0)
      {
         const int start = dateAt + 14;
         const int end = text.Find("\"", start);
         if (end > start)
            fetched.internalDate = text.Mid(start, end - start);
      }
   }

   namespace
   {
      // The status word of a tagged completion: "NO" of "M4 NO [UNAVAILABLE] ...".
      AnsiString StatusOf(const AnsiString &taggedLine)
      {
         const int first = taggedLine.Find(" ");
         if (first < 0)
            return "";

         AnsiString rest = taggedLine.Mid(first + 1);
         const int second = rest.Find(" ");
         return Upper(second >= 0 ? rest.Mid(0, second) : rest);
      }

      bool IsOneOf(const AnsiString &word, const char *const *words, size_t count)
      {
         for (size_t i = 0; i < count; i++)
         {
            if (word == words[i])
               return true;
         }
         return false;
      }
   }

   AnsiString
   MigrationImapClient::ResponseCode(const AnsiString &taggedLine)
   {
      // "<tag> <status> [CODE arguments] text"
      const int first = taggedLine.Find(" ");
      if (first < 0)
         return "";

      AnsiString rest = taggedLine.Mid(first + 1);
      const int second = rest.Find(" ");
      if (second < 0)
         return "";

      rest = rest.Mid(second + 1);
      rest.TrimLeft();
      if (!rest.StartsWith("["))
         return "";

      int end = 1;
      while (end < rest.GetLength() && rest.GetAt(end) != ']' && rest.GetAt(end) != ' ')
         end++;

      return Upper(rest.Mid(1, end - 1));
   }

   bool
   MigrationImapClient::IsCredentialRefusal(const AnsiString &taggedLine)
   {
      // RFC 5530's codes for credentials that are wrong, not allowed to act as
      // the user, or out of date; and a NO that says no more, which is how
      // most servers - this one included - refuse a wrong password.
      static const char *const refusals[] = { "AUTHENTICATIONFAILED", "AUTHORIZATIONFAILED", "EXPIRED" };

      if (StatusOf(taggedLine) != "NO")
         return false;

      const AnsiString code = ResponseCode(taggedLine);
      return code.IsEmpty() || IsOneOf(code, refusals, sizeof(refusals) / sizeof(refusals[0]));
   }

   bool
   MigrationImapClient::IsTransientRefusal(const AnsiString &taggedLine)
   {
      static const char *const passing[] = { "UNAVAILABLE", "SERVERBUG", "INUSE", "LIMIT", "THROTTLED" };

      const AnsiString status = StatusOf(taggedLine);
      if (status == "NO")
         return IsOneOf(ResponseCode(taggedLine), passing, sizeof(passing) / sizeof(passing[0]));

      // Microsoft 365, when the mailbox's back end is not reachable from the
      // front end that took the session: "BAD User is authenticated but not
      // connected." Any other BAD is a command the server will never take.
      if (status == "BAD")
         return Upper(taggedLine).Find("NOT CONNECTED") >= 0;

      return false;
   }

   void
   MigrationImapClient::ParseNamespace(const AnsiString &line, std::vector<AnsiString> &otherPrefixes)
   {
      // * NAMESPACE (("" "/")) (("Other Users/" "/")) (("Shared/" "/"))
      // - personal, other users', shared: each NIL or a list of (prefix
      // delimiter) pairs. The prefixes of the second and third are kept.
      otherPrefixes.clear();
      if (!Upper(line).StartsWith("* NAMESPACE "))
         return;

      const AnsiString rest = line.Mid(12);
      int group = 0;
      int depth = 0;
      bool firstOfPair = false;

      for (int i = 0; i < rest.GetLength(); i++)
      {
         const char c = rest.GetAt(i);

         if (c == '(')
         {
            depth++;
            if (depth == 2)
               firstOfPair = true;
            continue;
         }

         if (c == ')')
         {
            depth--;
            if (depth == 0)
               group++;
            continue;
         }

         if (depth == 0 && (c == 'N' || c == 'n') && Upper(rest.Mid(i, 3)) == "NIL")
         {
            group++;
            i += 2;
            continue;
         }

         if (c == '"')
         {
            AnsiString value;
            int j = i + 1;
            for (; j < rest.GetLength() && rest.GetAt(j) != '"'; j++)
            {
               char d = rest.GetAt(j);
               if (d == '\\' && j + 1 < rest.GetLength())
               {
                  j++;
                  d = rest.GetAt(j);
               }
               value += d;
            }

            if (depth == 2 && firstOfPair && group >= 1 && !value.IsEmpty())
               otherPrefixes.push_back(value);

            firstOfPair = false;
            i = j;
         }
      }
   }

   void
   MigrationImapClientTester::Test()
   {
      typedef MigrationImapClient C;

      Expect(C::Quote("a\"b\\c") == "\"a\\\"b\\\\c\"", "A quoted string escapes the quote and the backslash.");
      Expect(!C::NeedsLiteral("plain-Passw0rd!"), "Printable ASCII goes quoted.");
      Expect(C::NeedsLiteral("p\xC3\xA4ss"), "Eight-bit text goes as a literal.");

      __int64 length = 0;
      AnsiString before;
      Expect(C::ParseLiteralLength("* 1 FETCH (UID 7 BODY[] {1234}", length, before) && length == 1234 &&
             before == "* 1 FETCH (UID 7 BODY[] ", "A literal's length and what precedes it.");
      Expect(C::ParseLiteralLength("* LIST () \"/\" {5+}", length, before) && length == 5, "LITERAL+ is read.");
      Expect(!C::ParseLiteralLength("* OK [UIDVALIDITY 3] {not}", length, before), "Braces that are not a length are text.");

      C::Mailbox mailbox;
      Expect(C::ParseListLine("* LIST (\\HasNoChildren \\Sent) \"/\" \"Sent Items\"", mailbox) &&
             mailbox.name == "Sent Items" && mailbox.delimiter == "/" && mailbox.selectable &&
             mailbox.attributes.Find("\\Sent") >= 0, "A quoted name, its delimiter and its attributes.");
      Expect(C::ParseListLine("* LIST (\\Noselect \\HasChildren) \".\" Archive", mailbox) &&
             mailbox.name == "Archive" && !mailbox.selectable, "An atom name, and \\Noselect.");
      Expect(C::ParseListLine("* LIST () NIL INBOX", mailbox) && mailbox.name == "INBOX" && mailbox.delimiter.IsEmpty(),
             "NIL as the delimiter.");
      Expect(C::ParseListLine("* LIST () \"\\\\\" \"a\\\\b\"", mailbox) && mailbox.delimiter == "\\" && mailbox.name == "a\\b",
             "An escaped delimiter and an escaped name.");

      C::Fetched fetched;
      C::ParseFetchItems("* 12 FETCH (FLAGS (\\Seen $Label1) INTERNALDATE \"17-Jul-1996 02:44:25 -0700\" BODY[] {BODY} UID 34)", fetched);
      Expect(fetched.uid == 34 && fetched.flags == "\\Seen $Label1" && fetched.internalDate == "17-Jul-1996 02:44:25 -0700",
             "UID after the body, the flags and the internal date.");

      // What a refusal says: the credentials themselves, or the source unable
      // just now; a folder never to be opened, or one to try again.
      Expect(C::ResponseCode("M3 NO [UNAVAILABLE] Temporary authentication failure") == "UNAVAILABLE" &&
             C::ResponseCode("M3 NO Invalid user name or password.").IsEmpty(), "A completion's response code.");
      Expect(C::IsCredentialRefusal("M3 NO [AUTHENTICATIONFAILED] Invalid credentials (Failure)") &&
             C::IsCredentialRefusal("M3 NO Invalid user name or password.") &&
             !C::IsCredentialRefusal("M3 NO [UNAVAILABLE] Temporary authentication failure") &&
             !C::IsCredentialRefusal("M3 NO [ALERT] Too many simultaneous connections. (Failure)") &&
             !C::IsCredentialRefusal("M3 BAD Command syntax error"), "Only a refusal of the credentials fails a job at once.");
      Expect(C::IsTransientRefusal("M9 NO [UNAVAILABLE] Mailbox is busy") && C::IsTransientRefusal("M9 NO [INUSE] Mailbox in use") &&
             C::IsTransientRefusal("M9 BAD User is authenticated but not connected.") &&
             !C::IsTransientRefusal("M9 NO [NONEXISTENT] Unknown Mailbox") && !C::IsTransientRefusal("M9 NO Mailbox does not exist") &&
             !C::IsTransientRefusal("M9 BAD Invalid mailbox name"), "A folder refused for now, and one refused for good.");

      std::vector<AnsiString> prefixes;
      C::ParseNamespace("* NAMESPACE ((\"\" \"/\")) ((\"Other Users/\" \"/\")) ((\"Shared/\" \"/\"))", prefixes);
      Expect(prefixes.size() == 2 && prefixes[0] == "Other Users/" && prefixes[1] == "Shared/", "The other users' and shared prefixes.");
      C::ParseNamespace("* NAMESPACE ((\"\" \".\")) NIL NIL", prefixes);
      Expect(prefixes.empty(), "No other namespaces, none to leave out.");
      C::ParseNamespace("* NAMESPACE ((\"INBOX.\" \".\")) ((\"#Users.\" \".\")) NIL", prefixes);
      Expect(prefixes.size() == 1 && prefixes[0] == "#Users.", "A NIL third namespace after a list.");

      Expect(C::XOAuth2InitialResponse("a@b", "tok") == "dXNlcj1hQGIBYXV0aD1CZWFyZXIgdG9rAQE=",
             "The XOAUTH2 initial response: the address, the bearer, the separators.");
   }
}
