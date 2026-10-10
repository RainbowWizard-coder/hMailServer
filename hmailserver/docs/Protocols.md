# The Protocols page

The Control Panel's *Protocols* page (Connections & protocols) holds which of the
SMTP, IMAP and POP3 servers run, what each one accepts and offers, and how long an
idle connection is kept. Each setting carries two sentences on the page - what it
does, and what it does when it is off or at its default; this document keeps the
reasons, which were on the page and pushed its later settings below the fold.

## SMTP

### Refuse a full mailbox during the conversation

*Refuse a full mailbox during the conversation (452 4.2.2)*
(`RejectFullMailboxAtRcpt`, on). With it off, a message for a mailbox that is
already over quota is accepted and then answered with a non-delivery report to
the envelope sender. That report then goes from this server to somebody who sent
nothing, which is backscatter, and it is this server's reputation that pays for
it: the traffic that fills mailboxes fastest is the traffic that forges its
envelope sender. Refusing at RCPT TO with a temporary 452 instead leaves the
message in the sending server's queue, so a legitimate sender's mail waits for the
mailbox to be emptied and is eventually reported by the machine that knows who the
sender really is.

### Sending only as one's own address

*An authenticated session may only send as an address its account owns or has
been granted* (`SmtpAuthenticatedSenderCheck`, off). It is off by default because
nothing constrained MAIL FROM in an authenticated session before it, and an
installation may depend on that. The null sender is always allowed, and
unauthenticated sessions are unaffected.

### Archive retention

*Delete archived messages after (days; 0 = keep for ever)* (`ArchiveRetentionDays`,
0). The archive folder is a raw copy of every message that passes through the
server, and until this setting nothing ever removed anything from it, so on a busy
server it was a directory that only grew, and the administrator who found out was
the one whose disk filled up. The setting prunes it; its default keeps everything,
for the reason the page gives.

### Per-domain counters on /metrics

*Expose per-domain message counters on /metrics* (`MetricsPerDomainEnabled`, off).
Every other counter the metrics endpoint exposes is server-wide, which is what
stops metrics becoming reporting: you can see how much mail the server handled,
never how much any one domain did. It is off by default, and that is a cardinality
decision rather than a cautious one: every label value is a separate time series
in whatever scrapes the endpoint, so an operator hosting several thousand domains
should choose to pay for that rather than find out.
