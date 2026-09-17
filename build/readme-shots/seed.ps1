# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# A mailbox worth a screenshot on the bench's uicheck.test account: a dozen
# messages from named people and machines, dated over a fortnight, some read,
# one conversation of three, one with an attachment, and the headers that put
# them under the inbox's tabs (List-Unsubscribe, List-Post, a social network's
# domain, a no-reply address). Raw SMTP, so every header is ours. Run after
# bench.ps1 -Up (which makes the domain and the account).
$ErrorActionPreference = 'Stop'
$domainName = 'uicheck.test'
$to = "self@$domainName"

function Send-Raw([string]$from, [string]$text) {
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 25)
    $stream = $client.GetStream()
    $reader = New-Object IO.StreamReader($stream)
    $writer = New-Object IO.StreamWriter($stream)
    $writer.NewLine = "`r`n"; $writer.AutoFlush = $true
    $null = $reader.ReadLine()
    foreach ($line in @('EHLO bench', "MAIL FROM:<$from>", "RCPT TO:<$to>", 'DATA')) {
        $writer.WriteLine($line); Start-Sleep -Milliseconds 250
        while ($stream.DataAvailable) { $null = $reader.ReadLine() }
    }
    foreach ($line in ($text -split "`n")) { $writer.WriteLine($line.TrimEnd("`r")) }
    $writer.WriteLine('.')
    Start-Sleep -Milliseconds 600
    while ($stream.DataAvailable) { $null = $reader.ReadLine() }
    $writer.WriteLine('QUIT'); Start-Sleep -Milliseconds 200
    $client.Close()
}

function Stamp([int]$daysAgo, [string]$time) {
    $d = (Get-Date).Date.AddDays(-$daysAgo)
    $t = [DateTime]::Parse($time)
    $d.AddHours($t.Hour).AddMinutes($t.Minute).ToString('ddd, dd MMM yyyy HH:mm:ss zzz', [Globalization.CultureInfo]::InvariantCulture) -replace ':(\d\d)$', '$1'
}

$messages = @(
    @{ from = 'grace.hopper@example.org'; name = 'Grace Hopper'; subject = 'COBOL still runs the world'; days = 13; time = '09:12';
       body = "A retrospective on the language that would not die, and what its longevity says about the systems we are building now.`r`n`r`nWorth twenty minutes of your time." },
    @{ from = 'no-reply@calendar.example.com'; name = 'Team Calendar'; subject = 'Reminder: dentist, Tuesday 10:30'; days = 8; time = '07:00';
       body = 'This is an automated reminder. Reply CANCEL to cancel the appointment.' },
    @{ from = 'no-reply@calendar.example.com'; name = 'Team Calendar'; subject = 'Password will expire in 14 days'; days = 7; time = '08:30';
       body = 'Your password expires on 27 September. Change it in Settings before then.' },
    @{ from = 'eve@example.org'; name = 'Eve Online'; subject = 'Photos from the weekend'; days = 6; time = '18:42'; attachment = $true;
       body = 'Twelve pictures, the good ones are near the end.' },
    @{ from = 'alice@example.org'; name = 'Alice Cooper'; subject = 'Re: Design review notes'; days = 4; time = '15:05';
       body = "Agreed on all three. I would add: the message body is in a frame of its own, so the sender's markup never gets our origin.`r`n`r`n> Three things from today's review.." },
    @{ from = 'grace.hopper@example.org'; name = 'Grace Hopper'; subject = 'Weekly: what shipped'; days = 4; time = '11:20';
       body = 'Schema 6039, the header normaliser, CardDAV, the fetch-account routes, and the webmail redesign. Details in the roadmap.' },
    @{ from = 'bookings@heathrow.example.com'; name = 'Heathrow Airport'; subject = 'Your booking LHR-CDG is confirmed'; days = 3; time = '06:15';
       headers = 'Auto-Submitted: auto-generated'; body = 'Flight BA304 departs 07:15 on 20 September. Check in opens 24 hours before departure.' },
    @{ from = 'news@shop.example.com'; name = 'The Corner Shop'; subject = '20% off everything this weekend'; days = 2; time = '10:00';
       headers = "List-Unsubscribe: <mailto:leave@shop.example.com>`r`nPrecedence: bulk"; body = 'Two days only. Every aisle, every shelf. Show this message at the till.' },
    @{ from = 'notification@facebookmail.com'; name = 'Facebook'; subject = 'Carol commented on your photo'; days = 2; time = '19:48';
       headers = 'List-Unsubscribe: <https://www.facebook.com/leave>'; body = 'Carol Danvers commented: "Is that the one from Lisbon?"' },
    @{ from = 'members@list.example.org'; name = 'Dana Scully via Users'; subject = '[users] Anyone running the Linux package on Debian 13?'; days = 1; time = '13:37';
       headers = "List-Id: Users <users.list.example.org>`r`nList-Post: <mailto:users@list.example.org>`r`nList-Unsubscribe: <mailto:users-leave@list.example.org>"; body = 'The .deb wants a Boost the distribution does not ship. Did anyone get past it, or is the AppImage the answer?' },
    @{ from = 'invoices@northwind.example.com'; name = 'Northwind Invoices'; subject = 'Quarterly figures - draft for review'; days = 1; time = '08:15'; attachment = $true; unread = $true;
       body = 'Attached is the draft. The margins on page 3 need a second pair of eyes before Thursday.' },
    @{ from = 'bob@example.org'; name = 'Bob Cratchit'; subject = 'Thread root'; days = 0; time = '09:02'; id = '<root-1@example.org>'; unread = $true;
       body = 'Can we get the numbers together before the call?' },
    @{ from = 'carol@example.org'; name = 'Carol Danvers'; subject = 'Re: Thread root'; days = 0; time = '09:31'; id = '<reply-1@example.org>'; inReplyTo = '<root-1@example.org>'; unread = $true;
       body = "Sounds good, I will get the numbers to you by Thursday and we can go through them on the call.`r`n`r`n> Can we get the numbers together before the call?" },
    @{ from = 'bob@example.org'; name = 'Bob Cratchit'; subject = 'Re: Thread root'; days = 0; time = '09:42'; id = '<reply-2@example.org>'; inReplyTo = '<reply-1@example.org>'; references = '<root-1@example.org> <reply-1@example.org>'; unread = $true;
       body = "Thursday works. I will book the room.`r`n`r`n> Sounds good, I will get the numbers to you by Thursday" }
)

$sent = 0
foreach ($m in $messages) {
    $head = "From: $($m.name) <$($m.from)>`r`nTo: Me <$to>`r`nSubject: $($m.subject)`r`nDate: $(Stamp $m.days $m.time)`r`n"
    if ($m.id) { $head += "Message-ID: $($m.id)`r`n" }
    if ($m.inReplyTo) { $head += "In-Reply-To: $($m.inReplyTo)`r`n" }
    if ($m.references) { $head += "References: $($m.references)`r`n" }
    if ($m.headers) { $head += "$($m.headers)`r`n" }
    if ($m.attachment) {
        $boundary = 'b-' + [Guid]::NewGuid().ToString('N')
        $png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=='
        $text = $head + "MIME-Version: 1.0`r`nContent-Type: multipart/mixed; boundary=`"$boundary`"`r`n`r`n--$boundary`r`nContent-Type: text/plain; charset=utf-8`r`n`r`n$($m.body)`r`n--$boundary`r`nContent-Type: image/png; name=`"figures.png`"`r`nContent-Disposition: attachment; filename=`"figures.png`"`r`nContent-Transfer-Encoding: base64`r`n`r`n$png`r`n--$boundary--`r`n"
    } else {
        $text = $head + "Content-Type: text/plain; charset=utf-8`r`n`r`n$($m.body)`r`n"
    }
    Send-Raw $m.from $text
    $sent++
}
Start-Sleep 4
Write-Host "seeded $sent messages"

# The ones not marked unread are read, as a lived-in inbox is: through IMAP,
# the way a mail program would have left them.
$imap = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 143)
$s = $imap.GetStream(); $r = New-Object IO.StreamReader($s); $w = New-Object IO.StreamWriter($s); $w.NewLine = "`r`n"; $w.AutoFlush = $true
$null = $r.ReadLine()
$w.WriteLine("a1 LOGIN $to Original-Passw0rd!"); Start-Sleep -Milliseconds 400; while ($s.DataAvailable) { $null = $r.ReadLine() }
$w.WriteLine('a2 SELECT INBOX'); Start-Sleep -Milliseconds 400; while ($s.DataAvailable) { $null = $r.ReadLine() }
$unreadSubjects = @($messages | Where-Object { $_.unread } | ForEach-Object { $_.subject })
$i = 0
foreach ($m in $messages) {
    $i++
    if ($m.unread) { continue }
    $w.WriteLine("a$($i + 10) STORE $i +FLAGS (\Seen)"); Start-Sleep -Milliseconds 200; while ($s.DataAvailable) { $null = $r.ReadLine() }
}
$w.WriteLine('a99 LOGOUT'); Start-Sleep -Milliseconds 200; $imap.Close()
Write-Host "marked $($messages.Count - $unreadSubjects.Count) as read"
