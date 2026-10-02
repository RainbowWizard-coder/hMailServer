// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Common/Application/RoleManager.h"
#include "../Common/Util/AuditTrail.h"

namespace HM
{
   class Account;

   class COMAuthentication
   {
   public:
      COMAuthentication(void);
      ~COMAuthentication(void);

      std::shared_ptr<const Account> Authenticate(const String &sUsername, const String &sPassword);

      // As above, with a TOTP code. This is the ONLY path that can satisfy a second
      // factor: a mail client has nowhere to type one, which is precisely why an
      // account with a factor enrolled authenticates there through an app password
      // instead. An empty code is not a shortcut - an enrolled account still needs a
      // valid one.
      std::shared_ptr<const HM::Account> Authenticate(const String &sUsername, const String &sPassword, const String &sCode);

      void AttempAnonymousAuthentication();

      bool GetIsAuthenticated() const;

      // Whether the calling thread is running one of the server's own event
      // scripts (ScriptServer::IsScriptRunningOnThisThread). A credential that
      // never authenticated is held by one of two callers: that script host,
      // whose objects - the message, account and fetch account an event hands
      // it, and any it creates - carry no credential; or a COM client in another
      // process that never authenticated, or whose Application failed a later
      // Authenticate and so dropped the credential every object it handed out
      // shares. With an administrator password set, the second may be any
      // local user. Only the first is given what an unauthenticated object
      // could do before 6.3.6, and this tells them apart: a script's calls on
      // an hMailServer object run on the thread running the script, and a
      // client's on one of COM's own threads, which never runs one.
      static bool CallerIsEventScript();

      // The credential of the calling process. A program outside the service
      // signs in on an Application and then, as scripts always have, creates
      // the message it sends itself - Set m = CreateObject("hMailServer.Message")
      // - or a FetchAccount. Such an object makes a credential of its own that
      // never signs in, and since 6.3.6 that is refused outside an event script
      // (CallerIsEventScript). Marked by AnswerForCallingProcess, it now answers
      // every question below, call by call, with the credential of an
      // Application that its COM client process holds and has signed in on -
      // the most privileged of them where the process holds several, the latest
      // signed in among equals - and with none once that Application is
      // released or a later Authenticate on it fails. Nothing is granted that
      // the process does not hold already: it can ask the very same credential
      // for Application.CreateMessage, or reach the account's FetchAccounts.
      //
      // The process is the one the call came from over local RPC, COM's
      // transport between processes of one machine: the id that RPC has from
      // the kernel with the call, and the creation time that id has now, the
      // same as when the Application signed in - so a process given the id of
      // one that ended while its Application was still held (COM lets go of an
      // abandoned object minutes later) is not taken for it. A call that did
      // not come over local RPC - from another machine, or from inside the
      // service - answers for no process.
      static void SignedInFromCallingProcess(const void *application, const std::shared_ptr<COMAuthentication> &credential);
      static void ForgetSignIn(const void *application);
      void AnswerForCallingProcess() { answers_for_calling_process_ = true; }

      // Whether mail this credential queues may claim to be from anybody: the
      // server administrator's may, and so may an event script's objects,
      // which carry no credential. Every other credential is held to the
      // addresses it may send as (MaySendAs).
      bool SendsAsAnyAddress() const;

      // Whether mail this credential queues may claim to be from address: the
      // rule authenticated SMTP applies to MAIL FROM when
      // SmtpAuthenticatedSenderCheck is on, and the webmail's send route always
      // (SMTPConnection::SenderPermittedFor - the account's own address, an
      // alias that resolves to it, a mailbox whose owner granted it the post
      // right on their INBOX, an address it verified or a masked address of
      // its own; the null sender), and for a domain's administrator any
      // address of a domain this server holds that it administers, save an
      // account there whose authority it does not take in (TakesInAccount).
      // Reason says why not, for the log.
      bool MaySendAs(const String &address, String &reason) const;

      // The two questions this API has asked since 2007, and the ~315 places
      // in COM/Interface*.cpp that ask them, are now questions for
      // RoleManager: AdministerServer and AdministerDomain are permissions
      // like any other, held by serveradmin and domainadmin and by nothing
      // else. An account with no role assignment resolves to the grant its
      // AdminLevel has always given it, so every one of those call sites
      // answers exactly what it answered before - and a help desk or an
      // auditor, which hold neither permission, is refused every write in COM
      // without a single one of them being edited.
      bool GetIsDomainAdmin() const;
      bool GetIsServerAdmin() const;

      // The question the new roles need asked: may this credential do this
      // particular thing, here. Used at the few COM entry points a narrow role
      // reaches - a password reset, an unlock, a message trace - and refused
      // by build/check-role-choke-point.py anywhere that is not one of them.
      bool Allows(RolePermission permission) const;
      bool AllowsForDomain(RolePermission permission, const String &domainName) const;

      // Whether this credential reaches every domain rather than a list of them.
      bool ReachesEveryDomain() const;

      // Whether this credential's authority takes in the stored account's
      // (RoleManager::Encompasses): the rule Account.Save and MoveTo apply,
      // asked where a credential of the account's is issued or taken back
      // with no Account.Save to pass through - its app passwords. False for an
      // account that cannot be read.
      bool TakesInAccount(__int64 accountID) const;

      // THE rule for a COM write to what is IN an account - its fetch
      // accounts, its rules and their actions and criteria, its folders and
      // their grants, its messages, its recovery, its devices - asked where
      // the write is made, not where the object was reached, because an object
      // can be pointed elsewhere (a rule's AccountID, an action's RuleID) after
      // it was reached. True when the credential reaches the account as
      // Links.Account decides - the server administrator, an administrator of
      // its domain, or the account itself - and takes in its authority
      // (TakesInAccount). An account id of 0 is no account's (a public
      // folder, a queued message): what reaches it is gated where it is
      // reached. An account that is gone is the server administrator's alone
      // to tidy. An unauthenticated holder is answered yes on an event
      // script's thread, as Account.Save leaves it, as its calling process's
      // credential answers when it takes that (SignedInFromCallingProcess),
      // and no anywhere else (CallerIsEventScript): until 6.3.6 it was yes
      // for every one, so a client that created a FetchAccount or a Message
      // itself, or made its Application fail an Authenticate, changed any
      // account's content. The same holds for the three questions below.
      bool MayChangeAccount(__int64 accountID) const;

      // A rule's owner, by the rule's AccountID and DomainID: an account's
      // (MayChangeAccount), a domain's (the server administrator or that
      // domain's administrator) or the server's (the server administrator).
      bool MayChangeRuleOwnedBy(__int64 accountID, __int64 domainID) const;

      // The owner of the stored rule ruleID; true for 0, a rule not yet saved,
      // whose own Save asks MayChangeRuleOwnedBy.
      bool MayChangeRule(__int64 ruleID) const;

      // The owner of the stored folder folderID (MayChangeAccount of its
      // account; a public folder is no account's).
      bool MayChangeFolder(__int64 folderID) const;

      // What the COM writes answer when the rule above says no.
      static const wchar_t *const NotTakenIn;

      // What this credential holds, for the actor the audit trail records and
      // for the Control Panel, which hides a page a role may not use.
      RoleGrant GetGrant() const;

      // Who is acting, as RoleManager holds it. Installed beside the audit
      // trail's actor below, at the same moments and from the same grant, so
      // that the two cannot describe two different people.
      RoleActor GetActor() const;

      // The audit trail's identity chokepoint for COM. GetIsServerAdmin,
      // GetIsDomainAdmin and the three role questions above are the
      // authorisation questions every COM write asks before it writes - each
      // of them installs this actor, and RoleManager's, through InstallActor_ -
      // and they are asked ON THE THREAD that is about to do the writing -
      // which matters, because hMailServer's COM server is an out-of-process
      // MTA and RPC dispatches each call on whichever pool thread is free. An
      // actor installed when the client authenticated would be on a thread
      // that never writes anything.
      //
      // The COM methods that write without asking - the settings property
      // setters, which are reachable only once LoadSettings has asked once -
      // carry an AuditScope of their own built from this; see
      // COMAuthenticator::GetAuditActor and build/check-audit-choke-point.py.
      //
      // Its kind and its role come from the grant, never from AdminLevel alone:
      // an account whose AdminLevel says domain administrator but which has
      // been given the help desk role is recorded as the account it is,
      // holding the role it holds - not as an administrator.
      AuditTrail::Actor GetAuditActor() const;

      __int64 GetAccountID() const;
      __int64 GetDomainID() const;

      int GetAccessDenied() const;

   private:

      // The administrator credential: the ini password, and - when one is enrolled
      // (IniFileSettings::GetAdministratorTotpSecret) - a TOTP code. codePresented
      // says which overload the caller came through: without a code, an enrolled
      // administrator is refused outright, so a stolen password is not a
      // credential on its own. Sets account_ on success.
      void AuthenticateAdministrator_(const String &sPassword, const String &sCode, bool codePresented);

      // The one-time code of an owner - an account's address, or
      // "administrator" - whose password has just been accepted: refused while
      // the owner's wrong-code budget is spent, refused and counted when it is
      // wrong, refused and not counted when it has been taken once already, and
      // taken otherwise (SecondFactorGuard, Totp.h, which the REST API shares).
      // Logs each refusal.
      static bool AcceptCode_(const String &owner, const String &secret, const String &sCode);

      void ResolveGrant_();
      void InstallActor_() const;

      // GetActor and GetAuditActor of this credential's own sign-in, never
      // its calling process's: what those answer when this credential answers
      // for itself. A question it answers with its calling process's
      // credential is asked of that one before any actor is installed, and
      // that one installs its own.
      RoleActor OwnActor_() const;
      AuditTrail::Actor OwnAuditActor_() const;

      // The credential this one answers with for the call being made: that of
      // its calling process (SignedInFromCallingProcess) while it is marked
      // to and holds none of its own and the caller is no event script; null
      // otherwise, when it answers for itself.
      std::shared_ptr<const COMAuthentication> CallingProcessCredential_() const;

      // How much a credential that signed in may do, for choosing among the
      // ones a process holds: the server administrator 3, a domain
      // administrator 2, any other 1.
      int Rank_() const;

      // An account's credential that matched one of its app passwords rather
      // than its own password is not a COM logon: account_ is left empty. See
      // Authenticate.
      void RefuseAppPassword_(bool viaAppPassword);

      std::shared_ptr<const Account> account_;

      // Resolved once, when the credential was accepted, and read from there.
      // A role edited half way through a COM session does not change what the
      // session may do until it authenticates again - the same rule the REST
      // caller follows, for the same reason.
      RoleGrant grant_;

      // Set on the credential a directly created Message or FetchAccount makes
      // for itself (AnswerForCallingProcess), and on no other.
      bool answers_for_calling_process_ = false;
   };
}