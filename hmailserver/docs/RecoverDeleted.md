Recover deleted items
=====================

Outlook's *Recover items deleted from this folder*: a message a person deletes is kept
aside for a number of days, and they can put it back themselves, without asking an
administrator to restore a backup.

It is off by default. `recover_deleted_days` on `PUT /api/v1/settings` (0 to 365) turns
it on for the whole server; the copies are kept for that many days.

What is kept
------------

A message is kept when a **person deletes it**:

- IMAP `EXPUNGE`, `UID EXPUNGE`, and `CLOSE` on a folder with messages marked `\Deleted`;
- POP3 `DELE`, which takes effect at `QUIT`;
- the webmail's delete, and its emptying of a folder;
- the retention the person chose for their Trash, Junk and Inbox, and the
  administrator's retention policy.

A message is **not** kept when it is removed for any other reason: an IMAP `MOVE` (the
original is removed once it has been copied), a scheduled draft that has been sent, a
message still in the delivery queue, or an account being deleted. The places a person
deletes mail mark the delete as theirs (`RecoverableStore::Scope`), and nothing else is
kept, so the list shows what was deleted and not what was moved.

A client that moves mail the old way - `COPY`, then `\Deleted` and `EXPUNGE` - does
delete the original, and it is listed; putting it back makes a second copy beside the
one that was moved.

Putting it back
---------------

**Recover deleted items** in the webmail - on the Trash folder's menu, and in the
command palette - lists what the person deleted within the window, newest first, with
its subject, sender, the folder it was in, when it was deleted and its size. **Put back**
returns it to that folder with its flags (read, flagged and the rest); when the folder
has since been deleted it goes to a folder of the same path, and otherwise to the INBOX.
It is a new message with a new UID, and every open IMAP session is told. **Delete for
good** removes it now.

Where the copies are
--------------------

Each copy is a file under `<data directory>/Recoverable/<account id>/`, with a row in
`hm_recoverable` (schema 6068). They count against no quota, and they are not in the
backup: a backup carries what is in the mailboxes. The retention task - six-hourly, or
`Utilities.RunMessageRetention` on demand - ends the copies older than the window, all
of them when the setting is 0, and the directory of an account that no longer exists.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/recoverable` | `{items:[{id,folder,folder_id,subject,from,size,seen,deleted}],days}`; `deleted` in seconds since 1970. |
| `POST /api/v1/me/recoverable/{id}/restore` | Puts it back: `{folder_id, message_id}`. |
| `DELETE /api/v1/me/recoverable/{id}` | Gone for good. |
