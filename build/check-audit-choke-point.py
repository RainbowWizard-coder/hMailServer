#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every administrative change reaches the audit trail, and no write route skips it.

An audit trail with a hole in it is worse than none: it reads as a complete
record of who changed what, and the one change somebody went out of their way to
make is the one it does not have. The hole is never deliberate - it is a new COM
setter, or a new REST route, written by somebody who had no reason to know this
class existed. So it fails here, with the file and the line, rather than being
discovered by a regulator.

Nine rules, in the shape of build/check-authz-choke-point.py:

  recorded    AuditTrail::RecordStatement is called from ONE file - the
              statement chokepoint in the DAL - and ::RecordSettingChange from
              the two settings chokepoints, PropertySet for hm_settings and
              IniSettingStore for hm_inisettings. Another caller means one
              change recorded twice, or recorded in a place that does not know
              what the change was. ::RecordEvent, for a change that is neither a
              row nor a setting, is called only where such a change is made -
              the five ways a lockout is lifted (an account unlocked over REST
              and over COM, a POP3 mailbox lock cleared over COM, and the
              logon-failure list cleared over REST and over COM); the two ways
              a restore is asked for (Backup.StartRestore over COM and POST
              /api/v1/backup/restore), where what is recorded is the request,
              because the restore itself runs on the maintenance work queue
              with no actor on it; the two ways an administrator stops Web
              Push on every device of an account, whose rows are the person's
              own and not audited; and by
              MailboxMove, which is the other shape: a change that IS rows, so
              many of them and so plainly one act that recording each would
              bury it. It writes them with no actor on the thread and records
              the move once, naming both addresses; and by Profiles, the same
              shape the other way round: a profile applied or reverted is thirty
              settings changed as one act, each recorded by the settings
              chokepoint and the act recorded once with its counts; and by the
              eDiscovery export's download, which writes no row and is the act
              that takes the custodians' mail off the server; the two
              credential stores that live in ini files, not audited tables - an
              API key created or revoked over REST, and the administrator's
              second factor enrolled or disabled over REST and over COM; and by
              RestApiServer::RecordMailRead_, for somebody's mail read through
              an administrator's route - an account's folders and messages, a
              message's header fields and its file, a backup's listing of a
              mailbox, the two exports (one that stopped part of the way
              too), a held message's headers and text, the archive index
              searched and one of its entries read, an eDiscovery search's
              hits - which writes nothing at all, and for which a support
              session opened is the row beside it. RecordMailRead_ is called
              only from the files that answer those routes. Every one of these
              calls must be there.

  installed   The actor - who is making the change - is installed at one place
              per interface: RestApiServer::ProcessRequest_ for REST and the
              Deck, COMAuthentication::InstallActor_ for COM and so for the
              Control Panel - reached from every authorisation question COM
              asks: GetIsServerAdmin, GetIsDomainAdmin, and the Allows and
              AllowsForDomain questions a narrow role's entry points ask.
              AuditTrail::SetActor is named nowhere else, because an actor set
              anywhere else is a change attributed to whoever happened to be
              there before.

  com         Every COM method that writes either asks one of those
              authorisation questions - which installs the actor on the thread
              that is about to do the writing - or carries an AuditScope of its
              own. EXEMPT below is the list of write-shaped methods that change
              nothing persistent, each with the reason, and every name in it
              must be a COM method that exists: an exemption for a method
              nobody wrote reads as a decision somebody made about it.

  rest        Every route kind that mutates is answered through the dispatch in
              ProcessRequest_, which is where the scope is installed. A handler
              reached any other way is a change made with no actor on the
              thread, and so a change nobody made.

  store       IniSettingStore::WriteSetting and ::RemoveSetting, the two writes
              of hm_inisettings an interface reaches, each call RecordChange_.
              XMLLoad (a restore) must NOT: it runs on the maintenance work
              queue, where no actor is installed, so a call there records
              nothing while reading as though it did.

  table       hm_audit is written by AuditTrail and by nothing else. The row a
              caller writes for itself is the row that says whatever it likes.

  tables      The statement chokepoint records a table only when its name is in
              AuditTrail.cpp's AuditedTables, matched exactly - so a name there
              that is not the table the server writes records nothing and says
              nothing, which is how five of them (hm_domainaliases for
              hm_domain_aliases, and four more) left domain aliases, incoming
              relays, both white lists and blocked senders out of the record
              until 15 September 2026. Every name listed must be a table all
              three create scripts define, and every table they define must be
              either listed or named in NOT_AUDITED below with the reason. The
              second half is what makes a NEW configuration table a decision
              rather than an omission: hm_acl and hm_remotedomainpolicies were
              both simply never added. hm_settings and hm_inisettings must NOT
              be listed, because their chokepoints already record every change
              with the value it had, and a listing would record it twice.

  words       An AuditDescription - the account's address and the role's name in
              place of "raaccountid=12, raroleid=3" - is supplied by RoleManager,
              which writes the two role tables, and by the persister of the
              greylisting white list, whose address column holds the SQL LIKE
              pattern a typed wildcard was converted into (10.78.0.? is stored as
              10.78.0._) and so names an address no other surface shows; and by
              the eDiscovery store, whose rows are ids and states, named by their
              case, and whose search row carries its whole query. It changes
              what a row says and never whether it is written.

  scripts     Every script engine ScriptServer starts runs inside a
              ScriptActorBoundary - an Active Scripting engine on Windows, and a
              run of the JavaScript engine (ScriptBinding) on Linux. A script
              runs in process on the server thread that fired its event, and a
              COM object it authenticates installs that credential on the thread
              as every COM authority question does; without the boundary it
              stays there, and the server's next change on that thread is
              recorded as the script's credential's. The JavaScript engine has no
              COM to sign in to; its boundary keeps what a handler changes from
              being recorded as whoever was acting on the thread.

Run from the repository root: python3 build/check-audit-choke-point.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
COM = os.path.join(SERVER, "COM")
DBSCRIPTS = os.path.join(ROOT, "hmailserver", "source", "DBScripts")

# symbol -> the files (relative to hmailserver/source/Server, forward slashes)
# that may name it.
ALLOWED = {
   "RecordStatement(": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/SQL/DatabaseConnectionManager.cpp",   # the one statement chokepoint
   },
   "RecordSettingChange(": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/Application/PropertySet.cpp",         # the hm_settings chokepoint
      "Common/Application/IniSettingStore.cpp",     # the hm_inisettings chokepoint
   },
   # A change that writes no audited row and no setting: a lockout lifted, in
   # each of the ways one can be, and a restore asked for. Each file here makes
   # one.
   "RecordEvent(": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/Persistence/MailboxMove.cpp",         # a mailbox renamed or moved, as one entry naming both addresses
      "Common/Util/RestApiRoles.cpp",               # POST /api/v1/accounts/{address}/unlock
      "COM/InterfaceApplication.cpp",               # Application.UnlockAccount
      "COM/InterfaceAccount.cpp",                   # Account.UnlockMailbox, Account.RemovePushSubscriptions
      "Common/Util/RestApiPush.cpp",                # DELETE /api/v1/accounts/{address}/push
      "COM/InterfaceBackup.cpp",                    # Backup.StartRestore
      "Common/Util/RestApiServer.cpp",              # POST /api/v1/backup/restore; an API key created or revoked
      "Common/Util/RestApiOperator.cpp",            # the administrator's second factor enrolled or disabled
      "COM/InterfaceBackupManager.cpp",             # BackupManager.RestoreMailbox and RestoreMessage
      "Common/Util/RestApiBackupMailbox.cpp",       # POST /api/v1/backup/{archive}/mailboxes/{address}/restore
      "Common/Util/RestApiDiscovery.cpp",           # GET /api/v1/discovery/cases/{id}/exports/{export}: an export downloaded writes no row
      # Somebody's mail read through an administrator's route - the account's
      # folders and messages, a backup's listing of a mailbox, the two exports,
      # a held message's headers and preview, the archive index searched and an
      # entry of it read - which writes nothing at all:
      # RestApiServer::RecordMailRead_, the one place every such read is
      # recorded from (its own entry below names who may call it).
      "Common/Util/RestApiAccountResources.cpp",
      "Common/Util/RestApiInboundWebhooks.cpp",     # a message an inbound webhook kept read, retried or removed: its queue is traffic
      "Common/Util/RestApiSettings.cpp",            # POST /api/v1/settings/logon-failures/clear
      "COM/InterfaceSettings.cpp",                  # Settings.ClearLogonFailureList; Settings.EnrolAdministratorTOTP and DisableAdministratorTOTP
      "Common/Util/RestApiSpamTraps.cpp",           # DELETE /api/v1/spam-traps/hits
      "COM/InterfaceUtilities.cpp",                 # Utilities.ClearSpamTrapHits
      "Common/Util/RestApiAccountReview.cpp",       # a held message released or discarded, an account's review choices, the compromise lock cleared
      "Common/Util/RestApiSandbox.cpp",             # a message held for a sandbox's verdict released without it, or discarded
      "COM/InterfaceDeliveryQueue.cpp",             # DeliveryQueue.Remove of a message held for a sandbox's verdict
      "Common/Util/SignInStore.cpp",                # a device revoked, from every interface at once
      "Common/Util/RestApiAccountRecovery.cpp",     # a recovery address set or cleared, and a set of recovery codes made
      "Common/Util/RestApiDirectory.cpp",           # somebody's directory opt-out set or cleared by another hand
      "Common/Util/RestApiAddressBooks.cpp",        # DELETE /api/v1/accounts/{address}/address-books/{id}/shares/{target}
      # Mailbox encryption at rest: an account's key switched on or off,
      # forfeited or recovered, and a domain's recovery key made, replaced or
      # removed. The two tables are key material a sign-in writes with no actor,
      # so they are not audited row by row, and each administrator's act is
      # recorded here once, naming the account or the domain.
      "Common/Util/RestApiMailboxKeys.cpp",
      # A setting profile applied, reverted or its way back discarded - from REST,
      # COM and hmailserver --apply-profile alike, which all reach Profiles: thirty
      # settings changed as one act, recorded once with the counts beside the row
      # the settings chokepoint writes for each (docs/Profiles.md).
      "Common/Application/Profiles.cpp",
      # Maintenance mode ended by a stop of the servers - a service stop or a
      # reinitialise - rather than by DELETE /api/v1/server/drain: the "left"
      # that pairs the operator's "entered", recorded by the server under an
      # actor of its own, because no request thread carries one there.
      "Common/Application/Application.cpp",
      # A migration job cancelled while it runs: its own thread writes the row,
      # with no actor on it, so the request records who asked.
      "Common/Application/MigrationManager.cpp",
      # An administrator's install of an update (docs/ServiceAccount.md, "The
      # live update"): asked for, started and cancelled over COM from the Control
      # Panel on the server, and its outcome at the next start under the Windows
      # account the elevated Updater read from its own token. It installs files,
      # not rows or settings.
      "Common/Util/UpdateInstaller.cpp",
   },
   "AuditTrail::SetActor(": {
      "Common/Util/AuditTrail.cpp",                 # AuditScope itself
      "COM/COMAuthentication.cpp",                  # the COM identity chokepoint
   },
   "AuditTrail::ClearActor(": {
      "Common/Util/AuditTrail.cpp",
   },
   # A read of somebody's mail recorded: the routes that read it, and nothing
   # else - a call anywhere else is a read recorded where nobody decided it
   # was one, under whatever window the item it names happens to share.
   "RecordMailRead_(": {
      "Common/Util/RestApiServer.h",
      "Common/Util/RestApiAccountResources.cpp",    # the account's folders and messages, and the recorder itself
      "Common/Util/RestApiBackupMailbox.cpp",       # GET /api/v1/backup/{archive}/mailboxes/{address}
      "Common/Util/RestApiDiscovery.cpp",           # GET /api/v1/discovery/cases/{id}/searches/{search}: a page of its hits
      "Common/Util/RestApiOperator.cpp",            # POST /api/v1/accounts/{address}/export and GET .../export.zip
      "Common/Util/RestApiServer.cpp",              # GET /api/v1/quarantine/{id}/headers and /preview, GET /api/v1/archive and /api/v1/archive/{id}
   },
   # The words a row is recorded with, in place of the ones read from its
   # statement. Only for a table whose rows are nothing but ids, or whose name
   # column holds an encoding rather than a name, and only from the code that
   # writes that table: words that are wrong are a record that lies.
   "AuditDescription ": {
      "Common/Util/AuditTrail.cpp",
      "Common/Util/AuditTrail.h",
      "Common/Application/RoleManager.cpp",         # hm_roles and hm_roleassignments
      "Common/AntiSpam/SpamTrapMonitor.cpp",        # hm_spamtraps: a domain id and a local part going in, a row id coming out
      # hm_greylisting_whiteaddresses, whose address column is the SQL LIKE
      # pattern that what the administrator typed was converted into. The NAME
      # only: the detail is still read from the statement's own columns.
      "Common/Persistence/PersistentGreyListingWhiteAddress.cpp",
      # hm_neverbannames, whose delete statement carries only the row's id: a
      # row is named by its pattern, on the save as on the delete. The NAME
      # only: the detail is still read from the statement's own columns.
      "Common/Persistence/PersistentNeverBanName.cpp",
      # hm_migrationjobs, whose row names its target account by number: a job is
      # recorded as "source user -> target address" by the store that writes it.
      "Common/Util/MigrationStore.cpp",
      # The four eDiscovery tables, whose rows are case ids, owner ids, states
      # and times: each is named by its case ("case 3: <name>"), and a search's
      # row is recorded with its whole query - the one detail it supplies,
      # since "every search logged" is that row.
      "Common/Util/DiscoveryStore.cpp",
      # hm_smimeca and hm_smimeissued: a root's row is a domain id, a certificate
      # and a key envelope, and a revocation's statement carries two numbers, so
      # the authority names them by the domain and by the address and serial -
      # the NAME only, the detail still read from the statement's own columns.
      "Common/Util/OrganisationCa.cpp",
   },
}

# The call sites the rules above protect must exist, or they guard nothing.
REQUIRED = [
   ("Common/Util/UpdateInstaller.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(version), _T("install requested"),',
    "an administrator's install of an update asked for from the Control Panel is no longer recorded - it writes no audited row, and the only other trace would be an application-log line"),
   ("Common/Util/UpdateInstaller.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(outcome.version), action,',
    "the outcome of an administrator's install is no longer recorded under the Windows account that approved it"),
   ("Common/SQL/DatabaseConnectionManager.cpp",
    "AuditTrail::Instance()->RecordStatement(command, bResult);",
    "the statement chokepoint no longer records anything"),
   ("Common/Application/PropertySet.cpp",
    "AuditTrail::Instance()->RecordSettingChange(sPropertyName,",
    "the settings chokepoint no longer records anything"),
   ("Common/Application/IniSettingStore.cpp",
    "AuditTrail::Instance()->RecordSettingChange(name,",
    "the [Settings] store no longer records anything, so none of the keys that left hMailServer.INI is audited"),
   ("Common/Util/RestApiRoles.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("unlocked"),',
    "an account unlocked over REST is no longer recorded - the lockout is in memory, so no chokepoint sees it"),
   ("Common/Util/RestApiSpamTraps.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("spam trap hits"), _T("every recorded hit"), _T("cleared"),',
    "the record of spam trap hits cleared over REST is no longer recorded - hm_spamtraphits is traffic, so the statement chokepoint does not record the delete, and a clear whose whole effect is to remove evidence would leave no trace"),
   ("COM/InterfaceUtilities.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("spam trap hits"),',
    "the record of spam trap hits cleared over COM is no longer recorded - hm_spamtraphits is traffic, so the statement chokepoint does not record the delete, and a clear whose whole effect is to remove evidence would leave no trace"),
   ("COM/InterfaceApplication.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("unlocked"),',
    "an account unlocked over COM is no longer recorded - the lockout is in memory, so no chokepoint sees it"),
   ("COM/InterfaceAccount.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("account"), object_->GetAddress(), _T("unlocked"),',
    "a POP3 mailbox lock cleared through Account.UnlockMailbox is no longer recorded - the lock is in memory, so no chokepoint sees it"),
   ("Common/Util/RestApiPush.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("push-subscriptions-removed"),',
    "an account's Web Push devices stopped over REST is no longer recorded - hm_pushsubscriptions is the person's own and is not audited, so the statement chokepoint does not record the delete"),
   ("COM/InterfaceAccount.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("account"), object_->GetAddress(), _T("push-subscriptions-removed"),',
    "an account's Web Push devices stopped over COM is no longer recorded - hm_pushsubscriptions is the person's own and is not audited, so the statement chokepoint does not record the delete"),
   ("COM/InterfaceAccount.cpp",
    "authentication_->AllowsForDomain(HM::RolePermission::UnlockAccount, HM::StringParser::ExtractDomain(object_->GetAddress()))",
    "Account.UnlockMailbox no longer asks the permission an unlock needs, so anybody holding an account object clears its lock and installs no actor"),
   ("Common/Util/RestApiSettings.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("logon failures"), _T("every recorded failure"), _T("cleared"),',
    "the logon-failure list cleared over REST is no longer recorded - hm_logon_failures is traffic, so the statement chokepoint does not record the delete"),
   ("COM/InterfaceSettings.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("logon failures"), _T("every recorded failure"), _T("cleared"),',
    "the logon-failure list cleared over COM is no longer recorded - hm_logon_failures is traffic, so the statement chokepoint does not record the delete"),
   ("Common/Util/RestApiAccountReview.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("held message"), StringParser::IntToString(held.message_id), _T("released"), detail);',
    "a held message released over REST is no longer recorded - hm_heldmessages is traffic, so the statement chokepoint does not record the delete, and a release sends mail the server had decided not to"),
   ("Common/Util/RestApiAccountReview.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("held message"), StringParser::IntToString(held.message_id), _T("discarded"), detail);',
    "a held message discarded over REST is no longer recorded - hm_heldmessages is traffic, so the statement chokepoint does not record the delete"),
   ("Common/Util/RestApiSandbox.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), StringParser::IntToString(held.message_id), _T("released"), detail);',
    "a message held for a sandbox's verdict and released over REST is no longer recorded - hm_sandboxholds is traffic and the release is an update of the queue row, so the statement chokepoint records neither, and a release delivers mail the server was waiting to judge"),
   ("Common/Util/RestApiSandbox.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), StringParser::IntToString(held.message_id), _T("discarded"), detail);',
    "a message held for a sandbox's verdict and discarded over REST is no longer recorded - hm_sandboxholds is traffic, so the statement chokepoint does not record the delete"),
   ("COM/InterfaceDeliveryQueue.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("sandbox hold"), HM::StringParser::IntToString(held.message_id), _T("discarded"),',
    "a message held for a sandbox's verdict and removed over COM (DeliveryQueue.Remove) is no longer recorded - hm_sandboxholds is traffic and the queue row's delete is not an audited statement, so a message the server was waiting to judge would be destroyed with no trace"),
   ("Common/Util/RestApiAccountReview.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("review-changed"), detail);',
    "an account marked exempt from the compromised-account detection, or its sensitivity changed, is no longer recorded - hm_accountreview is written by the submission thread too, so it is not an audited table"),
   ("Common/Util/RestApiAccountReview.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("compromise-lock-cleared"),',
    "the compromise lock cleared alone over REST is no longer recorded - the lock is a row written with no actor and a hold in memory, so no chokepoint sees it"),
   ("Common/Util/RestApiAccountRecovery.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-address-set"),',
    "a recovery address set over REST is no longer recorded - hm_accountrecovery is written by the account itself and is not an audited table, and the address it names is the one a forgotten password can be reset from, which is the change an administrator most needs to be able to find afterwards"),
   ("Common/Util/RestApiAccountRecovery.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("recovery-codes-generated"),',
    "a new set of recovery codes made over REST is no longer recorded - the codes stand in for the account's second factor, and making a set ends the one before it"),
   ("Common/Util/RestApiDirectory.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("directory-opt-out-changed"),',
    "an administrator setting or clearing somebody's directory opt-out over REST is no longer recorded - hm_directory is not an audited table (the person writes their own title and telephone number from the webmail), so the statement chokepoint does not record it, and putting a person who asked to be left out back into their domain's listing would leave nothing but an application-log line"),
   ("COM/InterfaceAccount.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("account"), object_->GetAddress(), _T("directory-opt-out-changed"),',
    "the same change made through Account.Save() over COM - which is what the Control Panel's Directory entry tab does - is no longer recorded, for the same reason"),
   ("Common/Util/SignInStore.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("device"), record.device, _T("revoked"), detail);',
    "a device revoked is no longer recorded - hm_signins is not an audited table (a sign-in would be recorded on every authentication) and the sessions and connections a revoke ends leave no row at all, so the strongest thing anybody can do about a client they do not recognise would be the one thing the trail did not hold"),
   ("Common/Util/RestApiMailboxKeys.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("mailbox-key-recovered"),',
    "a mailbox key opened with the domain's recovery key, and a new password set for the account, is no longer recorded - hm_accountkeys is not audited row by row, so the one act that lets an administrator into an encrypted mailbox would leave nothing but an application-log line"),
   ("Common/Util/RestApiAddressBooks.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("address-book-share-removed"),',
    "a share of somebody's address book removed by an administrator is no longer recorded - hm_addressbookshares is the account's own table and is not audited, so the statement chokepoint does not record the delete, and an operator undoing a person's own sharing decision would leave no trace"),
   ("Common/Util/RestApiServer.cpp",
    "AuditScope auditScope(AuditActorFor_(caller));",
    "REST no longer says who is making a change"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("backup"), file, _T("restore started"),',
    "a restore started over REST is no longer recorded - the restore itself runs on the maintenance work queue with no actor on it, so the request is the only half that can be"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("server"), _T("drain"), _T("entered"),',
    "entering maintenance mode over REST is no longer recorded - the drain closes the listeners and pauses delivery without writing a row, so the request is the whole of what there is to record, and an operator who took the server off the air would leave no trace"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("server"), _T("drain"), _T("left"),',
    "leaving maintenance mode over REST is no longer recorded - the half that says who put the server back"),
   ("Common/Application/Application.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("server"), _T("drain"), _T("left"),',
    "maintenance mode ended by a stop or a reinitialise is no longer recorded - an operator who drained a node would find it accepting again with an 'entered' and no 'left'"),
   ("COM/InterfaceBackup.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("backup"), backup_->GetBackupFile(), _T("restore started"),',
    "a restore started over COM is no longer recorded - the restore itself runs on the maintenance work queue with no actor on it, so the request is the only half that can be"),
   ("Common/Util/RestApiBackupMailbox.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("backup"), job->GetRequest().archive_file, _T("mailbox restore started"),',
    "a mailbox restore started over REST is no longer recorded - it runs on the maintenance work queue with no actor on it, so the request is the only half that can be"),
   ("Common/Util/RestApiDiscovery.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("discovery export"), DiscoveryStore::CaseLabel(theCase), _T("downloaded"),',
    "an eDiscovery export downloaded is no longer recorded - the download writes no row, and it is the act that takes the custodians' mail off the server"),
   ("Common/Util/RestApiAccountResources.cpp",
    "AuditTrail::Instance()->RecordEvent(objectType, objectName, action, detail);",
    "somebody's mail read through an administrator's route is no longer recorded - a read writes no row and no setting, so no chokepoint sees it, and whose mail an administrator opened is the question the trail exists to answer"),
   ("Common/Util/RestApiAccountResources.cpp",
    "if (!FirstReadInWindow(key))",
    "a read of somebody's mail is no longer held to one row a minute for the same credential, address and item - a page that reads again at every click, and a client that pages a listing, would bury every other row of the trail"),
   ("Common/Util/RestApiAccountResources.cpp",
    'return recorded(ListFolders(bridge, account), _T("its folders listed"), _T("folders"));',
    "an account's folder tree read through GET /api/v1/accounts/{address}/folders is no longer recorded - the names of somebody's folders are their mailbox's"),
   ("Common/Util/RestApiAccountResources.cpp",
    "return recorded(ListPermissions(bridge, account, route.folder_id),",
    "a folder's permissions read through GET /api/v1/accounts/{address}/folders/{id}/permissions are no longer recorded - the route is view_mailbox's, as every read of the mailbox is"),
   ("Common/Util/RestApiAccountResources.cpp",
    'return recorded(ListAccountMessages(bridge, account), _T("every message listed"), _T("messages"));',
    "an account's messages listed through GET /api/v1/accounts/{address}/messages are no longer recorded - who wrote to the person, when, and how much"),
   ("Common/Util/RestApiAccountResources.cpp",
    "return recorded(ListFolderMessages(bridge, account, route.folder_id),",
    "a folder's messages listed through GET /api/v1/accounts/{address}/folders/{id}/messages are no longer recorded"),
   ("Common/Util/RestApiAccountResources.cpp",
    "return recorded(GetMessage(bridge, account, route.message_id),",
    "a message's header fields read through GET /api/v1/accounts/{address}/messages/{id} are no longer recorded - its subject, its sender and its recipients"),
   ("Common/Util/RestApiAccountResources.cpp",
    "return recorded(MessageSource(bridge, account, route.message_id),",
    "a message's whole file read through GET /api/v1/accounts/{address}/messages/{id}/source is no longer recorded - the mail itself, byte for byte"),
   ("Common/Util/RestApiBackupMailbox.cpp",
    'RecordMailRead_(_T("account"), address, _T("mailbox-read"), listed,',
    "a backup's listing of somebody's mailbox is no longer recorded - its senders, dates and subjects are the mailbox as it was"),
   ("Common/Util/RestApiOperator.cpp",
    'RecordMailRead_(_T("account"), account->GetAddress(), _T("mailbox-read"), directoryExported, String());',
    "a mailbox exported to a directory on the server's disk is no longer recorded - the whole of it read, and a copy left behind"),
   ("Common/Util/RestApiOperator.cpp",
    'RecordMailRead_(_T("account"), account->GetAddress(), _T("mailbox-read"), archiveExported, String());',
    "a mailbox exported to an archive on the server's disk is no longer recorded - the whole of it read, and a copy left behind"),
   ("Common/Util/RestApiOperator.cpp",
    'RecordMailRead_(_T("account"), account->GetAddress(), _T("mailbox-read"), directoryStopped, String());',
    "an export to a directory that stopped part of the way is no longer recorded - the messages it copied before it stopped stay on the disk"),
   ("Common/Util/RestApiOperator.cpp",
    'RecordMailRead_(_T("account"), account->GetAddress(), _T("mailbox-read"), archiveStopped, String());',
    "an export to an archive that stopped part of the way is no longer recorded - what it wrote before it stopped stays in the archive on the disk"),
   ("Common/Util/RestApiOperator.cpp",
    'RecordMailRead_(_T("account"), account->GetAddress(), _T("mailbox-read"), _T("everything held on it downloaded as one archive"), String());',
    "a mailbox downloaded whole through GET /api/v1/accounts/{address}/export.zip is no longer recorded - the strongest read there is"),
   ("Common/Util/RestApiServer.cpp",
    '_T("its header fields, from ") + message.sender + _T(" to ") + message.recipients,',
    "a held message's headers read through GET /api/v1/quarantine/{id}/headers are no longer recorded - the message itself, which is why the route asks view_mailbox"),
   ("Common/Util/RestApiServer.cpp",
    '_T("its text, from ") + message.sender + _T(" to ") + message.recipients,',
    "a held message's text read through GET /api/v1/quarantine/{id}/preview is no longer recorded - the message itself, which is why the route asks view_mailbox"),
   ("Common/Util/RestApiServer.cpp",
    'RecordMailRead_(_T("archive"), searchedIn, _T("searched"), searched,',
    "a search of the archive index through GET /api/v1/archive is no longer recorded - the senders, recipients and subjects of the archived copies of everybody's mail"),
   ("Common/Util/RestApiServer.cpp",
    'RecordMailRead_(_T("archive entry"), _T("archiveid ") + StringParser::IntToString(archiveId), _T("read"),',
    "one archive entry read through GET /api/v1/archive/{id} is no longer recorded - who wrote to whom about what, and where the copy lies"),
   ("Common/Util/RestApiDiscovery.cpp",
    'RecordMailRead_(_T("discovery search"), DiscoveryStore::CaseLabel(theCase), _T("read"),',
    "a page of an eDiscovery search's hits read through GET /api/v1/discovery/cases/{id}/searches/{search} is no longer recorded - the custodians' mail as the search found it, read by whoever holds view_discovery, who need not be whoever ran the search"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("support-session-opened"), supportDetail);',
    "a support session opened as an account is no longer recorded - the webmail opened as somebody else, which writes no audited row"),
   ("Common/Util/DiscoveryStore.cpp",
    "AuditDescription words(SearchesTable, CaseLabel(theCase), detail);",
    "an eDiscovery search is no longer recorded with its whole query - the statement's own detail cuts every value at 120 characters, and 'every search logged' is the query as it was asked"),
   ("COM/InterfaceBackupManager.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("backup"), job->GetRequest().archive_file, _T("mailbox restore started"),',
    "a mailbox restore started over COM is no longer recorded - it runs on the maintenance work queue with no actor on it, so the request is the only half that can be"),
   ("COM/InterfaceBackupManager.cpp",
    "if (!GetIsServerAdmin())",
    "BackupManager.RestoreMailbox no longer asks the question that installs the actor, so the row it writes is attributed to whoever used that COM pool thread last"),
   ("COM/InterfaceBackup.cpp",
    "if (!GetIsServerAdmin())",
    "Backup.StartRestore no longer asks the question that installs the actor, so the row it writes is attributed to whoever used that COM pool thread last"),
   ("COM/COMAuthentication.cpp",
    "AuditTrail::SetActor(GetAuditActor());",
    "COM no longer says who is making a change"),
   ("Common/Application/MigrationManager.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("migration job"), AuditName_(job), _T("cancelled"),',
    "a running migration job cancelled over REST is no longer recorded - its own thread writes the row, with no actor on it, so the request is the only half that can be"),
   ("Common/Util/AuditTrail.h",
    "bool Verify(__int64 &firstBroken, __int64 &rowsChecked, String &reason);",
    "the chain can no longer be walked, which is the whole point of chaining it"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("api key"), String(id), _T("created"),',
    "an API key created over REST is no longer recorded - the key store is an ini file, not an audited table, so a credential that can administer the server would be minted with nothing in the tamper-evident record"),
   ("Common/Util/RestApiServer.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("api key"), String(id), _T("revoked"),',
    "an API key revoked over REST is no longer recorded - the key store is an ini file, not an audited table"),
   ("Common/Util/RestApiOperator.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("administrator"), _T("second factor"), _T("enrolled"),',
    "the administrator's second factor enrolled over REST is no longer recorded - the secret lives in hMailServer.INI, written outside the settings chokepoint, so enrolling or replacing it would leave nothing in the tamper-evident record"),
   ("Common/Util/RestApiOperator.cpp",
    'AuditTrail::Instance()->RecordEvent(_T("administrator"), _T("second factor"), _T("disabled"),',
    "the administrator's second factor disabled over REST is no longer recorded - turning off the guard on the full-authority credential must be in the tamper-evident record"),
   ("COM/InterfaceSettings.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("administrator"), _T("second factor"), _T("enrolled"),',
    "the administrator's second factor enrolled over COM - the Control Panel's way - is no longer recorded, where the REST route's is"),
   ("COM/InterfaceSettings.cpp",
    'HM::AuditTrail::Instance()->RecordEvent(_T("administrator"), _T("second factor"), _T("disabled"),',
    "the administrator's second factor disabled over COM - the Control Panel's way - is no longer recorded, where the REST route's is"),
]

# The two administrative writes of hm_inisettings each have to reach the
# recorder, or a route through the one that does not is a way round it.
INI_STORE_RECORDS = ("IniSettingStore::WriteSetting", "IniSettingStore::RemoveSetting")

# Writes of hm_inisettings that must NOT call the recorder, because no actor is
# ever on the thread they run on and a call there records nothing while reading
# as though it did. XMLLoad is a restore, run as a BackupTask on the maintenance
# work queue; a RecordChange_ put back here would write rows attributed to
# nobody without making the trail any more complete. What a restore records is
# the REQUEST, written by the caller on the thread that knows who made it - see
# the two entries in REQUIRED above - and recording what the restore then
# CHANGES is still a decision that starts by carrying the actor to that thread.
# docs/AlertsAndAuditTrail.md says which half is which.
INI_STORE_CANNOT_RECORD = ("IniSettingStore::XMLLoad",)

# A COM method, by its signature.
COM_METHOD = re.compile(r"^STDMETHODIMP\s+(\w+)::(\w+)\s*\(", re.M)

# What a method body looks like when it PERSISTS something - a row through the
# object or collection layer, or a value through Configuration and so through
# hm_settings. A property setter on a business object is not here on purpose: it
# sets a field in memory and changes nothing until Save, which is.
HARD_MARKERS = ("SaveObject(", "DeleteObject(", "DeleteItem", "DeleteAll", "DeleteByDBID",
                "ini_file_settings_->Set", "antiVirusConfiguration_.", "AntiSpamConfiguration().Set")

# What installs the actor on the calling thread, one way or the other. The two
# role questions are here because COMAuthentication asks RoleManager through
# InstallActor_, which installs the audit trail's actor too: a help desk's
# password reset (InterfaceApplication::ResetPassword) asks
# authentication_->Allows(ResetPassword) and nothing else, and is recorded.
INSTALLS = ("GetIsServerAdmin(", "GetIsDomainAdmin(", "AuditScope ",
            "authentication_->Allows(", "authentication_->AllowsForDomain(")


# The settings interfaces reach the configuration through a cached pointer, and
# a getter names that pointer too - so what says "this writes" is a Set on it,
# directly or through one of the sub-configurations. Matched on the receiver so
# that GetBackupOption(BOSettings), which merely has the letters, is not a write.
CONFIG_SET = re.compile(r"(?:config_|cache_config_|ini_file_settings_)->(?:\w+\(\)->)?Set[A-Z]")

# The same through the IniFileSettings singleton, which is how the [Settings]
# store is reached from COM: the tarpit setters, SetIniSetting and
# DeleteIniSetting. DeleteIniSetting wrote through here with no actor of its own
# until this pattern existed, which the rule above could not see.
INI_WRITE = re.compile(r"IniFileSettings::Instance\(\)->(?:Set[A-Z]|WriteSettingsValue\(|RemoveSettingsValue\()")


def persists(body):
   if any(marker in body for marker in HARD_MARKERS):
      return True

   return CONFIG_SET.search(body) is not None or INI_WRITE.search(body) is not None


# A method that reaches one of those markers and still changes nothing in the
# configuration, with the reason. A method belongs here only when the thing it
# writes is not configuration; "nobody will notice" is not a reason.
EXEMPT = {
   "InterfaceAttachments::Add": "attaches a file to a message being composed; a message is traffic, not configuration.",
   "InterfaceAttachments::Clear": "as Add.",
   "InterfaceAttachment::Delete": "removes an attachment from a message being composed.",
   "InterfaceAttachment::SaveAs": "writes an attachment to a file the caller names; nothing in the server changes.",
   "InterfaceMessage::Save": "a message, which is traffic and not configuration.",
   "InterfaceMessages::Clear": "the messages of a folder.",
   "InterfaceMessages::DeleteByDBID": "one message of a folder.",
   "InterfaceAccount::DeleteMessages": "empties a mailbox; the account itself is unchanged.",
}

# The create scripts. A table has to be defined by all three to be listed: the
# statement chokepoint runs on every backend, and a name one of them spells
# differently is a hole on that backend only. SQL Server Compact runs the SQL
# Server script.
CREATE_SCRIPTS = ("CreateTablesMSSQL.sql", "CreateTablesMySQL.sql", "CreateTablesPGSQL.sql")

CREATE_TABLE = re.compile(r"\bcreate\s+table\s+(?:if\s+not\s+exists\s+)?[`\"\[]?(\w+)", re.I)

# Every table the create scripts define that is deliberately NOT one of the
# statement chokepoint's, with the reason. "Nobody changes it" is not a reason;
# "what changes it is traffic, or the account's own data, or another recorder"
# is.
NOT_AUDITED = {
   "hm_settings": "recorded by PropertySet with the value it had; listing it would record every change twice.",
   "hm_inisettings": "recorded by IniSettingStore with the value it had; listing it would record every change twice.",
   "hm_audit": "the trail itself, written by AuditTrail alone (rule 'table').",
   "hm_dbversion": "the schema version, written by the upgrade scripts and nobody else.",
   "hm_mailboxmoves": "the journal of a mailbox rename or move, written by MailboxMove as the move runs and by nothing else. The move itself IS on the trail, as one RecordEvent naming both addresses, which is why the journal is not: listing the table would record the row opened before the move starts and the row closed after it, under the same actor, for one change.",
   "hm_autoreplies": "the memory of who an out-of-office reply answered and when, written by SMTPVacationMessageCreator as replies go out and forgotten when the reply is switched off; traffic, not configuration.",
   "hm_bookinganswers": "a room's or a piece of equipment's memory of whom it answered this hour and whose request its delegates were told of, written by ResourceBooking as it decides; traffic, like the out-of-office memory.",
   "hm_bookingqueue": "what was delivered to a room or a piece of equipment and waits to be decided, written by ResourceBooking at delivery and removed when decided; traffic, like the delivery queue.",
   "hm_bookingdecisions": "whom a room or a piece of equipment accepted each booking from, written by ResourceBooking as it accepts and removed with the booking; traffic, like the answer memory.",
   "hm_messages": "messages are traffic.",
   "hm_messagerecipients": "the recipients of a message in the queue; traffic.",
   "hm_message_metadata": "per-message metadata the IMAP server keeps; traffic.",
   "hm_messageindexterms": "the full-text index; traffic.",
   "hm_messageindexstate": "the full-text index's progress; traffic.",
   "hm_messagetrace": "where each message went; traffic.",
   "hm_apisends": "what each API key sent through the send API (docs/SendApi.md), written by POST /api/v1/send once the message is queued; traffic, as the trace it is answered from is. The key it names is made and revoked by the administrator password's key routes, which are not table writes either.",
   "hm_webhookqueue": "the outbound webhook delivery queue, written by the paths that raise an event and by the delivery pass. "
                       "Traffic, and the server's own writes - there is no actor on an SMTP thread, so a listed table would record every "
                       "accepted message as a change nobody made. The subscriptions the rows belong to (hm_webhooks) ARE recorded.",
   "hm_inboundhookqueue": "the inbound webhook queue: one row per message a route took, written by the delivery path with no actor and "
                          "by the pass that posts it. Traffic, as the outbound webhooks' queue is. The routes (hm_inboundhooks) ARE "
                          "recorded, and a kept message read, retried or removed over REST is recorded through AuditTrail::RecordEvent.",
   "hm_migrationfolders": "a migration job's resume marks - a source folder's UIDVALIDITY and the highest UID copied - written by the "
                          "job's own thread every fifty messages under no actor, and removed with the job. The migrations and their "
                          "jobs (hm_migrations, hm_migrationjobs) ARE recorded.",
   "hm_quarantine": "held messages, written by delivery and by each account's own portal. An administrator releasing or deleting one is NOT "
                    "recorded, and docs/AlertsAndAuditTrail.md says so: both end in the same 'delete from hm_quarantine where "
                    "quarantineid = @ID', so a listed table would record a release and a delete as the same row, naming an id that "
                    "identifies nothing once the row is gone. Recording it needs a record of its own in QuarantineStore.",
   "hm_reputationchecks": "the last answer each DNS block list gave about each of the server's outbound addresses, rewritten "
                          "by the reputation watch's daily check under no actor and by an administrator's check on demand; a "
                          "listing would record every lookup. What the watch checks - the three ReputationWatch settings - is "
                          "in hm_settings, recorded by PropertySet with the value it had.",
   "hm_imapfolders": "every account's folders, which its mail client and its webmail create and delete as much as any administrator does - the webmail "
                     "under the account's own actor, so listing the table would record each folder a person makes in their own mailbox. Public "
                     "folders share the table, so an administrator creating, renaming or deleting one is NOT recorded, and "
                     "docs/AlertsAndAuditTrail.md says so. The permissions on folders (hm_acl) are audited.",
   "hm_imapexpunged": "IMAP's record of expunged messages for QRESYNC; traffic.",
   "hm_imap_metadata": "IMAP METADATA an account or its client sets on its own mailbox.",
   "hm_fetchaccounts_uids": "which messages an external fetch has already downloaded; traffic.",
   "hm_greylisting_triplets": "greylisting's record of the senders it has seen; traffic.",
   "hm_logon_failures": "failed logons, which are traffic and the auto-ban's business.",
   "hm_metricsamples": "metric history; traffic.",
   "hm_domainsamples": "the per-domain daily sample of spam, viruses and store size, written by DomainSamples; traffic.",
   "hm_alertevents": "what an alert condition raised; the alert rules that decide it (hm_alertrules) are audited.",
   "hm_complaints": "one row per RFC 5965 abuse report a provider's feedback loop posted to the complaint mailbox, written by the "
                    "arriving report with no actor at all - nobody here decided it - and pruned by the sweep; traffic. What an "
                    "administrator decides about the feedback loops is a setting (hm_settings) or the suppression list entry a "
                    "complaint made (hm_bouncesuppressions), and both are audited.",
   "hm_complaintvolume": "the per-domain daily tally of messages delivered and complaints received that the complaint rate is "
                         "worked out from, written by delivery and by an arriving report, with no actor; traffic.",
   "hm_dmarcreports": "one row per DMARC aggregate report a receiver posted to a domain's report mailbox, written by the arriving "
                      "report with no actor at all and pruned by the retention sweep; traffic. The mailbox it is read from is an "
                      "hm_domains column, which is audited.",
   "hm_dmarcreportrecords": "the records of those reports - one per source and verdict a receiver counted - written with the report "
                            "and gone with it by the cascade; traffic.",
   "hm_tlsreports": "one row per TLS-RPT report a sender posted to a domain's report mailbox, written by the arriving report with "
                    "no actor and pruned by the retention sweep; traffic.",
   "hm_knownsenders": "who an account has corresponded with, learned from its mail.",
   "hm_passwordhistory": "the hashes of an account's earlier passwords, written as a consequence of the password change the hm_accounts row already records.",
   "hm_accountprefs": "an account's own webmail preferences, set by the account in the portal.",
   "hm_calendars": "an account's own calendars, written by it over CalDAV and the portal.",
   "hm_calendarobjects": "the events in an account's own calendars.",
   "hm_calendarshares": "who else an account shares a calendar with, written by the account itself over the portal and CalDAV.",
   "hm_contacts": "an account's own address book, written by it over CardDAV and the portal.",
   "hm_directory": "one account's entry in the organisation's directory - its title, telephone numbers and photo - written by the "
                   "person themselves from the webmail as often as by an administrator, and holding nothing that decides what the "
                   "server does with mail; an administrator's change to one is in the application log with the credential that made "
                   "it. The one field of it that is not that is the opt-out, which is a decision of the person's that somebody else "
                   "can reverse: a change of diroptout by any hand but the person's own is recorded through AuditTrail::RecordEvent "
                   "as directory-opt-out-changed, by PUT /api/v1/accounts/{address}/directory and by Account.Save() over COM.",
   "hm_addressbooks": "an account's own address books, made and named by it in the portal.",
   "hm_addressbookshares": "who else an account shares an address book with - one account, or a whole domain - written by the account itself in the portal, "
                           "and removable by an administrator over /api/v1/accounts/{address}/address-books, which IS audited as an event.",
   "hm_recoverable": "copies of the messages an account deleted, written as the delete happens and ended by the retention sweep or the account itself; traffic.",
   "hm_accountrecovery": "the recovery address an account set for itself, written by the account over /api/v1/me/recovery and cleared by the account or by an administrator. "
                         "Not listed, because the row is also written with no actor at all - the unauthenticated page a confirmation link opens marks it confirmed - and a listed table would record that as a change nobody made. "
                         "The three acts an administrator needs to find ARE on the trail, through AuditTrail::RecordEvent: the address set, the address cleared, and a new set of codes made.",
   "hm_recoverytokens": "the one-shot links account recovery sends - confirm this address, reset this password - each issued, spent and swept with no actor on the thread; a listing would record every link and every sweep as a change nobody made. "
                        "What the links DO is recorded: the address set is an event, and a password reset is an hm_accounts write the statement chokepoint already sees, an application-log line, and the account.password-reset alert.",
   "hm_recoverycodes": "the hashes of an account's one-time recovery codes, written by the account when it makes a set and marked used by the sign-in thread; "
                       "traffic once the set exists, and the making of a set is recorded as an event of its own.",
   "hm_protectedmessages": "password-protected messages an account sent, written as it sends them, fetched by their readers, withdrawn by the account and ended by the sweep; traffic, and ciphertext this server cannot open.",
   "hm_confidential": "the messages that expire an account sent, written as it sends them, opened by their readers, withdrawn by the account and ended by the sweep; traffic.",
   "hm_accountactivity": "the compromised-account detection's baseline, one row per authenticated submission, written by the submission thread and pruned by the sweep; traffic.",
   "hm_accountrecipients": "whom an account has written to, learned by the submission thread from its messages and forgotten by the sweep; traffic.",
   "hm_accountreview": "an account's state under the compromised-account detection. The lock and the last shape seen are written by the "
                       "submission thread with no actor. The administrator's two choices - exempt, sensitivity - are recorded by "
                       "PUT /api/v1/accounts/{address}/review through AuditTrail::RecordEvent, and the lock cleared alone by "
                       "POST .../review/unlock the same way (the general unlock records its own event).",
   "hm_heldmessages": "the messages the compromised-account detection kept out of the delivery queue, written by the submission thread "
                      "with no actor and ended by the sweep; releasing or discarding one over REST is recorded through "
                      "AuditTrail::RecordEvent, naming the message, since a release sends mail the server had decided not to.",
   "hm_maskedaddresses": "the masked addresses an account made for itself in the portal, switched off and removed by it there; the delivery count is traffic.",
   "hm_senderaddresses": "the addresses elsewhere an account proved with a code and sends as, added and removed by the account itself in the portal.",
   "hm_files": "files an account attached as links from webmail; the account's own data.",
   "hm_scheduled": "an account's own scheduled sends and snoozed messages.",
   "hm_smimekeys": "an account's own S/MIME certificates, kept for it by the portal.",
   "hm_pgpring": "an account's own OpenPGP keys and its correspondents' public keys, kept for it by the webmail; the secret half is encrypted where this server cannot see it.",
   "hm_zeroaccess": "a zero-access mailbox's two sealed copies of its own key and the derivation parameters, kept for it by the webmail; the person's own, written by the self-service routes, and this server can open neither copy.",
   "hm_accountkeys": "an account's mailbox key for encryption at rest, in its wrapped and sealed forms: key material, written by a sign-in with the account's password (the first wrap, a wrap made again) with no actor on the thread, and by the administrator's routes, each of which records its act through AuditTrail::RecordEvent in RestApiMailboxKeys.cpp - naming the account, and never the key, which a row recorded whole would put in the trail.",
   "hm_domainrecovery": "a domain's recovery key for encryption at rest: its public half and its private half wrapped by a passphrase this server never stores. Made, replaced and removed only by the administrator's routes, each recorded through AuditTrail::RecordEvent in RestApiMailboxKeys.cpp naming the domain - a row recorded whole would copy the wrapped key into the trail, for anybody who reads it to guess the passphrase against.",
   "hm_recall": "message recall within the server: one row per local recipient of a message an account sent, written at acceptance and delivery, marked by the account's own recall and ended by the sweep; traffic.",
   "hm_sandboxholds": "the files of a message held for a sandbox's verdict, written by the delivery thread and the sandbox's poller with no actor and ended with the hold; traffic. An administrator releasing or discarding a held message over REST is recorded through AuditTrail::RecordEvent, naming the message, since a release delivers mail the server was waiting to judge.",
   "hm_sandboxverdicts": "the verdicts a sandbox gave, by SHA-256, written by the sandbox's poller with no actor and swept when they expire; traffic.",
   "hm_storeblobs": "the parts of messages the message store holds once and how many stored files name each, written by delivery, IMAP APPEND, copies and deletes with no actor as mail comes and goes, and rebuilt by a restore; the store's own bookkeeping, not configuration. What an administrator decides about the store - MessageStoreCompression and MessageStoreDedupMinKB - is a setting, recorded by PropertySet.",
   "hm_listarchive": "a managed list's archive: one row per message the list distributed, written by the delivery path and ended by the list's retention or the list's deletion; traffic. The list's settings are hm_distributionlists columns, which are audited.",
   "hm_warmupcounts": "the IP warm-up's daily counts per schedule and provider, written by the delivery queue as it sends and defers; traffic. The schedules themselves, hm_warmup, are audited.",
   "hm_digeststate": "when each account last had a quarantine digest and whether it opted out, written by the digest task and by the "
                     "account itself in the webmail; the domain's switch and hour are hm_domains columns, which are audited.",
   "hm_digesttokens": "the digest's one-use release links, written by the digest task and spent by whoever follows one; traffic, and "
                      "each row holds a token that releases a held message.",
   "hm_spamtraphits": "one row each time a spam trap was addressed - the address, the connecting address, the envelope sender "
                      "and what was done about it. Traffic, written by the SMTP connection with no actor on the thread, and the "
                      "record of the hit rather than a change anybody made. The traps themselves (hm_spamtraps) are "
                      "configuration and ARE audited; the ban a hit creates is a hm_securityranges row written with no actor, "
                      "as an auto-ban from failed logons is, and the sender it blocks is a hm_senderlist row written the same "
                      "way - docs/SpamTraps.md says so. CLEARING the record is not traffic and is recorded: "
                      "Utilities.ClearSpamTrapHits and DELETE /api/v1/spam-traps/hits each call RecordEvent, because an act "
                      "whose whole effect is to remove evidence is the one that must leave a trace.",
   "hm_dkimrotations": "where each domain's DKIM key rotation has got to - when the next key was made and first seen, the last DNS "
                       "check of each selector, the last error - rewritten by the rotation's pass every fifteen minutes under the "
                       "system actor, so a listing would record every lookup. Generating a next key, switching to it and cancelling "
                       "it each update the selector and key file columns of hm_domains, which are audited with the pass's actor or "
                       "the caller. A retire changes only this row and the key file, and is NOT recorded: "
                       "docs/DkimKeyRotation.md says so, and the application log has a line for it.",
   "hm_pushsubscriptions": "the browsers and phones an account asked to be told about its own mail on, written by that account in the "
                           "webmail and by the push sender when a push service says a device is gone. An administrator's only reach "
                           "into it is stopping every device of an account - Account.RemovePushSubscriptions over COM and DELETE "
                           "/api/v1/accounts/{address}/push over REST - and each of those records an event, push-subscriptions-removed, "
                           "under the actor; the switch, the limits and the VAPID key pair are hm_settings rows, and every change to "
                           "those is already recorded with the value it had.",
   "hm_bouncecounts": "how often each address outside the server bounced a domain's mail and when it was last delivered to, written by "
                      "delivery and by the delivery reports that arrive, with no actor; traffic. What an administrator decides about "
                      "bounces is a setting (hm_settings) or a suppression list entry (hm_bouncesuppressions), and both are audited.",
   "hm_bouncereportkeys": "the register of sent messages a delivery report is matched against, one row per message delivered to a "
                          "remote while reports are read, written by delivery and pruned by the sweep; traffic.",
   "hm_signins": "which clients have signed in to each account and when, written by every authentication on every protocol - by far the busiest write this server would ever record. What an administrator DECIDES about a client is recorded: revoking one writes an audit event through AuditTrail::RecordEvent (the sessions and connections it ends leave no row of their own), and the app password a revoke can deactivate is a column of hm_apppasswords, which is audited.",
}

# Tables whose chokepoint is not the statement one, and which must therefore
# never be listed.
RECORDED_ELSEWHERE = ("hm_settings", "hm_inisettings", "hm_audit")


def read(path):
   with open(path, "r", encoding="utf-8", errors="replace") as handle:
      return handle.read()


def relative(path):
   return os.path.relpath(path, SERVER).replace(os.sep, "/")


def sources():
   for base, _, names in os.walk(SERVER):
      for name in names:
         if name.endswith((".cpp", ".h")):
            yield os.path.join(base, name)


def check_allowed(problems):
   for path in sources():
      rel = relative(path)
      lines = read(path).split("\n")

      for number, line in enumerate(lines, 1):
         for symbol, allowed in ALLOWED.items():
            if symbol in line and rel not in allowed:
               problems.append("%s:%d: %s outside the audit chokepoints - see AuditTrail.h"
                               % (rel, number, symbol[:-1]))


def check_required(problems):
   for rel, needle, why in REQUIRED:
      path = os.path.join(SERVER, rel.replace("/", os.sep))
      text = read(path) if os.path.exists(path) else ""

      if needle not in text:
         problems.append("%s: '%s' is gone - %s" % (rel, needle, why))


def method_body(text, start):
   """The text of the method whose signature starts at start, by brace depth."""
   open_brace = text.find("{", start)
   if open_brace < 0:
      return ""

   depth = 0
   index = open_brace
   while index < len(text):
      if text[index] == "{":
         depth += 1
      elif text[index] == "}":
         depth -= 1
         if depth == 0:
            return text[open_brace:index + 1]
      index += 1

   return text[open_brace:]


def check_ini_store(problems):
   rel = "Common/Application/IniSettingStore.cpp"
   path = os.path.join(SERVER, rel.replace("/", os.sep))
   if not os.path.exists(path):
      problems.append("%s: is gone - the [Settings] store no longer records anything" % rel)
      return

   text = read(path)

   for function in INI_STORE_RECORDS:
      match = re.search(r"^\s*" + re.escape(function) + r"\s*\(", text, re.M)
      if match is None:
         problems.append("%s: %s is gone - an administrative write of hm_inisettings that is not recorded" % (rel, function))
         continue

      if "RecordChange_(" not in method_body(text, match.end()):
         line = text.count("\n", 0, match.start()) + 1
         problems.append("%s:%d: %s writes hm_inisettings without RecordChange_, so the change it makes leaves no audit row"
                         % (rel, line, function))

   for function in INI_STORE_CANNOT_RECORD:
      match = re.search(r"^\s*" + re.escape(function) + r"\s*\(", text, re.M)
      if match is None:
         continue

      if "RecordChange_(" in method_body(text, match.end()):
         line = text.count("\n", 0, match.start()) + 1
         problems.append("%s:%d: %s calls RecordChange_, but it runs where no audit actor is installed, so the call records nothing - "
                         "carry the actor to that thread first, and change docs/AlertsAndAuditTrail.md and INI_STORE_CANNOT_RECORD with it"
                         % (rel, line, function))


def check_com(problems):
   seen = set()

   for name in sorted(os.listdir(COM)):
      if not name.endswith(".cpp"):
         continue

      path = os.path.join(COM, name)
      text = read(path)

      for match in COM_METHOD.finditer(text):
         method = "%s::%s" % (match.group(1), match.group(2))
         seen.add(method)
         if method in EXEMPT:
            continue

         body = method_body(text, match.end())
         if not persists(body):
            continue

         if any(marker in body for marker in INSTALLS):
            continue

         line = text.count("\n", 0, match.start()) + 1
         problems.append("COM/%s:%d: %s persists something without installing the audit actor - ask "
                         "GetIsServerAdmin/GetIsDomainAdmin, or open an AuditScope(GetAuditActor()), "
                         "or name it in EXEMPT with the reason" % (name, line, method))

   # Until 15 September 2026 seven of these named InterfaceDatabase setters and
   # an InterfaceMessages::Delete that no COM file defines, so the list recorded
   # decisions about methods that did not exist - and the methods that really
   # write [Database], CreateInternalDatabase, CreateExternalDatabase and
   # SetDefaultDatabase, ask GetIsServerAdmin and need no exemption.
   for method in sorted(EXEMPT):
      if method not in seen:
         problems.append("build/check-audit-choke-point.py: EXEMPT names %s, which no file under COM defines - "
                         "remove it, or correct the name" % method)


def check_rest(problems):
   text = read(os.path.join(SERVER, "Common", "Util", "RestApiServer.cpp"))

   # The scope has to be installed BEFORE the dispatch, or the handlers run
   # without it.
   install = text.find("AuditScope auditScope(AuditActorFor_(caller));")
   dispatch = text.find("switch (route.kind)")

   if install < 0 or dispatch < 0 or install > dispatch:
      problems.append("Common/Util/RestApiServer.cpp: the audit scope is not installed before the route dispatch")

   # And exactly once: a second one would mean a second dispatch somewhere that
   # the first does not cover.
   if text.count("AuditScope auditScope(") != 1:
      problems.append("Common/Util/RestApiServer.cpp: the audit scope is installed %d times; there is one dispatch, so there is one install"
                      % text.count("AuditScope auditScope("))


def check_scripts(problems):
   rel = "Common/Scripting/ScriptServer.cpp"
   text = read(os.path.join(SERVER, "Common", "Scripting", "ScriptServer.cpp"))

   # An Active Scripting engine on Windows, and a run of the JavaScript engine the
   # Linux builds embed - each of ScriptBinding's three entry points makes an
   # engine of its own - on the other.
   engines = (text.count("pBasic->Initiate(")
              + text.count("ScriptBinding::RunScript(")
              + text.count("ScriptBinding::FindFunction(")
              + text.count("ScriptBinding::CallHandler("))
   boundaries = text.count("ScriptActorBoundary actors;")

   if engines == 0:
      problems.append("%s: no script engine is started here any more - the scripts rule guards nothing" % rel)
   elif boundaries != engines:
      problems.append("%s: %d script engine(s) are started and %d ScriptActorBoundary scope(s) opened - a script run "
                      "outside one leaves the credential it signed in to COM with on the server thread it ran on"
                      % (rel, engines, boundaries))


def check_table(problems):
   for path in sources():
      rel = relative(path)
      if rel == "Common/Util/AuditTrail.cpp":
         continue

      lower = read(path).lower()
      for marker in ("insert into hm_audit", "update hm_audit", "delete from hm_audit", '"hm_audit"'):
         if marker in lower:
            problems.append("%s: writes hm_audit directly; the chain is AuditTrail's to keep" % rel)
            break


def audited_tables(problems):
   """AuditedTables in AuditTrail.cpp, as (table, line) pairs."""
   rel = "Common/Util/AuditTrail.cpp"
   text = read(os.path.join(SERVER, "Common", "Util", "AuditTrail.cpp"))

   start = text.find("AuditedTables[] =")
   if start < 0:
      problems.append("%s: AuditedTables is gone - the statement chokepoint records nothing" % rel)
      return []

   end = text.find("};", start)
   block = text[start:end]
   first_line = text.count("\n", 0, start) + 1

   entries = []
   for match in re.finditer(r'\{\s*L"([^"]+)"\s*,\s*L"([^"]+)"\s*\}', block):
      entries.append((match.group(1), first_line + block.count("\n", 0, match.start())))

   if not entries:
      problems.append("%s: AuditedTables names no table - the statement chokepoint records nothing" % rel)

   return entries


def created_tables():
   """script name -> the set of tables it creates, lower-cased."""
   created = {}

   for script in CREATE_SCRIPTS:
      path = os.path.join(DBSCRIPTS, script)
      text = read(path) if os.path.exists(path) else ""
      created[script] = set(name.lower() for name in CREATE_TABLE.findall(text))

   return created


def check_tables(problems):
   rel = "Common/Util/AuditTrail.cpp"
   entries = audited_tables(problems)
   created = created_tables()

   for script, names in created.items():
      if not names:
         problems.append("hmailserver/source/DBScripts/%s: defines no table, so the audited table names cannot be checked" % script)
         return

   everywhere = set.intersection(*created.values())
   anywhere = set.union(*created.values())

   listed = set()
   for table, line in entries:
      if table != table.lower():
         problems.append("%s:%d: %s - AuditedTable lower-cases the statement's table name before matching, so a name with a capital in it matches nothing"
                         % (rel, line, table))

      name = table.lower()

      if name in listed:
         problems.append("%s:%d: %s is listed twice" % (rel, line, table))
      listed.add(name)

      if name in RECORDED_ELSEWHERE:
         problems.append("%s:%d: %s is recorded by its own chokepoint with the value it had; listing it records every change to it twice"
                         % (rel, line, table))
         continue

      if name not in anywhere:
         problems.append("%s:%d: %s is not a table any create script defines, so the statement chokepoint matches nothing and the changes it was meant to record are never recorded"
                         % (rel, line, table))
      elif name not in everywhere:
         missing = ", ".join(script for script, names in sorted(created.items()) if name not in names)
         problems.append("%s:%d: %s is not created by %s, so on that backend it is recorded under no name at all"
                         % (rel, line, table, missing))

      if name in NOT_AUDITED:
         problems.append("%s:%d: %s is listed AND named in NOT_AUDITED in build/check-audit-choke-point.py; it is one or the other"
                         % (rel, line, table))

   for table in sorted(anywhere):
      if table in listed or table in NOT_AUDITED:
         continue

      problems.append("hmailserver/source/DBScripts: %s is created but is neither in AuditTrail.cpp's AuditedTables nor in NOT_AUDITED in build/check-audit-choke-point.py - "
                      "a configuration table goes in the first, anything else in the second with the reason" % table)

   for table in sorted(NOT_AUDITED):
      if table not in anywhere:
         problems.append("build/check-audit-choke-point.py: NOT_AUDITED names %s, which no create script defines any more - remove it" % table)


def main():
   problems = []

   check_allowed(problems)
   check_required(problems)
   check_ini_store(problems)
   check_com(problems)
   check_rest(problems)
   check_scripts(problems)
   check_table(problems)
   check_tables(problems)

   if problems:
      print("Audit chokepoint: %d problem(s)" % len(problems))
      for problem in problems:
         print("  " + problem)
      return 1

   print("Audit chokepoint: every administrative change is recorded once, by whoever made it")
   return 0


if __name__ == "__main__":
   sys.exit(main())
