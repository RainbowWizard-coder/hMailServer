// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "FetchAccount.h"
#include "FetchAccountUIDs.h"
#include "FetchAccountUID.h"
#include "../Util/Crypt.h"
#include "../Util/XmlTree.h"

#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{


   FetchAccount::FetchAccount(void) :
      account_id_(0),
      port_(0),
      minutes_(30),
      days_to_keep_(0),
      is_active_(1),
      process_mimerecipients_(false),
      process_mimedate_(false),
      server_type_(POP3),
      use_anti_spam_(false),
      use_anti_virus_(false),
      enable_route_recipients_(false),
      mirror_folders_(false),
      origin_(OriginServerAdministrator),
      connection_security_(CSNone)
   {
 
   }

   FetchAccount::~FetchAccount(void)
   {

   }

   int
   FetchAccount::DaysToKeepStoredBefore6106(ServerType serverType, Origin origin, int stored)
   {
      if (serverType == IMAP)
      {
         if (stored == 0)
            return -1;

         if (stored < 0)
            return 0;

         return stored;
      }

      if (origin != OriginServerAdministrator && stored < 0)
         return 0;

      return stored;
   }

   FetchAccount::Origin
   FetchAccount::OriginFromNumber(long value)
   {
      switch (value)
      {
      case OriginServerAdministrator:
         return OriginServerAdministrator;
      case OriginWithoutServerAuthority:
         return OriginWithoutServerAuthority;
      default:
         // 1, or a value this build does not know: the strictest.
         return OriginReader;
      }
   }

   bool
   FetchAccount::CredentialsAreOneLine() const
   {
      for (const String *value : { &username_, &password_ })
      {
         if (value->FindOneOf(_T("\r\n")) != -1)
            return false;
         if ((size_t) value->GetLength() != wcslen(value->c_str()))
            return false;
      }

      return true;
   }

   std::shared_ptr<FetchAccountUIDs>
   FetchAccount::GetUIDs()
   {
      if (!uids_)
      {
         uids_ = std::shared_ptr<FetchAccountUIDs> (new FetchAccountUIDs);
         uids_->Refresh(id_);
      }

      return uids_;
   }

   bool 
   FetchAccount::XMLStore(XNode *pRuleNode, int iOptions)
   {
      XNode *pNode = pRuleNode->AppendChild(_T("FetchAccount"));

      pNode->AppendAttr(_T("Name"), name_);
      pNode->AppendAttr(_T("ServerAddress"), server_address_);
      pNode->AppendAttr(_T("ServerType"), StringParser::IntToString(server_type_));
      // port_ is a long. MSVC binds IntToString(long) to the int overload, because
      // long and int are the same 32-bit type on Windows; clang finds int, unsigned
      // int and __int64 all equally distant and cannot choose. The cast names the
      // overload the Windows build already selects, and no port value is lost.
      pNode->AppendAttr(_T("Port"), StringParser::IntToString((int) port_));
      pNode->AppendAttr(_T("Username"), username_);
      pNode->AppendAttr(_T("Password"), Crypt::Instance()->EnCrypt(password_, Crypt::ETBlowFish));
      pNode->AppendAttr(_T("Minutes"), StringParser::IntToString(minutes_));
      pNode->AppendAttr(_T("DaysToKeep"), StringParser::IntToString(days_to_keep_));
      // DaysToKeep in the one meaning FetchAccount.h states; an archive without
      // this was written before 6.3.6 and is read through DaysToKeepStoredBefore6106.
      pNode->AppendAttr(_T("DaysToKeepSchema"), _T("6106"));
      pNode->AppendAttr(_T("Active"), is_active_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("MIMERecipientHeaders"), mime_recipient_headers_);
      pNode->AppendAttr(_T("ProcessMIMERecipients"), process_mimerecipients_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("ProcessMIMEDate"), process_mimedate_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("UseAntiSpam"), use_anti_spam_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("UseAntiVirus"), use_anti_virus_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("EnableRouteRecipients"), enable_route_recipients_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("MirrorFolders"), mirror_folders_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("OAuthProvider"), oauth_provider_);
      pNode->AppendAttr(_T("OAuthToken"), oauth_token_.IsEmpty() ? String() : Crypt::Instance()->EnCrypt(oauth_token_, Crypt::ETBlowFish));
      pNode->AppendAttr(_T("MirrorSkip"), mirror_skip_);
      pNode->AppendAttr(_T("MirrorSeen"), mirror_seen_);
      pNode->AppendAttr(_T("Origin"), StringParser::IntToString((int) origin_));
      pNode->AppendAttr(_T("ConnectionSecurity"), StringParser::IntToString(connection_security_));

      if (!GetUIDs()->XMLStore(pNode, iOptions))
         return false;

      return true;
   }

   bool
   FetchAccount::XMLLoad(XNode *pNode, int iOptions)
   {
      name_ = pNode->GetAttrValue(_T("Name"));
      server_address_ = pNode->GetAttrValue(_T("ServerAddress"));
      server_type_ = (ServerType)_ttoi(pNode->GetAttrValue(_T("ServerType")));
      port_ =  _ttoi(pNode->GetAttrValue(_T("Port")));
      username_ = pNode->GetAttrValue(_T("Username"));
      password_ = Crypt::Instance()->DeCrypt(pNode->GetAttrValue(_T("Password")), Crypt::ETBlowFish);
      minutes_ = _ttoi(pNode->GetAttrValue(_T("Minutes")));
      days_to_keep_ = _ttoi(pNode->GetAttrValue(_T("DaysToKeep")));
      is_active_ = (pNode->GetAttrValue(_T("Active")) == _T("1"));
      mime_recipient_headers_ = pNode->GetAttrValue(_T("MIMERecipientHeaders"));
      process_mimerecipients_ = (pNode->GetAttrValue(_T("ProcessMIMERecipients")) == _T("1"));
      process_mimedate_ = (pNode->GetAttrValue(_T("ProcessMIMEDate")) == _T("1"));
      use_anti_spam_ = (pNode->GetAttrValue(_T("UseAntiSpam")) == _T("1"));
      use_anti_virus_ = (pNode->GetAttrValue(_T("UseAntiVirus")) == _T("1"));
      enable_route_recipients_ = (pNode->GetAttrValue(_T("EnableRouteRecipients")) == _T("1"));
      mirror_folders_ = (pNode->GetAttrValue(_T("MirrorFolders")) == _T("1"));
      oauth_provider_ = pNode->GetAttrValue(_T("OAuthProvider"));
      const String protectedToken = pNode->GetAttrValue(_T("OAuthToken"));
      oauth_token_ = protectedToken.IsEmpty() ? String() : Crypt::Instance()->DeCrypt(protectedToken, Crypt::ETBlowFish);
      mirror_skip_ = pNode->GetAttrValue(_T("MirrorSkip"));
      mirror_seen_ = pNode->GetAttrValue(_T("MirrorSeen"));
      // An archive written before 6104 has no such attribute: the account is
      // restored as it was judged then, an administrator's.
      origin_ = OriginFromNumber(_ttoi(pNode->GetAttrValue(_T("Origin"))));

      // Written before 6.3.6, when the IMAP fetcher, the webmail and the REST API
      // read 0 and -1 the other way round (FetchAccount.h).
      if (pNode->GetAttrValue(_T("DaysToKeepSchema")).IsEmpty())
         days_to_keep_ = DaysToKeepStoredBefore6106(server_type_, origin_, days_to_keep_);

      // Backwards compatibiltiy
      if (pNode->GetAttrValue(_T("UseSSL")) == _T("1"))
      {
         connection_security_ = CSSSL;
      }
      else
      {
         connection_security_ = (ConnectionSecurity) _ttoi(pNode->GetAttrValue(_T("ConnectionSecurity")));
      }

      return true;
   }

   bool 
   FetchAccount::XMLLoadSubItems(XNode *pNode, int iRestoreOptions)
   {
      if (!GetUIDs()->XMLLoad(pNode, iRestoreOptions))
         return false;

      return true;
   }
   namespace
   {
      void ExpectDays(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("DaysToKeepMessages: ") + what);
      }

      // An archive's FetchAccount element with the attributes the rule reads, and
      // DaysToKeepSchema when the archive was written by 6.3.6 or later.
      int DaysAfterLoading(const wchar_t *serverType, const wchar_t *origin, const wchar_t *days, bool writtenSince6106)
      {
         XNode node;
         node.name = _T("FetchAccount");
         node.AppendAttr(_T("Name"), _T("Archived"));
         node.AppendAttr(_T("ServerType"), serverType);
         node.AppendAttr(_T("DaysToKeep"), days);
         node.AppendAttr(_T("Origin"), origin);
         if (writtenSince6106)
            node.AppendAttr(_T("DaysToKeepSchema"), _T("6106"));

         FetchAccount account;
         if (!account.XMLLoad(&node, 0))
            throw std::logic_error("DaysToKeepMessages: an archived fetch account did not load");

         return account.GetDaysToKeep();
      }
   }

   void
   FetchAccountTester::Test()
   {
      // The one meaning: a negative number deletes in the session that collects, 0
      // never deletes, N deletes once the message has been on record more than N days.
      ExpectDays(FetchAccount::DeletesOnCollection(-1) && FetchAccount::DeletesOnCollection(-30), "a negative number does not delete on collection");
      ExpectDays(!FetchAccount::DeletesOnCollection(0) && !FetchAccount::DeletesOnCollection(7), "0 or a number of days deletes on collection");
      ExpectDays(FetchAccount::RemoteCopyDue(-1, 0.0), "-1 leaves a collected message on the remote server");
      ExpectDays(!FetchAccount::RemoteCopyDue(0, 0.0) && !FetchAccount::RemoteCopyDue(0, 100000.0), "0 deletes a message, however long it has been on record");
      ExpectDays(!FetchAccount::RemoteCopyDue(7, 6.9) && !FetchAccount::RemoteCopyDue(7, 7.0), "7 deletes a message on record for 7 days or less");
      ExpectDays(FetchAccount::RemoteCopyDue(7, 7.5), "7 leaves a message on record for more than 7 days");

      // Stored before 6106: an IMAP account's 0 was "delete" and its negative number
      // "keep"; a POP3 account's -1 was "delete", except where the webmail or the REST
      // API - which described it as "keep" - made or moved the account.
      const FetchAccount::Origin administrator = FetchAccount::OriginServerAdministrator;
      const FetchAccount::Origin reader = FetchAccount::OriginReader;
      const FetchAccount::Origin restricted = FetchAccount::OriginWithoutServerAuthority;
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::IMAP, administrator, 0) == -1, "an IMAP account's 0 does not go on deleting");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::IMAP, reader, -1) == 0, "an IMAP account's -1 does not go on keeping");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::IMAP, administrator, -5) == 0, "an IMAP account's -5 does not go on keeping");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::IMAP, restricted, 7) == 7, "an IMAP account's 7 days changed");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, administrator, -1) == -1, "an administrator's POP3 -1 changed");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, administrator, 0) == 0, "an administrator's POP3 0 changed");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, administrator, 7) == 7, "an administrator's POP3 7 days changed");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, reader, -1) == 0, "a reader's POP3 -1, written as keep, goes on deleting");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, restricted, -1) == 0, "a restricted key's POP3 -1, written as keep, goes on deleting");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, reader, 0) == 0, "a reader's POP3 0 starts to be deleted");
      ExpectDays(FetchAccount::DaysToKeepStoredBefore6106(FetchAccount::POP3, reader, 30) == 30, "a reader's POP3 30 days changed");

      // An archive: written before 6.3.6, read through the rule; written since, as it is.
      ExpectDays(DaysAfterLoading(_T("1"), _T("0"), _T("0"), false) == -1, "an IMAP account archived at 0 before 6.3.6 does not go on deleting");
      ExpectDays(DaysAfterLoading(_T("1"), _T("1"), _T("-1"), false) == 0, "an IMAP account archived at -1 before 6.3.6 does not go on keeping");
      ExpectDays(DaysAfterLoading(_T("0"), _T("1"), _T("-1"), false) == 0, "a reader's POP3 account archived at -1 before 6.3.6 goes on deleting");
      ExpectDays(DaysAfterLoading(_T("0"), _T("0"), _T("-1"), false) == -1, "an administrator's POP3 account archived at -1 changed");
      ExpectDays(DaysAfterLoading(_T("1"), _T("0"), _T("0"), true) == 0, "an IMAP account archived at 0 by 6.3.6 was translated again");
      ExpectDays(DaysAfterLoading(_T("1"), _T("0"), _T("-1"), true) == -1, "an IMAP account archived at -1 by 6.3.6 was translated again");
      ExpectDays(DaysAfterLoading(_T("0"), _T("1"), _T("-1"), true) == -1, "a reader's POP3 account archived at -1 by 6.3.6 was translated again");
   }
}