Public folders and groups
=========================

Shared IMAP folders that several accounts open, and the security groups that let
one permission reach several accounts at once: what the Control Panel's *Public
folders* and *Groups* pages do (*Accounts & domains* in the sidebar). Both belong
to the server rather than to one domain, so both are the server administrator's;
a domain's accounts, aliases and lists are on the *Domains & accounts* page
([DomainsAndAccounts.md](DomainsAndAccounts.md)).

Public folders
--------------

A public folder is an IMAP folder that no one account owns. A mail program shows
it under the public folder name the server gives the shared tree (*Public folder
name*, on the *Protocols* page's *IMAP* tab), to the accounts its permissions let
see it. Messages reach a public folder when somebody with the right copies, moves
or appends one into it.

The page lists the folders as a tree, each under the folder it is in.

* **Add folder** asks for a name and creates the folder. A folder inside another
  is named with the IMAP hierarchy delimiter between the two names - `Sales.2026`
  makes `2026` inside `Sales`, and `Sales` as well if it is not there yet, where
  the delimiter is a full stop - and the delimiter is *Folder hierarchy
  delimiter*, on the same *IMAP* tab. With a folder selected, the name starts
  with that folder's name and the delimiter, so that what is typed goes inside
  it. A name that begins or ends with the delimiter or has two together, a name
  of more than 255 characters, a folder more than 25 levels down, or one that is
  there already is refused under the box, and what was typed stays.
* A folder whose own name holds the delimiter - one at the top named
  `Sales.2026`, which Add folder made of such a name before 6.3.6 - is pointed
  out above the list: a mail program finds a folder by splitting its name at the
  delimiter, so it cannot open that one. Add folder with the same name makes the
  sub-folder the name spells.
* **Permissions** opens the folder's access-control list (below): who may see the
  folder, read it and change it.
* **Delete** deletes the folder, the folders inside it and every message in
  them, after asking - the question says how many sub-folders go with it - with
  Cancel the answer Enter gives. There is no undo; a backup carries the public
  folders ([Backup.md](Backup.md)).

From the keyboard, Enter or F2 - or a double-click - opens the selected folder's
permissions, Delete deletes it after asking and Insert adds one.

Permissions
-----------

Each entry of a folder's list gives rights to one of three: an **account**, by its
address; a **group**, by its name, which gives the rights to every account in the
group; or **anyone**, every account of the server. *Add*, *Edit* and *Remove* work
on the list, and each entry's row names its rights. The rights are IMAP's (RFC
4314):

| Right | Lets the account |
|---|---|
| Lookup | See the folder in its folder list |
| Read | Open the folder and read its messages |
| Seen | Set or clear the read mark of the folder's messages, which opening a message also sets; without it, reading leaves the mark as it was. A message in a public folder has one read mark, the same for every account that opens the folder |
| Flags | Set the other flags - flagged, answered, a keyword |
| Insert | Copy or append a message into the folder |
| Post | Post to the folder - IMAP's `p` right, which on an account's INBOX is also the right to send as that account ([SendAs.md](SendAs.md)) |
| Create | Make sub-folders inside it |
| Delete folder | Delete the folder |
| Delete messages | Mark its messages deleted |
| Expunge | Remove the messages marked deleted |
| Administer | Change the folder's permissions |

A folder an account can read but not *Lookup* is one it never sees in a mail
program, so the two are given together.

Groups
------

A group is a name and a list of accounts, from any domain of the server. It
exists so that several accounts can be given the same rights at once: a public
folder's permissions name the group, a calendar is shared with it, and every
account listed in it gets what it is given. An account added to the group later
gets the rights from then on; one removed loses them.

* **Add** names a new group; **Edit** renames it.
* **Members...**, the list's first verb, opens the group's list of accounts; Enter
  and a double-click on a group open it too, and F2 renames the group. In it,
  *Add account...* lists the server's accounts that are not members yet, under a
  search box, *Find an account*, that narrows the list by address as it is typed
  in. Ctrl and Shift choose several, and *Add* - or Enter, which in the search
  box takes the one account the search leaves, or a double-click on an account -
  puts them in the group. *Remove* takes the member selected off the list, after
  asking. A group with no members grants nothing to anyone.
* **Delete** deletes the group after asking. The folder permissions and the
  calendar shares given to it are removed with it, so its members lose them.

From the keyboard, Delete deletes the selected group after asking and Insert adds
one.

A group is the server's way of sharing, and is not a mailing address: mail cannot
be sent to a group. An address that delivers to several accounts is a
distribution list ([DistributionLists.md](DistributionLists.md)).
