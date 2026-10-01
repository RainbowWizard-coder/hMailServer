# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Bring the bench's REST listener up on 127.0.0.1:9530 with a throwaway domain
# and account for a browser check of the portal, the way the self-service
# fixture does (ini RestApiPort + Application.Reinitialize), and take it all
# down again afterwards.  -Up | -Down
param([switch]$Up, [switch]$Down, [switch]$Seed, [switch]$Seed4, [switch]$Seed6)
$ErrorActionPreference = 'Stop'
# Join-Path one child at a time: Windows PowerShell 5.1, which runs this as
# "powershell -File", takes no third positional argument.
$repo = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
$ini = Join-Path $repo 'hmailserver\source\Server\hMailServer\x64\Release\hMailServer.ini'
$domainName = 'uicheck.test'

$app = New-Object -ComObject hMailServer.Application
$null = $app.Authenticate('Administrator', 'testar')

function Set-Ini([string]$key, [string]$value) {
    $app.Settings.SetIniSetting($key, $value)
}

if ($Up) {
    $existing = $null
    try { $existing = $app.Domains.ItemByName($domainName) } catch {}
    if ($existing) { $existing.Delete() }
    $d = $app.Domains.Add(); $d.Name = $domainName; $d.Active = $true; $d.Save()
    $a = $d.Accounts.Add(); $a.Address = "self@$domainName"; $a.Password = 'Original-Passw0rd!'; $a.Active = $true; $a.Save()
    $al = $d.Aliases.Add(); $al.Name = "sales@$domainName"; $al.Value = "self@$domainName"; $al.Active = $true; $al.Save()
    Set-Ini 'RestApiBindAddress' '127.0.0.1'
    Set-Ini 'RestApiPort' '9530'
    Set-Ini 'RestApiCertificateFile' ''
    Set-Ini 'RestApiPrivateKeyFile' ''
    $app.Reinitialize()
    Start-Sleep 2
    $r = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:9530/portal' -TimeoutSec 15
    Write-Host ("portal answers {0}, {1} bytes" -f $r.StatusCode, $r.Content.Length)
}
if ($Seed) {
    # A mailbox with something in it: one message from outside, one from a
    # colleague, and a conversation of three (a root and two replies).
    $smtp = New-Object System.Net.Mail.SmtpClient('127.0.0.1', 25)
    $send = {
        param($from, $subject, $body, $messageId, $inReplyTo, $references)
        $m = New-Object System.Net.Mail.MailMessage
        $m.From = $from; $m.To.Add("self@$domainName"); $m.Subject = $subject; $m.Body = $body
        if ($messageId) { $m.Headers.Add('Message-ID', $messageId) }
        if ($inReplyTo) { $m.Headers.Add('In-Reply-To', $inReplyTo) }
        if ($references) { $m.Headers.Add('References', $references) }
        $smtp.Send($m); $m.Dispose()
    }
    & $send 'alice@example.com' 'From outside' 'Hello from outside.' '<outside-1@example.com>' $null $null
    & $send "colleague@$domainName" 'From a colleague' 'Hello from inside.' "<inside-1@$domainName>" $null $null
    & $send 'bob@example.org' 'Thread root' 'The root of the conversation.' '<root@example.org>' $null $null
    Start-Sleep 1
    & $send 'carol@example.org' 'Re: Thread root' 'The first reply.' '<reply-1@example.org>' '<root@example.org>' '<root@example.org>'
    Start-Sleep 1
    & $send 'bob@example.org' 'Re: Thread root' 'The second reply.' '<reply-2@example.org>' '<reply-1@example.org>' '<root@example.org> <reply-1@example.org>'
    $smtp.Dispose()
    Start-Sleep 4
    Write-Host 'seeded 5 messages'
}
if ($Seed4) {
    # An HTML message shaped like a phish - a link that says paypal.com and
    # goes elsewhere, a remote tracking image - and a message with a picture.
    $smtp = New-Object System.Net.Mail.SmtpClient('127.0.0.1', 25)
    $m = New-Object System.Net.Mail.MailMessage
    $m.From = 'phisher@example.com'; $m.To.Add("self@$domainName"); $m.Subject = 'Your account needs attention'
    $m.IsBodyHtml = $true
    $m.Body = '<p>Please sign in at <a href="https://evil.example/login">https://www.paypal.com/signin</a> today.</p><img src="https://tracker.example.org/pixel.gif" alt="">'
    $smtp.Send($m); $m.Dispose()
    $png = [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==')
    $tmp = Join-Path $env:TEMP 'hm-ui-pixel.png'; [IO.File]::WriteAllBytes($tmp, $png)
    $m = New-Object System.Net.Mail.MailMessage
    $m.From = 'friend@example.org'; $m.To.Add("self@$domainName"); $m.Subject = 'Picture from the weekend'; $m.Body = 'A picture attached, as promised.'
    $a = New-Object System.Net.Mail.Attachment($tmp, 'image/png'); $m.Attachments.Add($a)
    $smtp.Send($m); $m.Dispose(); $a.Dispose()
    $smtp.Dispose()
    Start-Sleep 4
    Write-Host 'seeded the phish and the picture'
}
if ($Seed6) {
    # A list message whose List-Unsubscribe names a mailto on the bench, and
    # the account it names, so the unsubscribe can be seen to arrive.
    $d = $app.Domains.ItemByName($domainName)
    $l = $d.Accounts.Add(); $l.Address = "leave@$domainName"; $l.Password = 'Original-Passw0rd!'; $l.Active = $true; $l.Save()
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 25)
    $stream = $client.GetStream(); $reader = New-Object IO.StreamReader($stream); $writer = New-Object IO.StreamWriter($stream); $writer.NewLine = "`r`n"; $writer.AutoFlush = $true
    $null = $reader.ReadLine()
    foreach ($line in @('EHLO bench', 'MAIL FROM:<news@example.com>', "RCPT TO:<self@$domainName>", 'DATA')) { $writer.WriteLine($line); Start-Sleep -Milliseconds 300; while ($stream.DataAvailable) { $null = $reader.ReadLine() } }
    $writer.WriteLine("From: news@example.com"); $writer.WriteLine("To: self@$domainName"); $writer.WriteLine('Subject: Weekly letter'); $writer.WriteLine("List-Unsubscribe: <mailto:leave@${domainName}?subject=Unsubscribe%20me>"); $writer.WriteLine(''); $writer.WriteLine('The weekly letter.'); $writer.WriteLine('.')
    Start-Sleep -Milliseconds 800; while ($stream.DataAvailable) { $null = $reader.ReadLine() }
    $writer.WriteLine('QUIT'); Start-Sleep -Milliseconds 300; $client.Close()
    Start-Sleep 3
    Write-Host 'seeded the list message and the leave account'
}
if ($Down) {
    $existing = $null
    try { $existing = $app.Domains.ItemByName($domainName) } catch {}
    if ($existing) { $existing.Delete(); Write-Host "domain $domainName removed" }
    Set-Ini 'RestApiPort' '0'
    $app.Reinitialize()
    Write-Host 'REST listener off'
}
