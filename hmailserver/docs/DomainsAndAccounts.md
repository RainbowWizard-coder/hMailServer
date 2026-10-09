Domains and accounts
====================

The domains this server carries mail for, the mailboxes in each, and the other
addresses a domain answers: what the Control Panel's *Domains & accounts* page
does (*Domains* in the sidebar, under *Accounts & domains*), and the two dialogs
it opens, the domain's and the account's. The Control Deck's *Domains* view and
`/api/v1/domains` do the same things; this document follows the Control Panel.

The page has four cards. The **domains** of the server on the left, with a search
and a form to add one; on the right, the **accounts** of the domain selected, with
a search and a form to add one; and under them that domain's **aliases** and its
**distribution lists**, each with a form to add one. Every row carries its own
buttons: one that opens its dialog - *Properties* on a domain, *Edit* on an
account, the gear on a list, beside the list's *Members* - and one in red that
deletes it after asking; an alias has no dialog. From the keyboard, Enter opens
the selected row's dialog, Delete deletes it after asking, and Shift+F10 opens the
same verbs as a menu.

Adding a domain
---------------

Type the name in **Domain** - `example.com`, with at least one dot - and press
**Add domain**. The domain is created active and selected, so the accounts card
beside it is the new domain's and the next step, an account, is in front of you.
A name the server already has, or one it refuses, is said in the server's words
and nothing is created.

A domain on this server receives mail once the world is told to send it here: its
MX record has to name this server. The *DNS records* page (*Set up this server*)
lists the records each domain should publish - the MX, SPF, DKIM, DMARC, MTA-STS
and TLS-RPT records - and checks whether each one is live, and the *Setup
checklist* says which of them are still missing ([SetupChecklist.md](SetupChecklist.md)).

Only a server administrator adds and deletes domains (`manage_domains`,
[Roles.md](Roles.md)). A domain administrator sees the page with their own domain
alone in the list, and manages its accounts, aliases and lists.

Active and inactive
-------------------

Every domain, account and distribution list can be switched off without being
deleted: *Domain enabled* on the *General* tab of the domain's dialog, *Account
enabled* on the account's, *List is active* in the list's. The page shows
*Inactive* beside a domain or an account that is switched off.

* **An inactive domain** is kept with everything in it, but mail to any of its
  addresses is refused as an unknown recipient ("Domain has been disabled."), and
  nobody can sign in to its accounts, over any protocol.
* **An inactive account** keeps its mailbox; mail to it is refused ("Account is
  not active.") and it cannot sign in.
* **An inactive distribution list** is not expanded; mail to its address is
  refused as an unknown recipient ("Distribution list is not active.").

Switching something off is the way to stop it without losing it; deleting it is
the way to lose it.

Adding an account
-----------------

Select the domain, then fill in the accounts card's form: **Address** (the
mailbox's whole address, `user@example.com`, at the selected domain) and
**Password**, and press **Create account**. **Generate** fills the password box
with a strong random password and copies it to the clipboard; the eye in the box
shows it. The account is created active, with the server's defaults for
everything else; **Edit** on its row opens its dialog, which holds the rest. An
address or a password the server refuses is said in the server's words, and
nothing is created.

Aliases and distribution lists
------------------------------

Both are addresses of the domain that are not mailboxes.

* **An alias** delivers to one other address. Give the **Alias address**
  (`sales@example.com`) and what it **Delivers to** (any address, on this server
  or elsewhere) and press **Add alias**. Mail to the alias goes to the target as
  it is; nobody signs in to an alias.
* **A distribution list** delivers to several. Give the **List address** and
  press **Add list**, then add its members with the *Members* button on its row -
  addresses typed one at a time, or picked from Active Directory. The list's
  dialog (the gear on its row) says who may send to it - anyone, its members, the
  domain's own addresses or announcements only - with an optional moderator and
  bounce address, and can find the rest of its members itself by a **membership
  query**, so that a list such as `everyone@` needs nobody to keep it up to date
  ([DistributionLists.md](DistributionLists.md)). With the list manager on, it is
  a mailing list people join and leave by mail ([MailingLists.md](MailingLists.md)).

An alias for a whole domain - every address at `example.org` delivered to the same
name at `example.com` - is a *domain alias*, on the *Names* tab of the domain's
dialog, not an alias of the page.

The domain's dialog
-------------------

*Properties* on a domain's row. Everything in it applies to that domain alone.

| Tab | What it holds |
|---|---|
| General | Whether the domain is enabled; its name - changing it renames the domain and moves every account, alias and list with it, after asking; the postmaster address, to which mail for an address the domain does not have is redirected; the Active Directory domain of a domain synchronised from one |
| Names | Domain aliases: other names this domain answers to, so that `user@alias` is delivered to `user@` this domain |
| Limits | The largest the domain, a message and a new account may be, how many accounts, aliases and lists it may have, how old a message may grow before it is deleted, the quota policy - when the owner is warned, how far past the limit mail is still accepted, when sending stops ([QuotaPolicy.md](QuotaPolicy.md)) - and the plus-addressing character |
| Signature | A signature added to every message the domain's accounts send, plain text and HTML, and whether it replaces, follows or stands in for the account's own ([Signatures.md](Signatures.md)) |
| Relay | Where mail *from* this domain leaves: a relay host of its own, in place of the server-wide one |
| Out of office | One automatic reply for every account of the domain that has none of its own ([OutOfOffice.md](OutOfOffice.md)) |
| Tagging and disclaimer | The external-sender tag, the first-contact note and the disclaimer ([DomainTransforms.md](DomainTransforms.md)) |
| Impersonation protection | Mail from outside that borrows the name of an account here or of a protected person ([ImpersonationProtection.md](ImpersonationProtection.md)) |
| DKIM | Signing the domain's mail: the selector, the key and the record to publish, and the key's rotation ([DkimKeyRotation.md](DkimKeyRotation.md)) |
| Allow and block list | The addresses and domains the domain's accounts want or do not ([SenderListsAndDigests.md](SenderListsAndDigests.md)) |
| Quarantine digest | The daily message to each account listing what was held for it |
| Reports received | The mailbox the DMARC and TLS-RPT reports for the domain are read from ([DmarcReports.md](DmarcReports.md)) |
| Restricted delivery | The only domains its accounts may exchange mail with ([RestrictedDelivery.md](RestrictedDelivery.md)) |
| Country blocking | Who may sign in to its accounts, by country ([CountryBlocking.md](CountryBlocking.md)) |
| OpenPGP | Whether its accounts' public keys are published ([OpenPGP.md](OpenPGP.md)) |
| Webhooks | Whether the domain's events reach the outbound webhooks ([Webhooks.md](Webhooks.md)) |
| Levels | The domain's own values of the settings that have levels ([SettingLevels.md](SettingLevels.md)) |

The account's dialog
--------------------

*Edit* on an account's row.

| Tab | What it holds |
|---|---|
| General | Whether the account is enabled; the address - changing it renames the mailbox, and *Move or rename...* moves it to another domain or keeps the old address ([MailboxMove.md](MailboxMove.md)); the administration level (a normal user, the domain's administrator or the server's); the quota; how old a message may grow; restricted delivery; the person's name; a new password; the last sign-in; a zero-access mailbox ([ZeroAccess.md](ZeroAccess.md)) |
| Forwarding | An address the account's mail is forwarded to |
| Auto-reply | The account's vacation message, its dates, and another for colleagues ([OutOfOffice.md](OutOfOffice.md)) |
| Spam | The account's own spam thresholds, which apply to its copy at delivery |
| Signature | The account's signature, plain text and HTML ([Signatures.md](Signatures.md)) |
| Sieve | The account's Sieve filter script |
| External | Mailboxes elsewhere this account's mail is collected from, over POP3 or IMAP ([Migration.md](Migration.md)) |
| App passwords | Passwords for one mail program each, which can be revoked one at a time |
| Two-factor | The account's second factor |
| Recovery | How a forgotten password is recovered ([AccountRecovery.md](AccountRecovery.md)) |
| Rules | The account's own mail flow rules ([MailFlowRules.md](MailFlowRules.md)) |
| Folders | The mailbox's IMAP folders, emptying and unlocking it, and the person's data - everything exported as one file, or every trace erased ([PersonalData.md](PersonalData.md)) |
| Windows directory | A Windows or Active Directory account that checks the password in place of this server |
| Directory entry | What the organisation's directory shows beside the person's name ([Directory.md](Directory.md)) |
| Identity | A person, a room or a piece of equipment ([RoomsAndEquipment.md](RoomsAndEquipment.md)), and the identity provider's ID for provisioning ([Scim.md](Scim.md)) |
| Levels | The account's own values of the settings that have levels ([SettingLevels.md](SettingLevels.md)) |

Deleting
--------

Each delete asks first, naming what goes, with Cancel the answer Enter gives,
and none can be undone. Deleting a domain deletes every account in it - the
question counts them - with their mailboxes and messages, and every alias and
list of the domain. Deleting an account deletes its mailbox and every message in
it. A backup taken before is the only way back ([Backup.md](Backup.md)).

Public folders and groups, which are the server's and not a domain's, are on
their own pages beside this one ([PublicFoldersAndGroups.md](PublicFoldersAndGroups.md)).
