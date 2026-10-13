// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>

namespace HM
{
   class MessageData;
   class SURBLServer;
   class SURBLLookupPool;

   class SURBL
   {
   public:
      SURBL(void);
      ~SURBL(void);

      // What the URI lists are asked about for one message (ChooseNamesToAsk):
      // the names, in the order they are asked; how many domains the message's
      // links name - each registrable domain, by the whole Public Suffix List
      // and by its ICANN section, and each address, once - and how many of
      // those are among the names. A message whose domains are not all asked
      // about scores SurblTooManyDomainsScore (SpamTestSURBL).
      struct Choice
      {
         std::vector<String> names;
         size_t domains = 0;
         size_t domains_asked = 0;

         bool EveryDomainAsked() const { return domains_asked >= domains; }
      };

      // The names the URI lists are asked about for the links in a message:
      // every link of the text part (read as text) and of the HTML part (read
      // as HTML), and of the names NamesToAsk gives for their hosts the ones
      // ChooseNamesToAsk chooses. False when there is none.
      bool ExtractUrls(std::shared_ptr<MessageData> pMessageData, Choice &choice);

      // The most domains one list is asked about for one message, the most
      // names in all - those domains, then host names under them - and the
      // most host names of one registrable domain among them. Every domain a
      // message links to is asked about, up to MaxDomainsAsked, so that other
      // links, up to that many, do not dilute the chance that a listed one is
      // asked about. Until 6.3.6 the first 16 names were asked, and in the 6.3.6
      // tree before its release 48 drawn from all of them: the order was not
      // the sender's to choose, but the chance was - a listed link beside N
      // made-up domains was asked about in 48 of N+1 messages. A message with
      // more domains than MaxDomainsAsked is not asked about whole and scores
      // SurblTooManyDomainsScore instead, so that the padding does not come
      // free. That holds for a listed domain. A list that lists a host name and
      // not its domain is asked about the host names left room for - two drawn
      // under a domain, to MaxNamesAsked in all, in an order the message does
      // not choose - and made-up host names still make a listed one less likely
      // to be asked about, with no score for it.
      //
      // MaxNamesAsked is the hard ceiling that keeps one message from becoming
      // a DNS amplifier: a message of the stock anti-spam size limit, 1,024 KB,
      // can link to some 30,000 made-up domains, and each would be a query to
      // each active list, through this server's resolver, against the list's
      // own limit on queries from one server. 256 queries to a list for one
      // message is five times what 48 was and stays far below that, while 200
      // registrable domains is more than a newsletter of a hundred links names:
      // legitimate mail is asked about whole, and the score falls on padding.
      static constexpr size_t MaxDomainsAsked = 200;
      static constexpr size_t MaxNamesAsked = 256;
      static constexpr size_t MaxHostNamesPerDomain = 2;

      // How many of one list's lookups for one message are in flight at once
      // (Run), at most. 256 names one after another would not fit the ten
      // seconds each list has; sixteen at a time asks them in sixteen rounds
      // of one lookup's time, and holds at most sixteen of the resolver's
      // queries per list. The thread testing the message makes one of them;
      // the rest are made on threads of SURBLLookupPool::Server(), which every
      // message being tested at once shares, PoolThreads of them in all.
      static constexpr size_t LookupsAtOnce = 16;

      // The threads of SURBLLookupPool::Server(): the most that help the URI
      // lists' lookups along for the whole server, however many messages are
      // being tested at once. Each message tested makes its own lookups one at
      // a time whatever the pool is doing, and takes up to LookupsAtOnce - 1
      // of these in turn with the other messages: at the stock 15 TCP/IP
      // threads, fifteen messages at once each ask about five names at a time,
      // 256 names in some fifty lookups' time, which fits the ten seconds
      // while a lookup takes under a fifth of a second; four at once ask
      // sixteen each. In the 6.3.6 tree before its release every message
      // started LookupsAtOnce - 1 threads of its own, so the threads and the
      // resolver's queries grew with the messages being tested at once -
      // fifteen for each - and a server with many TCP/IP threads under a flood
      // of messages that link to many domains could start thousands.
      static constexpr size_t PoolThreads = 64;

      // Which of the names a message's links give the lists are asked about, in
      // the order they are asked: html_hosts are the hosts the HTML part links
      // to and text_hosts the text part's, each in the order linked. The links
      // are taken by registrable domain, each domain once, and a domain the
      // HTML part links to - what an HTML mail program shows - before one only
      // the text part does. Every domain's own names - its registrable domain,
      // and under a suffix of the list's private section the ICANN section's
      // too - are asked before any host name under one, up to max_domains
      // distinct own names; the host names follow, every domain's first before
      // any domain's second, up to max_names in all. When a part's domains, or
      // their host names, do not all fit, the domains are taken in an order
      // shuffled by seed, and when a domain has more host names than
      // MaxHostNamesPerDomain the ones asked about are drawn by seed too -
      // never in the order the message gives:
      // until 6.3.6 the first 16 names in the order of the links were asked,
      // so a message that put 16 harmless links first - in a text part an HTML
      // mail program never shows - kept its listed one from every list. The
      // choice says how many domains there were and how many are asked.
      static Choice ChooseNamesToAsk(const std::vector<String> &html_hosts, const std::vector<String> &text_hosts,
                                     size_t max_domains, size_t max_names, unsigned int seed);

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

      // Asks one list about the names, up to LookupsAtOnce at a time - the
      // calling thread and threads of SURBLLookupPool::Server() - and answers
      // false when the list lists one of them. No lookup is begun after ten
      // seconds, or once the list has listed a name or refused a query; those
      // already begun are waited for, and a listing among them counts. A name
      // whose lookup did not complete, asked twice
      // (DNSResolver::GetDnsListAnswers), is passed over and the rest are
      // asked (upstream PR 640): before 6.3.7 it ended the list for the
      // message, and a listed link after it was never asked about.
      bool Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls);

      // Run with the lookup of its own: lookup(name, answers) is the A lookup
      // of name, false when it was not answered. What Run asks the resolver is
      // asked of it, from up to LookupsAtOnce threads at once - the calling
      // thread and threads of pool (the server's, when none is given). For the
      // class test, which counts the lookups, how many are in flight together
      // and on which threads.
      typedef std::function<bool(const String &name, std::vector<String> &answers)> Lookup;
      bool Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls, const Lookup &lookup);
      bool Run(std::shared_ptr<SURBLServer> pSURBLServer, const std::vector<String> &vecUrls, const Lookup &lookup,
               SURBLLookupPool &pool);

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
      // name, the host is percent-decoded as UTF-8, a host that is not ASCII is
      // read by UTS 46 as the URL standard's domain parser reads it (Idna.h) -
      // the code points it ignores deleted (a soft hyphen, a zero-width space,
      // the variation selectors), the ones it maps replaced (fullwidth letters
      // and digits by ASCII ones, U+3002 and the other full stops by '.'), NFC,
      // and each label in its ASCII form, "xn--" and its Punycode for one in
      // Unicode - an IPv4 address may be written in any of the
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
      // name), where those are a name; a label in Unicode that UTS 46 refuses
      // (a code point it disallows, the bidi or joiner rules broken), or whose
      // xn-- form is longer than 63 characters, is such a label. A host that is
      // not ASCII is read in more ways than a browser's, and the hosts each
      // reading hands over that those before it did not are handed over after
      // them: as it is written, as 6.3.6 read every host - percent-decoded, the
      // code points UTS 46 ignores deleted, ASCII in lower case, nothing
      // mapped, so that any character past ASCII is one a name cannot hold -
      // so that every host 6.3.6 handed over for a link is handed over still;
      // and, read by UTS 46, the part of the host before each place where a
      // mail program may end a link in running text, which UTS 46 would take
      // into the host's last label or make a label of - a closing quotation
      // mark, a guillemet, a dash, CJK punctuation, CJK text after U+3002. A
      // mail program ends a link at the first of the characters it ends links
      // at, and they differ, so the places are the first character past ASCII,
      // written as itself (not percent-encoded, which a mail program sees as
      // ASCII), of each of five kinds - punctuation that UTS 46 maps to a full
      // stop (U+3002), to a name's characters alone (a fullwidth hyphen-minus)
      // or to anything else (a quotation mark, a guillemet, a dash, CJK
      // punctuation), and a symbol it maps to a name's characters alone (the
      // trade mark sign, a circled letter) or to anything else (a heart, the
      // copyright sign, an emoji) - where the run of punctuation and symbols
      // the host part ends with begins, and the first full stop past ASCII
      // before text past ASCII: seven at most, however many such characters
      // the host holds (SURBL.cpp's LinkEnds_). So a link followed by such a
      // character hands over the name before it, whatever the name is written
      // in and whatever symbols it or a label before it holds -
      // http://<fullwidth evil>.example<U+201D> hands over
      // evil.xn--example-b46c and evil.example, and http://i<U+2665>.ws<U+201D>
      // xn--i-n3p.ws - and so does such a host in an attribute's value, which a
      // browser is given whole: one more name asked about, as for an ASCII
      // one. A host that
      // is not ASCII and can lead nowhere - more than 1,012 code points UTS 46
      // neither deletes nor makes a trailing full stop of, one of them mapped
      // past ASCII or five full stops among them, so that its ASCII form would
      // be longer than any name and it is no address, or a code point past
      // ASCII that UTS 46 maps to text a domain may not hold (U+FDFA's spaces,
      // a no-break space, U+2100's '/'), which the URL standard's host parser
      // fails - is not read by UTS 46 at all, and hands over what it does as
      // written and before such a
      // character. Any other IPv6 address hands over nothing. html: text is HTML,
      // whose character references are read as the browser reads them - every
      // named one HTML defines (HtmlReferences.h), and one HTML reads without
      // its ';' read so but in an attribute's value before an '=' or a letter
      // or digit - and a link opened by a quote runs to the closing quote; and
      // a quoted value of a tag is read again as the URL parser reads it, its
      // tabs and line breaks deleted, and the hosts only that reading finds
      // are handed over too.
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
      // port, percent-decoded as UTF-8, read by UTS 46 when it is not ASCII
      // (each label in its ASCII form, so b<U+00FC>cher.example is
      // xn--bcher-kva.example), and in lower case - a name without its
      // trailing dots and its leading "www.", or an IPv4 address in dotted
      // decimal, which an IPv4-mapped or NAT64 IPv6 address in brackets is too.
      // Empty when that is not one name or address a list could be asked
      // about: any other IPv6 address, a name with a character a name cannot
      // hold, a host that is not ASCII with a label UTS 46 refuses (a browser
      // fails it) or that can lead nowhere (FindLinkHosts says when), one
      // label, nothing. The URL
      // entries of the sender lists (SenderPattern) are read with it, so that
      // an entry and the links it is matched against name a host alike: an
      // entry written in Unicode covers a link written in its xn-- form, and
      // the other way round.
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

      void Collect_(std::shared_ptr<MessageData> pMessageData, Choice &out);
   };

   // The threads that help the URI lists' lookups along (SURBL::Run). The
   // server has one pool, Server(), which every message being tested at once
   // shares, so that a flood of messages that link to many domains costs a
   // bounded number of threads and of the resolver's queries - the pool's
   // threads on top of those testing the messages - and is shared fairly
   // among the messages, so that none waits for another's lookups to be done
   // while its list's ten seconds run out, after which the list goes unasked
   // for it and it is accepted without the list's verdict:
   //   - the thread testing a message makes its lookups itself, whatever the
   //     pool is doing, so every message goes on one lookup at a time at
   //     least;
   //   - a thread of the pool makes one lookup for a message and then turns to
   //     the next message in turn, so that the messages being tested share
   //     the pool, and one that came first with many names does not keep it
   //     until it is done;
   //   - no message has more than helpersPerRun of its threads at once, so
   //     that none takes the whole pool.
   // Threads are started as messages need them, up to the pool's size, and a
   // thread with nothing to do for idleMilliseconds ends.
   class SURBLLookupPool
   {
   public:
      SURBLLookupPool(size_t threads, size_t helpersPerRun, int idleMilliseconds);

      // Asks the pool's threads to end and waits until they have: nothing may
      // be running alongside then. The server's pool is never destroyed.
      ~SURBLLookupPool();

      SURBLLookupPool(const SURBLLookupPool &) = delete;
      SURBLLookupPool &operator=(const SURBLLookupPool &) = delete;

      // The server's: SURBL::PoolThreads threads, LookupsAtOnce - 1 for one
      // message at most, a minute's idleness.
      static SURBLLookupPool &Server();

      // step run on the calling thread until it answers false, and meanwhile
      // on up to helpers of the pool's threads (helpersPerRun at most), in
      // turn with what else runs alongside. step makes one lookup, answers
      // false when there was none left to make, and never throws. Returns once
      // it has answered false here and none of the pool's threads is inside
      // it.
      void RunAlongside(const std::function<bool()> &step, size_t helpers);

      // For the class test: threads ever started, threads now, and the most at
      // once.
      struct Counts
      {
         size_t started = 0;
         size_t running = 0;
         size_t most_running = 0;
      };

      Counts GetCounts() const;

      struct State;

   private:
      std::shared_ptr<State> state_;
   };

   // SURBL's reading of links held to what a browser does with them: the
   // userinfo, ports, IPv6 and the IPv4 addresses it carries, percent-encoding
   // as UTF-8, UTS 46 (the code points it ignores, fullwidth letters and
   // digits, the ideographic full stops, NFC, xn-- forms, the labels it
   // refuses, a megabyte of a code point it maps to eighteen, and a host that
   // can lead nowhere weighed and not mapped, a megabyte of short or empty
   // labels among them, and one that holds a code point mapped to text a
   // domain may not hold), the host as it is written beside it, the host
   // before each place a mail program may end a link however it is written -
   // seven places at most, and each part read as it is read from nothing -
   // the IPv4 forms,
   // backslashes,
   // links with no scheme, quoted-printable soft line breaks, HTML character
   // references - every named one HTML defines, read as the tokenizer reads
   // it, in an attribute's value and in text - and quotes, and punctuation
   // after a link, past ASCII as well as in it; the names asked
   // about for each host, the ICANN section's domain among them; which of a
   // message's names are asked - every domain up to MaxDomainsAsked, whatever
   // else the message links to, and a message with more told apart - and that
   // Run asks a list about all of them several at a time, through a stand-in
   // for the resolver, passes over a lookup that fails and asks the rest, and
   // stops at one that throws; that many
   // messages tested at once share a pool's threads, held to its size, each
   // message in turn and none with more than its share; the lists'
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
