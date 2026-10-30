Learning from the Junk folder
=============================

When a user moves a message into their Junk folder, they are saying it is spam;
when they move one out of it, that it is not. This server can tell the spam
filter so, every time, without a script, a mailbox scan or a second program:
SpamAssassin, as it has since `SpamAssassinLearnOnMove` came, or Rspamd, through
its controller. One filter learns at a time, the one `MoveLearningBackend` names.

**Nothing is taught until you switch it on.** The backend is SpamAssassin by
default, and SpamAssassin is taught only while the `SpamAssassinLearnOnMove` key
of `hMailServer.ini` is 1 (it is 0 as shipped). Rspamd is taught only once
`MoveLearningBackend` is `rspamd` and its controller's address and a classifier
are set. A server that changes none of these learns nothing, and a server that
had `SpamAssassinLearnOnMove` on before this release learns exactly as it did:
the same moves, the same requests to spamd, the same user.

Which moves teach
-----------------

A lesson is a move between two of one account's own folders, into its Junk
folder (spam) or out of it into any other folder (ham, "not spam"):

| What the user does | Lesson |
|---|---|
| IMAP `MOVE` or `UID MOVE` from any folder into Junk | spam |
| IMAP `MOVE` or `UID MOVE` from Junk into any other folder | ham |
| IMAP `COPY` or `UID COPY` into or out of Junk | spam or ham, while `MoveLearningFromCopy` is on (the default, as it always did); nothing while it is off |
| The webmail's Junk button, or its move out of Junk | spam or ham |
| A move between two folders neither of which is Junk | nothing |
| A move from Junk to Junk (another folder designated so cannot exist; see below) | nothing |
| Deleting a message, `STORE +FLAGS (\Deleted)`, `EXPUNGE`, emptying Junk | nothing: nothing moved anywhere |
| A `COPY`, then a `STORE` of `\Deleted`, then an `EXPUNGE` | the `COPY` is a lesson by `MoveLearningFromCopy`; the other two are separate commands, and nothing joins them into a move |
| A move into or out of a public folder | nothing |
| A move between two accounts' folders | nothing |
| A command that fails, part way or whole | nothing: its copies are taken out again, and so are its lessons |

A `COPY` leaves the message where it was as well. Older clients move with a
`COPY` followed by a `STORE` and an `EXPUNGE`, which is why a `COPY` into Junk
teaches by default; a server whose clients all speak `MOVE` (RFC 6851) can turn
`MoveLearningFromCopy` off, so that only a move is taken as a judgement. The
setting governs both backends.

A lesson is given only after the command has succeeded and its work is done -
for a `MOVE`, after the originals have been expunged - and never on the IMAP
command's own path: the command queues it and answers the client, and the
lesson goes to the filter afterwards, from the server's asynchronous work queue,
outside every folder lock and transaction. A filter that is slow or down never
makes an IMAP command slower or makes it fail.

### Whose lesson it is

The lesson is the mailbox owner's: the account that owns both folders. It names
that owner - never the sender, nor anything in the message.

- **Rspamd** is taught only when the account signed in to the session owns both
  folders. A user moving messages in another account's mailbox that is shared or
  delegated to them teaches nobody: the judgement would be the delegate's and the
  statistics the owner's. The IMAP master user, signing in to an account's
  mailbox (`ImapMasterUser`), is signed in to that account, and its moves are that
  account's.
- **SpamAssassin** keeps the rule it has had since it came: both folders one
  account's, and the lesson that account's - for spamd's user, the owner when
  `SpamAssassinUserFromRecipient` is on - whoever moved the message.

### Which folder is Junk

The Junk folder is the one `LIST` marks `\Junk` (RFC 6154), resolved as it is
for every client: a folder explicitly designated `\Junk` - by `CREATE ... (USE
(\Junk))`, or on the Control Panel - wherever it is in the tree, and when none
is, the first top-level folder named, in this order and in any case, `Junk`,
`Junk E-mail`, `Junk Email` or `Spam`. An account has one Junk folder at most.

So when a mailbox has both a `Junk` and a `Spam` folder and neither is
designated, **`Junk` is the Junk folder and `Spam` is an ordinary folder**: a
move into `Spam` teaches nothing, and a move from `Junk` into `Spam` teaches ham.
When `Spam` is designated `\Junk`, `Spam` is the Junk folder and `Junk` is the
ordinary one. A client that files spam into `Spam` beside an undesignated `Junk`
is best fixed by designating `Spam`; nothing else recognises a second spam
folder, so that one move can never be taken two ways.

### A mailbox encrypted at rest

A message of a mailbox encrypted at rest (EncryptionAtRest.md) is never sent out
of the server in the clear: a move of one teaches nothing, for either backend,
and the application log says so.

How lessons run, and what can be lost
-------------------------------------

| | SpamAssassin | Rspamd |
|---|---|---|
| Queue | the asynchronous work queue, a task per lesson, as before | a queue of its own of at most 1,000 lessons, sent in order, one at a time, by one task of the asynchronous work queue |
| A full queue | as before | the command's newest lessons are dropped, with one line in the application log for the command; the client is never made to wait |
| Bound on one lesson | spamd's connection's own session ceiling | `RspamdTimeoutSeconds` (15) for the whole request: the name's lookup, the connection, TLS, sending and the answer |
| Retries | none | none |
| A restart of the service | lessons waiting are lost | lessons waiting are lost |

Every Rspamd lesson's outcome is one line in the application log - the message's
id, the owner, spam or ham, the classifier, and what Rspamd said - and never the
password or any of the message. The misconfigurations an administrator must put
right are also in the error log, once in ten minutes each:

| Code | What it means |
|---|---|
| HM8580 | `MoveLearningBackend` is Rspamd, and `RspamdControllerUrl` is empty or not an address this server sends to |
| HM8581 | `MoveLearningBackend` is Rspamd, and `RspamdClassifier` names no classifier |
| HM8582 | the controller refused the password (`RspamdControllerPassword`) |
| HM8583 | the controller answered not found: no such classifier ("class 'spam' is not defined in classifier ..."), or the address is not Rspamd's controller |

A lesson queued before the backend was changed is let go, not sent to the new
one.

Rspamd
------

Each lesson is one request to Rspamd's controller (its worker on port 11334 by
default), as Rspamd's protocol documents it: `POST /learnspam` or `POST
/learnham`, the whole message as stored (RFC 5322) as the body, and four
headers:

| Header | Value |
|---|---|
| `Password` | `RspamdControllerPassword`, when one is set |
| `Classifier` | `RspamdClassifier` |
| `Deliver-To` | the mailbox owner's full address |
| `Rcpt` | the same address, as the request's only envelope recipient |

### What to configure on the Rspamd side

**A classifier with per-user statistics.** Rspamd keeps a classifier's
statistics for each user when its configuration says `per_user = true`, and it
takes the user from the request's principal recipient: the `Deliver-To` header
first, then the envelope recipients (`Rcpt`), then the message's own recipient
headers. A lesson carries the owner in the first two, so a user's moves teach that
user's statistics and the message's `To:` never chooses whose. A per-user
classifier needs the Redis backend. For example, with Rspamd 3:

```
# /etc/rspamd/local.d/classifier-bayes.conf
backend = "redis";
servers = "127.0.0.1:6379";
per_user = true;
name = "bayes_user";
```

and `RspamdClassifier` set to `bayes_user`. The name is what the `Classifier`
header names; set one, since Rspamd requires a name wherever there is more than
one classifier. A message is then scanned against the statistics of its
recipient, which Rspamd learns from that recipient's own moves. Rspamd keeps each
user's statistics in Redis under `RS` followed by the address - the hash
`RSowner@example.com`, whose `learns_spam` and `learns_ham` count the lessons -
and the classes it learned each message as in `learned_ids`.

**The global classifier is never taught by a user's move unless you name it.**
Without a `Classifier` header Rspamd teaches every classifier it has, so this
server never sends a lesson without one: with `RspamdClassifier` empty nothing is
taught (HM8581). If you do want every user's moves to teach the whole server,
name the global classifier - that is a choice this server leaves to you.

**The controller's password.** Learning is one of the controller's privileged
commands: Rspamd checks the `Password` header against the controller's
`enable_password`, or against its `password` when there is no
`enable_password`:

```
# /etc/rspamd/local.d/worker-controller.inc
password = "$2$...";           # rspamadm pw
enable_password = "$2$...";    # rspamadm pw; the one a lesson must carry
```

The controller configuration Rspamd ships has a password of its own: set yours.
Rspamd lets a request from one of its `secure_ip` addresses - `127.0.0.1` and
`::1` by default - in without a password. A controller on the same machine as
this server is reached from there; if you would rather every lesson proved
itself, remove this server's address from `secure_ip` and set the password here.

**Never expose the controller to the internet.** It can be reconfigured with the
enable password, and anybody who can reach it can teach it. Keep it on the
loopback or on a network only the mail servers reach. This server sends to it
over `https` - its certificate verified against the machine's roots with the host
name checked, as every HTTPS client of this server verifies, with no switch to
turn that off - or over plain `http` to this machine only (`localhost`,
`127.0.0.0/8`, `::1`), since the password and every message taught would
otherwise cross the network in the clear. A controller on another machine is put
behind a TLS reverse proxy, and `RspamdControllerUrl` may carry the path it is
served under (`https://rspamd.example.net/controller`). The request never goes
through `HttpProxy`, and never to a link-local or cloud metadata address.

### Repeated lessons and corrections

Rspamd keeps, for each user, which class it learned each message as (its learn
cache, in Redis). A message taught again as the same class - moved out of Junk
and back in - is answered "has been already learned as spam, ignore it" and
changes nothing; the application log says "already learned as that, nothing
changed", and nothing is retried. A message taught as the other class - a
correction, moved out of Junk after it was moved in - is unlearned from the first
class and learned as the second, by Rspamd, in the same request, so one message
never counts as both. Rspamd answers a lesson it declines - its learn conditions,
or a message with too few or too many tokens for the classifier - with 204, which
is logged as declined.

A message larger than 50 MB, Rspamd's own default `max_message`, is not sent.

SpamAssassin
------------

Unchanged: while `SpamAssassinLearnOnMove` is 1 and SpamAssassin is on, a lesson
is spamc's `TELL` to the spamd on `SpamAssassinHost` and `SpamAssassinPort`,
`Message-class: spam` or `ham`, `Set: local`, the Bayes store spamd's verdicts
come from, as the user `SpamAssassinUser` names, or the mailbox owner while
`SpamAssassinUserFromRecipient` is on. spamd refuses `TELL` unless it was started
with `--allow-tell`. The two `[Settings]` keys stay in `hMailServer.ini` until
the server's other keys move to the database with them; every setting this
release adds is in the database.

The settings
------------

| Setting (`hm_settings`) | COM (`Settings.AntiSpam`) | REST (`/api/v1/settings/antispam`) | Default |
|---|---|---|---|
| `MoveLearningBackend` | `MoveLearningBackend`: 0 SpamAssassin, 1 Rspamd | `move_learning_backend`: `spamassassin` or `rspamd` | SpamAssassin |
| `MoveLearningFromCopy` | `MoveLearningFromCopy` | `move_learning_from_copy` | on |
| `RspamdControllerUrl` | `RspamdControllerUrl` | `rspamd_controller_url` | empty |
| `RspamdControllerPassword` | `SetRspamdControllerPassword(password)`, write-only | `rspamd_controller_password`, write-only | empty |
| `RspamdClassifier` | `RspamdClassifier` | `rspamd_classifier` | empty |
| `RspamdTimeoutSeconds` | `RspamdTimeoutSeconds`, 1 to 300 | `rspamd_timeout_seconds`, 1 to 300 | 15 |

All six are on the Control Panel's Anti-spam settings page, Content filters tab,
in the card *Learning from the Junk folder*; the Control Deck draws them from the
REST schema. Each takes effect at the next move. The password is stored
protected (SecretsAtRest.md), never read back over COM or REST, never logged and
left out of the backup archive: after a restore, enter it again. The address may
not carry a user name, a password, a query or a fragment.

Where it lives
--------------

| | |
|---|---|
| The rule | `hmailserver/source/Server/Common/AntiSpam/MoveLearning.h` and `.cpp` |
| Rspamd | `hmailserver/source/Server/Common/AntiSpam/RspamdLearner.h` and `.cpp` |
| SpamAssassin | `hmailserver/source/Server/Common/AntiSpam/SpamAssassin/SpamAssassinLearner.h` and `.cpp` |
| The IMAP commands | `IMAPCopy.cpp`, `IMAPMove.cpp`, and the post-commit dispatch in `IMAPMoveDispatch.cpp` |
| Tests | `MoveLearningTester` (`move-learning-tests` and `rspamd-learn-tests` on Linux, `ClassTester` on Windows); `RegressionTests.AntiSpam.SpamAssassinLearning` and `RspamdLearning`; on Linux `AntiSpam.RspamdLive` against a real Rspamd, when `HMTEST_RSPAMD_PASSWORD` names its controller's password |
