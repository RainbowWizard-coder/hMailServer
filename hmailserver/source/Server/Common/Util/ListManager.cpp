// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ListManager.h"
#include "ListArchive.h"
#include "ByteBuffer.h"
#include "DisclaimerAdder.h"
#include "DistributionListMembership.h"
#include "MessageOrigin.h"
#include "MessageUtilities.h"
#include "Utilities.h"
#include "Unicode.h"
#include "File.h"
#include "Time.h"
#include "Hashing/HashCreator.h"

#include "../Application/ObjectCache.h"
#include "../AntiSpam/DKIM/DKIMSigner.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/Domain.h"
#include "../BO/DomainAliases.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/Message.h"
#include "../BO/MessageData.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../BO/ServerMessages.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentMessageRecipient.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentServerMessage.h"
#include "../TCPIP/IPAddress.h"
#include "../../SMTP/RecipientParser.h"
#include "../../SMTP/DeliveryFailure.h"
#include "../../SMTP/DeliveryReportParser.h"
#include "../../SMTP/SMTPConfiguration.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const size_t TokenBytes = 32;

      // A confirmation, a refusal, the help or a request to leave goes to one
      // address of one list at most once an hour. Somebody who forges requests
      // in another person's name makes the list write to that person once, not
      // once per request.
      const int ThrottleSeconds = 3600;

      // And a list answers at most this many requests an hour from one
      // requester - the page's client, or the server that handed a mail to
      // this one - so that one stranger cannot spend the list's answers and
      // close it to everybody else; and at most this many an hour in all, so
      // that forged mail naming a thousand victims makes the list write to
      // two hundred of them at most, not a thousand.
      const int MostNoticesPerRequesterPerHour = 20;
      const int MostNoticesPerListPerHour = 200;

      // The pages: requests an hour from one address.
      const int PageRequestsPerAddressPerHour = 120;

      // How many entries each in-memory map keeps before its oldest go: the
      // throttle, the page budget and the notice budgets.
      const size_t PageBudgetCeiling = 10000;

      // How much of a bounce is read for its report and its member tag.
      const size_t MostBounceBytes = 2 * 1024 * 1024;

      // How much of a command message's text is searched for a token.
      const int MostCommandTextCharacters = 16 * 1024;

      const wchar_t *const SuffixSubscribe = L"-subscribe";
      const wchar_t *const SuffixUnsubscribe = L"-unsubscribe";
      const wchar_t *const SuffixRequest = L"-request";
      const wchar_t *const SuffixOwner = L"-owner";
      const wchar_t *const SuffixBounces = L"-bounces";

      std::mutex throttle_mutex;
      std::map<String, time_t> throttled_until;

      std::mutex budget_mutex;
      struct Budget
      {
         time_t since = 0;
         int count = 0;
      };
      std::map<AnsiString, Budget> budgets;

      std::mutex notice_mutex;
      std::map<String, Budget> notice_budgets;

      // Each map's entries past its ceiling go, oldest first - never the map
      // wholesale, which would hand every address its hour back at once.
      template <class Key, class Value, class Age>
      void EvictOldest(std::map<Key, Value> &entries, size_t ceiling, Age age)
      {
         if (entries.size() <= ceiling)
            return;

         std::vector<std::pair<time_t, Key> > byAge;
         byAge.reserve(entries.size());
         for (const auto &entry : entries)
            byAge.push_back(std::make_pair(age(entry.second), entry.first));

         std::sort(byAge.begin(), byAge.end());

         const size_t excess = entries.size() - ceiling;
         for (size_t i = 0; i < excess && i < byAge.size(); i++)
            entries.erase(byAge[i].second);
      }

      // A value for a query string: letters, digits and "-._~@" as they are,
      // everything else percent-encoded from its UTF-8 bytes.
      AnsiString UrlEncode(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         static const char *hex = "0123456789ABCDEF";
         AnsiString out;
         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char c = (unsigned char) utf8.GetAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '.' || c == '_' || c == '~' || c == '@')
               out += (char) c;
            else
            {
               out += '%';
               out += hex[(c >> 4) & 0x0f];
               out += hex[c & 0x0f];
            }
         }

         return out;
      }

      // Whether any list is managed at all: -1 not known, 0 none, 1 some, and
      // when that was asked. What keeps the delivery path free on a server that
      // has no managed list, which is every server until somebody makes one.
      std::mutex managed_mutex;
      int managed_state = -1;
      time_t managed_checked = 0;
      const int ManagedStateSeconds = 60;

      String Lowered(const String &value)
      {
         String lowered = value;
         lowered.Trim();
         lowered.ToLower();
         return lowered;
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      // One header value, with anything that could start another header taken
      // out: every value written here is an address or a text an administrator
      // or a stranger chose.
      String OneLine(const String &value)
      {
         String flat = value;
         flat.Replace(_T("\r"), _T(" "));
         flat.Replace(_T("\n"), _T(" "));
         flat.Trim();
         return flat;
      }

      bool IsAllAscii(const String &value)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            if (value.GetAt(i) > 126 || value.GetAt(i) < 32)
               return false;
         }

         return true;
      }

      // The local part of a managed list's command address, read without the
      // server: which command, which list local part, and for the VERP form
      // the member it encodes. Pure, so that ListManagerTester can pin it.
      bool SplitLocalPart(const String &localPart, String &listLocal, ListManager::Command &command, String &verpMember)
      {
         listLocal.Empty();
         verpMember.Empty();
         command = ListManager::CommandNone;

         String lowered = localPart;
         lowered.ToLower();

         // The VERP form first: name-bounces+local=domain. The member's own
         // local part may hold a plus or an equals sign of its own, so the
         // split is at the FIRST plus after -bounces and the LAST equals sign
         // after it, since a domain holds neither.
         const String bouncesPlus = String(SuffixBounces) + _T("+");
         const int verpAt = lowered.Find(bouncesPlus);
         if (verpAt > 0)
         {
            const String encoded = localPart.Mid(verpAt + bouncesPlus.GetLength());
            const int equals = encoded.ReverseFind(_T("="));
            if (equals <= 0 || equals == encoded.GetLength() - 1)
               return false;

            const String memberLocal = encoded.Mid(0, equals);
            const String memberDomain = encoded.Mid(equals + 1);
            if (memberDomain.Find(_T(".")) <= 0)
               return false;

            listLocal = localPart.Mid(0, verpAt);
            verpMember = memberLocal + _T("@") + memberDomain;
            verpMember.ToLower();
            command = ListManager::CommandBounces;
            return true;
         }

         struct Suffix
         {
            const wchar_t *text;
            ListManager::Command command;
         };

         // -unsubscribe before -subscribe, so that the longer is read whole.
         const Suffix suffixes[] =
         {
            { SuffixUnsubscribe, ListManager::CommandUnsubscribe },
            { SuffixSubscribe, ListManager::CommandSubscribe },
            { SuffixRequest, ListManager::CommandRequest },
            { SuffixOwner, ListManager::CommandOwner },
            { SuffixBounces, ListManager::CommandBounces },
         };

         for (const Suffix &suffix : suffixes)
         {
            const String text = suffix.text;

            if (lowered.GetLength() > text.GetLength() && lowered.EndsWith(text))
            {
               listLocal = localPart.Mid(0, localPart.GetLength() - text.GetLength());
               command = suffix.command;
               return true;
            }
         }

         return false;
      }

      String MacroText(String text, const std::vector<std::pair<String, String> > &macros)
      {
         for (const std::pair<String, String> &macro : macros)
            text.Replace(macro.first, macro.second);

         return text;
      }
   }

   // ---- addresses ------------------------------------------------------------

   bool
   ListManager::ParseCommandAddress(const String &address, Command &command, std::shared_ptr<const DistributionList> &list, String &verpMember)
   {
      command = CommandNone;
      list.reset();
      verpMember.Empty();

      const int at = address.ReverseFind(_T("@"));
      if (at < 1 || at == address.GetLength() - 1)
         return false;

      const String localPart = address.Mid(0, at);
      const String domain = address.Mid(at + 1);

      String listLocal;
      Command parsed = CommandNone;
      String member;

      // The cheap test first: an address that ends in none of the five words
      // costs nothing more, and that is nearly every address there is - this
      // runs at every RCPT TO to a hosted domain.
      if (!SplitLocalPart(localPart, listLocal, parsed, member) || listLocal.IsEmpty())
         return false;

      // An administrator's address of exactly this name wins: an account, an
      // alias or a list called name-owner@ is theirs, not the manager's.
      if (CacheContainer::Instance()->GetAccount(address) ||
          CacheContainer::Instance()->GetAlias(address) ||
          CacheContainer::Instance()->GetDistributionList(address))
         return false;

      std::shared_ptr<const DistributionList> base = CacheContainer::Instance()->GetDistributionList(listLocal + _T("@") + domain);
      if (!base || !base->GetManaged() || !base->GetActive())
         return false;

      command = parsed;
      list = base;
      verpMember = member;
      return true;
   }

   bool
   ListManager::ParseAddress(const String &address, Command &command, std::shared_ptr<const DistributionList> &list, String &verpMember)
   {
      if (ParseCommandAddress(address, command, list, verpMember))
         return true;

      command = CommandNone;
      list.reset();
      verpMember.Empty();

      // The list itself. A managed list's own address is one recipient of its
      // own; an unmanaged list is what it always was and is not answered here.
      std::shared_ptr<const DistributionList> own = CacheContainer::Instance()->GetDistributionList(address);
      if (!own || !own->GetManaged() || !own->GetActive())
         return false;

      command = CommandPost;
      list = own;
      return true;
   }

   bool
   ListManager::IsListOwnAddress(const String &address, const String &listAddress)
   {
      // Read without the server, for the loops that ask it of every member.
      const String lowered = Lowered(address);
      const String listLowered = Lowered(listAddress);

      if (lowered == listLowered)
         return true;

      const int at = lowered.ReverseFind(_T("@"));
      const int listAt = listLowered.ReverseFind(_T("@"));
      if (at < 1 || listAt < 1 || lowered.Mid(at + 1) != listLowered.Mid(listAt + 1))
         return false;

      String listLocal, member;
      Command command = CommandNone;
      return SplitLocalPart(lowered.Mid(0, at), listLocal, command, member) && listLocal == listLowered.Mid(0, listAt);
   }

   String
   ListManager::CommandAddress(const String &listAddress, Command command)
   {
      const int at = listAddress.ReverseFind(_T("@"));
      if (at < 1)
         return listAddress;

      const String local = listAddress.Mid(0, at);
      const String domain = listAddress.Mid(at + 1);

      switch (command)
      {
      case CommandSubscribe:
         return local + SuffixSubscribe + _T("@") + domain;
      case CommandUnsubscribe:
         return local + SuffixUnsubscribe + _T("@") + domain;
      case CommandRequest:
         return local + SuffixRequest + _T("@") + domain;
      case CommandOwner:
         return local + SuffixOwner + _T("@") + domain;
      case CommandBounces:
         return local + SuffixBounces + _T("@") + domain;
      default:
         return listAddress;
      }
   }

   String
   ListManager::VerpAddress(const String &listAddress, const String &member)
   {
      const int at = listAddress.ReverseFind(_T("@"));
      const int memberAt = member.ReverseFind(_T("@"));
      if (at < 1 || memberAt < 1)
         return CommandAddress(listAddress, CommandBounces);

      return listAddress.Mid(0, at) + SuffixBounces + _T("+") + member.Mid(0, memberAt) + _T("=") + member.Mid(memberAt + 1) +
             _T("@") + listAddress.Mid(at + 1);
   }

   String
   ListManager::OwnerOf(std::shared_ptr<const DistributionList> list)
   {
      if (!list)
         return String();

      String owner = list->GetOwner();
      owner.Trim();
      if (!owner.IsEmpty())
         return owner;

      owner = list->GetModeratorAddress();
      owner.Trim();
      if (!owner.IsEmpty())
         return owner;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(list->GetDomainID());
      if (domain && !domain->GetPostmaster().IsEmpty())
         return domain->GetPostmaster();

      const String listDomain = StringParser::ExtractDomain(list->GetAddress());
      if (listDomain.IsEmpty())
         return String();

      return _T("postmaster@") + listDomain;
   }

   // ---- settings ---------------------------------------------------------------

   String
   ListManager::BaseUrl()
   {
      String url = Configuration::Instance()->GetListBaseUrl();
      String reason;
      if (!IsUsableBaseUrl(url, reason))
         return String();

      url.Trim();
      while (url.Right(1) == _T("/"))
         url = url.Mid(0, url.GetLength() - 1);

      return url;
   }

   bool
   ListManager::IsUsableBaseUrl(const String &value, String &reason)
   {
      String url = value;
      url.Trim();

      // Empty is the setting's off: no link anywhere.
      if (url.IsEmpty())
         return true;

      // An absolute http(s) URL and nothing else, in printable ASCII: it goes
      // into a header of every copy the list sends, and into every
      // confirmation. A host name that is not ASCII is written in its
      // punycode form.
      String lowered = url;
      lowered.ToLower();

      if (lowered.Find(_T("http://")) != 0 && lowered.Find(_T("https://")) != 0)
      {
         reason = _T("The base URL is absolute and begins with http:// or https://, such as https://lists.example.com.");
         return false;
      }

      if (!IsAllAscii(url))
      {
         reason = _T("The base URL is written in printable ASCII: a host name that is not is written in its punycode form (xn--...), and a tab or a line break is not part of a URL.");
         return false;
      }

      if (url.Find(_T(" ")) >= 0 || url.Find(_T(">")) >= 0 || url.Find(_T("<")) >= 0 || url.Find(_T(",")) >= 0)
      {
         reason = _T("The base URL goes into a header of every copy a list sends, so it holds no space, comma or angle bracket.");
         return false;
      }

      return true;
   }

   int
   ListManager::HardBouncesToUnsubscribe()
   {
      return Configuration::Instance()->GetListHardBouncesToUnsubscribe();
   }

   int
   ListManager::SoftBouncesToUnsubscribe()
   {
      return Configuration::Instance()->GetListSoftBouncesToUnsubscribe();
   }

   int
   ListManager::BounceWindowDays()
   {
      return Configuration::Instance()->GetListBounceWindowDays();
   }

   // ---- tokens -----------------------------------------------------------------

   String
   ListManager::NewToken()
   {
      unsigned char bytes[TokenBytes];

      if (RAND_bytes(bytes, (int) TokenBytes) != 1)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7255, "ListManager::NewToken",
            "No entropy was available for a mailing-list token, so the request or the member it was for was left as it was.");
         return String();
      }

      static const char *digits = "0123456789abcdef";

      String token;
      for (size_t i = 0; i < TokenBytes; i++)
      {
         token += (wchar_t) digits[(bytes[i] >> 4) & 0x0f];
         token += (wchar_t) digits[bytes[i] & 0x0f];
      }

      return token;
   }

   bool
   ListManager::IsTokenText(const String &token)
   {
      if (token.GetLength() != (int) (TokenBytes * 2))
         return false;

      for (int i = 0; i < token.GetLength(); i++)
      {
         const wchar_t c = token.GetAt(i);
         if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            return false;
      }

      return true;
   }

   AnsiString
   ListManager::MemberTag(const String &token, const String &listAddress)
   {
      if (token.IsEmpty())
         return AnsiString();

      // A digest of the token rather than the token itself: the tag travels in
      // every copy and comes back in every bounce, and a bounce is read by
      // whoever handles the postmaster's mail on the way. What it proves is
      // that the report came back from a copy sent to this member of this
      // list; the token, which is the member's own page, is not in it.
      HashCreator hasher(HashCreator::SHA256);
      AnsiString digest = hasher.GenerateHashNoSalt(Utf8(token + _T(":") + Lowered(listAddress)), HashCreator::hex);
      digest.ToLower();

      return digest.Mid(0, 32);
   }

   String
   ListManager::EnsureListSecret(std::shared_ptr<const DistributionList> list)
   {
      if (!list)
         return String();

      if (IsTokenText(list->GetSecret()))
         return list->GetSecret();

      const String secret = NewToken();
      if (secret.IsEmpty())
         return String();

      // Written only while the column is empty, and read back: of two threads
      // making one, both use the first.
      PersistentDistributionList::SaveSecretIfEmpty(list->GetID(), secret);

      std::shared_ptr<DistributionList> stored = ListById(list->GetID());
      if (!stored || !IsTokenText(stored->GetSecret()))
         return String();

      return stored->GetSecret();
   }

   String
   ListManager::QueryMemberToken(const String &secret, const String &address)
   {
      if (!IsTokenText(secret))
         return String();

      const AnsiString key = Utf8(secret);
      const AnsiString data = Utf8(Lowered(address));

      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int length = 0;

      if (HMAC(EVP_sha256(), key.c_str(), key.GetLength(), (const unsigned char *) data.c_str(), (size_t) data.GetLength(), digest, &length) == nullptr ||
          length < TokenBytes)
         return String();

      static const char *digits = "0123456789abcdef";

      String token;
      for (size_t i = 0; i < TokenBytes; i++)
      {
         token += (wchar_t) digits[(digest[i] >> 4) & 0x0f];
         token += (wchar_t) digits[digest[i] & 0x0f];
      }

      return token;
   }

   // ---- lists and members ------------------------------------------------------

   std::shared_ptr<DistributionList>
   ListManager::ListById(__int64 listId)
   {
      std::shared_ptr<DistributionList> list(new DistributionList);
      if (!PersistentDistributionList::ReadObject(list, listId))
         return std::shared_ptr<DistributionList>();

      return list;
   }

   std::shared_ptr<DistributionList>
   ListManager::ListByAddress(const String &address)
   {
      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      const String primary = aliases ? aliases->ApplyAliasesOnAddress(address) : address;

      std::shared_ptr<DistributionList> list(new DistributionList);
      if (!PersistentDistributionList::ReadObject(list, primary))
         return std::shared_ptr<DistributionList>();

      return list;
   }

   std::shared_ptr<DistributionListRecipient>
   ListManager::MemberOf(__int64 listId, const String &address)
   {
      std::shared_ptr<DistributionListRecipient> member(new DistributionListRecipient);
      if (!PersistentDistributionListRecipient::ReadByAddress(member, listId, OneLine(address)))
         return std::shared_ptr<DistributionListRecipient>();

      return member;
   }

   bool
   ListManager::QueryAnswers(std::shared_ptr<const DistributionList> list, const String &address)
   {
      if (!list || list->GetMembership().IsEmpty())
         return false;

      // The query alone, whatever rows the list has: resolved as a posting
      // resolves it, into addresses lower-cased, sorted and unique. Asked when
      // somebody joins or leaves, never per copy.
      String problem;
      bool unavailable = false;
      const std::vector<String> answered = DistributionListMembership::Resolve(list->GetMembership(), list->GetDomainID(), problem, unavailable);

      return std::binary_search(answered.begin(), answered.end(), Lowered(address));
   }

   bool
   ListManager::EnsureToken(std::shared_ptr<DistributionListRecipient> member)
   {
      if (!member)
         return false;

      if (IsTokenText(member->GetToken()))
         return true;

      const String token = NewToken();
      if (token.IsEmpty())
         return false;

      // Only the token, and only where there is none: a whole-row save here
      // would put back whatever changed since the row was read.
      if (!PersistentDistributionListRecipient::SaveTokenIfEmpty(member, token) || !IsTokenText(member->GetToken()))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::EnsureToken",
            Formatter::Format("The token of list member {0} could not be saved. Its copy goes without a way to leave by link, and its bounces cannot be read.",
               member->GetAddress()));
         member->SetToken(String());
         return false;
      }

      return true;
   }

   // ---- the delivery path ------------------------------------------------------

   bool
   ListManager::AnyManagedList()
   {
      const time_t now = time(nullptr);

      {
         std::lock_guard<std::mutex> guard(managed_mutex);
         if (managed_state >= 0 && now - managed_checked < ManagedStateSeconds)
            return managed_state == 1;
      }

      SQLCommand command("select count(*) as c from hm_distributionlists where distributionlistmanaged = 1");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      // A database that did not answer is answered "some" and not remembered:
      // the lookups that follow cost a query each, where "none" would hand a
      // list's address to external delivery.
      if (!recordset || recordset->IsEOF())
         return true;

      const int state = recordset->GetLongValue("c") > 0 ? 1 : 0;

      std::lock_guard<std::mutex> guard(managed_mutex);
      managed_state = state;
      managed_checked = now;
      return state == 1;
   }

   void
   ListManager::ForgetManagedState()
   {
      std::lock_guard<std::mutex> guard(managed_mutex);
      managed_state = -1;
   }

   namespace
   {
      // Whether the header block carries a DKIM-Signature whose d= is the
      // domain: read from the raw block, each signature field unfolded and its
      // tags split, as a verifier reads them.
      bool CarriesSignatureOf(const AnsiString &header, const String &domain)
      {
         AnsiString wanted = AnsiString(domain);
         wanted.ToLower();
         if (wanted.IsEmpty())
            return false;

         AnsiString lowered = header;
         lowered.ToLower();
         lowered.Replace("\r\n\t", " ");
         lowered.Replace("\r\n ", " ");

         const std::vector<AnsiString> lines = StringParser::SplitString(lowered, "\r\n");
         for (const AnsiString &line : lines)
         {
            if (line.Find("dkim-signature:") != 0)
               continue;

            const std::vector<AnsiString> tags = StringParser::SplitString(line.Mid(15), ";");
            for (const AnsiString &tag : tags)
            {
               AnsiString value = tag;
               value.Replace(" ", "");
               value.Replace("\t", "");

               if (value.Find("d=") == 0 && value.Mid(2) == wanted)
                  return true;
            }
         }

         return false;
      }
   }

   void
   ListManager::TakeListRecipients(std::shared_ptr<Message> message, std::vector<std::shared_ptr<MessageRecipient> > &held)
   {
      if (!message)
         return;

      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return;

      // The shipped default, and the one that must cost nothing: no list is
      // managed, so no recipient can be one of the manager's.
      if (!AnyManagedList())
         return;

      struct Taken
      {
         std::shared_ptr<MessageRecipient> recipient;
         Command command;
         std::shared_ptr<const DistributionList> list;
         String verp_member;
      };

      std::vector<Taken> taken;

      {
         // owned by this call: the recipients of the message this delivery holds
         const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients = recipients->GetVector();

         for (const std::shared_ptr<MessageRecipient> &recipient : vecRecipients)
         {
            // An account is a mailbox, never the manager's. The recipients the
            // manager answers are the ones with no account behind them, and of
            // those only the ones at a domain this server hosts can be a list.
            if (!recipient || recipient->GetLocalAccountID() != 0)
               continue;

            const String domainName = StringParser::ExtractDomain(recipient->GetAddress());
            if (domainName.IsEmpty() || !CacheContainer::Instance()->GetDomain(domainName))
               continue;

            Taken entry;
            if (!ParseAddress(recipient->GetAddress(), entry.command, entry.list, entry.verp_member))
               continue;

            entry.recipient = recipient;
            taken.push_back(entry);
         }
      }

      if (taken.empty())
         return;

      for (const Taken &entry : taken)
      {
         bool answered = true;

         try
         {
            switch (entry.command)
            {
            case CommandPost:
            {
               bool started = false;

               try
               {
                  answered = Distribute_(message, entry.list, started);
               }
               catch (...)
               {
                  // Before the first member's copy was queued, the posting is
                  // tried again on the next pass. After it, a second try would
                  // send the members already queued a second copy, so what
                  // went, went, and it is reported.
                  answered = started;

                  if (started)
                  {
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::TakeListRecipients",
                        Formatter::Format("Posting {0} to {1} failed part of the way through its distribution. The members already queued are sent it; it is not tried again, which would send them a second copy.",
                           message->GetID(), entry.recipient->GetAddress()));
                  }
                  else
                  {
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::TakeListRecipients",
                        Formatter::Format("Posting {0} to {1} could not be distributed. It is kept in the queue and tried again.",
                           message->GetID(), entry.recipient->GetAddress()));
                  }
               }

               break;
            }
            case CommandBounces:
               HandleBounce_(message, entry.list, entry.verp_member);
               break;
            case CommandSubscribe:
            case CommandUnsubscribe:
            case CommandRequest:
               HandleCommand_(message, entry.list, entry.command);
               break;
            case CommandOwner:
            default:
               // name-owner@ is expanded to the owner at RCPT TO and never
               // reaches here as itself; a queue row from before a change of
               // owner is logged and let go rather than guessed at.
               LOG_APPLICATION(Formatter::Format("ListManager: message {0} to {1} was not answered: the owner's address is expanded when the message is accepted.",
                  message->GetID(), entry.recipient->GetAddress()));
               break;
            }
         }
         catch (...)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::TakeListRecipients",
               Formatter::Format("Message {0} to {1} could not be handled by the list manager and has been taken off the queue for that recipient.",
                  message->GetID(), entry.recipient->GetAddress()));
         }

         // A posting that could not be read is held: its row stays, so that the
         // message is tried again, and so does its place in the message until
         // SetAside - which keeps local delivery from taking the message's own
         // file for a single local recipient beside it.
         if (!answered)
         {
            held.push_back(entry.recipient);
            continue;
         }

         // Answered, one way or another: the recipient goes, from the database
         // and from the message, so that neither local nor external delivery
         // ever tries to deliver to a list address. Taken off AFTER it was
         // answered - a delivery thread that dies in between distributes again
         // on the next pass rather than not at all.
         if (!PersistentMessageRecipient::DeleteObject(entry.recipient))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::TakeListRecipients",
               Formatter::Format("Message {0} was answered for {1} but that recipient could not be removed from the database.",
                  message->GetID(), entry.recipient->GetAddress()));
         }

         // owned by this call: the recipients of the message this delivery holds, erased through the reference
         std::vector<std::shared_ptr<MessageRecipient> > &live = recipients->GetVector();
         for (auto it = live.begin(); it != live.end(); it++)
         {
            if (*it == entry.recipient)
            {
               live.erase(it);
               break;
            }
         }
      }
   }

   void
   ListManager::SetAside(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held)
   {
      if (!message || held.empty())
         return;

      std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
      if (!recipients)
         return;

      // owned by this call: the recipients of the message this delivery holds, erased through the reference
      std::vector<std::shared_ptr<MessageRecipient> > &live = recipients->GetVector();
      for (const std::shared_ptr<MessageRecipient> &recipient : held)
      {
         auto found = std::find(live.begin(), live.end(), recipient);
         if (found != live.end())
            live.erase(found);
      }
   }

   bool
   ListManager::HoldForRetry(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held,
                             bool alreadyRescheduled, std::vector<DeliveryFailure> &failures)
   {
      if (!message || held.empty())
         return alreadyRescheduled;

      // Rescheduled already, for an outside recipient's deferral: the held
      // rows stay in the database with the message, and are tried on its next
      // pass with the rest.
      if (alreadyRescheduled)
         return true;

      std::shared_ptr<SMTPConfiguration> smtp = Configuration::Instance()->GetSMTPConfiguration();
      const long retries = smtp ? smtp->GetNoOfRetries() : 0;
      long minutes = smtp ? smtp->GetMinutesBetweenTry() : 60;
      if (minutes < 1)
         minutes = 1;

      if ((long) message->GetNoOfRetries() < retries)
      {
         PersistentMessage::SetNextTryTime(message->GetID(), true, minutes);

         // Unlocked, so that a later delivery thread picks it up.
         PersistentMessage::UnlockObject(message);

         LOG_APPLICATION(Formatter::Format("ListManager: posting {0} could not be read for its list and is tried again in {1} minute(s), try {2} of {3}.",
            message->GetID(), (int) minutes, (int) message->GetNoOfRetries() + 1, (int) retries));
         return true;
      }

      // The retries are spent. The sender is told, as local delivery tells
      // them when it cannot save a message (HM6081): a class 4 status, 4.3.0,
      // since what failed is this server and not the list or its members - a
      // list manager downstream reading 5.x.x would unsubscribe an address
      // that never had anything wrong with it.
      for (const std::shared_ptr<MessageRecipient> &recipient : held)
      {
         const int notify = recipient->GetDSNNotify();
         const bool suppressFailureDsn = notify != MessageRecipient::DSNNotifyDefault &&
                                         (notify & MessageRecipient::DSNNotifyFailure) == 0;

         if (!suppressFailureDsn)
         {
            failures.push_back(DeliveryFailure(recipient->GetAddress(), _T("4.3.0"),
               Formatter::Format("{0}\r\n   Error Type: SMTP\r\n   Error Description: Delivery failed\r\n   Additional information: The message could not be read to be sent to the members of this mailing list, and was tried again until the retries ran out. The server administrator should check the hMailServer error log.\r\n\r\n",
                  recipient->GetAddress())));
         }

         if (!PersistentMessageRecipient::DeleteObject(recipient))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::HoldForRetry",
               Formatter::Format("Posting {0} to {1} has run out of retries, and that recipient could not be removed from the database.",
                  message->GetID(), recipient->GetAddress()));
         }
      }

      LOG_APPLICATION(Formatter::Format("ListManager: posting {0} could not be read for its list after {1} tries; its sender is told.",
         message->GetID(), (int) message->GetNoOfRetries() + 1));

      return false;
   }

   bool
   ListManager::Distribute_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, bool &started)
   {
      started = false;

      const String listAddress = list->GetAddress();

      // A report of a failed delivery sent to the list's own address is not a
      // posting. It goes nowhere: distributing it would send every member a
      // bounce about somebody else's mail.
      if (message->GetFromAddress().IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("ListManager: message {0} to {1} has a null sender - a delivery report, not a posting - and was not distributed.",
            message->GetID(), listAddress));
         return true;
      }

      // Mail this server judged to be spam and still delivered - marked rather
      // than refused - is not handed on to every member. Said in the
      // application log, where an owner asking after a posting that never
      // arrived finds it.
      if (message->GetFlagSpam())
      {
         LOG_APPLICATION(Formatter::Format("ListManager: message {0} from {1} to {2} was marked as spam and was not distributed.",
            message->GetID(), message->GetFromAddress(), listAddress));
         return true;
      }

      const String fileName = PersistentMessage::GetFileName(message);
      const AnsiString header = PersistentMessage::LoadHeader(fileName, false);

      if (header.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::Distribute_",
            Formatter::Format("The posting {0} to list {1} could not be read. It is kept in the queue and tried again.", message->GetID(), listAddress));
         return false;
      }

      // A copy of this list's own coming back - a member's server forwarding
      // it into the list again, or two lists that are each other's member -
      // carries the X-Loop every copy is given. It stops here.
      if (CarriesLoopFor_(header, listAddress))
      {
         LOG_APPLICATION(Formatter::Format("ListManager: message {0} to {1} already went through that list (X-Loop), and was not distributed again.",
            message->GetID(), listAddress));
         return true;
      }

      std::shared_ptr<MessageData> data(new MessageData());
      if (!data->LoadFromMessage(fileName, message) || !data->GetMimeMessage() ||
          (!data->GetMimeMessage()->FieldExists("Received") && !data->GetMimeMessage()->FieldExists("From")))
      {
         // LoadFromMessage answering true is not proof the file was read (see
         // DistributionListSender::LoadedMessageIsGenuine_): what it makes of
         // a file it could not open is a new message with a charset and a
         // MIME-Version and nothing else. The Received field this server writes
         // on every SMTP message, or the From every posting has - the webmail's
         // send writes no Received - is the evidence the file was read.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::Distribute_",
            Formatter::Format("The posting {0} to list {1} could not be loaded (an antivirus or a backup holding the file is the usual cause). It is kept in the queue and tried again.",
               message->GetID(), listAddress));
         return false;
      }

      std::shared_ptr<MimeBody> mime = data->GetMimeMessage();

      // Whether the list changes what an author's DKIM signature covers: the
      // subject, a Reply-To, the body, or list fields the posting brought.
      bool altered = false;

      // Whatever list headers the posting brought with it describe another
      // list, or are a member's attempt at ours: each copy gets this list's own.
      const char *const replaced[] =
      {
         "List-Id", "List-Post", "List-Help", "List-Subscribe", "List-Unsubscribe", "List-Unsubscribe-Post",
         "List-Owner", "List-Archive", "Precedence", "X-hMailServer-List-Member", "Return-Path"
      };

      for (const char *field : replaced)
      {
         if (mime->FieldExists(field) && AnsiString(field).CompareNoCase("Return-Path") != 0)
            altered = true;

         // Every occurrence, and never more tries than there were fields.
         for (int guard = 0; guard < 64 && mime->FieldExists(field); guard++)
            mime->DeleteField(AnsiString(field));
      }

      // The subject prefix, once: a reply's subject already carries it, and
      // "Re: [list] [list] ..." is what every list that got this wrong looks
      // like. Compared without case, anywhere in the subject.
      const String prefix = OneLine(list->GetSubjectPrefix());
      if (!prefix.IsEmpty())
      {
         const String subject = data->GetSubject();
         String loweredSubject = subject;
         loweredSubject.ToLower();
         String loweredPrefix = prefix;
         loweredPrefix.ToLower();

         if (loweredSubject.Find(loweredPrefix) < 0)
         {
            data->SetSubject(subject.IsEmpty() ? prefix : prefix + _T(" ") + subject);
            altered = true;
         }
      }

      // Replies to the list, when the list says so; otherwise the posting's own
      // Reply-To, or none, as the author wrote it.
      if (list->GetReplyToList())
      {
         data->SetFieldValue(_T("Reply-To"), listAddress);
         altered = true;
      }

      // The footer, as the disclaimer adder appends a rule's text: to the plain
      // and the HTML part, and to neither of a signed or encrypted message,
      // which appending would break.
      const String footer = list->GetFooter();
      if (!footer.IsEmpty())
      {
         std::shared_ptr<MessageData> footed = data;
         if (DisclaimerAdder::AppendText(message, footer, footed, listAddress))
            altered = true;
         else
         {
            LOG_DEBUG(Formatter::Format("ListManager: the footer of {0} was not added to posting {1} (signed or encrypted, no text part, or already carried).",
               listAddress, message->GetID()));
         }
      }

      // The author's DKIM signatures. A copy the list changed carries none of
      // them - each would fail at every receiver, and a failing signature is
      // scored worse than none - and neither does a copy carrying a signature
      // of the list's own domain, made before the list's fields existed, since
      // the copy is signed again as that domain once they do (QueueCopy). A
      // copy the list did not change keeps an outside author's signature,
      // which still verifies and still aligns with the author's From.
      if (altered || CarriesSignatureOf(header, StringParser::ExtractDomain(listAddress)))
      {
         for (int guard = 0; guard < 64 && mime->FieldExists("DKIM-Signature"); guard++)
            mime->DeleteField(AnsiString("DKIM-Signature"));
      }

      // The list's copy, written once beside the posting and read by every
      // member copy, the archive and the digest.
      const String copyFile = fileName + _T(".list");

      if (!data->Write(copyFile))
      {
         FileUtilities::DeleteFile(copyFile);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7250, "ListManager::Distribute_",
            Formatter::Format("The list copy of posting {0} to {1} could not be written. It is kept in the queue and tried again.", message->GetID(), listAddress));
         return false;
      }

      // The archive and the digest read the list's copy, not the posting.
      if (list->GetArchive() != DistributionList::ArchiveNone || list->GetDigest())
         ListArchive::Store(list, copyFile, data);

      // From here on a copy may have been queued, and a failure is a part of
      // the posting sent rather than a posting to try again.
      started = true;

      int each = 0, digest = 0, paused = 0, failed = 0;

      // The addresses whose rows stand between the membership query and the
      // list's mail: the members, who are sent it as rows, and the addresses
      // that left. A request still waiting is neither - an address the query
      // answers is sent the list's mail whatever request somebody made in its
      // name.
      std::set<String> rowAddresses;

      std::shared_ptr<DistributionListRecipients> rows = list->GetMembers();
      const std::vector<std::shared_ptr<DistributionListRecipient> > snapshot = rows ? rows->GetSnapshot() : std::vector<std::shared_ptr<DistributionListRecipient> >();

      for (const std::shared_ptr<DistributionListRecipient> &row : snapshot)
      {
         if (!row)
            continue;

         const String address = OneLine(row->GetAddress());
         if (address.IsEmpty())
            continue;

         if (row->GetTakesQueryPlace())
            rowAddresses.insert(Lowered(address));

         // A request is not a member, whether it waits for its own confirmation
         // or for the owner; nor is an address that has left.
         if (!row->GetIsMember())
            continue;

         // A member on the digest of a list that no longer sends one is sent
         // each message: switching the digest off must not silence the people
         // who chose it.
         if (row->GetDelivery() == DistributionListRecipient::DeliveryDigest && list->GetDigest())
         {
            digest++;
            continue;
         }

         if (row->GetDelivery() == DistributionListRecipient::DeliveryNone)
         {
            paused++;
            continue;
         }

         // A member that is this list, or one of its own addresses, would be the
         // list writing to itself.
         if (IsListOwnAddress(address, listAddress))
         {
            LOG_APPLICATION(Formatter::Format("ListManager: {0} is a member of its own list {1}; no copy was sent to it.", address, listAddress));
            continue;
         }

         EnsureToken(row);

         if (QueueCopy(list, address, row->GetToken(), copyFile, false))
            each++;
         else
            failed++;
      }

      // The accounts the list's membership query answers, which have no row
      // and so no delivery choice of their own: each message, with a way out
      // by link made from the list's secret - leaving writes the row that
      // keeps them out - and the list's mailto addresses. A query that could
      // not be answered is reported, and the rows above have still been sent.
      if (!list->GetMembership().IsEmpty())
      {
         bool unavailable = false;
         const std::vector<String> members = DistributionListMembership::Members(list, unavailable);

         if (unavailable)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7258, "ListManager::Distribute_",
               Formatter::Format("The membership query of list {0} could not be answered, so posting {1} went to the list's own members alone.",
                  listAddress, message->GetID()));
         }

         // Made once for the posting, not once per member.
         const String secret = BaseUrl().IsEmpty() ? String() : EnsureListSecret(list);

         for (const String &member : members)
         {
            if (rowAddresses.find(Lowered(member)) != rowAddresses.end())
               continue;

            if (IsListOwnAddress(member, listAddress))
               continue;

            if (QueueCopy(list, member, QueryMemberToken(secret, member), copyFile, true))
               each++;
            else
               failed++;
         }
      }

      FileUtilities::DeleteFile(copyFile);

      if (each > 0)
         Application::Instance()->SubmitPendingEmail();

      // Formatter takes five arguments at most, so the counts are two pieces.
      LOG_APPLICATION(Formatter::Format("ListManager: posting {0} to {1} was sent to {2} member(s), and {3} could not be queued;",
         message->GetID(), listAddress, each, failed) +
         Formatter::Format(" {0} take the digest and {1} are paused.", digest, paused));

      return true;
   }

   String
   ListManager::MemberLink_(std::shared_ptr<const DistributionList> list, const String &member, const String &token, bool queryMember)
   {
      const String baseUrl = BaseUrl();
      if (baseUrl.IsEmpty() || !IsTokenText(token) || !list)
         return String();

      String link = baseUrl + _T("/lists/unsubscribe?t=") + token;

      // An account the membership query answers has no row for the token to
      // name: the link names the address and the list, and the token is the
      // proof that this server wrote it.
      if (queryMember)
         link += _T("&m=") + String(UrlEncode(member)) + _T("&l=") + StringParser::IntToString(list->GetID());

      return link;
   }

   AnsiString
   ListManager::CopyHeaders(std::shared_ptr<const DistributionList> list, const String &member, const String &token, bool queryMember)
   {
      AnsiString lines;

      const String listAddress = list->GetAddress();

      // Every value below becomes raw bytes in the header block, and this path
      // has no RFC 6532 handling: a list whose address is not ASCII gets none of
      // the RFC 2369 set, which is less wrong than a mangled one.
      if (!IsAllAscii(listAddress))
         return lines;

      const AnsiString list7 = AnsiString(listAddress);
      const String baseUrl = BaseUrl();
      const AnsiString base7 = AnsiString(baseUrl);

      const String listId = list->GetRfc2919ListId();
      if (!listId.IsEmpty())
         lines += "List-Id: " + AnsiString(listId) + "\r\n";

      if (list->IsAnnouncementOnly())
         lines += "List-Post: NO\r\n";
      else
         lines += "List-Post: <mailto:" + list7 + ">\r\n";

      lines += "List-Help: <mailto:" + AnsiString(CommandAddress(listAddress, CommandRequest)) + "?subject=help>\r\n";

      AnsiString subscribe = "<mailto:" + AnsiString(CommandAddress(listAddress, CommandSubscribe)) + "?subject=subscribe>";
      if (!baseUrl.IsEmpty() && list->GetSubscribePolicy() != DistributionList::SubscribeClosed)
         subscribe += ", <" + base7 + "/lists/" + list7 + "/subscribe>";
      lines += "List-Subscribe: " + subscribe + "\r\n";

      // RFC 2369's mailto, and the member's own https link when there is a
      // page to take it: a member's token names the member, and a query
      // member's names its address under the list's secret, so the POST needs
      // no session and no confirmation. Without a base URL, or for a member
      // with neither, the mailto alone - a link pointing at nothing is worse
      // than none.
      AnsiString unsubscribe = "<mailto:" + AnsiString(CommandAddress(listAddress, CommandUnsubscribe)) + "?subject=unsubscribe>";
      const String link = IsAllAscii(member) ? MemberLink_(list, member, token, queryMember) : String();
      if (!link.IsEmpty())
         unsubscribe += ", <" + AnsiString(link) + ">";
      lines += "List-Unsubscribe: " + unsubscribe + "\r\n";

      // RFC 8058 3.1: the URI a one-click POST goes to is HTTPS. A link over
      // plain http is still there for a person to follow; the one-click a mail
      // provider acts on is offered only over https.
      String loweredLink = link;
      loweredLink.ToLower();
      if (!link.IsEmpty() && loweredLink.Find(_T("https://")) == 0)
         lines += "List-Unsubscribe-Post: List-Unsubscribe=One-Click\r\n";

      lines += "List-Owner: <mailto:" + AnsiString(CommandAddress(listAddress, CommandOwner)) + ">\r\n";

      if (!baseUrl.IsEmpty() && list->GetArchive() != DistributionList::ArchiveNone)
         lines += "List-Archive: <" + base7 + "/lists/" + list7 + "/archive>\r\n";

      lines += "Precedence: list\r\n";

      // Kept on the copy whatever other X-Loop fields it carries, so that a
      // posting that comes round through another list is still recognised.
      lines += "X-Loop: " + list7 + "\r\n";

      // The tag bounces are believed by: a member with a row, whose bounces are
      // counted. An account the query answers has no row to count them on.
      const AnsiString tag = queryMember ? AnsiString() : MemberTag(token, listAddress);
      if (!tag.IsEmpty())
         lines += "X-hMailServer-List-Member: " + tag + "\r\n";

      return lines;
   }

   bool
   ListManager::QueueCopy(std::shared_ptr<const DistributionList> list, const String &member, const String &token, const String &contentFile, bool queryMember)
   {
      std::shared_ptr<Message> copy(new Message());
      copy->SetState(Message::Delivering);

      const String copyName = PersistentMessage::GetFileName(copy);
      const String directory = FileUtilities::GetFilePath(copyName);
      if (!FileUtilities::Exists(directory))
         FileUtilities::CreateDirectory(directory);

      const AnsiString headers = CopyHeaders(list, member, token, queryMember);

      bool written = false;

      try
      {
         File out;
         File in;

         written = out.Open(copyName, File::OTCreate) &&
                   out.Write(headers) &&
                   in.Open(contentFile, File::OTReadOnly) &&
                   out.Write(in) &&
                   out.FlushToDisk();

         out.Close();
         in.Close();
      }
      catch (...)
      {
         written = false;
      }

      if (!written)
      {
         FileUtilities::DeleteFile(copyName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7251, "ListManager::QueueCopy",
            Formatter::Format("The copy of a posting to {0} for {1} could not be written, so that member has not been sent it.",
               list->GetAddress(), member));
         return false;
      }

      // Signed as the list's domain when that domain signs, over the list's
      // fields now that they are there: the author's signature was taken off
      // a copy the list changed, and RFC 8058 section 4 asks for a valid
      // signature over List-Unsubscribe and List-Unsubscribe-Post before a
      // receiver offers the one-click. The copy's own delivery pass then finds
      // a signature of that domain and does not sign again.
      DKIMSigner signer;
      signer.SignAsDomain(copy, AnsiString(StringParser::ExtractDomain(list->GetAddress())));

      // The list's bounce address, or with VERP the member's own form of it,
      // is the envelope sender of every copy: a failure comes back to the
      // manager, and never to whoever wrote the posting.
      copy->SetFromAddress(list->GetVerp() ? VerpAddress(list->GetAddress(), member) : CommandAddress(list->GetAddress(), CommandBounces));
      copy->SetSize(FileUtilities::FileSize(copyName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(member, copy->GetRecipients(), recipientOk);

      if (copy->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(copyName);
         LOG_APPLICATION(Formatter::Format("ListManager: {0}, a member of {1}, is not an address mail can be delivered to, so no copy was sent to it.",
            member, list->GetAddress()));
         return false;
      }

      if (!PersistentMessage::SaveObject(copy))
      {
         FileUtilities::DeleteFile(copyName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7251, "ListManager::QueueCopy",
            Formatter::Format("The copy of a posting to {0} for {1} could not be queued, so that member has not been sent it.",
               list->GetAddress(), member));
         return false;
      }

      return true;
   }

   // ---- commands -----------------------------------------------------------------

   bool
   ListManager::IsAutomatic_(const AnsiString &header)
   {
      AnsiString autoSubmitted = MessageOrigin::FirstFieldValue(header, "Auto-Submitted");
      autoSubmitted.Trim();
      if (!autoSubmitted.IsEmpty() && autoSubmitted.CompareNoCase("no") != 0)
         return true;

      AnsiString precedence = MessageOrigin::FirstFieldValue(header, "Precedence");
      precedence.Trim();
      precedence.ToLower();
      if (precedence == "bulk" || precedence == "junk" || precedence == "list")
         return true;

      // Mail from a list is not a person asking for anything.
      if (!MessageOrigin::FirstFieldValue(header, "List-Id").IsEmpty())
         return true;

      return false;
   }

   bool
   ListManager::CarriesLoopFor_(const AnsiString &header, const String &listAddress)
   {
      const AnsiString wanted = AnsiString(Lowered(listAddress));
      if (wanted.IsEmpty())
         return false;

      // Every X-Loop field, not only the first: a posting that has been round
      // two lists carries one for each. The header is unfolded first, and read
      // a line at a time.
      AnsiString lowered = header;
      lowered.ToLower();
      lowered.Replace("\r\n\t", " ");
      lowered.Replace("\r\n ", " ");

      const std::vector<AnsiString> lines = StringParser::SplitString(lowered, "\r\n");

      for (const AnsiString &line : lines)
      {
         if (line.Find("x-loop:") != 0)
            continue;

         AnsiString value = line.Mid(7);
         value.Trim();
         value.Replace("<", "");
         value.Replace(">", "");

         if (value == wanted)
            return true;
      }

      return false;
   }

   String
   ListManager::FindToken_(const String &text)
   {
      // The first run of exactly 64 lower-case hex characters. A reply quotes
      // the subject and usually the body; either carries the token.
      int run = 0;
      for (int i = 0; i <= text.GetLength(); i++)
      {
         const wchar_t c = i < text.GetLength() ? text.GetAt(i) : L' ';
         const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');

         if (hex)
         {
            run++;
            continue;
         }

         if (run == (int) (TokenBytes * 2))
            return text.Mid(i - run, run);

         run = 0;
      }

      return String();
   }

   void
   ListManager::HandleCommand_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, Command command)
   {
      const String listAddress = list->GetAddress();
      const String sender = OneLine(message->GetFromAddress());

      // A null sender is a delivery report, and a report is nobody asking.
      if (sender.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("ListManager: message {0} to a command address of {1} has a null sender and was not answered.",
            message->GetID(), listAddress));
         return;
      }

      const String fileName = PersistentMessage::GetFileName(message);
      const AnsiString header = PersistentMessage::LoadHeader(fileName, false);

      // An automatic reply, a vacation message or another list's mail is never
      // answered: two servers answering each other's answers is a mail loop.
      if (IsAutomatic_(header))
      {
         LOG_APPLICATION(Formatter::Format("ListManager: message {0} from {1} to a command address of {2} is automatic (Auto-Submitted, Precedence or List-Id) and was not answered.",
            message->GetID(), sender, listAddress));
         return;
      }

      // The subject and the start of the text, where a reply carries the token.
      String text;
      {
         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);
         text = mimeHeader.GetUnicodeFieldValue("Subject");

         std::shared_ptr<MessageData> data(new MessageData());
         if (data->LoadFromMessage(fileName, message))
         {
            String body = data->GetBody();
            if (body.GetLength() > MostCommandTextCharacters)
               body = body.Mid(0, MostCommandTextCharacters);
            text += _T(" ") + body;
         }
      }

      String loweredText = text;
      loweredText.ToLower();
      const String token = FindToken_(loweredText);

      // Whose request this is, for the ceilings on what the list answers: the
      // server that handed the message to this one.
      const String requester = RequesterOf_(message);

      if (command == CommandSubscribe || command == CommandRequest)
      {
         if (!token.IsEmpty())
         {
            ConfirmFromMail_(list, token, sender);
            return;
         }

         if (command == CommandRequest)
         {
            std::vector<std::pair<String, String> > macros;
            macros.push_back(std::make_pair(String(_T("%MACRO_SUBSCRIBE%")), CommandAddress(listAddress, CommandSubscribe)));
            macros.push_back(std::make_pair(String(_T("%MACRO_UNSUBSCRIBE%")), CommandAddress(listAddress, CommandUnsubscribe)));
            macros.push_back(std::make_pair(String(_T("%MACRO_REQUEST%")), CommandAddress(listAddress, CommandRequest)));
            macros.push_back(std::make_pair(String(_T("%MACRO_OWNERADDRESS%")), CommandAddress(listAddress, CommandOwner)));

            if (!Throttled_(list->GetID(), sender + _T("#help")) && NoticeAllowed_(list->GetID(), listAddress, requester))
               SendNotice_(list, sender, String(), _T("LIST_HELP_SUBJECT"), _T("LIST_HELP"), String(), macros);
            return;
         }

         // The address asking to join is the envelope sender, and the
         // confirmation goes to it: whoever asks in somebody else's name makes
         // the list write to that somebody once an hour at most, and nothing
         // more happens unless they confirm.
         const RequestOutcome outcome = RequestSubscription(list, sender, requester);
         LOG_APPLICATION(Formatter::Format("ListManager: {0} asked by mail to join {1}: outcome {2}.", sender, listAddress, (int) outcome));
         return;
      }

      if (command == CommandUnsubscribe)
      {
         // A reply carrying the token the request to leave was answered with:
         // the address leaves. Only its owner was sent that token.
         if (!token.IsEmpty())
         {
            std::shared_ptr<DistributionListRecipient> member(new DistributionListRecipient);
            if (PersistentDistributionListRecipient::ReadByToken(member, token) && member->GetListID() == list->GetID() && member->GetIsMember())
            {
               const String address = member->GetAddress();
               if (LeaveRow_(list, member))
               {
                  LOG_APPLICATION(Formatter::Format("ListManager: {0} left {1} by mail.", address, listAddress));
                  TellLeft_(list, address);
               }
               return;
            }

            // An account the membership query answers, replying from its own
            // address with the token made from the list's secret.
            if (QueryAnswers(list, sender) && token == QueryMemberToken(EnsureListSecret(list), sender))
            {
               if (OptOut_(list, sender))
               {
                  LOG_APPLICATION(Formatter::Format("ListManager: {0}, an account the membership query of {1} answers, left it by mail.", sender, listAddress));
                  TellLeft_(list, sender);
               }
               return;
            }

            LOG_APPLICATION(Formatter::Format("ListManager: a message from {0} to leave {1} carried a token that names nobody on that list; nothing was done.",
               sender, listAddress));
            return;
         }

         // A request to leave, which is answered with a link and a token and
         // does nothing more: the envelope sender is free text, and anybody who
         // knew a member's address could otherwise take them off the list.
         RequestLeave_(list, sender, requester);
         return;
      }
   }

   void
   ListManager::RequestLeave_(std::shared_ptr<const DistributionList> list, const String &sender, const String &requester)
   {
      const String listAddress = list->GetAddress();

      bool queryMember = false;
      String token;

      std::shared_ptr<DistributionListRecipient> member = MemberOf(list->GetID(), sender);
      if (member && member->GetIsMember())
      {
         EnsureToken(member);
         token = member->GetToken();
      }
      else if (!member && QueryAnswers(list, sender))
      {
         queryMember = true;
         token = QueryMemberToken(EnsureListSecret(list), sender);
      }
      else
      {
         // Not answered: an answer would tell a stranger who is not on the list.
         LOG_APPLICATION(Formatter::Format("ListManager: {0} asked by mail to leave {1}, and is not a member; nothing was done.", sender, listAddress));
         return;
      }

      if (!IsTokenText(token))
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0} asked by mail to leave {1}, and no token could be made for the answer; nothing was sent.", sender, listAddress));
         return;
      }

      if (Throttled_(list->GetID(), sender + _T("#leave")) || !NoticeAllowed_(list->GetID(), listAddress, requester))
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0} asked by mail to leave {1} again within the hour, or past the list's ceiling; nothing was sent.", sender, listAddress));
         return;
      }

      const String link = MemberLink_(list, sender, token, queryMember);

      std::vector<std::pair<String, String> > macros;
      macros.push_back(std::make_pair(String(_T("%MACRO_ADDRESS%")), sender));
      macros.push_back(std::make_pair(String(_T("%MACRO_LINK%")), link.IsEmpty()
         ? String(_T("(this server offers no page for it - reply instead)"))
         : link));

      if (SendNotice_(list, sender, CommandAddress(listAddress, CommandUnsubscribe),
                      _T("LIST_LEAVE_SUBJECT"), _T("LIST_LEAVE"), _T(" [") + token + _T("]"), macros))
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0} asked by mail to leave {1}, and was sent a link to confirm it.", sender, listAddress));
      }
   }

   bool
   ListManager::LeaveRow_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member)
   {
      // An address the membership query also answers would be sent the list's
      // mail again from the next posting if its row simply went: the row stays,
      // saying it has left.
      if (QueryAnswers(list, member->GetAddress()))
      {
         member->SetConfirmed(DistributionListRecipient::OptedOut);
         member->SetDelivery(DistributionListRecipient::DeliveryNone);
         member->SetSubscribed(Now_());

         if (!PersistentDistributionListRecipient::SaveState(member))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::LeaveRow_",
               Formatter::Format("{0} left list {1}, and the row that keeps the membership query's answer out could not be saved.", member->GetAddress(), list->GetAddress()));
            return false;
         }

         return true;
      }

      if (!PersistentDistributionListRecipient::DeleteObject(member))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::LeaveRow_",
            Formatter::Format("{0} left list {1}, and its row could not be deleted.", member->GetAddress(), list->GetAddress()));
         return false;
      }

      return true;
   }

   bool
   ListManager::OptOut_(std::shared_ptr<const DistributionList> list, const String &address)
   {
      // An account the membership query answers, leaving: a row that says so,
      // which Distribute_ reads as "send this address nothing".
      std::shared_ptr<DistributionListRecipient> member = MemberOf(list->GetID(), address);

      if (member)
      {
         if (member->GetConfirmed() == DistributionListRecipient::OptedOut)
            return true;

         return LeaveRow_(list, member);
      }

      member = std::shared_ptr<DistributionListRecipient>(new DistributionListRecipient);
      member->SetListID(list->GetID());
      member->SetAddress(Lowered(OneLine(address)));
      member->SetConfirmed(DistributionListRecipient::OptedOut);
      member->SetDelivery(DistributionListRecipient::DeliveryNone);
      member->SetSubscribed(Now_());

      String reason;
      if (!PersistentDistributionListRecipient::SaveObject(member, reason, PersistenceModeNormal))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::OptOut_",
            Formatter::Format("{0} left list {1}, and the row that keeps the membership query's answer out could not be saved ({2}).", address, list->GetAddress(), reason));
         return false;
      }

      return true;
   }

   void
   ListManager::TellLeft_(std::shared_ptr<const DistributionList> list, const String &address)
   {
      std::vector<std::pair<String, String> > macros;
      macros.push_back(std::make_pair(String(_T("%MACRO_ADDRESS%")), address));
      SendNotice_(list, address, String(), _T("LIST_UNSUBSCRIBED_SUBJECT"), _T("LIST_UNSUBSCRIBED"), String(), macros);
   }

   void
   ListManager::ConfirmFromMail_(std::shared_ptr<const DistributionList> list, const String &token, const String &sender)
   {
      std::shared_ptr<DistributionListRecipient> member(new DistributionListRecipient);

      // A token of another list, or of nobody, is the same answer: nothing.
      if (!PersistentDistributionListRecipient::ReadByToken(member, token) || member->GetListID() != list->GetID())
      {
         LOG_APPLICATION(Formatter::Format("ListManager: a reply from {0} to {1} carried a token that names no request of that list; nothing was done.",
            sender, list->GetAddress()));
         return;
      }

      if (member->GetConfirmed() == DistributionListRecipient::Pending)
      {
         if (Now_() - member->GetSubscribed() > (__int64) RequestLifetimeDays * 86400)
         {
            PersistentDistributionListRecipient::DeleteObject(member);
            LOG_APPLICATION(Formatter::Format("ListManager: the request of {0} to join {1} had lapsed when it was confirmed; it was removed.",
               member->GetAddress(), list->GetAddress()));
            return;
         }

         ConfirmRow_(list, member);
         return;
      }

      if (member->GetConfirmed() == DistributionListRecipient::AwaitingOwner)
      {
         // The owner approves on the page the approval notice links to, signed
         // in to the webmail as the owner's own account, or on the Control
         // Deck. A reply proves less: the envelope sender is free text, and
         // this server's Received field says a session signed in, not as whom.
         LOG_APPLICATION(Formatter::Format("ListManager: a reply from {0} to approve {1} for {2} approves nothing: the owner approves on the page the notice links to, signed in to the webmail, or on the Control Deck.",
            sender, member->GetAddress(), list->GetAddress()));
         return;
      }

      LOG_APPLICATION(Formatter::Format("ListManager: a reply from {0} to {1} carried the token of somebody already a member, or who has left; nothing was done.",
         sender, list->GetAddress()));
   }

   bool
   ListManager::ConfirmRow_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member)
   {
      // A list closed since the request was made takes nobody by it.
      if (list->GetSubscribePolicy() == DistributionList::SubscribeClosed)
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0} confirmed a request to join {1}, which has been closed since; nothing was done.",
            member->GetAddress(), list->GetAddress()));
         return false;
      }

      // The token is spent: the one that confirmed is not the one the member's
      // page, the owner's approval or the one-click will carry.
      const String token = NewToken();
      if (token.IsEmpty())
         return false;

      const bool moderated = list->GetSubscribePolicy() == DistributionList::SubscribeModerated;

      member->SetToken(token);
      member->SetConfirmed(moderated ? DistributionListRecipient::AwaitingOwner : DistributionListRecipient::Confirmed);
      if (!moderated)
         member->SetSubscribed(Now_());

      if (!PersistentDistributionListRecipient::SaveState(member))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::ConfirmRow_",
            Formatter::Format("The confirmation of {0} for list {1} could not be saved.", member->GetAddress(), list->GetAddress()));
         return false;
      }

      if (!moderated)
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0} confirmed and joined {1}.", member->GetAddress(), list->GetAddress()));
         return true;
      }

      // Moderated: the owner is asked, with the new token in a link to the
      // approval page, where the owner approves signed in to the webmail.
      const String baseUrl = BaseUrl();

      std::vector<std::pair<String, String> > macros;
      macros.push_back(std::make_pair(String(_T("%MACRO_ADDRESS%")), member->GetAddress()));
      macros.push_back(std::make_pair(String(_T("%MACRO_LINK%")), baseUrl.IsEmpty()
         ? String(_T("(this server offers no page for it - approve them in the Control Deck)"))
         : baseUrl + _T("/lists/approve?t=") + token));

      const String owner = OwnerOf(list);
      if (!owner.IsEmpty())
      {
         SendNotice_(list, owner, String(), _T("LIST_APPROVE_SUBJECT"), _T("LIST_APPROVE"), String(), macros);
      }

      LOG_APPLICATION(Formatter::Format("ListManager: {0} confirmed a request to join {1}; the owner {2} was asked to approve it.",
         member->GetAddress(), list->GetAddress(), owner));
      return true;
   }

   ListManager::RequestOutcome
   ListManager::RequestSubscription(std::shared_ptr<const DistributionList> list, const String &requested, const String &requester)
   {
      if (!list || !list->GetManaged() || !list->GetActive())
         return RequestFailed;

      const String address = Lowered(OneLine(requested));
      if (address.IsEmpty() || address.GetLength() > 255 || !StringParser::IsValidEmailAddress(address))
         return RequestFailed;

      // Nobody joins a list as the list, or as one of its own addresses.
      if (IsListOwnAddress(address, list->GetAddress()))
         return RequestFailed;

      if (list->GetSubscribePolicy() == DistributionList::SubscribeClosed)
      {
         if (Throttled_(list->GetID(), address + _T("#refused")) || !NoticeAllowed_(list->GetID(), list->GetAddress(), requester))
            return RequestThrottled;

         std::vector<std::pair<String, String> > macros;
         SendNotice_(list, address, String(), _T("LIST_REFUSED_SUBJECT"), _T("LIST_REFUSED"), String(), macros);
         return RequestRefused;
      }

      std::shared_ptr<DistributionListRecipient> member = MemberOf(list->GetID(), address);

      // A member, a request already with the owner, or an address that has
      // left and is kept out by its row: there is nothing to ask. (One that
      // left comes back through the administrator, who takes its row away.)
      if (member && member->GetConfirmed() != DistributionListRecipient::Pending)
         return RequestAlreadyMember;

      // An account the membership query answers is a member already. Giving it
      // a request of its own would make its row stand where the query's answer
      // is, and a stranger could keep any such member from the list's mail by
      // asking in its name.
      if (!member && QueryAnswers(list, address))
         return RequestAlreadyMember;

      // One confirmation an hour per address per list - a row still pending
      // from under an hour ago is the proof - and the list's ceilings.
      if (member && Now_() - member->GetSubscribed() < ThrottleSeconds)
         return RequestThrottled;

      if (Throttled_(list->GetID(), address) || !NoticeAllowed_(list->GetID(), list->GetAddress(), requester))
         return RequestThrottled;

      const String token = NewToken();
      if (token.IsEmpty())
         return RequestFailed;

      const bool existing = member ? true : false;
      if (!member)
      {
         member = std::shared_ptr<DistributionListRecipient>(new DistributionListRecipient);
         member->SetListID(list->GetID());
         member->SetAddress(address);
      }

      member->SetConfirmed(DistributionListRecipient::Pending);
      member->SetDelivery(DistributionListRecipient::DeliveryEach);
      member->SetSubscribed(Now_());
      member->SetToken(token);

      String reason;
      const bool saved = existing
         ? PersistentDistributionListRecipient::SaveState(member)
         : PersistentDistributionListRecipient::SaveObject(member, reason, PersistenceModeNormal);

      if (!saved)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::RequestSubscription",
            Formatter::Format("The request of {0} to join list {1} could not be saved ({2}).", address, list->GetAddress(), reason));
         return RequestFailed;
      }

      const String baseUrl = BaseUrl();

      std::vector<std::pair<String, String> > macros;
      macros.push_back(std::make_pair(String(_T("%MACRO_ADDRESS%")), address));
      macros.push_back(std::make_pair(String(_T("%MACRO_LINK%")), baseUrl.IsEmpty()
         ? String(_T("(this server offers no page for it - reply instead)"))
         : baseUrl + _T("/lists/confirm?t=") + token));

      if (!SendNotice_(list, address, CommandAddress(list->GetAddress(), CommandSubscribe),
                       _T("LIST_CONFIRM_SUBJECT"), _T("LIST_CONFIRM"), _T(" [") + token + _T("]"), macros))
         return RequestFailed;

      return RequestSent;
   }

   bool
   ListManager::Throttled_(__int64 listId, const String &address)
   {
      const String key = StringParser::IntToString(listId) + _T(":") + Lowered(address);
      const time_t now = time(nullptr);

      std::lock_guard<std::mutex> guard(throttle_mutex);

      for (auto it = throttled_until.begin(); it != throttled_until.end();)
         it = it->second <= now ? throttled_until.erase(it) : std::next(it);

      auto found = throttled_until.find(key);
      if (found != throttled_until.end() && found->second > now)
         return true;

      throttled_until[key] = now + ThrottleSeconds;

      // Past its ceiling the map loses its oldest entries, never all of them:
      // clearing it would give every address its hour back at once.
      EvictOldest(throttled_until, PageBudgetCeiling, [](time_t until) { return until; });
      return false;
   }

   bool
   ListManager::NoticeAllowed_(__int64 listId, const String &listAddress, const String &requester)
   {
      const String listKey = StringParser::IntToString(listId) + _T("|");
      const String requesterKey = listKey + (requester.IsEmpty() ? String(_T("unknown")) : Lowered(requester));
      const String allKey = listKey + _T("*");
      const time_t now = time(nullptr);

      std::lock_guard<std::mutex> guard(notice_mutex);

      for (auto it = notice_budgets.begin(); it != notice_budgets.end();)
         it = now - it->second.since >= ThrottleSeconds ? notice_budgets.erase(it) : std::next(it);

      Budget &mine = notice_budgets[requesterKey];
      if (mine.since == 0)
         mine.since = now;

      Budget &all = notice_budgets[allKey];
      if (all.since == 0)
         all.since = now;

      // Said once an hour for each ceiling, the moment it is reached, so that
      // an owner asking why somebody's request went unanswered can find it;
      // the count goes one past the ceiling to remember it was said.
      if (mine.count >= MostNoticesPerRequesterPerHour)
      {
         if (mine.count == MostNoticesPerRequesterPerHour)
         {
            mine.count++;
            LOG_APPLICATION(Formatter::Format("ListManager: {0} has answered {1} requests from {2} in the last hour, its ceiling for one requester; the rest of that hour's are not answered.",
               listAddress, MostNoticesPerRequesterPerHour, requester));
         }
         return false;
      }

      if (all.count >= MostNoticesPerListPerHour)
      {
         if (all.count == MostNoticesPerListPerHour)
         {
            all.count++;
            LOG_APPLICATION(Formatter::Format("ListManager: {0} has answered {1} requests in the last hour, its ceiling; the rest of that hour's are not answered.",
               listAddress, MostNoticesPerListPerHour));
         }
         return false;
      }

      mine.count++;
      all.count++;

      EvictOldest(notice_budgets, PageBudgetCeiling, [](const Budget &budget) { return budget.since; });
      return true;
   }

   String
   ListManager::RequesterOf_(std::shared_ptr<Message> message)
   {
      // The address of the server that handed the message to this one, as the
      // delivery path reads it: what a forger sending from one place shares
      // with every other message sent from there.
      return MessageUtilities::GetSendersIP(message);
   }

   bool
   ListManager::SendNotice_(std::shared_ptr<const DistributionList> list, const String &to, const String &replyTo,
                            const String &subjectName, const String &textName, const String &subjectSuffix,
                            const std::vector<std::pair<String, String> > &extraMacros)
   {
      std::shared_ptr<ServerMessages> messages = Configuration::Instance()->GetServerMessages();
      if (!messages)
         return false;

      const String listAddress = list->GetAddress();

      // The owner's own address goes only to the owner. Anybody else - a
      // stranger refused, a member told they have left - is given the list's
      // name-owner@, which reaches the same person without handing out their
      // address to whoever writes to the list.
      const String owner = OwnerOf(list);
      const bool toOwner = !owner.IsEmpty() && owner.CompareNoCase(OneLine(to)) == 0;

      std::vector<std::pair<String, String> > macros = extraMacros;
      macros.push_back(std::make_pair(String(_T("%MACRO_LIST%")), listAddress));
      macros.push_back(std::make_pair(String(_T("%MACRO_OWNER%")), toOwner ? owner : CommandAddress(listAddress, CommandOwner)));

      const String subject = OneLine(MacroText(messages->GetMessage(subjectName), macros) + subjectSuffix);
      String body = MacroText(messages->GetMessage(textName), macros);
      body.Replace(_T("\r\n"), _T("\n"));
      body.Replace(_T("\n"), _T("\r\n"));

      const String from = CommandAddress(listAddress, CommandRequest);

      AnsiString content;
      content += "From: <" + AnsiString(OneLine(from)) + ">\r\n";
      content += "To: <" + Utf8(OneLine(to)) + ">\r\n";
      if (!replyTo.IsEmpty())
         content += "Reply-To: <" + AnsiString(OneLine(replyTo)) + ">\r\n";

      // A translated subject is written as RFC 2047 encoded words; an ASCII one
      // as it is.
      if (IsAllAscii(subject))
         content += "Subject: " + AnsiString(subject) + "\r\n";
      else
         content += "Subject: " + MIMEUnicodeEncoder::EncodeValue("utf-8", subject) + "\r\n";

      content += "Date: " + AnsiString(Time::GetCurrentMimeDate()) + "\r\n";
      content += "Message-ID: " + AnsiString(Utilities::GenerateMessageID()) + "\r\n";

      // RFC 3834: an answer the server wrote. Another server's vacation reply
      // does not answer it, and the manager does not answer one of those.
      content += "Auto-Submitted: auto-replied\r\n";
      content += "X-Auto-Response-Suppress: All\r\n";
      content += "X-Loop: " + AnsiString(OneLine(listAddress)) + "\r\n";
      content += "MIME-Version: 1.0\r\n";
      content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
      content += "Content-Transfer-Encoding: 8bit\r\n";
      content += "\r\n";
      content += Utf8(body);

      std::shared_ptr<Message> notice(new Message());
      notice->SetState(Message::Delivering);

      // The null sender: a notice that cannot be delivered is not reported to
      // anybody, and cannot start a loop.
      notice->SetFromAddress(_T(""));

      const String fileName = PersistentMessage::GetFileName(notice);

      if (!FileUtilities::WriteToFile(fileName, content))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7253, "ListManager::SendNotice_",
            Formatter::Format("The {0} notice of list {1} to {2} could not be written and has NOT been sent.", textName, listAddress, to));
         return false;
      }

      notice->SetSize(FileUtilities::FileSize(fileName));

      bool recipientOk = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(to, notice->GetRecipients(), recipientOk);

      if (notice->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(fileName);
         LOG_APPLICATION(Formatter::Format("ListManager: the {0} notice of {1} was not sent: {2} is not an address mail can be delivered to.",
            textName, listAddress, to));
         return false;
      }

      if (!PersistentMessage::SaveObject(notice))
      {
         FileUtilities::DeleteFile(fileName);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7253, "ListManager::SendNotice_",
            Formatter::Format("The {0} notice of list {1} to {2} could not be queued and has NOT been sent.", textName, listAddress, to));
         return false;
      }

      Application::Instance()->SubmitPendingEmail();

      LOG_APPLICATION(Formatter::Format("ListManager: {0} of {1} sent to {2}.", textName, listAddress, to));
      return true;
   }

   // ---- bounces --------------------------------------------------------------------

   void
   ListManager::HandleBounce_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, const String &verpMember)
   {
      const String listAddress = list->GetAddress();
      const String fileName = PersistentMessage::GetFileName(message);

      // The report up to the parser's own ceiling. A larger one - a server
      // that returns the whole failed message, attachments and all - is read
      // that far: the report's own part, and the returned header with the
      // member's tag in it, come first.
      AnsiString raw;
      bool truncated = false;
      {
         unsigned __int64 size = 0;
         if (FileUtilities::FileSize64(fileName, size) && size > 0)
         {
            File file;
            if (file.Open(fileName, File::OTReadOnly))
            {
               std::shared_ptr<ByteBuffer> buffer = file.ReadChunk((int) MostBounceBytes);
               file.Close();

               if (buffer && buffer->GetSize() > 0)
                  raw.append(buffer->GetCharBuffer(), buffer->GetSize());

               truncated = size > (unsigned __int64) MostBounceBytes;
            }
         }
      }

      if (raw.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("ListManager: bounce {0} to {1} could not be read; it was dropped.",
            message->GetID(), listAddress));
         return;
      }

      DeliveryReport report;
      const bool parsed = DeliveryReportParser::Parse(raw, report);

      // Which members the bounce is about, and what it says of each: the VERP
      // address names one whatever the report says, and a plain report names
      // its final recipients. One entry per member, however many groups name
      // it. Only a failure is a bounce: a group whose Action is delayed - the
      // "still trying" warning many servers send after a few hours - or
      // delivered, relayed or expanded, is not.
      std::map<String, String> named;
      bool onlyNotFailures = false;

      if (!verpMember.IsEmpty())
      {
         String status;
         bool failed = false;
         bool other = false;

         if (parsed)
         {
            for (const DeliveryReportRecipient &reported : report.recipients)
            {
               if (!reported.action.IsEmpty() && reported.action != _T("failed"))
               {
                  other = true;
                  continue;
               }

               if (!failed || reported.final_recipient.CompareNoCase(verpMember) == 0)
                  status = reported.status;

               failed = true;
            }
         }

         // A report that could be read and names nothing that failed is a
         // warning, not a bounce. One that could not be read - or was cut at
         // the ceiling before its report part - still names the member by its
         // address, and counts as soft.
         if (other && !failed)
            onlyNotFailures = true;
         else
            named[Lowered(verpMember)] = status;
      }
      else if (parsed)
      {
         for (const DeliveryReportRecipient &reported : report.recipients)
         {
            if (!reported.action.IsEmpty() && reported.action != _T("failed"))
            {
               onlyNotFailures = true;
               continue;
            }

            const String key = Lowered(reported.final_recipient);
            if (!key.IsEmpty() && named.find(key) == named.end())
               named[key] = reported.status;
         }
      }

      if (named.empty())
      {
         if (onlyNotFailures)
         {
            LOG_APPLICATION(Formatter::Format("ListManager: report {0} to {1} is a delay warning or a report of delivery, not a bounce; it was not counted.",
               message->GetID(), listAddress));
         }
         else
         {
            LOG_APPLICATION(Formatter::Format("ListManager: bounce {0} to {1} names no member (no VERP address and no report that can be read{2}); it was dropped.",
               message->GetID(), listAddress, String(truncated ? _T(", and it was larger than a report is") : _T(""))));
         }
         return;
      }

      AnsiString loweredRaw = raw;
      loweredRaw.ToLower();

      for (const auto &entry : named)
      {
         const String &address = entry.first;
         const String &status = entry.second;

         // A report that says the message was delivered, or relayed, is not a
         // bounce at all.
         if (!status.IsEmpty() && status.GetAt(0) == L'2')
            continue;

         std::shared_ptr<DistributionListRecipient> member = MemberOf(list->GetID(), address);

         if (!member || !member->GetIsMember())
         {
            LOG_APPLICATION(Formatter::Format("ListManager: bounce {0} to {1} is about {2}, who is not a member; it was logged and dropped.",
               message->GetID(), listAddress, address));
            continue;
         }

         // The member tag: a bounce is believed only when it came back from a
         // copy this list sent to this member. Anybody can write to the bounce
         // address; only the member's copy, or its returned header, carries this.
         const AnsiString tag = MemberTag(member->GetToken(), listAddress);
         if (tag.IsEmpty() || loweredRaw.Find(tag) < 0)
         {
            LOG_APPLICATION(Formatter::Format("ListManager: bounce {0} to {1} about {2} does not carry the member's tag from a copy the list sent, so it was not counted.",
               message->GetID(), listAddress, address));
            continue;
         }

         const bool hard = !status.IsEmpty() && status.GetAt(0) == L'5';
         CountBounce_(list, member, hard, status);
      }
   }

   void
   ListManager::CountBounce_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member, bool hard, const String &status)
   {
      const __int64 now = Now_();

      // Counted where the count is stored, and read back: two reports handled
      // at once on two delivery threads both count. A soft bounce more than
      // the window after the first of the current count starts a new count; a
      // delivery that succeeds does not reset it, the window does.
      const __int64 windowStart = now - (__int64) BounceWindowDays() * 86400;

      if (!PersistentDistributionListRecipient::AddBounce(member, hard, now, windowStart))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::CountBounce_",
            Formatter::Format("A bounce from {0} on list {1} could not be counted.", member->GetAddress(), list->GetAddress()));
         return;
      }

      LOG_APPLICATION(Formatter::Format("ListManager: a {0} bounce from {1} on {2} was counted: {3} hard, {4} soft.",
         String(hard ? _T("hard") : _T("soft")), member->GetAddress(), list->GetAddress(), member->GetHardBounces(), member->GetSoftBounces()));

      String reason;

      if (hard && member->GetHardBounces() >= HardBouncesToUnsubscribe())
         reason = Formatter::Format("{0} hard bounce(s), the last with status {1}.", member->GetHardBounces(), status.IsEmpty() ? String(_T("(none given)")) : status);
      else if (!hard && member->GetSoftBounces() >= SoftBouncesToUnsubscribe())
         reason = Formatter::Format("{0} soft bounce(s) within {1} day(s), the last with status {2}.", member->GetSoftBounces(), BounceWindowDays(),
            status.IsEmpty() ? String(_T("(none that could be read)")) : status);

      // Below its count, or no longer sent the list's mail - stopped by an
      // earlier bounce, when the owner was told, or paused by the member's own
      // choice, which the bounces leave as the member's own.
      if (reason.IsEmpty() || member->GetDelivery() == DistributionListRecipient::DeliveryNone)
         return;

      // The reason is what says the bounces stopped the delivery, and what the
      // owner's reset reads to know it may start it again.
      member->SetDelivery(DistributionListRecipient::DeliveryNone);
      member->SetReason(reason);

      if (!PersistentDistributionListRecipient::SaveDeliveryState(member))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7254, "ListManager::CountBounce_",
            Formatter::Format("The bounces of {0} on list {1} reached their count, and its delivery could not be stopped.", member->GetAddress(), list->GetAddress()));
         return;
      }

      // The owner is told once: this is the moment delivery stops, and a member
      // already stopped is not stopped again.
      std::vector<std::pair<String, String> > macros;
      macros.push_back(std::make_pair(String(_T("%MACRO_ADDRESS%")), member->GetAddress()));
      macros.push_back(std::make_pair(String(_T("%MACRO_REASON%")), reason));

      const String owner = OwnerOf(list);
      if (!owner.IsEmpty())
         SendNotice_(list, owner, String(), _T("LIST_BOUNCED_SUBJECT"), _T("LIST_BOUNCED"), String(), macros);

      LOG_APPLICATION(Formatter::Format("ListManager: {0} is no longer sent the mail of {1}: {2}", member->GetAddress(), list->GetAddress(), reason));
   }

   // ---- the member's pages ---------------------------------------------------------

   namespace
   {
      ListManager::Outcome Gone()
      {
         ListManager::Outcome outcome;
         outcome.http_status = 410;
         outcome.heading = _T("This link no longer works");
         outcome.detail = _T("It has been used, it has lapsed, or the list it belonged to has gone. Nothing was changed.");
         return outcome;
      }

      // The row a token names, and its list, when both are there and the list
      // is managed. The one answer for every way a token can fail.
      bool Resolve(const String &token, std::shared_ptr<DistributionListRecipient> &member, std::shared_ptr<DistributionList> &list)
      {
         if (!ListManager::IsTokenText(token))
            return false;

         member = std::shared_ptr<DistributionListRecipient>(new DistributionListRecipient);
         if (!PersistentDistributionListRecipient::ReadByToken(member, token))
            return false;

         list = ListManager::ListById(member->GetListID());
         return list && list->GetManaged();
      }

      // Compared in time that does not depend on where they differ.
      bool SameToken(const String &a, const String &b)
      {
         if (a.GetLength() != b.GetLength())
            return false;

         int difference = 0;
         for (int i = 0; i < a.GetLength(); i++)
            difference |= (int) (a.GetAt(i) ^ b.GetAt(i));

         return difference == 0;
      }

      // An account the membership query answers, from its link: the list, the
      // HMAC of the address under the list's secret, and the query answering
      // the address still. A row standing in the query's place - a member, or
      // an address that has left - means the link is not the way in.
      bool ResolveQueryMember(const String &token, const String &address, __int64 listId, std::shared_ptr<DistributionList> &list)
      {
         if (!ListManager::IsTokenText(token) || address.IsEmpty() || listId <= 0)
            return false;

         list = ListManager::ListById(listId);
         if (!list || !list->GetManaged())
            return false;

         const String expected = ListManager::QueryMemberToken(list->GetSecret(), address);
         if (expected.IsEmpty() || !SameToken(expected, token))
            return false;

         std::shared_ptr<DistributionListRecipient> row = ListManager::MemberOf(list->GetID(), address);
         if (row && row->GetTakesQueryPlace())
            return false;

         return ListManager::QueryAnswers(list, address);
      }
   }

   ListManager::Outcome
   ListManager::InspectConfirmation(const String &token)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      if (!Resolve(token, member, list) || member->GetConfirmed() != DistributionListRecipient::Pending ||
          Now_() - member->GetSubscribed() > (__int64) RequestLifetimeDays * 86400 ||
          list->GetSubscribePolicy() == DistributionList::SubscribeClosed)
         return Gone();

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.heading = Formatter::Format("Join {0}?", list->GetAddress());
      outcome.detail = list->GetSubscribePolicy() == DistributionList::SubscribeModerated
         ? Formatter::Format("{0} asked to join. Confirm, and the person who runs the list is asked to let you in.", member->GetAddress())
         : Formatter::Format("{0} asked to join. Confirm, and it is sent the list's mail from the next message on.", member->GetAddress());
      return outcome;
   }

   ListManager::Outcome
   ListManager::Confirm(const String &token)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      if (!Resolve(token, member, list) || member->GetConfirmed() != DistributionListRecipient::Pending ||
          Now_() - member->GetSubscribed() > (__int64) RequestLifetimeDays * 86400 ||
          list->GetSubscribePolicy() == DistributionList::SubscribeClosed)
         return Gone();

      if (!ConfirmRow_(list, member))
      {
         Outcome failed;
         failed.http_status = 500;
         failed.heading = _T("That could not be saved");
         failed.detail = _T("Try the link again in a moment.");
         return failed;
      }

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.heading = member->GetConfirmed() == DistributionListRecipient::AwaitingOwner
         ? String(_T("Thank you - the owner has been asked"))
         : Formatter::Format("You have joined {0}", list->GetAddress());
      outcome.detail = member->GetConfirmed() == DistributionListRecipient::AwaitingOwner
         ? Formatter::Format("The person who runs {0} decides who joins it. You will get its mail once they let you in.", list->GetAddress())
         : String(_T("Every message from the list carries a link at which you can leave it, take a daily digest instead, or pause it."));
      return outcome;
   }

   ListManager::Outcome
   ListManager::InspectMember(const String &token, const String &queryAddress, __int64 queryListId)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      // An account the membership query answers: it may leave, and nothing
      // else - its delivery is the list's, since it has no row of its own.
      if (!queryAddress.IsEmpty() || queryListId != 0)
      {
         if (!ResolveQueryMember(token, queryAddress, queryListId, list))
            return Gone();

         Outcome outcome;
         outcome.ok = true;
         outcome.query_member = true;
         outcome.list_address = list->GetAddress();
         outcome.member_address = Lowered(queryAddress);
         outcome.heading = Formatter::Format("Your subscription to {0}", list->GetAddress());
         outcome.detail = Formatter::Format("{0} is sent the list's mail as one of the accounts the list is sent to. Leave the list, and it is sent nothing more.", Lowered(queryAddress));
         return outcome;
      }

      if (!Resolve(token, member, list) || !member->GetIsMember())
         return Gone();

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.delivery = member->GetDelivery();
      outcome.heading = Formatter::Format("Your subscription to {0}", list->GetAddress());
      outcome.detail = Formatter::Format("{0} is a member. Leave the list, or choose how its mail comes.", member->GetAddress());
      return outcome;
   }

   ListManager::Outcome
   ListManager::LeaveByToken(const String &token, const String &queryAddress, __int64 queryListId)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;
      String address;

      if (!queryAddress.IsEmpty() || queryListId != 0)
      {
         if (!ResolveQueryMember(token, queryAddress, queryListId, list))
            return Gone();

         address = Lowered(queryAddress);

         if (!OptOut_(list, address))
         {
            Outcome failed;
            failed.http_status = 500;
            failed.heading = _T("That could not be done");
            failed.detail = _T("You are still on the list. Try the link again in a moment.");
            return failed;
         }
      }
      else
      {
         // A member's token, and a member's only: a request's token - which
         // no copy of the list's mail carries - does not leave anything.
         if (!Resolve(token, member, list) || !member->GetIsMember())
            return Gone();

         address = member->GetAddress();

         if (!LeaveRow_(list, member))
         {
            Outcome failed;
            failed.http_status = 500;
            failed.heading = _T("That could not be done");
            failed.detail = _T("You are still on the list. Try the link again in a moment.");
            return failed;
         }
      }

      LOG_APPLICATION(Formatter::Format("ListManager: {0} left {1} by its link.", address, list->GetAddress()));

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = address;
      outcome.heading = Formatter::Format("You have left {0}", list->GetAddress());
      outcome.detail = Formatter::Format("{0} will not be sent the list's mail any more.", address);
      return outcome;
   }

   ListManager::Outcome
   ListManager::SetDeliveryByToken(const String &token, int delivery)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      if (!Resolve(token, member, list) || !member->GetIsMember())
         return Gone();

      String reason;
      if (!SetDelivery(list, member->GetAddress(), delivery, reason))
      {
         Outcome failed;
         failed.http_status = 400;
         failed.heading = _T("That could not be changed");
         failed.detail = reason;
         return failed;
      }

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.delivery = delivery;
      outcome.heading = _T("Saved");
      outcome.detail = delivery == DistributionListRecipient::DeliveryDigest
         ? Formatter::Format("{0} is sent one digest a day.", member->GetAddress())
         : delivery == DistributionListRecipient::DeliveryNone
            ? Formatter::Format("{0} stays a member and is sent nothing until you turn it back on.", member->GetAddress())
            : Formatter::Format("{0} is sent every message.", member->GetAddress());
      return outcome;
   }

   ListManager::Outcome
   ListManager::InspectApproval(const String &token)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      if (!Resolve(token, member, list) || member->GetConfirmed() != DistributionListRecipient::AwaitingOwner ||
          Now_() - member->GetSubscribed() > (__int64) RequestLifetimeDays * 86400)
         return Gone();

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.heading = Formatter::Format("Let {0} join {1}?", member->GetAddress(), list->GetAddress());
      outcome.detail = Formatter::Format("{0} asked to join and confirmed that the address is theirs. Only {1}, signed in to this server's webmail, can let them in here.",
         member->GetAddress(), OwnerOf(list));
      return outcome;
   }

   ListManager::Outcome
   ListManager::ApproveByToken(const String &token, const String &signedInAddress)
   {
      std::shared_ptr<DistributionListRecipient> member;
      std::shared_ptr<DistributionList> list;

      if (!Resolve(token, member, list) || member->GetConfirmed() != DistributionListRecipient::AwaitingOwner ||
          Now_() - member->GetSubscribed() > (__int64) RequestLifetimeDays * 86400)
         return Gone();

      const String owner = OwnerOf(list);

      // The owner's own account, signed in to the webmail: the token says the
      // link came from the owner's mailbox, the session says who is pressing
      // the button. Either alone is not the owner.
      if (signedInAddress.IsEmpty())
      {
         Outcome refused;
         refused.http_status = 403;
         refused.heading = _T("Sign in first");
         refused.detail = Formatter::Format("Sign in to this server's webmail as {0}, then open the link in the message again and press the button.", owner);
         return refused;
      }

      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      const String signedIn = aliases ? aliases->ApplyAliasesOnAddress(signedInAddress) : signedInAddress;
      const String ownerAddress = aliases ? aliases->ApplyAliasesOnAddress(owner) : owner;

      if (owner.IsEmpty() || signedIn.CompareNoCase(ownerAddress) != 0)
      {
         LOG_APPLICATION(Formatter::Format("ListManager: {0}, signed in, tried to approve {1} for {2}; only its owner {3} can.",
            signedInAddress, member->GetAddress(), list->GetAddress(), owner));

         Outcome refused;
         refused.http_status = 403;
         refused.heading = _T("This is not the owner's account");
         refused.detail = Formatter::Format("Only {0} can let people into {1}. You are signed in as {2}.", owner, list->GetAddress(), signedInAddress);
         return refused;
      }

      String reason;
      if (!Approve(list, member->GetAddress(), reason))
      {
         Outcome failed;
         failed.http_status = 500;
         failed.heading = _T("That could not be saved");
         failed.detail = _T("Try the link again in a moment.");
         return failed;
      }

      Outcome outcome;
      outcome.ok = true;
      outcome.list_address = list->GetAddress();
      outcome.member_address = member->GetAddress();
      outcome.heading = Formatter::Format("{0} has joined {1}", member->GetAddress(), list->GetAddress());
      outcome.detail = _T("They are sent the list's mail from the next message on.");
      return outcome;
   }

   // ---- administration -----------------------------------------------------------

   bool
   ListManager::Approve(std::shared_ptr<const DistributionList> list, const String &address, String &reason)
   {
      std::shared_ptr<DistributionListRecipient> member = list ? MemberOf(list->GetID(), address) : std::shared_ptr<DistributionListRecipient>();
      if (!member)
      {
         reason = "not a member or a request of this list";
         return false;
      }

      if (member->GetConfirmed() == DistributionListRecipient::Confirmed)
         return true;

      if (member->GetConfirmed() != DistributionListRecipient::AwaitingOwner)
      {
         reason = "the request has not been confirmed by its address yet";
         return false;
      }

      const String token = NewToken();
      if (token.IsEmpty())
      {
         reason = "no token could be made";
         return false;
      }

      member->SetConfirmed(DistributionListRecipient::Confirmed);
      member->SetSubscribed(Now_());
      member->SetToken(token);

      if (!PersistentDistributionListRecipient::SaveState(member))
      {
         reason = "the approval could not be saved";
         return false;
      }

      LOG_APPLICATION(Formatter::Format("ListManager: the owner approved {0}, who has joined {1}.", member->GetAddress(), list->GetAddress()));
      return true;
   }

   bool
   ListManager::ResetBounces(std::shared_ptr<const DistributionList> list, const String &address, String &reason)
   {
      std::shared_ptr<DistributionListRecipient> member = list ? MemberOf(list->GetID(), address) : std::shared_ptr<DistributionListRecipient>();
      if (!member || !member->GetIsMember())
      {
         reason = "not a member of this list";
         return false;
      }

      return ResetBounces(member, reason);
   }

   bool
   ListManager::ResetBounces(std::shared_ptr<DistributionListRecipient> member, String &reason)
   {
      if (!member || member->GetID() == 0)
      {
         reason = "not a member of this list";
         return false;
      }

      // The reason is set when, and only when, the bounces stopped the
      // delivery (CountBounce_), and the member's own choice clears it
      // (SetDelivery). So a member the bounces stopped is sent each message
      // again, and one who chose to pause stays paused, whatever its counts.
      const bool stoppedByBounces = !member->GetReason().IsEmpty();

      member->SetHardBounces(0);
      member->SetSoftBounces(0);
      member->SetSoftSince(0);
      member->SetReason(String());

      if (stoppedByBounces && member->GetDelivery() == DistributionListRecipient::DeliveryNone)
         member->SetDelivery(DistributionListRecipient::DeliveryEach);

      if (!PersistentDistributionListRecipient::SaveDeliveryState(member))
      {
         reason = "the member could not be saved";
         return false;
      }

      return true;
   }

   bool
   ListManager::SetDelivery(std::shared_ptr<const DistributionList> list, const String &address, int delivery, String &reason)
   {
      if (delivery < DistributionListRecipient::DeliveryEach || delivery > DistributionListRecipient::DeliveryNone)
      {
         reason = "delivery must be each, digest or none";
         return false;
      }

      std::shared_ptr<DistributionListRecipient> member = list ? MemberOf(list->GetID(), address) : std::shared_ptr<DistributionListRecipient>();
      if (!member || !member->GetIsMember())
      {
         reason = "not a member of this list";
         return false;
      }

      // Turning delivery back on clears the bounces that stopped it, as the
      // owner's reset does: the member is saying the address works again.
      if (delivery != DistributionListRecipient::DeliveryNone)
      {
         member->SetHardBounces(0);
         member->SetSoftBounces(0);
         member->SetSoftSince(0);
      }

      // Whatever the member chose is the member's own: a pause chosen here is
      // not one the owner's reset of the bounces lifts.
      member->SetReason(String());
      member->SetDelivery(delivery);

      if (!PersistentDistributionListRecipient::SaveDeliveryState(member))
      {
         reason = "the member could not be saved";
         return false;
      }

      return true;
   }

   bool
   ListManager::Leave(std::shared_ptr<const DistributionList> list, const String &address, String &reason)
   {
      std::shared_ptr<DistributionListRecipient> member = list ? MemberOf(list->GetID(), address) : std::shared_ptr<DistributionListRecipient>();
      if (!member || member->GetConfirmed() == DistributionListRecipient::OptedOut)
      {
         reason = "not a member of this list";
         return false;
      }

      if (!LeaveRow_(list, member))
      {
         reason = "the member could not be removed";
         return false;
      }

      return true;
   }

   // ---- the pages' budget ----------------------------------------------------------

   bool
   ListManager::AllowUnauthenticated(const IPAddress &peer)
   {
      const AnsiString key = peer.ToString();

      // A process on this machine is not a scraper, and the regression suite is
      // one - loopback is exempt, as it is from the failed-logon count.
      if (key == "127.0.0.1" || key == "::1")
         return true;

      const time_t now = time(nullptr);

      std::lock_guard<std::mutex> guard(budget_mutex);

      for (auto it = budgets.begin(); it != budgets.end();)
         it = now - it->second.since >= ThrottleSeconds ? budgets.erase(it) : std::next(it);

      Budget &budget = budgets[key];
      if (budget.since == 0 || now - budget.since >= ThrottleSeconds)
      {
         budget.since = now;
         budget.count = 0;
      }

      if (budget.count >= PageRequestsPerAddressPerHour)
         return false;

      budget.count++;

      // Past its ceiling the map loses its oldest entries, never all of them:
      // clearing it would give every address its hour back at once.
      EvictOldest(budgets, PageBudgetCeiling, [](const Budget &entry) { return entry.since; });
      return true;
   }

   bool
   ListManager::IsUsAscii_(const String &value)
   {
      return IsAllAscii(value);
   }

   __int64
   ListManager::Now_()
   {
      return (__int64) time(nullptr);
   }

   // ---- the tester -----------------------------------------------------------------

   void
   ListManagerTester::Test()
   {
      String listLocal, member;
      ListManager::Command command = ListManager::CommandNone;

      if (!SplitLocalPart(_T("team-subscribe"), listLocal, command, member) || listLocal != _T("team") || command != ListManager::CommandSubscribe)
         throw std::logic_error("team-subscribe was not read as the subscribe address of team.");

      if (!SplitLocalPart(_T("team-unsubscribe"), listLocal, command, member) || listLocal != _T("team") || command != ListManager::CommandUnsubscribe)
         throw std::logic_error("team-unsubscribe was read as something other than the unsubscribe address of team.");

      if (!SplitLocalPart(_T("Team-Request"), listLocal, command, member) || listLocal != _T("Team") || command != ListManager::CommandRequest)
         throw std::logic_error("A request address in capitals was not recognised.");

      if (SplitLocalPart(_T("team"), listLocal, command, member))
         throw std::logic_error("A plain local part was read as a command address.");

      if (SplitLocalPart(_T("-bounces"), listLocal, command, member))
         throw std::logic_error("A local part that is only the suffix was read as a command address.");

      // VERP both ways, with a member whose own local part holds + and =.
      const String verp = ListManager::VerpAddress(_T("team@example.test"), _T("a+b=c@remote.example"));
      if (verp != _T("team-bounces+a+b=c=remote.example@example.test"))
         throw std::logic_error("The VERP address of a member was not what the grammar says.");

      if (!SplitLocalPart(_T("team-bounces+a+b=c=remote.example"), listLocal, command, member) ||
          listLocal != _T("team") || command != ListManager::CommandBounces || member != _T("a+b=c@remote.example"))
         throw std::logic_error("A VERP address did not decode to the member it encodes.");

      if (SplitLocalPart(_T("team-bounces+nomember"), listLocal, command, member))
         throw std::logic_error("A VERP address with no member in it was accepted.");

      if (ListManager::CommandAddress(_T("team@example.test"), ListManager::CommandOwner) != _T("team-owner@example.test"))
         throw std::logic_error("The owner address of a list was not name-owner@.");

      if (!ListManager::IsTokenText(_T("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")))
         throw std::logic_error("A token was refused.");

      if (ListManager::IsTokenText(_T("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef")) ||
          ListManager::IsTokenText(_T("0123456789abcdef")))
         throw std::logic_error("Something that is not a token was accepted as one.");

      const AnsiString tag = ListManager::MemberTag(_T("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), _T("team@example.test"));
      if (tag.GetLength() != 32 || tag != ListManager::MemberTag(_T("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), _T("TEAM@example.test")))
         throw std::logic_error("A member tag was not 32 characters, or depended on the case of the list's address.");

      if (tag == ListManager::MemberTag(_T("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), _T("other@example.test")))
         throw std::logic_error("The same token gave the same tag on two lists.");

      if (!ListManager::MemberTag(String(), _T("team@example.test")).IsEmpty())
         throw std::logic_error("A member without a token was given a tag.");

      // A query member's token: 64 hex characters, the same for the address in
      // any case, another for another address or another secret, and none at
      // all without a secret.
      const String secret = _T("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
      const String queryToken = ListManager::QueryMemberToken(secret, _T("jo@example.test"));
      if (!ListManager::IsTokenText(queryToken) || queryToken != ListManager::QueryMemberToken(secret, _T("JO@Example.test")))
         throw std::logic_error("A query member's token was not 64 hex characters, or depended on the case of the address.");

      if (queryToken == ListManager::QueryMemberToken(secret, _T("al@example.test")) ||
          queryToken == ListManager::QueryMemberToken(_T("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"), _T("jo@example.test")))
         throw std::logic_error("Two addresses, or two secrets, gave a query member the same token.");

      if (!ListManager::QueryMemberToken(String(), _T("jo@example.test")).IsEmpty())
         throw std::logic_error("A query member was given a token without the list's secret.");

      // What may be stored as the base URL: an absolute http(s) URL in
      // printable ASCII, and nothing BaseUrl() would quietly drop.
      String reason;
      if (!ListManager::IsUsableBaseUrl(_T("https://lists.example.test"), reason) ||
          ListManager::IsUsableBaseUrl(_T("ftp://lists.example.test"), reason) ||
          ListManager::IsUsableBaseUrl(_T("https://l\u00e4sts.example.test"), reason))
         throw std::logic_error("The base URL rule accepted what it refuses, or refused what it accepts.");
   }
}
