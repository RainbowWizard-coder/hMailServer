// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OutboundOAuth2TokenClient.h"

#include "../Application/IniFileSettings.h"
#include "Charset.h"
#include "HttpsClient.h"
#include "Parsing/StringParser.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <time.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A stalled identity provider must cost a bounded wait on the delivery
      // thread, exactly as a stalled MTA-STS policy host does: the whole request,
      // from the name lookup to the end of the answer.
      const int HttpsTimeoutSeconds = 20;

      // A token response is a small JSON object; a megabyte of it is not one.
      const size_t MaxResponseSize = 1024 * 1024;

      __int64 CurrentUnixTime()
      {
         return static_cast<__int64>(::time(nullptr));
      }
   }

   OutboundOAuth2TokenClient::OutboundOAuth2TokenClient()
   {
   }

   bool
   OutboundOAuth2TokenClient::IsFetchHost(const String &serverAddress)
   {
      String address = serverAddress;
      address.Trim();
      if (address.IsEmpty())
         return false;

      std::vector<String> hosts = StringParser::SplitString(IniFileSettings::Instance()->GetFetchOAuth2Hosts(), ",");
      for (String host : hosts)
      {
         host.Trim();
         if (!host.IsEmpty() && host.CompareNoCase(address) == 0)
            return true;
      }

      return false;
   }

   void
   OutboundOAuth2TokenClient::Invalidate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      cached_token_.Empty();
      cache_expires_at_ = 0;
   }

   bool
   OutboundOAuth2TokenClient::GetToken(String &token, String &errorMessage)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (!cached_token_.IsEmpty() && CurrentUnixTime() < cache_expires_at_)
      {
         token = cached_token_;
         return true;
      }

      __int64 expiresInSeconds = 0;
      if (!FetchToken_(token, expiresInSeconds, errorMessage))
         return false;

      cached_token_ = token;

      // Refresh at 80% of the reported lifetime, floored so a provider
      // reporting very short lifetimes still gets a cache rather than a
      // fetch per message, and never longer than the token actually lives.
      __int64 usable = (expiresInSeconds * 8) / 10;
      if (usable < 60)
         usable = expiresInSeconds > 60 ? expiresInSeconds - 30 : expiresInSeconds;

      cache_expires_at_ = CurrentUnixTime() + usable;

      return true;
   }

   AnsiString
   OutboundOAuth2TokenClient::UrlEncode_(const String &value)
   {
      AnsiString utf8 = Charset::ToMultiByte(value, "utf-8");

      AnsiString encoded;
      for (int i = 0; i < utf8.GetLength(); i++)
      {
         unsigned char ch = static_cast<unsigned char>(utf8[i]);

         bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
                           ch == '_' || ch == '~';

         if (unreserved)
         {
            encoded += static_cast<char>(ch);
         }
         else
         {
            char escaped[4];
            sprintf_s(escaped, sizeof(escaped), "%%%02X", ch);
            encoded += escaped;
         }
      }

      return encoded;
   }

   bool
   OutboundOAuth2TokenClient::FetchToken_(String &token, __int64 &expiresInSeconds, String &errorMessage)
   {
      IniFileSettings *settings = IniFileSettings::Instance();

      String url = settings->GetOutboundOAuth2TokenUrl();
      String clientId = settings->GetOutboundOAuth2ClientId();
      String clientSecret = settings->GetOutboundOAuth2ClientSecret();
      String scope = settings->GetOutboundOAuth2Scope();

      if (url.IsEmpty() || clientId.IsEmpty() || clientSecret.IsEmpty())
      {
         errorMessage = _T("Outbound OAuth2 is not fully configured: OutboundOAuth2TokenUrl, OutboundOAuth2ClientId and OutboundOAuth2ClientSecret are all required.");
         return false;
      }

      // The endpoint must be HTTPS: the request carries the client secret, and
      // an identity provider that offered plain HTTP would not be one.
      if (url.Mid(0, 8).CompareNoCase(_T("https://")) != 0)
      {
         errorMessage = _T("OutboundOAuth2TokenUrl must be an https:// URL.");
         return false;
      }

      String remainder = url.Mid(8);
      int slash = remainder.Find(_T("/"));
      String host = slash >= 0 ? remainder.Mid(0, slash) : remainder;
      AnsiString path = slash >= 0 ? AnsiString(remainder.Mid(slash)) : AnsiString("/");

      AnsiString body;
      body.append("grant_type=client_credentials&client_id=");
      body.append(UrlEncode_(clientId));
      body.append("&client_secret=");
      body.append(UrlEncode_(clientSecret));
      body.append("&scope=");
      body.append(UrlEncode_(scope));

      AnsiString responseBody;
      if (!HttpsPost_(host, path, body, responseBody, errorMessage))
         return false;

      try
      {
         std::stringstream stream(std::string(responseBody.c_str()));
         boost::property_tree::ptree parsed;
         boost::property_tree::read_json(stream, parsed);

         std::string accessToken = parsed.get<std::string>("access_token", "");
         __int64 expiresIn = parsed.get<__int64>("expires_in", 0);

         if (accessToken.empty())
         {
            std::string error = parsed.get<std::string>("error", "");
            std::string description = parsed.get<std::string>("error_description", "");

            errorMessage = _T("The token endpoint returned no access token. ");
            if (!error.empty())
               errorMessage += String(error.c_str()) + _T(": ") + String(description.c_str());

            return false;
         }

         token = accessToken.c_str();
         expiresInSeconds = expiresIn > 0 ? expiresIn : 300;
         return true;
      }
      catch (...)
      {
         errorMessage = _T("The token endpoint's response could not be parsed as JSON.");
         return false;
      }
   }

   bool
   OutboundOAuth2TokenClient::HttpsPost_(const String &host, const AnsiString &path, const AnsiString &body,
                                         AnsiString &responseBody, String &errorMessage)
   {
      // Through HttpsClient, whose deadline bounds the whole request - the name
      // lookup, the connection, the handshake, sending and reading the answer to
      // its end - on every platform. The synchronous client this replaces handed
      // SO_RCVTIMEO a DWORD, which Linux refuses (it takes a timeval), and Asio's
      // synchronous calls on POSIX wait with no timeout anyway: a token endpoint
      // that accepted and never answered held the delivery thread for good, and
      // every thread behind it that wanted a token waited on the cache's lock.
      // Certificate verification against the machine's store with the host name
      // checked, SNI and the TLS 1.2 floor are HttpsClient's, as they were this
      // client's; so is a port in the URL, which the old client resolved as part
      // of the host name, and HttpProxy, which it did not use.
      HttpsClient::Response response;
      String error;
      if (!HttpsClient::Request("POST", AnsiString("https://") + AnsiString(host) + path, std::vector<AnsiString>(),
                                "application/x-www-form-urlencoded", body, response, error, HttpsTimeoutSeconds, MaxResponseSize))
      {
         errorMessage = String(_T("Could not reach the token endpoint: ")) + error;
         return false;
      }

      // An OAuth error is a 400 whose JSON carries the reason, which FetchToken_
      // reads and which is worth more than "not 200"; only an answer with nothing
      // to read is an error here.
      responseBody = response.body;
      if (response.status_code != 200 && responseBody.IsEmpty())
      {
         errorMessage.Format(_T("The token endpoint answered HTTP %d with nothing to read."), response.status_code);
         return false;
      }

      return true;
   }
}
