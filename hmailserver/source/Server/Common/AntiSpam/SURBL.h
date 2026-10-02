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
      // every link of the text part (read as text) and of the HTML part (read
      // as HTML), and of the names NamesToAsk gives for their hosts the ones
      // ChooseNamesToAsk chooses, MaxNamesAsked at most. False when there is
      // none.
      bool ExtractUrls(std::shared_ptr<MessageData> pMessageData, std::vector<String> &vecUrls);

      // The most names one list is asked about for one message - each list
      // also stops after ten seconds - and the most host names of one
      // registrable domain among them, beside the domain's own names.
      static constexpr size_t MaxNamesAsked = 48;
      static constexpr size_t MaxHostNamesPerDomain = 2;

      // Which of the names a message's links give the lists are asked about, in
      // the order they are asked: html_hosts are the hosts the HTML part links
      // to and text_hosts the text part's, each in the order linked. The links
      // are taken by registrable domain, each domain once, and a domain the
      // HTML part links to - what an HTML mail program shows - before one only
      // the text part does. Within that, every domain's own name is asked
      // before any domain's second name, and so on, up to max_names. When a
      // part's domains do not all fit, they are taken in an order shuffled by
      // seed, and when a domain has more host names than MaxHostNamesPerDomain
      // the ones asked about are drawn by seed too - never in the order the
      // message gives: until 6.3.6 the first 16
      // names in the order of the links were asked, so a message that put 16
      // harmless links first - in a text part an HTML mail program never shows
      // - kept its listed one from every list.
      static std::vector<String> ChooseNamesToAsk(const std::vector<String> &html_hosts, const std::vector<String> &text_hosts,
                                                  size_t max_names, unsigned int seed);

      // No limit on the text read (FindLinkHosts, ForEachLinkHost).
      static constexpr size_t WholeText = static_cast<size_t>(-1);

      // The host each link in a message goes to, handed to visit in the order
      // the links come - the text part read as text, then the HTML part as
      // HTML (FindLinkHosts) - until visit returns false: a name as the link
      // has it, an IPv4 address in its own order. The names an HTML
      // document's type and fonts name (w3.org, fonts.googleapis.com and the
      // like) are left out. Every link, however many: the URL entries of the
      // sender lists read them all (SenderListCache::ReadUrlLinks), as the
      // SURBL test does before it chooses what to ask (ExtractUrls).
      // maxCharacters, when given, is how much of each of the two parts is
      // read (FindLinkHosts).
      static void ForEachLinkHost(std::shared_ptr<MessageData> pMessageData, const std::function<bool(const String &host)> &visit,
                                  size_t maxCharacters = WholeText);

      bool Run(std::shared_ptr<SURBLServer> pSURBLServer, std::vector<String> &vecUrls);

      // The host each link in text goes to, handed to visit in the order the
      // links come, until visit returns false. A link is an http or https
      // scheme followed by any slashes and backslashes, "://" after any scheme
      // or none, or either percent-encoded, its slashes encoded or not
      // (http%3A%5C%5C, %3A%2F%2F, http%3A//: a link inside another link's
      // query); or, with no scheme, two slashes or backslashes or more that
      // begin an HTML attribute's value (href="//evil.example/", which a
      // browser resolves against the page's own scheme) or that follow a query
      // parameter's '=' (?u=//evil.example, ?u=%2F%2Fevil.example). Its
      // authority is read as RFC 3986 and the WHATWG URL standard read it: the
      // host follows the last '@' before the path, a port is not part of a
      // name, the host is percent-decoded as UTF-8 and the code points UTS 46
      // ignores are deleted from it (a soft hyphen, a zero-width space, the
      // variation selectors), an IPv4 address may be written in any of the
      // forms a browser accepts (0x7f.1, 2130706433), and an IPv6 address in
      // brackets is the IPv4 address it carries when it is IPv4-mapped
      // (::ffff:192.0.2.1) or under the NAT64 prefix (64:ff9b::c000:201), and
      // nothing otherwise. A link in running text is made by the mail program
      // that shows it, which may end it before the '@', at a ')', a '"' or a
      // '}': such a link also hands over the name before that character
      // (http://evil.example)@x.example/ hands over x.example and
      // evil.example); and at a code point UTS 46 ignores that a script's \s
      // matches (U+FEFF, U+180E, U+200B): such a link also hands over the hosts
      // before the first of each of them. What is handed over is a name in
      // lower case with no trailing dot and no leading "www.", of two labels or
      // more, each of 1 to 63 of a-z, 0-9, '-' and '_', or an IPv4 address in
      // dotted decimal. A host with any other character in it hands over the
      // name up to that character (a link followed by punctuation) and the
      // labels after the last label that holds one (a working link's registered
      // name), where those are a name. Any other IPv6 address, and a host that
      // is only Unicode, hand over nothing: an internationalised name is not
      // mapped by UTS 46 nor turned into its xn-- form. html: text is HTML,
      // whose character references are read as the browser reads them, and a
      // link opened by a quote runs to the closing quote; and a quoted value of
      // a tag is read again as the URL parser reads it, its tabs and line
      // breaks deleted, and the hosts only that reading finds are handed over
      // too.
      //
      // maxCharacters: a text longer than this is read only as far as its
      // first part (FirstPartForLinks), and a link whose reading runs on to
      // the end of that part is not handed over, since what follows the cut
      // could make its host another; a link that ends before it is read as
      // in the whole text. So the time is that of reading maxCharacters,
      // whatever the text's length, and no host is handed over that the
      // whole text would not hand over.
      static void FindLinkHosts(const String &text, bool html, const std::function<bool(const String &host)> &visit,
                                size_t maxCharacters = WholeText);

      // The part of a text longer than maxCharacters that FindLinkHosts reads:
      // its first maxCharacters characters, less - in an HTML part - a
      // character reference the cut would split, an '&' that nothing but the
      // characters a reference is written in ('#', letters and digits)
      // follows to the cut, and what follows it. The whole text when it is not
      // longer.
      static String FirstPartForLinks(const String &text, bool html, size_t maxCharacters);

      // What the lists are asked about for one host FindLinkHosts handed over:
      // an IPv4 address reversed; a name, its registrable domain by the whole
      // Public Suffix List, and, where that differs, its registrable domain
      // by the list's ICANN section alone - tenant.github.io is asked about as
      // itself and as github.io, since a list that is not wildcarded (URIBL)
      // lists a shared-hosting domain under that name.
      static std::vector<String> NamesToAsk(const String &host);

      // The host a link whose authority is this goes to, in the form
      // FindLinkHosts hands it over: the part after the last '@', without its
      // port, percent-decoded as UTF-8 with the code points UTS 46 ignores
      // deleted, and in lower case - a name without its trailing dots and its
      // leading "www.", or an IPv4 address in dotted decimal, which an
      // IPv4-mapped or NAT64 IPv6 address in brackets is too. Empty when that
      // is not one name or address a list could be asked about: any other
      // IPv6 address, a name with a character a name cannot hold, one label,
      // nothing. The URL entries of the sender lists (SenderPattern) are read
      // with it, so that an entry and the links it is matched against name a
      // host alike.
      static String HostOfAuthority(const String &authority);

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

      void Collect_(std::shared_ptr<MessageData> pMessageData, std::vector<String> &out);
   };

   // SURBL's reading of links held to what a browser does with them: the
   // userinfo, ports, IPv6 and the IPv4 addresses it carries, percent-encoding
   // as UTF-8 and the code points UTS 46 ignores, the IPv4 forms, backslashes,
   // links with no scheme, quoted-printable soft line breaks, HTML character
   // references and quotes, and punctuation after a link; the names asked
   // about for each host, the ICANN section's domain among them; the lists'
   // error codes; and the sender lists' URL entries, whose host is read the
   // same way (HostOfAuthority) and which are matched against every link of a
   // message (SenderPattern::UrlEntryHostsCovering). It runs inside the
   // service with the other class tests (ClassTester), and on Linux in
   // public-suffix-tests. No DNS, no database, no setting. It throws
   // std::logic_error naming the first thing that is wrong.
   class SURBLTester
   {
   public:
      void Test();
   };
}
