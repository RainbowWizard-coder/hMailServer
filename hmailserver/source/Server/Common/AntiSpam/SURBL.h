// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>

namespace HM
{
   class MessageData;
   class SURBLServer;

   class SURBL
   {
   public:
      SURBL(void);
      ~SURBL(void);

      // The names the URI lists are asked about for the links in a message:
      // each link's host and its registrable domain, and an IPv4 address with
      // its octets reversed, at most 16 of them. The text part is read as text
      // and the HTML part as HTML (FindLinkHosts). False when there is none.
      bool ExtractUrls(std::shared_ptr<MessageData> pMessageData, std::vector<String> &vecUrls);

      // The hosts the links in a message go to, for the URL entries of the
      // sender lists (SenderListCache::DecideUrls): a name as the link has it,
      // an IPv4 address in its own order, at most 16 of them. False when there
      // is none.
      bool ExtractHosts(std::shared_ptr<MessageData> pMessageData, std::vector<String> &hosts);

      bool Run(std::shared_ptr<SURBLServer> pSURBLServer, std::vector<String> &vecUrls);

      // The host each link in text goes to, handed to visit in the order the
      // links come, until visit returns false. A link is an http or https
      // scheme followed by any slashes and backslashes, "://" after any scheme
      // or none, or either percent-encoded (http%3A%5C%5C, %3A%2F%2F: a link
      // inside another link's query). Its authority is read as RFC 3986 and
      // the WHATWG URL standard read it: the host follows the last '@' before
      // the path, a port and a bracketed IPv6 address are not part of a name,
      // the host is percent-decoded, and an IPv4 address may be written in any
      // of the forms a browser accepts (0x7f.1, 2130706433). A link in running
      // text is made by the mail program that shows it, which may end it
      // before the '@', at a ')', a '"' or a '}': such a link also hands over
      // the name before that character (http://evil.example)@x.example/ hands
      // over x.example and evil.example). What is handed over is a name in
      // lower case with no trailing dot and no leading "www.", of two labels or
      // more, each of 1 to 63 of a-z, 0-9, '-' and '_', or an IPv4 address in
      // dotted decimal. A host with any other character in it hands over the
      // name up to that character (a link followed by punctuation) and the
      // labels after the last label that holds one (a working link's
      // registered name), where those are a name. An IPv6 address, and a host
      // that is only Unicode, hand over nothing. html: text is HTML, whose
      // character references are read as the browser reads them, and a link
      // opened by a quote runs to the closing quote.
      static void FindLinkHosts(const String &text, bool html, const std::function<bool(const String &host)> &visit);

      // What the lists are asked about for one host FindLinkHosts handed over:
      // an IPv4 address reversed, a name and its registrable domain.
      static std::vector<String> NamesToAsk(const String &host);

      // What a list's answer is. The URI lists answer some queries with a code
      // that is not a verdict on the name, and that code is never a listing,
      // whatever the server's expected result says:
      //   127.255.255.0/24  Spamhaus's errors (DNSBL usage FAQ): .252 the
      //                     zone's name is mistyped, .254 the query came
      //                     through a public resolver, .255 too many queries.
      //                     Refused: every query of this server is.
      //   127.0.0.1         SURBL and URIBL: the query was blocked. Blocked:
      //                     not a refusal of every query, since Hostkarma
      //                     answers it to a name on its whitelist.
      //   127.0.1.255       Spamhaus DBL, and 127.0.2.255 Spamhaus ZRD: IP
      //   127.0.2.255       queries not supported. Each answers it to every
      //                     address it is asked about (Spamhaus's dataset
      //                     documentation: "under any aspect treated as an
      //                     error code").
      // meaning is the list's reason, in a phrase for the log.
      enum AnswerKind
      {
         AnswerVerdict = 0,
         AnswerRefused = 1,
         AnswerNotForAddresses = 2,
         AnswerBlocked = 3
      };

      static AnswerKind ClassifyAnswer(const String &answer, String &meaning);

   private:

      bool Collect_(std::shared_ptr<MessageData> pMessageData, bool namesToAsk, std::vector<String> &out);
   };

   // SURBL's reading of links held to what a browser does with them: the
   // userinfo, ports, IPv6, percent-encoding, the IPv4 forms, backslashes,
   // quoted-printable soft line breaks, HTML character references and quotes,
   // and punctuation after a link; the names asked about for each host; and
   // the lists' error codes. It runs inside the service with the other class
   // tests (ClassTester), and on Linux in public-suffix-tests. No DNS, no
   // database, no setting. It throws std::logic_error naming the first thing
   // that is wrong.
   class SURBLTester
   {
   public:
      void Test();
   };
}
