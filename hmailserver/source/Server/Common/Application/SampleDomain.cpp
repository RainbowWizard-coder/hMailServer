// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The sample domain. See SampleDomain.h for what it is and what it is not.
//
// HOW IT IS BUILT
//
// Every object is made through the same persistence the Control Panel, the
// Control Deck and the REST API make one through - PersistentDomain,
// PersistentAccount (with createInbox true, or the mail would be accepted and
// dropped), PersistentDistributionList, PersistentDistributionListRecipient and
// PersistentRule - so the sample is a domain like any other, is audited like any
// other, and can be edited and deleted by hand afterwards.
//
// The messages are not written into mailboxes. Each one is composed as RFC 5322
// text, written to the queue's own file, given a recipient list by the SMTP
// server's own RecipientParser, saved as a message in the Delivering state, and
// the deliverer woken. That is the same path a message accepted over SMTP takes
// from the moment DATA ends, so everything that happens to real mail happens to
// these: the distribution list expands, the account's rule files the invoice
// into a folder that the rule itself creates, quota is counted, the message
// index is written, and every IMAP and webmail session is told. Writing rows
// into hm_messages directly would have been a dozen lines shorter and would have
// produced mail that no rule had seen.
//
// THE PASSWORD
//
// Minted here, per creation, by AppPassword::GenerateSecret - twenty characters
// of Crockford base32 from RAND_bytes, grouped in fives, the same credential an
// app password is - and shown once, in the report and in the sentence that goes
// with it. Nothing stores the clear text: the accounts carry the hash, as every
// account does, and a later GET of the sample cannot tell anybody what the
// password was.
//
// It used to be a constant printed in hmailserver/docs/SampleDomain.md. That is
// what this file's first version did, and it was wrong: this server's default
// address range (SecurityRanges.cpp, options 96203) lets an authenticated
// session relay local-to-remote from anywhere, so a published password made
// three open relays out of every installation that had ever tried the sample and
// not removed it. A random one costs the feature nothing - the sample is still
// one call and still needs no choices made - and removes that entirely.
//
// No password policy check and no breach check, still. Twenty random base32
// characters are 98 bits; a policy that demanded a digit would refuse some of
// them for no reason, and the breach check has nothing to find in a value that
// has never existed before. build/check-password-choice-sites.py names the sites
// that must ask; this is not one of them because it asks the policy nothing.
#include "StdAfx.h"
#include "SampleDomain.h"

#include "Application.h"
#include "Configuration.h"
#include "ErrorManager.h"
#include "IniFileSettings.h"
#include "Logger.h"
#include "../BO/Account.h"
#include "../BO/Accounts.h"
#include "../BO/AppPassword.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/DistributionLists.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../BO/Rule.h"
#include "../BO/RuleAction.h"
#include "../BO/RuleActions.h"
#include "../BO/RuleCriteria.h"
#include "../BO/RuleCriterias.h"
#include "../BO/Rules.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Persistence/PersistentDomain.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/PersistentRule.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Util/Crypt.h"
#include "../Util/FileUtilities.h"
#include "../Util/Time.h"
#include "../Util/Unicode.h"
#include "../Util/Utilities.h"
#include "../../SMTP/DeliveryQueue.h"
#include "../../SMTP/RecipientParser.h"

#include <set>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const wchar_t *SampleDomain::DomainName = _T("example.test");
   const wchar_t *SampleDomain::ListAddress = _T("everyone@example.test");

   namespace
   {
      // The three mailboxes, in the order the report lists them. The names are
      // memorable on purpose: a page showing "Account 1, Account 2" teaches
      // nothing about what an address list looks like.
      struct SampleAccount
      {
         const wchar_t *address;
         const wchar_t *first;
         const wchar_t *last;
      };

      const SampleAccount Mailboxes[] =
      {
         { _T("alice@example.test"), _T("Alice"), _T("Okafor") },
         { _T("bob@example.test"),   _T("Bob"),   _T("Ferreira") },
         { _T("carol@example.test"), _T("Carol"), _T("Ridley") }
      };

      const int MailboxCount = (int) (sizeof(Mailboxes) / sizeof(Mailboxes[0]));

      // The rule the sample puts on the first mailbox, and the folder it files
      // into. The folder is not created here: the rule's own action creates it
      // the first time it fires, which is what proves the rule ran.
      const wchar_t *RuleName = _T("File the invoices");
      const wchar_t *RuleFolder = _T("Invoices");
      const wchar_t *RuleSubjectWord = _T("Invoice");

      AnsiString
      Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      AnsiString
      JsonEscape(const AnsiString &value)
      {
         AnsiString out;
         const char *chars = value.c_str();
         for (size_t i = 0; i < value.size(); i++)
         {
            const unsigned char c = (unsigned char) chars[i];
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) c);
                  out += escaped;
               }
               else
                  out += (char) c;
            }
         }
         return out;
      }

      AnsiString
      JsonString(const String &value)
      {
         return "\"" + JsonEscape(Utf8(value)) + "\"";
      }

      AnsiString
      Number(long long value)
      {
         AnsiString out;
         out.Format("%lld", value);
         return out;
      }

      // A header value with no way out of the header: a sample subject is ours
      // rather than a caller's, but a line break in one would still be a folded
      // header rather than the text somebody meant.
      String
      HeaderSafe(const String &value)
      {
         String out = value;
         out.Replace(_T("\r"), _T(" "));
         out.Replace(_T("\n"), _T(" "));
         return out;
      }

      // One message of the sample, before it becomes a file and a queue row.
      struct SampleMessage
      {
         const wchar_t *from;
         const wchar_t *to;          // one or more addresses, comma separated
         const wchar_t *subject;
         const wchar_t *body;
         bool reply;                 // threads onto the message before it
         bool attachment;            // a small CSV, so a page shows a paperclip
      };

      const SampleMessage SampleMessages[] =
      {
         { _T("alice@example.test"), _T("bob@example.test, carol@example.test"),
           _T("Welcome to the new mail server"),
           _T("Morning both,\r\n\r\nWe are on the new server from today. Your address has not changed and your old mail is where it was.\r\n\r\nIf anything looks wrong, say so here rather than filing a ticket - it is quicker this week.\r\n\r\nAlice\r\n"),
           false, false },

         { _T("bob@example.test"), _T("alice@example.test"),
           _T("Re: Welcome to the new mail server"),
           _T("Working here. Folders, search and the calendar all came across.\r\n\r\nOne thing: the phone asked me to accept the certificate again. Expected?\r\n\r\nBob\r\n"),
           true, false },

         { _T("carol@example.test"), _T("everyone@example.test"),
           _T("The coffee machine is fixed"),
           _T("It descales itself on Mondays now, so do not be alarmed by the noise at eight.\r\n\r\nCarol\r\n"),
           false, false },

         { _T("bob@example.test"), _T("alice@example.test"),
           _T("Invoice 2026-041 for September hosting"),
           _T("Alice,\r\n\r\nInvoice 2026-041 is attached to the supplier's portal as usual - 340.00 for September, due on the 30th.\r\n\r\nBob\r\n"),
           false, false },

         { _T("carol@example.test"), _T("alice@example.test, bob@example.test"),
           _T("September figures"),
           _T("The month's numbers are in the file attached. Nothing alarming; the delivery costs are up because of the two returns.\r\n\r\nCarol\r\n"),
           false, true },

         { _T("alice@example.test"), _T("everyone@example.test"),
           _T("Planned restart on Sunday at 02:00"),
           _T("The server restarts at two on Sunday morning for updates. Mail sent during the restart is held by the sending server and arrives a few minutes late; nothing is lost.\r\n\r\nAlice\r\n"),
           false, false },

         { _T("bob@example.test"), _T("carol@example.test"),
           _T("Could you look at the backup report?"),
           _T("Last night's backup says it skipped one mailbox. I cannot tell from the summary whether that matters.\r\n\r\nBob\r\n"),
           false, false },

         { _T("carol@example.test"), _T("bob@example.test"),
           _T("Re: Could you look at the backup report?"),
           _T("It was the mailbox we removed on Friday, so the backup is right and the report is out of date. I have cleared it.\r\n\r\nCarol\r\n"),
           true, false }
      };

      const int MessageCount = (int) (sizeof(SampleMessages) / sizeof(SampleMessages[0]));

      // The CSV the one attachment carries. Small, readable, and text, so no
      // page has to decode anything to show that an attachment is there.
      const char *AttachmentName = "september-figures.csv";
      const char *AttachmentText =
         "week,orders,returns,delivery\r\n"
         "1,412,3,188.40\r\n"
         "2,455,2,201.15\r\n"
         "3,398,9,244.80\r\n"
         "4,471,4,196.05\r\n";

      // The message as it goes into the queue file. previousId threads a reply
      // onto the message before it; an empty one simply leaves the two headers
      // off, which is what a message that starts a thread looks like.
      AnsiString
      Compose(const SampleMessage &message, const String &messageId, const String &previousId)
      {
         const AnsiString date = AnsiString(Time::GetCurrentMimeDate());

         AnsiString content;
         content += "Return-Path: <" + Utf8(HeaderSafe(String(message.from))) + ">\r\n";
         content += "From: <" + Utf8(HeaderSafe(String(message.from))) + ">\r\n";
         content += "To: " + Utf8(HeaderSafe(String(message.to))) + "\r\n";
         content += "Subject: " + Utf8(HeaderSafe(String(message.subject))) + "\r\n";
         content += "Date: " + date + "\r\n";
         content += "Message-ID: " + Utf8(HeaderSafe(messageId)) + "\r\n";

         if (message.reply && !previousId.IsEmpty())
         {
            content += "In-Reply-To: " + Utf8(HeaderSafe(previousId)) + "\r\n";
            content += "References: " + Utf8(HeaderSafe(previousId)) + "\r\n";
         }

         content += "X-hMailServer-Sample: example.test\r\n";
         content += "MIME-Version: 1.0\r\n";

         if (!message.attachment)
         {
            content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
            content += "Content-Transfer-Encoding: 8bit\r\n";
            content += "\r\n";
            content += Utf8(String(message.body));
            return content;
         }

         // A boundary of this server's own making rather than a fixed string:
         // two sample messages with the same boundary in one mailbox is a
         // needless thing to have to think about later. Built from the message
         // id with everything that is not a letter or a digit dropped, because
         // RFC 2046's bchars do not include the "@" a message id carries.
         AnsiString boundary = "hm-sample-";
         {
            const AnsiString source = Utf8(messageId);
            for (size_t i = 0; i < source.size() && boundary.size() < 40; i++)
            {
               const char c = source.c_str()[i];
               if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                  boundary += c;
            }
         }

         content += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n";
         content += "\r\n";
         content += "This is a message in MIME format.\r\n";
         content += "\r\n--" + boundary + "\r\n";
         content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
         content += "Content-Transfer-Encoding: 8bit\r\n";
         content += "\r\n";
         content += Utf8(String(message.body));
         content += "\r\n--" + boundary + "\r\n";
         content += AnsiString("Content-Type: text/csv; charset=\"us-ascii\"; name=\"") + AttachmentName + "\"\r\n";
         content += "Content-Transfer-Encoding: 7bit\r\n";
         content += AnsiString("Content-Disposition: attachment; filename=\"") + AttachmentName + "\"\r\n";
         content += "\r\n";
         content += AttachmentText;
         content += "\r\n--" + boundary + "--\r\n";

         return content;
      }

      // Each address of a comma-separated To, trimmed. The list address is one
      // of these; the recipient parser is what expands it.
      std::vector<String>
      SplitAddresses(const String &addresses)
      {
         std::vector<String> out;
         String rest = addresses;
         while (!rest.IsEmpty())
         {
            const int comma = rest.Find(_T(","));
            String one = comma < 0 ? rest : rest.Mid(0, comma);
            rest = comma < 0 ? String() : rest.Mid(comma + 1);
            one.Trim();
            if (!one.IsEmpty())
               out.push_back(one);
         }
         return out;
      }

      // One message into the delivery queue: the file first, the row second, as
      // ConfidentialMessages::SendCode writes one. envelopeRecipients counts
      // what the parser made of the To - the list address becomes its members
      // here, which is what makes the sample's list visibly a list.
      bool
      Queue(const SampleMessage &message, const String &messageId, const String &previousId, int &envelopeRecipients)
      {
         envelopeRecipients = 0;

         const AnsiString content = Compose(message, messageId, previousId);

         std::shared_ptr<Message> queued = std::shared_ptr<Message>(new Message());
         queued->SetState(Message::Delivering);
         queued->SetFromAddress(String(message.from));

         const String fileName = PersistentMessage::GetFileName(queued);
         if (!FileUtilities::WriteToFile(fileName, content))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6974, "SampleDomain::Queue",
               String(_T("A sample message could not be written to the delivery queue: ")) + fileName);
            return false;
         }

         queued->SetSize(FileUtilities::FileSize(fileName));

         RecipientParser parser;
         const std::vector<String> addresses = SplitAddresses(String(message.to));
         for (const String &address : addresses)
         {
            bool recipientOk = false;
            parser.CreateMessageRecipientList(address, queued->GetRecipients(), recipientOk);
         }

         envelopeRecipients = queued->GetRecipients()->GetCount();

         if (envelopeRecipients == 0 || !PersistentMessage::SaveObject(queued))
         {
            FileUtilities::DeleteFile(fileName);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6975, "SampleDomain::Queue",
               String(_T("A sample message could not be queued for ")) + String(message.to));
            return false;
         }

         return true;
      }

      // How many messages are in the mailboxes of one account, for the removal's
      // report. Asked per account rather than with a subquery over the domain,
      // because every backend this server supports answers this one the same way.
      long long
      CountMessages(__int64 accountId)
      {
         SQLCommand command(_T("select count(*) as samplecount from hm_messages where messageaccountid = @ACCOUNTID"));
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;

         return recordset->GetInt64Value("samplecount");
      }

      // The domain's own mail still in the DELIVERY QUEUE, dropped, and how many
      // messages that was.
      //
      // Create answers as soon as the eight messages are in the queue, so
      // pressing Make and then Remove - the obvious thing to do when trying the
      // feature out - used to leave queued copies addressed to mailboxes that
      // had just been deleted. LocalDelivery then correctly reports HM5165, "the
      // recipient account appears to have been deleted after the message was
      // received", once per copy, against whatever the server does next.
      //
      // Only a message whose EVERY recipient is one of these accounts is
      // dropped. A learner who sent from a sample mailbox to an outside address
      // has a queued message with a real recipient on it, and taking that away
      // would be destroying somebody's mail rather than tidying up the sample's.
      //
      // DeliveryQueue::Remove is what DELETE /api/v1/queue/<id> calls: it takes
      // the row and the file, and it declines a message whose row lock the
      // deliverer holds at that moment. Such a message is being delivered NOW,
      // while the accounts are still here, so it arrives. It is counted only if
      // it really went, which is why each one is read back.
      long long
      DropQueuedMail(std::shared_ptr<Domain> domain)
      {
         std::set<__int64> accountIds;
         Accounts accounts(domain->GetID());
         accounts.Refresh();
         for (int i = 0; i < accounts.GetCount(); i++)
         {
            std::shared_ptr<Account> account = accounts.GetItem(i);
            if (account)
               accountIds.insert(account->GetID());
         }

         if (accountIds.empty())
            return 0;

         // Every message that names one of them, once each. Asked per account,
         // for the reason CountMessages gives.
         std::set<__int64> candidates;
         for (__int64 accountId : accountIds)
         {
            SQLCommand command(_T("select distinct recipientmessageid as queuedid from hm_messagerecipients where recipientlocalaccountid = @ACCOUNTID"));
            command.AddParameter("@ACCOUNTID", accountId);

            std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
            if (!recordset)
               continue;

            while (!recordset->IsEOF())
            {
               candidates.insert(recordset->GetInt64Value("queuedid"));
               recordset->MoveNext();
            }
         }

         long long dropped = 0;
         for (__int64 messageId : candidates)
         {
            std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
            if (!PersistentMessage::ReadObject(message, messageId))
               continue;

            // The delivery queue alone. A delivered message lives in a mailbox
            // and goes with the account it is in.
            if (message->GetState() != Message::Delivering)
               continue;

            std::shared_ptr<MessageRecipients> recipients = message->GetRecipients();
            if (!recipients || recipients->GetCount() == 0)
               continue;

            bool oursAlone = true;
            // owned by this call: the recipients of a message this loop read from the database
            for (std::shared_ptr<MessageRecipient> recipient : recipients->GetVector())
            {
               if (!recipient || accountIds.find(recipient->GetLocalAccountID()) == accountIds.end())
               {
                  oursAlone = false;
                  break;
               }
            }

            if (!oursAlone)
               continue;

            DeliveryQueue::Remove(messageId);

            // Read back, because Remove is silent about a message it could not
            // lock, and a report that counted those would be claiming to have
            // dropped mail that is about to be delivered.
            std::shared_ptr<Message> stillThere = std::shared_ptr<Message>(new Message());
            if (!PersistentMessage::ReadObject(stillThere, messageId))
               dropped++;
         }

         return dropped;
      }

      // The accounts, the lists and the rules of a domain, counted before the
      // domain goes. After the delete there is nothing left to count, and a
      // report that says "removed the domain" and nothing else is the report
      // this row exists to replace.
      struct Tally
      {
         Tally() : accounts(0), lists(0), rules(0), messages(0) {}
         long long accounts;
         long long lists;
         long long rules;
         long long messages;
      };

      Tally
      Count(std::shared_ptr<Domain> domain)
      {
         Tally tally;

         Accounts accounts(domain->GetID());
         accounts.Refresh();
         for (int i = 0; i < accounts.GetCount(); i++)
         {
            std::shared_ptr<Account> account = accounts.GetItem(i);
            if (!account)
               continue;

            tally.accounts++;
            tally.messages += CountMessages(account->GetID());

            Rules rules(account->GetID());
            rules.Refresh();
            tally.rules += rules.GetCount();
         }

         std::shared_ptr<DistributionLists> lists = domain->GetDistributionLists();
         if (lists)
         {
            lists->Refresh();
            tally.lists = lists->GetCount();
         }

         Rules domainRules(0, domain->GetID());
         domainRules.Refresh();
         tally.rules += domainRules.GetCount();

         return tally;
      }
   }

   SampleDomain::State
   SampleDomain::Examine(std::shared_ptr<Domain> &domain)
   {
      domain.reset();

      // RefreshChecked rather than Refresh, so that "there is no example.test"
      // and "hm_domains could not be read" are different answers.
      //
      // Refresh returns void and throws DBLoad_'s bool away, and
      // Collection::DBLoad_ deliberately leaves the collection untouched when a
      // read fails - which, for a Domains built on the line above, means it stays
      // EMPTY. Asking it for example.test therefore answered "not here" for a
      // database that could not be read at all, and Remove acted on that: it
      // cleared the marker, and the sample it had just failed to see became
      // foreign for ever afterwards.
      Domains domains;
      if (!domains.RefreshChecked())
         return StateUnknown;

      std::shared_ptr<Domain> found = domains.GetItemByName(String(DomainName));
      if (!found)
         return StateAbsent;

      domain = found;

      // The marker and the domain that carries it. A marker naming a domain that
      // is gone is no marker at all, and an example.test with another id is
      // somebody else's domain whatever the marker says.
      const __int64 marker = Configuration::Instance()->GetSampleDomainId();
      return (marker != 0 && marker == found->GetID()) ? StatePresent : StateForeign;
   }

   __int64
   SampleDomain::PresentDomainId()
   {
      std::shared_ptr<Domain> domain;

      // StateUnknown answers 0 like an absent one, and that is the right answer
      // for this caller: the checklist would rather count a sample domain as an
      // ordinary one for the length of a database hiccup than leave a real
      // domain out of its count for ever.
      if (Examine(domain) != StatePresent || !domain)
         return 0;

      return domain->GetID();
   }

   AnsiString
   SampleDomain::StateJson()
   {
      std::shared_ptr<Domain> domain;
      const State state = Examine(domain);

      AnsiString json = "{\"state\":\"";
      json += state == StatePresent ? "present" : state == StateForeign ? "foreign"
            : state == StateUnknown ? "unknown" : "absent";
      json += "\",\"domain\":" + JsonString(String(DomainName));
      json += ",\"domain_id\":" + Number(state == StatePresent && domain ? domain->GetID() : 0);
      json += ",\"exists\":";
      json += domain ? "true" : "false";
      json += ",\"list\":" + JsonString(String(ListAddress));

      // No password here, and none anywhere but the creation's own answer: the
      // one the mailboxes get is minted per creation and hashed into them, and
      // this document is readable with view_configuration. A client shows this
      // flag as "it was shown when it was made" rather than leaving a blank
      // where a secret used to be.
      json += ",\"password_shown_once\":true";
      json += ",\"accounts\":[";

      for (int i = 0; i < MailboxCount; i++)
      {
         if (i > 0)
            json += ",";
         json += JsonString(String(Mailboxes[i].address));
      }

      json += "]}";
      return json;
   }

   SampleDomain::Outcome
   SampleDomain::Create(AnsiString &json, String &sentence)
   {
      json = "";
      sentence = _T("");

      std::shared_ptr<Domain> existing;
      const State state = Examine(existing);

      if (state == StateUnknown)
      {
         // Nothing is created over a table this server could not read: an
         // example.test may be sitting in it.
         sentence = String(_T("The domain table could not be read, so nothing was created. The error log says why the database is not answering."));
         json = StateJson();
         return OutcomeFailed;
      }

      if (state == StatePresent)
      {
         sentence.Format(_T("The sample domain %s is already here. Remove it first if you want a fresh one."), DomainName);
         json = StateJson();
         return OutcomeRefused;
      }

      if (state == StateForeign)
      {
         // The one refusal this feature exists to make. The regression suite's
         // own domain is example.test, and so is a domain somebody added while
         // reading RFC 2606: neither was made here and neither is removed here.
         sentence.Format(_T("A domain named %s already exists on this server and the sample did not make it, so nothing was created. Delete that domain yourself if it is not wanted."), DomainName);
         json = StateJson();
         return OutcomeRefused;
      }

      // THE PASSWORD, before anything is written: a creation that cannot mint one
      // must not leave half a domain behind. AppPassword::GenerateSecret answers
      // an empty string when RAND_bytes fails and reports that itself, because a
      // credential out of a broken generator looks exactly like a good one.
      const String password = AppPassword::GenerateSecret();
      if (password.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6977, "SampleDomain::Create",
            String(_T("The sample domain was not created: no password could be generated.")));

         sentence = String(_T("The sample domain was not created, because this server could not generate a password for its mailboxes. Nothing was changed."));
         json = StateJson();
         return OutcomeFailed;
      }

      // THE DOMAIN. A fresh Domain object, as InterfaceDomains::Add makes one,
      // so that every default a domain made in the Control Panel gets is the
      // default here: the plus-addressing character, the signature settings, the
      // limits switched off, no domain alias.
      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
      domain->SetName(String(DomainName));
      domain->SetIsActive(true);
      domain->SetPostmaster(String(Mailboxes[0].address));

      String saveError;
      if (!PersistentDomain::SaveObject(domain, saveError, PersistenceModeNormal) || domain->GetID() == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6970, "SampleDomain::Create",
            String(_T("The sample domain could not be created: ")) + (saveError.IsEmpty() ? String(_T("the row could not be written.")) : saveError));

         sentence = saveError.IsEmpty()
            ? String(_T("The sample domain could not be created; the error log says why."))
            : saveError;

         return OutcomeFailed;
      }

      // The marker, written as soon as there is an id to write. Before the
      // accounts, deliberately: if the accounts fail the domain is still the
      // sample's, and the removal can take it away.
      Configuration::Instance()->SetSampleDomainId(domain->GetID());

      const int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      const String hashedPassword = Crypt::Instance()->EnCrypt(password, (Crypt::EncryptionType) preferredHashAlgorithm);

      int madeAccounts = 0;
      for (int i = 0; i < MailboxCount; i++)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         account->SetDomainID(domain->GetID());
         account->SetAddress(String(Mailboxes[i].address));
         account->SetPassword(hashedPassword);
         account->SetPasswordEncryption(preferredHashAlgorithm);
         account->SetPersonFirstName(String(Mailboxes[i].first));
         account->SetPersonLastName(String(Mailboxes[i].last));
         account->SetActive(true);
         account->SetAdminLevel(Account::NormalUser);

         String accountError;

         // createInbox true. Without it the row is written, no INBOX is, and
         // every message addressed to the account is accepted and silently lost -
         // the defect HandleCreateAccount_ carries a paragraph about.
         if (!PersistentAccount::SaveObject(account, accountError, true, PersistenceModeNormal))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6971, "SampleDomain::Create",
               String(_T("A sample account could not be created: ")) + String(Mailboxes[i].address) + _T(" ") + accountError);
            continue;
         }

         madeAccounts++;
      }

      // THE LIST. Public, so that anybody can post to it: a learner who has to
      // authenticate before the list does anything has learnt about the list's
      // security model rather than about lists.
      int listMembers = 0;
      std::shared_ptr<DistributionList> list = std::shared_ptr<DistributionList>(new DistributionList());
      list->SetDomainID(domain->GetID());
      list->SetAddress(String(ListAddress));
      list->SetActive(true);
      list->SetRequireAuth(false);
      list->SetListMode(DistributionList::LMPublic);

      String listError;
      if (!PersistentDistributionList::SaveObject(list, listError, PersistenceModeNormal) || list->GetID() == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6972, "SampleDomain::Create",
            String(_T("The sample distribution list could not be created: ")) + listError);
      }
      else
      {
         for (int i = 0; i < MailboxCount; i++)
         {
            std::shared_ptr<DistributionListRecipient> member = std::shared_ptr<DistributionListRecipient>(new DistributionListRecipient());
            member->SetListID(list->GetID());
            member->SetAddress(String(Mailboxes[i].address));

            String memberError;
            if (PersistentDistributionListRecipient::SaveObject(member, memberError, PersistenceModeNormal))
               listMembers++;
         }
      }

      // THE RULE, on the first mailbox: a subject carrying the word Invoice is
      // filed into a folder of that name. The folder is created by the rule the
      // first time it runs, which is why the sample sends an invoice.
      bool madeRule = false;
      std::shared_ptr<Account> ruleAccount = std::shared_ptr<Account>(new Account());
      if (PersistentAccount::ReadObject(ruleAccount, String(Mailboxes[0].address)) && ruleAccount->GetID() > 0)
      {
         std::shared_ptr<Rule> rule = std::shared_ptr<Rule>(new Rule());
         rule->SetAccountID(ruleAccount->GetID());
         rule->SetDomainID(0);
         rule->SetName(String(RuleName));
         rule->SetActive(true);
         rule->SetUseAND(true);
         rule->SetSortOrder(1);

         std::shared_ptr<RuleCriteria> criteria = std::shared_ptr<RuleCriteria>(new RuleCriteria());
         criteria->SetUsePredefined(true);
         criteria->SetPredefinedField(RuleCriteria::FTSubject);
         criteria->SetMatchType(RuleCriteria::Contains);
         criteria->SetMatchValue(String(RuleSubjectWord));
         rule->GetCriterias()->AddItem(criteria);

         std::shared_ptr<RuleAction> action = std::shared_ptr<RuleAction>(new RuleAction());
         action->SetType(RuleAction::MoveToIMAPFolder);
         action->SetIMAPFolder(String(RuleFolder));
         action->SetSortOrder(1);
         rule->GetActions()->AddItem(action);

         String ruleError;
         if (PersistentRule::SaveObject(rule, ruleError, PersistenceModeNormal))
            madeRule = true;
         else
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6973, "SampleDomain::Create",
               String(_T("The sample rule could not be created: ")) + ruleError);
      }

      // THE MAIL. Queued in order, so that the two replies follow the messages
      // they answer; the deliverer takes them in the order they were written.
      int queued = 0;
      int deliveries = 0;
      String previousId;
      for (int i = 0; i < MessageCount; i++)
      {
         const String messageId = Utilities::GenerateMessageID();

         int recipients = 0;
         if (Queue(SampleMessages[i], messageId, previousId, recipients))
         {
            queued++;
            deliveries += recipients;
         }

         previousId = messageId;
      }

      // The deliverer, where there is one. With none - the command line beside a
      // stopped server - the messages wait in the queue and the next pass of a
      // running server's deliverer picks them up within the minute, which is why
      // this is not an error.
      if (Application::Instance()->GetSMTPDeliveryManager())
         Application::Instance()->SubmitPendingEmail();

      String logLine;
      logLine.Format(_T("SampleDomain: %s created with %d accounts, a list of %d, %d rule(s) and %d queued message(s)."),
         DomainName, madeAccounts, listMembers, madeRule ? 1 : 0, queued);
      LOG_APPLICATION(logLine);

      json = "{\"created\":true";
      json += ",\"domain\":" + JsonString(String(DomainName));
      json += ",\"domain_id\":" + Number(domain->GetID());

      // The only time anything says it. Nothing kept the clear text, so a second
      // read of the sample cannot answer this key at all.
      json += ",\"password\":" + JsonString(password);
      json += ",\"accounts\":[";
      for (int i = 0; i < MailboxCount; i++)
      {
         if (i > 0)
            json += ",";
         json += "{\"address\":" + JsonString(String(Mailboxes[i].address));
         json += ",\"name\":" + JsonString(String(Mailboxes[i].first) + _T(" ") + String(Mailboxes[i].last)) + "}";
      }
      json += "],\"accounts_created\":" + Number(madeAccounts);
      json += ",\"list\":" + JsonString(String(ListAddress));
      json += ",\"list_members\":" + Number(listMembers);
      json += ",\"rule\":" + JsonString(String(RuleName));
      json += ",\"rule_account\":" + JsonString(String(Mailboxes[0].address));
      json += ",\"rule_folder\":" + JsonString(String(RuleFolder));
      json += ",\"rules\":" + Number(madeRule ? 1 : 0);
      json += ",\"messages\":" + Number(queued);
      json += ",\"deliveries\":" + Number(deliveries);

      // The password is in the sentence as well as in the report, because every
      // surface that offers the creation shows the sentence - the API's message,
      // the COM document, both command lines, the Control Panel's dialog and the
      // Deck's toast - and this is the one moment any of them can show it.
      sentence.Format(_T("%s was created with %d mailboxes, the list %s and %d messages on their way into it. All of the mailboxes sign in with the password %s, which is shown here once and is not stored anywhere."),
         DomainName, madeAccounts, ListAddress, queued, password.c_str());

      // The same sentence in the document, so that a client with no words of its
      // own for this has one to show and the command line and the API say the
      // same thing.
      json += ",\"message\":" + JsonString(sentence);
      json += "}";

      return OutcomeDone;
   }

   SampleDomain::Outcome
   SampleDomain::Remove(AnsiString &json, String &sentence)
   {
      json = "";
      sentence = _T("");

      std::shared_ptr<Domain> domain;
      const State state = Examine(domain);

      if (state == StateUnknown)
      {
         // The one answer that must NOT be confused with "it has already gone".
         // Clearing the marker here would orphan a sample that is still there:
         // the next call would find example.test with no marker, call it foreign,
         // and refuse to touch it for ever.
         sentence = String(_T("The domain table could not be read, so nothing was removed and nothing was changed. The error log says why the database is not answering."));
         json = StateJson();
         return OutcomeFailed;
      }

      if (state == StateForeign)
      {
         sentence.Format(_T("The domain %s on this server was not made by the sample, so it was not removed. Delete it yourself if it is not wanted."), DomainName);
         json = StateJson();
         return OutcomeRefused;
      }

      if (state == StateAbsent)
      {
         // A marker whose domain has gone - deleted by hand, or by a regression
         // run - is cleared here rather than left to name a row that is not
         // there. Saying so is the honest answer: nothing was removed now,
         // because something removed it already.
         const bool hadMarker = Configuration::Instance()->GetSampleDomainId() != 0;
         if (hadMarker)
            Configuration::Instance()->SetSampleDomainId(0);

         sentence = hadMarker
            ? String(_T("The sample domain had already been deleted by hand; the record of it has been cleared and nothing was removed."))
            : String(_T("There is no sample domain on this server, so nothing was removed."));

         json = StateJson();
         return OutcomeRefused;
      }

      const __int64 domainId = domain->GetID();
      const Tally tally = Count(domain);

      // Before the delete, and only before it: after it the accounts are gone
      // and there is no way left to tell the sample's queued mail from anybody
      // else's.
      const long long queued = DropQueuedMail(domain);

      // Through the collection, as HandleDeleteDomain_ and InterfaceDomain::Delete
      // go: PersistentDomain::DeleteObject takes the accounts with their messages
      // and their folders, the aliases, the distribution lists and the domain
      // aliases, rewrites whatever another domain pointed at any of those names,
      // drops the domain from the cache, and removes its data and Sieve
      // directories.
      Domains domains;
      domains.Refresh();

      if (!domains.DeleteItemByDBID(domainId))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6976, "SampleDomain::Remove",
            String(_T("The sample domain could not be removed: ")) + String(DomainName));

         // The queued mail went first and cannot be put back, so the sentence
         // says so rather than claiming nothing was changed. The domain and
         // everything in it are still here; only mail that was on its way
         // between its own mailboxes has gone.
         if (queued > 0)
            sentence.Format(_T("The sample domain could not be removed; the error log says why. It is still here, but %I64d message(s) of its own that were still in the delivery queue had already been dropped."), queued);
         else
            sentence = String(_T("The sample domain could not be removed; the error log says why. Nothing was changed."));

         json = StateJson();
         return OutcomeFailed;
      }

      Configuration::Instance()->SetSampleDomainId(0);

      String logLine;
      logLine.Format(_T("SampleDomain: %s removed with %I64d accounts, %I64d list(s), %I64d rule(s), %I64d message(s) and %I64d still in the delivery queue."),
         DomainName, tally.accounts, tally.lists, tally.rules, tally.messages, queued);
      LOG_APPLICATION(logLine);

      json = "{\"removed\":true";
      json += ",\"domain\":" + JsonString(String(DomainName));
      json += ",\"domain_id\":" + Number(domainId);
      json += ",\"accounts\":" + Number(tally.accounts);
      json += ",\"lists\":" + Number(tally.lists);
      json += ",\"rules\":" + Number(tally.rules);
      json += ",\"messages\":" + Number(tally.messages);
      json += ",\"queued\":" + Number(queued);

      sentence.Format(_T("%s was removed: %I64d mailboxes, %I64d distribution list(s), %I64d rule(s) and %I64d message(s) went with it."),
         DomainName, tally.accounts, tally.lists, tally.rules, tally.messages);

      // Said only when there was some, because "and 0 still in the queue" is a
      // sentence about nothing.
      if (queued > 0)
      {
         String alsoQueued;
         alsoQueued.Format(_T(" %I64d message(s) still in the delivery queue for it were dropped rather than left addressed to mailboxes that have gone."), queued);
         sentence += alsoQueued;
      }

      json += ",\"message\":" + JsonString(sentence);
      json += "}";

      return OutcomeDone;
   }
}
