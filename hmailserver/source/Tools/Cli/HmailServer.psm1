# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   The hMailServer PowerShell module: the REST API with PowerShell's own grammar
   in front of it.

   build/hmconfig.ps1 came first and does a different job - it exports the whole
   configuration as one reviewable JSON document and applies it somewhere else -
   and it talks COM, so it is Windows-only and needs the Control Panel's world.
   This is the other half: per-object verbs over HTTP, so it works against a
   server on another machine, against the Linux build, and from a session that
   has never heard of COM.

       Connect-HmServer -Url https://mail.example.com:8045
       Get-HmDomain
       New-HmAccount -Domain example.com -Address ann@example.com -Password (Read-Host -AsSecureString)
       Set-HmSetting -Group antispam -Property @{ use_spf = $true }
       Get-HmQueue | Where-Object tries -gt 3

   Every function answers objects, not text, because that is what a PowerShell
   user will do something with. Nothing here formats anything.

   The session - the address and the credential - is held in the module for the
   life of it, and every function takes -Url, -Password and -ApiKey of its own
   for a one-off call against another server.

   hmctl beside this file is the same vocabulary for a shell that is not
   PowerShell; the two are checked against one recorded server by
   build/check-cli.py, so they cannot drift apart in what they ask for.
#>

Set-StrictMode -Version Latest

$script:Session = $null

# ---------------------------------------------------------------------------
# The session
# ---------------------------------------------------------------------------

function Connect-HmServer {
    <#
    .SYNOPSIS
        Remembers which server to administer, and how to prove who you are.
    .DESCRIPTION
        Either an administrator password or an API key. Nothing is sent by this
        function: the credential is held and used by the first call that needs
        it, so a wrong password is reported by the call that fails, not here.

        -Insecure accepts a certificate nobody verified, and is refused for any
        host but the loopback: sending an administrator password to a remote
        host over an unverified connection is not a convenience.
    .EXAMPLE
        Connect-HmServer -Url https://mail.example.com:8045
    .EXAMPLE
        Connect-HmServer -Url https://localhost:8045 -Insecure -ApiKey $key
    #>
    [CmdletBinding(DefaultParameterSetName = 'Password')]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Url,

        [Parameter(ParameterSetName = 'Password')]
        [System.Security.SecureString] $Password,

        [Parameter(Mandatory = $true, ParameterSetName = 'ApiKey')]
        [string] $ApiKey,

        [switch] $Insecure,

        [int] $TimeoutSeconds = 30
    )

    $uri = [System.Uri] $Url

    if ($Insecure -and $uri.Host -notin @('localhost', '127.0.0.1', '::1')) {
        throw "-Insecure is refused for $($uri.Host): it is allowed for the loopback only, because it would send the credential to a host nobody verified. Give the server a certificate the client trusts."
    }

    if ($PSCmdlet.ParameterSetName -eq 'Password' -and -not $Password) {
        $Password = Read-Host -Prompt "Administrator password for $Url" -AsSecureString
    }

    $script:Session = [pscustomobject] @{
        Url            = $Url.TrimEnd('/')
        Password       = $Password
        ApiKey         = if ($PSCmdlet.ParameterSetName -eq 'ApiKey') { $ApiKey } else { $null }
        Insecure       = [bool] $Insecure
        TimeoutSeconds = $TimeoutSeconds
    }

    $script:Session
}

function Disconnect-HmServer {
    <#
    .SYNOPSIS
        Forgets the server and the credential held for it.
    #>
    [CmdletBinding()]
    param()
    $script:Session = $null
}

function Get-HmSession {
    <#
    .SYNOPSIS
        The session Connect-HmServer is holding, without the credential's value.
    #>
    [CmdletBinding()]
    param()
    if (-not $script:Session) { return $null }
    [pscustomobject] @{
        Url        = $script:Session.Url
        Credential = if ($script:Session.ApiKey) { 'API key' } elseif ($script:Session.Password) { 'administrator password' } else { 'none' }
        Insecure   = $script:Session.Insecure
    }
}

# ---------------------------------------------------------------------------
# The one place that talks to the server
# ---------------------------------------------------------------------------

function Invoke-HmApi {
    <#
    .SYNOPSIS
        Any method on any path of the REST API - the escape hatch, and what
        every other function here is built on.
    .DESCRIPTION
        Use it for a route newer than this module. -Body takes a hashtable or
        any object; it is sent as JSON.
    .EXAMPLE
        Invoke-HmApi -Method GET -Path /api/v1/status
    .EXAMPLE
        Invoke-HmApi -Method POST -Path /api/v1/domains -Body @{ name = 'example.com' }
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('GET', 'POST', 'PUT', 'DELETE', 'PATCH')]
        [string] $Method,

        [Parameter(Mandatory = $true)]
        [string] $Path,

        [object] $Body,

        [string] $Url,
        [System.Security.SecureString] $Password,
        [string] $ApiKey,
        [switch] $Insecure
    )

    $session = $script:Session

    if ($Url -or $Password -or $ApiKey) {
        $session = [pscustomobject] @{
            Url            = if ($Url) { $Url.TrimEnd('/') } elseif ($session) { $session.Url } else { $null }
            Password       = if ($Password) { $Password } elseif ($session) { $session.Password } else { $null }
            ApiKey         = if ($ApiKey) { $ApiKey } elseif ($session) { $session.ApiKey } else { $null }
            Insecure       = if ($Insecure) { $true } elseif ($session) { $session.Insecure } else { $false }
            TimeoutSeconds = if ($session) { $session.TimeoutSeconds } else { 30 }
        }
    }

    if (-not $session -or -not $session.Url) {
        throw 'No server: run Connect-HmServer first, or pass -Url and a credential.'
    }

    $headers = @{ Accept = 'application/json' }

    if ($session.ApiKey) {
        $headers['Authorization'] = 'Bearer ' + $session.ApiKey
    }
    elseif ($session.Password) {
        # Built by hand rather than with -Credential: the API takes Basic with
        # the user name Administrator, and PowerShell's own credential handling
        # would negotiate before it sent one.
        $plain = [System.Net.NetworkCredential]::new('', $session.Password).Password
        $raw = [System.Text.Encoding]::UTF8.GetBytes('Administrator:' + $plain)
        $headers['Authorization'] = 'Basic ' + [System.Convert]::ToBase64String($raw)
        $plain = $null
    }
    else {
        throw 'No credential: Connect-HmServer with a password or an API key.'
    }

    $arguments = @{
        Uri             = $session.Url + $Path
        Method          = $Method
        Headers         = $headers
        TimeoutSec      = $session.TimeoutSeconds
        ErrorAction     = 'Stop'
    }

    if ($null -ne $Body) {
        # As UTF-8 bytes, not as a string: Windows PowerShell 5.1 writes a
        # string body in ISO-8859-1, so an imported name with an accented
        # letter reached the server as a byte that is not UTF-8, and a letter
        # outside Latin-1 as its base letter. PowerShell 7 sent UTF-8 already.
        $arguments['Body'] = [System.Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 10 -Compress))
        $arguments['ContentType'] = 'application/json'
    }

    if ($session.Insecure -and $PSVersionTable.PSVersion.Major -ge 6) {
        $arguments['SkipCertificateCheck'] = $true
    }

    try {
        if ($PSVersionTable.PSVersion.Major -lt 6) {
            # Windows PowerShell 5.1 decodes a response that names no charset as
            # ISO-8859-1, and the server's JSON names none (JSON is UTF-8, and
            # RFC 8259 defines no charset parameter). So the bytes are read and
            # decoded as UTF-8 here; otherwise Export-HmAccount wrote an e with
            # a diaeresis as two characters of mojibake, and the next import
            # sent them back.
            $response = Invoke-WebRequest @arguments -UseBasicParsing
            $text = [System.Text.Encoding]::UTF8.GetString($response.RawContentStream.ToArray())
            if (-not $text.Trim()) { return }
            if ([string] $response.Headers['Content-Type'] -like 'application/json*') { return , ($text | ConvertFrom-Json) }
            return $text
        }
        Invoke-RestMethod @arguments
    }
    catch {
        # By name rather than as a typed catch: Windows PowerShell 5.1 has no
        # HttpResponseException, and a catch naming it answered every refusal
        # there with "Unable to find type" instead of the server's reason.
        $failure = $_.Exception.GetType().FullName
        if ($failure -ne 'System.Net.WebException' -and $failure -ne 'Microsoft.PowerShell.Commands.HttpResponseException') {
            throw
        }

        $status = $null
        try { $status = [int] $_.Exception.Response.StatusCode } catch { }

        $detail = $null
        try {
            $detail = ($_.ErrorDetails.Message | ConvertFrom-Json -ErrorAction Stop).error
        }
        catch {
            if ($_.ErrorDetails) { $detail = $_.ErrorDetails.Message }
        }

        switch ($status) {
            401 { throw "The server refused the credential (401). Check the administrator password or the API key." }
            403 { throw "Forbidden (403)$(if ($detail) { ': ' + $detail })." }
            404 { throw "Not found (404): $Path$(if ($detail) { ' - ' + $detail })." }
            default { throw "$Method $Path failed$(if ($status) { " ($status)" })$(if ($detail) { ': ' + $detail })." }
        }
    }
}

function Get-HmCollection {
    <#
    .SYNOPSIS
        The rows out of an answer, whether the route returns a list or an object
        holding one. Internal, but exported because every Get- here uses it.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] $Answer,
        [Parameter(Mandatory = $true)] [string] $Key
    )

    if ($null -eq $Answer) { return @() }
    if ($Answer -is [System.Array]) { return $Answer }
    if ($Answer.PSObject.Properties.Name -contains $Key) { return @($Answer.$Key) }

    foreach ($property in $Answer.PSObject.Properties) {
        if ($property.Value -is [System.Array]) { return $property.Value }
    }

    return @($Answer)
}

# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------

function Get-HmStatus {
    <#
    .SYNOPSIS
        What the server says about itself: version, database, uptime, listeners.
    #>
    [CmdletBinding()]
    param()
    Invoke-HmApi -Method GET -Path '/api/v1/status'
}

# ---------------------------------------------------------------------------
# Domains
# ---------------------------------------------------------------------------

function Get-HmDomain {
    <#
    .SYNOPSIS
        Every domain, or one of them whole.
    .EXAMPLE
        Get-HmDomain | Where-Object active -eq $false
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Domain')]
        [string] $Name
    )
    process {
        if ($Name) {
            Invoke-HmApi -Method GET -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Name))
        }
        else {
            Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/domains') -Key 'domains'
        }
    }
}

function New-HmDomain {
    <#
    .SYNOPSIS
        Adds a domain.
    .EXAMPLE
        New-HmDomain -Name example.com
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string] $Name,

        # No default: a field this module did not send is a field the server
        # decides, and inventing one here would mean the two clients ask for
        # different things for the same command.
        [bool] $Active,

        [hashtable] $Property
    )

    $body = @{ name = $Name }
    if ($PSBoundParameters.ContainsKey('Active')) { $body['active'] = $Active }
    if ($Property) { foreach ($key in $Property.Keys) { $body[$key] = $Property[$key] } }

    if ($PSCmdlet.ShouldProcess($Name, 'Add domain')) {
        Invoke-HmApi -Method POST -Path '/api/v1/domains' -Body $body
    }
}

function Set-HmDomain {
    <#
    .SYNOPSIS
        Changes a domain. The server replaces the whole domain from what it is
        given, so pass every field you mean to keep, or read it first.
    .EXAMPLE
        Set-HmDomain -Name example.com -Property @{ active = $false }
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Name,

        [Parameter(Mandatory = $true)]
        [hashtable] $Property
    )
    process {
        if ($PSCmdlet.ShouldProcess($Name, 'Change domain')) {
            Invoke-HmApi -Method PUT -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Name)) -Body $Property
        }
    }
}

function Remove-HmDomain {
    <#
    .SYNOPSIS
        Deletes a domain, its accounts and their mail.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Name
    )
    process {
        if ($PSCmdlet.ShouldProcess($Name, 'Delete domain, its accounts and their mail')) {
            Invoke-HmApi -Method DELETE -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Name)) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# Accounts
# ---------------------------------------------------------------------------

function Get-HmAccount {
    <#
    .SYNOPSIS
        The accounts of a domain, or one account by its address.
    .DESCRIPTION
        Listed under the domain that owns them and read at their own address,
        which is how the API is shaped: an account belongs to a domain, and an
        address is unique in itself.
    .EXAMPLE
        Get-HmAccount -Domain example.com | Select-Object address, active
    #>
    [CmdletBinding(DefaultParameterSetName = 'Domain')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'Domain')]
        [string] $Domain,

        [Parameter(Mandatory = $true, ParameterSetName = 'Address', ValueFromPipelineByPropertyName = $true)]
        [string] $Address
    )
    process {
        if ($PSCmdlet.ParameterSetName -eq 'Address') {
            Invoke-HmApi -Method GET -Path ("/api/v1/accounts/" + [uri]::EscapeDataString($Address))
        }
        else {
            Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Domain) + "/accounts")) -Key 'accounts'
        }
    }
}

function New-HmAccount {
    <#
    .SYNOPSIS
        Adds a mailbox.
    .EXAMPLE
        New-HmAccount -Domain example.com -Address ann@example.com -Password (Read-Host -AsSecureString)
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string] $Domain,

        [Parameter(Mandatory = $true, Position = 1)]
        [string] $Address,

        [Parameter(Mandatory = $true, Position = 2)]
        [object] $Password,

        [bool] $Active,

        [hashtable] $Property
    )

    $plain = if ($Password -is [System.Security.SecureString]) {
        [System.Net.NetworkCredential]::new('', $Password).Password
    }
    else {
        [string] $Password
    }

    $body = @{ address = $Address; password = $plain }
    if ($PSBoundParameters.ContainsKey('Active')) { $body['active'] = $Active }
    if ($Property) { foreach ($key in $Property.Keys) { $body[$key] = $Property[$key] } }

    if ($PSCmdlet.ShouldProcess($Address, 'Add account')) {
        Invoke-HmApi -Method POST -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Domain) + "/accounts") -Body $body
    }
}

function Set-HmAccount {
    <#
    .SYNOPSIS
        Changes an account.
    .EXAMPLE
        Set-HmAccount -Address ann@example.com -Property @{ max_size_mb = 500 }
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Address,

        [Parameter(Mandatory = $true)]
        [hashtable] $Property
    )
    process {
        if ($PSCmdlet.ShouldProcess($Address, 'Change account')) {
            Invoke-HmApi -Method PUT -Path ("/api/v1/accounts/" + [uri]::EscapeDataString($Address)) -Body $Property
        }
    }
}

function Remove-HmAccount {
    <#
    .SYNOPSIS
        Deletes an account and its mail.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Address
    )
    process {
        if ($PSCmdlet.ShouldProcess($Address, 'Delete account and its mail')) {
            Invoke-HmApi -Method DELETE -Path ("/api/v1/accounts/" + [uri]::EscapeDataString($Address)) | Out-Null
        }
    }
}

function Export-HmAccountArchive {
    <#
    .SYNOPSIS
        Writes everything the server holds on an account to one .zip on the
        server's disk: mail as mbox per folder, contacts as vCard, calendars,
        preferences, the Sieve script, the log lines naming the address and an
        account.xml with the rest.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Address,
        [Parameter(Mandatory = $true)]
        [string] $Path
    )
    process {
        Invoke-HmApi -Method POST -Path ("/api/v1/accounts/" + [uri]::EscapeDataString($Address) + "/export") -Body @{ archive = $Path }
    }
}

function Remove-HmAddressTraces {
    <#
    .SYNOPSIS
        Erases every stored trace of an address that deleting its account
        leaves; with -IncludeArchive the archive tree (held copies excepted),
        with -IncludeLogs the log lines rewritten.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Address,
        [switch] $IncludeArchive,
        [switch] $IncludeLogs
    )
    process {
        if ($PSCmdlet.ShouldProcess($Address, 'Erase every stored trace of the address')) {
            Invoke-HmApi -Method POST -Path '/api/v1/addresses/erase' -Body @{ address = $Address; include_archive = [bool] $IncludeArchive; include_logs = [bool] $IncludeLogs }
        }
    }
}

# ---------------------------------------------------------------------------
# Aliases, groups
# ---------------------------------------------------------------------------

function Get-HmAlias {
    <#
    .SYNOPSIS
        A domain's address aliases.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string] $Domain
    )
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Domain) + "/aliases")) -Key 'aliases'
}

function New-HmAlias {
    <#
    .SYNOPSIS
        Adds an alias: one address answering for another.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [Parameter(Mandatory = $true, Position = 1)] [string] $Name,
        [Parameter(Mandatory = $true, Position = 2)] [string] $Value,
        [bool] $Active
    )
    $body = @{ name = $Name; value = $Value }
    if ($PSBoundParameters.ContainsKey('Active')) { $body['active'] = $Active }
    if ($PSCmdlet.ShouldProcess("$Name -> $Value", 'Add alias')) {
        Invoke-HmApi -Method POST -Path ("/api/v1/domains/" + [uri]::EscapeDataString($Domain) + "/aliases") -Body $body
    }
}

function Remove-HmAlias {
    <#
    .SYNOPSIS
        Deletes an alias by its address.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Address
    )
    process {
        if ($PSCmdlet.ShouldProcess($Address, 'Delete alias')) {
            Invoke-HmApi -Method DELETE -Path ("/api/v1/aliases/" + [uri]::EscapeDataString($Address)) | Out-Null
        }
    }
}

function Get-HmSpamTrap {
    <#
    .SYNOPSIS
        A domain's spam traps: addresses of it that were never real.
    .DESCRIPTION
        A message to one is refused exactly as a message to an address this
        server does not have, and then the connecting address is banned and the
        envelope sender blocked. Get-HmSpamTrapHit says what has addressed them.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string] $Domain
    )
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path (Get-HmSpamTrapPath -Domain $Domain)) -Key 'spam_traps'
}

function Add-HmSpamTrap {
    <#
    .SYNOPSIS
        Declares an address of a domain a spam trap.
    .DESCRIPTION
        Address is a whole address in the domain, or just the part before the @,
        which is taken to be in it. An address an account, an alias or a
        distribution list already has is refused - and one that is a trap cannot
        be given to any of them while it is. Declaring an address that is already
        a trap changes nothing and is not an error.
    .EXAMPLE
        Add-HmSpamTrap -Domain example.com -Address never-used
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [Parameter(Mandatory = $true, Position = 1)] [string] $Address
    )
    if ($PSCmdlet.ShouldProcess($Address, 'Declare a spam trap')) {
        Invoke-HmApi -Method POST -Path (Get-HmSpamTrapPath -Domain $Domain) -Body @{ address = $Address }
    }
}

function Remove-HmSpamTrap {
    <#
    .SYNOPSIS
        Stops trapping an address.
    .DESCRIPTION
        What its hits already did - a banned address, a blocked sender - stays
        until it expires or is removed by hand, and so does the record of them.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [string] $Address
    )
    process {
        if ($PSCmdlet.ShouldProcess($Address, 'Remove spam trap')) {
            Invoke-HmApi -Method DELETE -Path ('/api/v1/spam-traps/' + [uri]::EscapeDataString($Address)) | Out-Null
        }
    }
}

function Get-HmSpamTrapHit {
    <#
    .SYNOPSIS
        What has addressed a spam trap: newest first, with what was done about it.
    .DESCRIPTION
        Every domain's, or one domain's with -Domain. Each entry carries the
        trap, the connecting address, the envelope sender, whether the address
        was banned and the sender blocked, and a note saying why not when not.
    .EXAMPLE
        Get-HmSpamTrapHit -Limit 20 | Format-Table time, address, ip_address, sender
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)] [string] $Domain,
        [int] $Limit
    )
    $path = if ($Domain) { (Get-HmSpamTrapPath -Domain $Domain) + '/hits' } else { '/api/v1/spam-traps/hits' }
    if ($Limit) { $path = $path + '?limit=' + $Limit }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Clear-HmSpamTrapHit {
    <#
    .SYNOPSIS
        Clears the record of spam trap hits and says how many rows went.
    .DESCRIPTION
        The bans and the blocked senders the hits made are untouched: this clears
        a record, it undoes nothing.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if ($PSCmdlet.ShouldProcess('every recorded spam trap hit', 'Clear')) {
        Invoke-HmApi -Method DELETE -Path '/api/v1/spam-traps/hits'
    }
}

function Get-HmSpamTrapPath {
    param([Parameter(Mandatory = $true)] [string] $Domain)
    '/api/v1/domains/' + [uri]::EscapeDataString($Domain) + '/spam-traps'
}

function Get-HmJournalRule {
    <#
    .SYNOPSIS
        The journaling and dual-delivery rules: whose mail is copied, and where.
    .DESCRIPTION
        Each rule names a domain or one account, which way the mail is going
        (inbound, outbound or both), where the copy goes, and whether the copy is
        an envelope report with the original attached or the message itself. With
        no rule nothing is copied anywhere.
    .EXAMPLE
        Get-HmJournalRule | Format-Table target, direction, mode, journal_address, enabled
    #>
    [CmdletBinding()]
    param()
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/journal-rules') -Key 'journal_rules'
}

function New-HmJournalRule {
    <#
    .SYNOPSIS
        Writes a journaling or dual-delivery rule.
    .DESCRIPTION
        Scope is domain or account and Target the domain name or the account
        address. Mode report sends the Exchange envelope report with the original
        attached as message/rfc822; mode copy sends the message itself with one
        header added. In force for the next accepted message.
    .EXAMPLE
        New-HmJournalRule -Target example.com -JournalAddress archive@compliance.example -Direction both
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Target,
        [Parameter(Mandatory = $true, Position = 1)] [string] $JournalAddress,
        [ValidateSet('domain', 'account')] [string] $Scope = 'domain',
        [ValidateSet('inbound', 'outbound', 'both')] [string] $Direction = 'both',
        [ValidateSet('report', 'copy')] [string] $Mode = 'report',
        [string] $Note
    )
    $body = @{ scope = $Scope; target = $Target; journal_address = $JournalAddress; direction = $Direction; mode = $Mode }
    if ($PSBoundParameters.ContainsKey('Note')) { $body['note'] = $Note }
    if ($PSCmdlet.ShouldProcess($Target, 'Add a journaling rule')) {
        Invoke-HmApi -Method POST -Path '/api/v1/journal-rules' -Body $body
    }
}

function Set-HmJournalRule {
    <#
    .SYNOPSIS
        Changes a journaling rule; a field not given keeps its value.
    .EXAMPLE
        Set-HmJournalRule -Id 3 -Enabled $false
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id,
        [ValidateSet('domain', 'account')] [string] $Scope,
        [string] $Target,
        [string] $JournalAddress,
        [ValidateSet('inbound', 'outbound', 'both')] [string] $Direction,
        [ValidateSet('report', 'copy')] [string] $Mode,
        [bool] $Enabled,
        [string] $Note
    )
    process {
        $body = @{}
        if ($PSBoundParameters.ContainsKey('Scope')) { $body['scope'] = $Scope }
        if ($PSBoundParameters.ContainsKey('Target')) { $body['target'] = $Target }
        if ($PSBoundParameters.ContainsKey('JournalAddress')) { $body['journal_address'] = $JournalAddress }
        if ($PSBoundParameters.ContainsKey('Direction')) { $body['direction'] = $Direction }
        if ($PSBoundParameters.ContainsKey('Mode')) { $body['mode'] = $Mode }
        if ($PSBoundParameters.ContainsKey('Enabled')) { $body['enabled'] = $Enabled }
        if ($PSBoundParameters.ContainsKey('Note')) { $body['note'] = $Note }
        if ($PSCmdlet.ShouldProcess([string] $Id, 'Change a journaling rule')) {
            Invoke-HmApi -Method PUT -Path ('/api/v1/journal-rules/' + $Id) -Body $body
        }
    }
}

function Remove-HmJournalRule {
    <#
    .SYNOPSIS
        Removes a journaling rule.
    .DESCRIPTION
        The rule stops copying at once. What it has already sent is wherever it
        was sent: this server keeps no second copy and withdraws nothing.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id
    )
    process {
        if ($PSCmdlet.ShouldProcess([string] $Id, 'Remove journaling rule')) {
            Invoke-HmApi -Method DELETE -Path ('/api/v1/journal-rules/' + $Id) | Out-Null
        }
    }
}

function Get-HmGroup {
    <#
    .SYNOPSIS
        The account groups, which a folder permission can name as one principal.
    #>
    [CmdletBinding()]
    param([Parameter(Position = 0)] [int] $Id)
    if ($Id) {
        Invoke-HmApi -Method GET -Path "/api/v1/groups/$Id"
    }
    else {
        Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/groups') -Key 'groups'
    }
}

function New-HmGroup {
    <#
    .SYNOPSIS
        Adds an account group.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Name)
    if ($PSCmdlet.ShouldProcess($Name, 'Add group')) {
        Invoke-HmApi -Method POST -Path '/api/v1/groups' -Body @{ name = $Name }
    }
}

function Remove-HmGroup {
    <#
    .SYNOPSIS
        Deletes an account group. The accounts themselves are untouched.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id)
    process {
        if ($PSCmdlet.ShouldProcess("group $Id", 'Delete group')) {
            Invoke-HmApi -Method DELETE -Path "/api/v1/groups/$Id" | Out-Null
        }
    }
}

function Get-HmGroupMember {
    <#
    .SYNOPSIS
        The accounts in a group.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory = $true, Position = 0)] [int] $Id)
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path "/api/v1/groups/$Id/members") -Key 'members'
}

function Add-HmGroupMember {
    <#
    .SYNOPSIS
        Puts an account in a group, by address or by account id.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [int] $Id,
        [Parameter(Mandatory = $true, Position = 1, ValueFromPipelineByPropertyName = $true)] [string] $Account
    )
    process {
        $body = if ($Account -match '@') { @{ address = $Account } } else { @{ account_id = [int] $Account } }
        if ($PSCmdlet.ShouldProcess("$Account into group $Id", 'Add group member')) {
            Invoke-HmApi -Method POST -Path "/api/v1/groups/$Id/members" -Body $body
        }
    }
}

function Remove-HmGroupMember {
    <#
    .SYNOPSIS
        Takes an account out of a group.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [int] $Id,
        [Parameter(Mandatory = $true, Position = 1, ValueFromPipelineByPropertyName = $true)] [string] $Account
    )
    process {
        if ($PSCmdlet.ShouldProcess("$Account out of group $Id", 'Remove group member')) {
            Invoke-HmApi -Method DELETE -Path ("/api/v1/groups/$Id/members/" + [uri]::EscapeDataString($Account)) | Out-Null
        }
    }
}

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

$script:SettingGroups = @('settings', 'antispam', 'antivirus', 'logging', 'backup', 'cache', 'indexing', 'scripting', 'bounces', 'complaints', 'directories')

function Get-HmSettingPath {
    param([string] $Group)
    if (-not $Group -or $Group -eq 'settings') { return '/api/v1/settings' }
    return "/api/v1/settings/$Group"
}

function Get-HmSetting {
    <#
    .SYNOPSIS
        A settings group whole, or one setting out of it.
    .EXAMPLE
        Get-HmSetting -Group antispam
    .EXAMPLE
        (Get-HmSetting).max_message_size_kb
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)]
        [ValidateSet('settings', 'antispam', 'antivirus', 'logging', 'backup', 'cache', 'indexing', 'scripting', 'bounces', 'push', 'complaints', 'directories', 'openpgp')]
        [string] $Group = 'settings',

        [Parameter(Position = 1)]
        [string] $Name
    )

    $answer = Invoke-HmApi -Method GET -Path (Get-HmSettingPath -Group $Group)

    if ($Name) {
        if ($answer.PSObject.Properties.Name -notcontains $Name) {
            throw "No setting '$Name' in the $Group group."
        }
        return $answer.$Name
    }

    $answer
}

function Set-HmSetting {
    <#
    .SYNOPSIS
        Changes settings in a group. Any subset of its keys; the server
        validates them as the Control Panel does and applies them only when
        every one is accepted.
    .EXAMPLE
        Set-HmSetting -Property @{ max_message_size_kb = 51200 }
    .EXAMPLE
        Set-HmSetting -Group antispam -Property @{ use_spf = $true; spam_mark_threshold = 5 }
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Position = 0)]
        [ValidateSet('settings', 'antispam', 'antivirus', 'logging', 'backup', 'cache', 'indexing', 'scripting', 'bounces', 'push', 'complaints', 'directories', 'openpgp')]
        [string] $Group = 'settings',

        [Parameter(Mandatory = $true)]
        [hashtable] $Property
    )

    if ($PSCmdlet.ShouldProcess("the $Group settings", 'Change ' + ($Property.Keys -join ', '))) {
        Invoke-HmApi -Method PUT -Path (Get-HmSettingPath -Group $Group) -Body $Property
    }
}

function Test-HmBreachCheckEndpoint {
    <#
    .SYNOPSIS
        Asks the password breach check's range endpoint for the prefix 00000 and
        answers what it said: reachable, status, entries, padded and milliseconds,
        or error. Nothing is changed.
    .DESCRIPTION
        POST /api/v1/settings/breach-check/test. Without -Url the configured
        endpoint (breach_check_url) is asked. The endpoint's own answer is never
        returned, only what it said about itself.
    .EXAMPLE
        Test-HmBreachCheckEndpoint
    .EXAMPLE
        Test-HmBreachCheckEndpoint -Url https://mirror.example/range/
    #>
    [CmdletBinding()]
    param([Parameter(Position = 0)] [string] $Url)

    $body = @{}
    if ($Url) { $body.url = $Url }
    Invoke-HmApi -Method POST -Path '/api/v1/settings/breach-check/test' -Body $body
}

function Get-HmIniSetting {
    <#
    .SYNOPSIS
        The server settings that were the [Settings] section of hMailServer.ini,
        each as name, value and secret, or one of them by name. A secret's value
        is listed as "(set)"; asked for by name, it is the value.
    #>
    [CmdletBinding()]
    param([Parameter(Position = 0)] [string] $Name)
    if ($Name) {
        Invoke-HmApi -Method GET -Path ("/api/v1/settings/ini/" + [uri]::EscapeDataString($Name))
    }
    else {
        Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/settings/ini') -Key 'settings'
    }
}

function Set-HmIniSetting {
    <#
    .SYNOPSIS
        Stores one server setting - a key of the [Settings] section of
        hMailServer.ini before schema 6042 - in the database, which is the
        settings store, and the server writes the file's copy. Most apply when
        the service next starts.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Name,
        [Parameter(Mandatory = $true, Position = 1)] [string] $Value
    )
    if ($PSCmdlet.ShouldProcess($Name, "Set to $Value")) {
        Invoke-HmApi -Method PUT -Path ("/api/v1/settings/ini/" + [uri]::EscapeDataString($Name)) -Body @{ value = $Value }
    }
}

# ---------------------------------------------------------------------------
# The queue, held mail, the logs, the backup
# ---------------------------------------------------------------------------

function Get-HmQueue {
    <#
    .SYNOPSIS
        The delivery queue. held is 'sandbox' for a message waiting for a
        sandbox's verdict, and empty for every other: such a message is not the
        queue's to retry or remove (the server answers 409), and
        /api/v1/sandbox/held releases or discards it.
    .EXAMPLE
        Get-HmQueue | Where-Object tries -gt 3 | Format-Table
    .EXAMPLE
        Get-HmQueue | Where-Object held -ne 'sandbox' | Remove-HmQueueMessage
    #>
    [CmdletBinding()]
    param()
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/queue') -Key 'messages' | ForEach-Object {
        # Every item carries held, so a table shows the column and a filter on
        # it reads an empty string rather than nothing.
        if (-not ($_.PSObject.Properties.Name -contains 'held')) {
            $_ | Add-Member -NotePropertyName held -NotePropertyValue ''
        }
        $_
    }
}

function Start-HmQueueDelivery {
    <#
    .SYNOPSIS
        Retries a queued message now rather than at its next try.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id)
    process {
        if ($PSCmdlet.ShouldProcess("message $Id", 'Retry delivery now')) {
            Invoke-HmApi -Method POST -Path "/api/v1/queue/$Id/retry"
        }
    }
}

function Remove-HmQueueMessage {
    <#
    .SYNOPSIS
        Takes a message out of the delivery queue. It is not delivered and not
        bounced.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param([Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id)
    process {
        if ($PSCmdlet.ShouldProcess("message $Id", 'Delete from the queue')) {
            Invoke-HmApi -Method DELETE -Path "/api/v1/queue/$Id" | Out-Null
        }
    }
}

function Get-HmQuarantine {
    <#
    .SYNOPSIS
        The held mail.
    #>
    [CmdletBinding()]
    param()
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/quarantine') -Key 'messages'
}

function Restore-HmQuarantineMessage {
    <#
    .SYNOPSIS
        Releases a held message to its original recipients. Delivery is direct
        rather than back through the filters: a release is an administrator
        overruling them.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [int] $Id)
    process {
        if ($PSCmdlet.ShouldProcess("held message $Id", 'Release to its recipients')) {
            Invoke-HmApi -Method POST -Path "/api/v1/quarantine/$Id/release"
        }
    }
}

function Get-HmLog {
    <#
    .SYNOPSIS
        The log files, or the tail of one.
    .EXAMPLE
        Get-HmLog
    .EXAMPLE
        Get-HmLog -Name hmailserver_2026-09-15.log -Lines 200
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)] [string] $Name,
        [int] $Lines = 50
    )
    if ($Name) {
        $answer = Invoke-HmApi -Method GET -Path ("/api/v1/logs/" + [uri]::EscapeDataString($Name) + "?lines=$Lines")
        if ($answer.PSObject.Properties.Name -contains 'lines') { return $answer.lines }
        return $answer
    }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/logs') -Key 'logs'
}

function Get-HmBackup {
    <#
    .SYNOPSIS
        What the server's own backup is doing, and when it last ran.
    #>
    [CmdletBinding()]
    param()
    Invoke-HmApi -Method GET -Path '/api/v1/backup'
}

function Start-HmBackup {
    <#
    .SYNOPSIS
        Starts the server's own backup. It runs in the server; this returns as
        soon as it has begun.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if ($PSCmdlet.ShouldProcess('the server', 'Start a backup')) {
        Invoke-HmApi -Method POST -Path '/api/v1/backup' -Body @{}
    }
}

function Restore-HmBackup {
    <#
    .SYNOPSIS
        Puts a backup archive back. Destructive: it replaces the parts it
        restores.
    .DESCRIPTION
        POST /api/v1/backup/restore. -Passphrase is the one the archive was
        encrypted with; it is sent to the server for this one call and kept
        nowhere. Everything that can be checked is checked before anything is
        changed - a missing or wrong passphrase, a damaged archive, an archive
        from a newer hMailServer, a part the archive does not hold - and the
        restore then runs in the server. Get-HmBackup says how it went.
    .EXAMPLE
        Restore-HmBackup -File 'D:\backups\HMBackup 2026-09-15 020000.7z' -Domains -Messages -Passphrase (Read-Host -AsSecureString)
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string] $File,

        [System.Security.SecureString] $Passphrase,

        [switch] $Settings,
        [switch] $Domains,
        [switch] $Messages
    )

    if (-not ($Settings -or $Domains -or $Messages)) {
        throw 'Choose what to restore: -Settings, -Domains, -Messages (messages only together with -Domains).'
    }

    $body = @{
        file             = $File
        restore_settings = [bool] $Settings
        restore_domains  = [bool] $Domains
        restore_messages = [bool] $Messages
    }

    if ($Passphrase) {
        $body['passphrase'] = [System.Net.NetworkCredential]::new('', $Passphrase).Password
    }

    if ($PSCmdlet.ShouldProcess($File, 'Restore this archive over what is on the server')) {
        Invoke-HmApi -Method POST -Path '/api/v1/backup/restore' -Body $body
    }
}

function Set-HmBackupPassphrase {
    <#
    .SYNOPSIS
        The passphrase the backup archive is encrypted with: set it, or with
        -Clear stop encrypting.
    .DESCRIPTION
        PUT /api/v1/settings/backup. Nothing ever reads the passphrase back -
        Get-HmSetting -Group backup answers encryption_passphrase_set and never
        the passphrase itself - so keep it somewhere other than beside the
        backups. A restore asks for it, and an archive whose passphrase is lost
        cannot be read by anybody, this server included.
    .EXAMPLE
        Set-HmBackupPassphrase -Passphrase (Read-Host -AsSecureString)
    .EXAMPLE
        Set-HmBackupPassphrase -Clear
    #>
    [CmdletBinding(SupportsShouldProcess = $true, DefaultParameterSetName = 'Set')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'Set')]
        [System.Security.SecureString] $Passphrase,

        [Parameter(Mandatory = $true, ParameterSetName = 'Clear')]
        [switch] $Clear
    )

    $plain = if ($Clear) { '' } else { [System.Net.NetworkCredential]::new('', $Passphrase).Password }
    $action = if ($Clear) { 'Stop encrypting the backup archive' } else { 'Set the backup passphrase' }

    if ($PSCmdlet.ShouldProcess('the backup settings', $action)) {
        Invoke-HmApi -Method PUT -Path '/api/v1/settings/backup' -Body @{ encryption_passphrase = $plain }
    }
}

function Test-HmRuleCriterion {
    <#
    .SYNOPSIS
        Asks the server what a rule criterion would decide, without a rule.
    .EXAMPLE
        Test-HmRuleCriterion -Type contains -Value viagra -Against 'cheap viagra here'
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [ValidateSet('equals', 'contains', 'less_than', 'greater_than', 'regex', 'not_contains', 'not_equals', 'wildcard')]
        [string] $Type,

        [Parameter(Mandatory = $true, Position = 1)] [string] $Value,
        [Parameter(Mandatory = $true, Position = 2)] [string] $Against
    )

    $answer = Invoke-HmApi -Method POST -Path '/api/v1/rules/match' -Body @{
        match_value = $Value
        match_type  = $Type
        test_value  = $Against
    }

    [bool] $answer.match
}

# ---------------------------------------------------------------------------
# DKIM key rotation
# ---------------------------------------------------------------------------

function Get-HmDkimRotation {
    <#
    .SYNOPSIS
        A domain's DKIM key rotation: the phase, the current, next, retiring and
        retired keys, what is waiting, and the TXT records to publish, keep or
        remove.
    .DESCRIPTION
        The status document GET /api/v1/domains/{domain}/dkim/rotation answers.
        Times are seconds since the epoch. -Records answers only the records, one
        object per record with its Slot, Action, Host, Type and Value - the list
        to hand to whoever edits the DNS. The schedule itself is five properties
        of the domain: Set-HmDomain -Name example.com -Property @{ dkim_rotation_enabled = $true }.
    .EXAMPLE
        Get-HmDkimRotation -Domain example.com -Records | Where-Object Action -eq publish
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain,
        [switch] $Records
    )
    process {
        $status = Invoke-HmApi -Method GET -Path (Get-HmDkimRotationPath -Domain $Domain)
        if (-not $Records) { return $status }

        foreach ($slot in 'current', 'next', 'retiring', 'retired') {
            $entry = $status.$slot
            if (-not $entry) { continue }
            foreach ($record in @($entry.records)) {
                if (-not $record) { continue }
                # A record listed for removal carries no value.
                $names = $record.PSObject.Properties.Name
                [pscustomobject] @{
                    Slot     = $slot
                    Selector = $entry.selector
                    Action   = $record.action
                    Host     = $record.host
                    Type     = if ($names -contains 'type') { $record.type } else { 'TXT' }
                    Value    = if ($names -contains 'value') { $record.value } else { '' }
                }
            }
        }
    }
}

function Start-HmDkimRotation {
    <#
    .SYNOPSIS
        Rotate now: the server generates the next key and stages it. Signing does
        not move until its record has resolved for the propagation period.
    .EXAMPLE
        Start-HmDkimRotation -Domain example.com
    .EXAMPLE
        Start-HmDkimRotation -Domain example.com -Selector mail2026
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain,
        [string] $Selector
    )
    process {
        if ($PSCmdlet.ShouldProcess($Domain, 'Generate and stage the next DKIM key')) {
            $body = @{}
            if ($Selector) { $body['selector'] = $Selector }
            Invoke-HmApi -Method POST -Path ((Get-HmDkimRotationPath -Domain $Domain) + '/start') -Body $body
        }
    }
}

function Test-HmDkimRotation {
    <#
    .SYNOPSIS
        Looks the rotation's TXT records up from the server's own resolver now,
        rather than at the next pass, and answers the status with what was found.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain
    )
    process {
        Invoke-HmApi -Method POST -Path ((Get-HmDkimRotationPath -Domain $Domain) + '/check') -Body @{}
    }
}

function Complete-HmDkimRotation {
    <#
    .SYNOPSIS
        Moves signing to the next key now.
    .DESCRIPTION
        Refused unless the next key's record matches in a check the server makes
        for this request, and - without -Force - until the record has resolved for
        the whole propagation period. -Force skips the period, never the check.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain,
        [switch] $Force
    )
    process {
        $what = if ($Force) { 'Move signing to the next DKIM key without waiting for the propagation period' } else { 'Move signing to the next DKIM key' }
        if ($PSCmdlet.ShouldProcess($Domain, $what)) {
            $body = @{}
            if ($Force) { $body['force'] = $true }
            Invoke-HmApi -Method POST -Path ((Get-HmDkimRotationPath -Domain $Domain) + '/switch') -Body $body
        }
    }
}

function Remove-HmDkimRetiringKey {
    <#
    .SYNOPSIS
        Retires the old selector now, before its retention period ends. Its TXT
        record may then be removed, and a key file the rotation generated for it
        is deleted.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain
    )
    process {
        if ($PSCmdlet.ShouldProcess($Domain, 'Retire the old DKIM selector')) {
            Invoke-HmApi -Method POST -Path ((Get-HmDkimRotationPath -Domain $Domain) + '/retire') -Body @{}
        }
    }
}

function Stop-HmDkimRotation {
    <#
    .SYNOPSIS
        Cancels the next key: the staged selector is removed and the key file the
        rotation generated for it deleted. Signing does not change.
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)]
        [Alias('Name')]
        [string] $Domain
    )
    process {
        if ($PSCmdlet.ShouldProcess($Domain, 'Cancel the next DKIM key')) {
            Invoke-HmApi -Method DELETE -Path ((Get-HmDkimRotationPath -Domain $Domain) + '/next')
        }
    }
}

function Invoke-HmDkimRotation {
    <#
    .SYNOPSIS
        One pass of the DKIM key rotation over every domain, now - the pass the
        server makes every fifteen minutes.
    .DESCRIPTION
        -SecondsAhead moves the clock forward for this pass, which is how the
        regression suite tries a propagation or retention period without waiting.
        It is a test's tool, not for a production server: a switch or retire the
        pass makes happens at once, early, and the times it records are the moved
        clock's and stay recorded, so the server's own passes afterwards wait that
        much longer. Server administrators only; a non-zero offset is written to
        the application log.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([ValidateRange(0, 316224000)] [int] $SecondsAhead = 0)
    if ($PSCmdlet.ShouldProcess('every domain', 'Run a pass of the DKIM key rotation')) {
        $body = @{}
        if ($SecondsAhead -gt 0) { $body['seconds_ahead'] = $SecondsAhead }
        Invoke-HmApi -Method POST -Path '/api/v1/dkim/rotation/run' -Body $body
    }
}

function Get-HmDkimRotationPath {
    param([Parameter(Mandatory = $true)] [string] $Domain)
    '/api/v1/domains/' + [uri]::EscapeDataString($Domain) + '/dkim/rotation'
}

function Get-HmReputation {
    <#
    .SYNOPSIS
        What the last check of this server's own outbound addresses found.
    .DESCRIPTION
        GET /api/v1/reputation. Without -Results the report itself; with it, one
        object per address and list, which is what Where-Object and Format-Table
        want. Listed is True, False or $null, and the third one - the list refused
        the query, was not answering, or could not be reached - is not False.
    .EXAMPLE
        Get-HmReputation -Results | Where-Object Listed -eq $true
    #>
    [CmdletBinding()]
    param([switch] $Results)

    $report = Invoke-HmApi -Method GET -Path '/api/v1/reputation'
    if (-not $Results) { return $report }

    foreach ($address in @($report.addresses)) {
        foreach ($result in @($address.results)) {
            [pscustomobject] @{
                Address     = $address.address
                List        = $result.list
                Status      = $result.status
                Listed      = $result.listed
                Answer      = $result.code
                Reason      = $result.reason
                ListedSince = $result.listed_since
                Checked     = $result.checked
            }
        }
    }
}

function Invoke-HmReputationCheck {
    <#
    .SYNOPSIS
        Checks this server's outbound addresses against the DNS block lists now,
        rather than waiting for the daily check.
    .DESCRIPTION
        POST /api/v1/reputation/check. Answers the report with a run member saying
        what the check did. Server administrators only; it makes a handful of DNS
        queries while the call waits.
    .EXAMPLE
        (Invoke-HmReputationCheck).run.listed
    #>
    [CmdletBinding()]
    param()

    Invoke-HmApi -Method POST -Path '/api/v1/reputation/check' -Body @{}
}

# ---------------------------------------------------------------------------
# Bounces and the suppression list
# ---------------------------------------------------------------------------

function Get-HmSuppressionPath {
    param([string] $Domain, [string] $Tail)
    "/api/v1/domains/" + [uri]::EscapeDataString($Domain) + $Tail
}

function Get-HmSuppression {
    <#
    .SYNOPSIS
        The addresses a domain no longer sends to because mail to them bounced,
        or because an administrator put them there.
    .EXAMPLE
        Get-HmSuppression -Domain example.com | Format-Table address, category, status, created
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [string] $Search
    )
    $path = Get-HmSuppressionPath -Domain $Domain -Tail '/suppressions'
    if ($Search) { $path += '?search=' + [uri]::EscapeDataString($Search) }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Add-HmSuppression {
    <#
    .SYNOPSIS
        Puts an address on a domain's suppression list by hand. The next message
        from the domain to it is refused, dropped or delivered and recorded, as
        the bounce settings say.
    .EXAMPLE
        Add-HmSuppression -Domain example.com -Address gone@elsewhere.test -Note 'from the old export' -ExpiresDays 90
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [Parameter(Mandatory = $true, Position = 1)] [string] $Address,
        [string] $Note,
        [ValidateRange(0, 3650)] [int] $ExpiresDays = 0
    )
    $body = @{ address = $Address; expires_days = $ExpiresDays }
    if ($PSBoundParameters.ContainsKey('Note')) { $body['note'] = $Note }
    if ($PSCmdlet.ShouldProcess("$Address for $Domain", 'Add to the suppression list')) {
        Invoke-HmApi -Method POST -Path (Get-HmSuppressionPath -Domain $Domain -Tail '/suppressions') -Body $body
    }
}

function Remove-HmSuppression {
    <#
    .SYNOPSIS
        Takes an address off a domain's suppression list. The next message to it
        is sent, and its run of hard bounces starts again.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [Parameter(Mandatory = $true, Position = 1, ValueFromPipelineByPropertyName = $true)] [string] $Address
    )
    process {
        if ($PSCmdlet.ShouldProcess("$Address for $Domain", 'Remove from the suppression list')) {
            Invoke-HmApi -Method DELETE -Path (Get-HmSuppressionPath -Domain $Domain -Tail ('/suppressions/' + [uri]::EscapeDataString($Address))) | Out-Null
        }
    }
}

function Get-HmBounceCount {
    <#
    .SYNOPSIS
        The bounce counts a domain's suppression list is fed from: hard, soft,
        mailbox full, policy and other, and the run of hard bounces.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [string] $Search
    )
    $path = Get-HmSuppressionPath -Domain $Domain -Tail '/bounces'
    if ($Search) { $path += '?search=' + [uri]::EscapeDataString($Search) }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Test-HmBounceStatus {
    <#
    .SYNOPSIS
        Asks the server how it would read a bounce: a bare status as a delivery
        report's Status field, a reply with its three digits as a refusal met
        while delivering. Reads and writes nothing.
    .EXAMPLE
        Test-HmBounceStatus -Status '550 5.1.1 User unknown'
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Status,
        [Parameter(Position = 1)] [string] $Diagnostic = ''
    )
    Invoke-HmApi -Method POST -Path '/api/v1/bounces/classify' -Body @{ status = $Status; diagnostic = $Diagnostic }
}

function Invoke-HmBounceMaintenance {
    <#
    .SYNOPSIS
        Runs the suppression list's hourly sweep now: expired entries, old
        counts and old report keys.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if ($PSCmdlet.ShouldProcess('the suppression lists', 'Expire and prune')) {
        Invoke-HmApi -Method POST -Path '/api/v1/bounces/expire'
    }
}

# ---------------------------------------------------------------------------
# Outbound webhooks
# ---------------------------------------------------------------------------

function Get-HmWebhook {
    <#
    .SYNOPSIS
        The outbound webhook subscriptions: where this server's mail events go,
        what each one wants, and how its deliveries have gone. The shared secret
        is never returned; SecretSet says whether one is configured.
    .EXAMPLE
        Get-HmWebhook | Format-Table name, url, events, enabled, pending, dead_lettered
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)] [string] $Name
    )
    if ($Name) { return Invoke-HmApi -Method GET -Path ('/api/v1/webhooks/' + [uri]::EscapeDataString($Name)) }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/webhooks') -Key 'webhooks'
}

function New-HmWebhook {
    <#
    .SYNOPSIS
        Subscribes an endpoint to this server's mail events. The bodies it
        receives are signed HMAC-SHA256 over the timestamp, a full stop and the
        raw body, under the secret given here.
    .EXAMPLE
        New-HmWebhook -Name ticketing -Url https://tickets.example/hook -Secret $s -Events message.bounced -Domains example.com
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Name,
        [Parameter(Mandatory = $true, Position = 1)] [string] $Url,
        [Parameter(Mandatory = $true, Position = 2)] [string] $Secret,
        [string[]] $Events,
        [string[]] $Domains,
        [string[]] $Addresses,
        [bool] $Enabled = $true,
        [switch] $IncludeSubject,
        [ValidateRange(0, 100000)] [int] $MaxPerMinute = 0
    )
    $body = @{ name = $Name; url = $Url; secret = $Secret; enabled = $Enabled;
               include_subject = [bool] $IncludeSubject; max_per_minute = $MaxPerMinute }
    if ($PSBoundParameters.ContainsKey('Events')) { $body['events'] = ($Events -join ',') }
    if ($PSBoundParameters.ContainsKey('Domains')) { $body['domains'] = ($Domains -join ',') }
    if ($PSBoundParameters.ContainsKey('Addresses')) { $body['addresses'] = ($Addresses -join ',') }
    if ($PSCmdlet.ShouldProcess($Name, 'Create a webhook subscription')) {
        Invoke-HmApi -Method POST -Path '/api/v1/webhooks' -Body $body
    }
}

function Set-HmWebhook {
    <#
    .SYNOPSIS
        Changes one subscription. A parameter not given keeps the value it has,
        and the secret is only replaced when one is given - so switching a
        subscription off cannot clear what it signs with.
    .EXAMPLE
        Set-HmWebhook -Name ticketing -Enabled $false
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Name,
        [string] $Url,
        [string] $Secret,
        [string[]] $Events,
        [string[]] $Domains,
        [string[]] $Addresses,
        [bool] $Enabled,
        [bool] $IncludeSubject,
        [ValidateRange(0, 100000)] [int] $MaxPerMinute
    )
    process {
        $body = @{}
        if ($PSBoundParameters.ContainsKey('Url')) { $body['url'] = $Url }
        if ($PSBoundParameters.ContainsKey('Secret')) { $body['secret'] = $Secret }
        if ($PSBoundParameters.ContainsKey('Events')) { $body['events'] = ($Events -join ',') }
        if ($PSBoundParameters.ContainsKey('Domains')) { $body['domains'] = ($Domains -join ',') }
        if ($PSBoundParameters.ContainsKey('Addresses')) { $body['addresses'] = ($Addresses -join ',') }
        if ($PSBoundParameters.ContainsKey('Enabled')) { $body['enabled'] = $Enabled }
        if ($PSBoundParameters.ContainsKey('IncludeSubject')) { $body['include_subject'] = $IncludeSubject }
        if ($PSBoundParameters.ContainsKey('MaxPerMinute')) { $body['max_per_minute'] = $MaxPerMinute }
        if ($PSCmdlet.ShouldProcess($Name, 'Change a webhook subscription')) {
            Invoke-HmApi -Method PUT -Path ('/api/v1/webhooks/' + [uri]::EscapeDataString($Name)) -Body $body
        }
    }
}

function Remove-HmWebhook {
    <#
    .SYNOPSIS
        Removes a subscription, and with it everything of its that is still
        queued - a delivery to an endpoint that no longer exists is a request
        nobody wants made.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Name
    )
    process {
        if ($PSCmdlet.ShouldProcess($Name, 'Remove a webhook subscription')) {
            Invoke-HmApi -Method DELETE -Path ('/api/v1/webhooks/' + [uri]::EscapeDataString($Name)) | Out-Null
        }
    }
}

function Get-HmWebhookDelivery {
    <#
    .SYNOPSIS
        The webhook delivery queue, newest first: what was sent, what is still
        owed, and why a dead letter was given up on. Each row carries the body
        exactly as it was sent.
    .EXAMPLE
        Get-HmWebhookDelivery -State dead-lettered | Format-Table id, webhook, event, attempts, error
    #>
    [CmdletBinding()]
    param(
        [string] $Webhook,
        [ValidateSet('pending', 'delivered', 'dead-lettered')] [string] $State,
        [ValidateRange(1, 1000)] [int] $Limit = 100
    )
    $query = @('limit=' + $Limit)
    if ($Webhook) { $query += 'webhook=' + [uri]::EscapeDataString($Webhook) }
    if ($State) { $query += 'state=' + [uri]::EscapeDataString($State) }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path ('/api/v1/webhooks/deliveries?' + ($query -join '&'))) -Key 'deliveries'
}

function Invoke-HmWebhookDelivery {
    <#
    .SYNOPSIS
        Runs one webhook delivery pass now - the pass the scheduled task makes
        every minute - and removes what the retention no longer keeps.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if ($PSCmdlet.ShouldProcess('the webhook queue', 'Deliver what is due')) {
        Invoke-HmApi -Method POST -Path '/api/v1/webhooks/run' -Body @{}
    }
}

function Test-HmWebhook {
    <#
    .SYNOPSIS
        Raises one event through exactly the path a real one takes - the same
        matching, the same domain switches, the same rate ceiling - so that what
        arrives is what would arrive. Answers how many subscriptions took it.
    .EXAMPLE
        Test-HmWebhook -Event message.bounced -Domain example.com
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [ValidateSet('message.received', 'message.delivered', 'message.bounced', 'message.quarantined', 'account.locked')]
        [string] $Event,
        [string] $Domain,
        [string] $Account
    )
    $body = @{}
    if ($Event) { $body['event'] = $Event }
    if ($Domain) { $body['domain'] = $Domain }
    if ($Account) { $body['account'] = $Account }
    if ($PSCmdlet.ShouldProcess('the webhook subscriptions', 'Raise a test event')) {
        Invoke-HmApi -Method POST -Path '/api/v1/webhooks/test' -Body $body
    }
}

# ---------------------------------------------------------------------------
# Devices: the clients that have signed in to an account
# ---------------------------------------------------------------------------

function Get-HmDevice {
    <#
    .SYNOPSIS
        The clients that have signed in to an account - protocol, address, the
        program where the protocol carries one, which credential each used and
        when each was first and last seen - most recently used first.
    .DESCRIPTION
        The same entries the account holder sees on the webmail's Security page,
        read under the account's address by an administrator. The id of an entry
        is what Revoke-HmDevice takes.
    .EXAMPLE
        Get-HmDevice -Address ann@example.com | Format-Table protocol, address, credential, credential_name, last_seen
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Address
    )
    process {
        Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path ("/api/v1/accounts/" + [uri]::EscapeDataString($Address) + "/devices")) -Key 'devices'
    }
}

function Revoke-HmDevice {
    <#
    .SYNOPSIS
        Ends what one client of an account holds now: its browser session, and
        its live IMAP, POP3 and SMTP connections.
    .DESCRIPTION
        Not a ban: the entry stays, marked, and the client can sign in again with
        a credential that still works. -Credential also turns off the app
        password the client signed in with, which is the answer to a phone that
        has been stolen. -Forget removes the entry rather than marking it.
    .EXAMPLE
        Revoke-HmDevice -Address ann@example.com -Id 0123456789abcdef0123456789abcdef -Credential
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Address,
        [Parameter(Mandatory = $true, Position = 1, ValueFromPipelineByPropertyName = $true)] [Alias('id')] [string] $DeviceId,
        [switch] $Credential,
        [switch] $Forget
    )
    process {
        $path = "/api/v1/accounts/" + [uri]::EscapeDataString($Address) + "/devices/" + [uri]::EscapeDataString($DeviceId)
        $query = @()
        if ($Credential) { $query += 'credential=revoke' }
        if ($Forget) { $query += 'forget=true' }
        if ($query.Count -gt 0) { $path += '?' + ($query -join '&') }
        if ($PSCmdlet.ShouldProcess("$DeviceId of $Address", 'Revoke the client')) {
            Invoke-HmApi -Method DELETE -Path $path
        }
    }
}


function Get-HmComplaint {
    <#
    .SYNOPSIS
        The abuse reports the large providers' feedback loops have sent about a
        domain's mail: who complained, about which message, and which provider
        reported it.
    .EXAMPLE
        Get-HmComplaint -Domain example.com | Format-Table address, feedback_type, sender, arrival
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [string] $Search
    )
    $path = Get-HmSuppressionPath -Domain $Domain -Tail '/complaints'
    if ($Search) { $path += '?search=' + [uri]::EscapeDataString($Search) }
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Get-HmComplaintRate {
    <#
    .SYNOPSIS
        Every hosted domain's complaint rate over the last seven days, worst
        first. rate_ppm is complaints in a million messages delivered; judged is
        false for a domain that has sent too little for a rate to mean anything.
        This is the number the large providers throttle and block on.
    .EXAMPLE
        Get-HmComplaintRate | Format-Table domain, sent, complaints, rate_ppm, judged
    #>
    [CmdletBinding()]
    param()
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path '/api/v1/complaints/rates') -Key 'domains'
}

function Invoke-HmComplaintMaintenance {
    <#
    .SYNOPSIS
        Runs the feedback loops' hourly sweep now: complaints older than
        retention_days, and the daily tally rows behind the rate.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if ($PSCmdlet.ShouldProcess('the complaints', 'Prune the old ones')) {
        Invoke-HmApi -Method POST -Path '/api/v1/complaints/expire'
    }
}

function Get-HmDmarcSummary {
    <#
    .SYNOPSIS
        Who sends as a domain, and with what result: the DMARC aggregate
        reports the receivers of its mail posted to its report mailbox, added
        up over the window. messages, passed (aligned DKIM or aligned SPF),
        failed, quarantined and rejected, then by_day for a chart and sources
        (one per address that sent as the domain, most messages first).
    .EXAMPLE
        Get-HmDmarcSummary -Domain example.com -Days 30 | Select-Object -ExpandProperty sources | Format-Table ip, header_from, messages, passed, failed
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [int] $Days = 30
    )
    Invoke-HmApi -Method GET -Path ((Get-HmSuppressionPath -Domain $Domain -Tail '/dmarc-reports/summary') + '?days=' + $Days)
}

function Get-HmDmarcReport {
    <#
    .SYNOPSIS
        The DMARC aggregate reports read for a domain, newest first - or, with
        -Id, one report with its records: the source, the count, what the
        receiver did and the aligned DKIM and SPF verdicts.
    .EXAMPLE
        Get-HmDmarcReport -Domain example.com | Format-Table id, org, begin, end, messages, passed
    .EXAMPLE
        (Get-HmDmarcReport -Domain example.com -Id 31).records | Format-Table source_ip, count, disposition, dkim, spf
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [long] $Id = 0,
        [int] $Limit = 50,
        [int] $Offset = 0
    )
    if ($Id -gt 0) {
        return Invoke-HmApi -Method GET -Path (Get-HmSuppressionPath -Domain $Domain -Tail ('/dmarc-reports/' + $Id))
    }
    $path = (Get-HmSuppressionPath -Domain $Domain -Tail '/dmarc-reports') + '?limit=' + $Limit + '&offset=' + $Offset
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Get-HmTlsReport {
    <#
    .SYNOPSIS
        The TLS-RPT reports read for a domain over the window: one item per
        policy of each report, with the sessions that succeeded and failed and
        the failures by type.
    .EXAMPLE
        Get-HmTlsReport -Domain example.com | Format-Table org, policy_type, successful, failed
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [int] $Days = 30,
        [int] $Limit = 50,
        [int] $Offset = 0
    )
    $path = (Get-HmSuppressionPath -Domain $Domain -Tail '/tls-reports') + '?days=' + $Days + '&limit=' + $Limit + '&offset=' + $Offset
    Get-HmCollection -Answer (Invoke-HmApi -Method GET -Path $path) -Key 'items'
}

function Invoke-HmDomainReportMaintenance {
    <#
    .SYNOPSIS
        Runs the domain reports' retention sweep now: DMARC aggregate and
        TLS-RPT reports read more than domain_report_retention_days ago are
        removed, or more than -OlderThanDays ago when given (0 removes every
        report of every domain). The sweep runs in passes of 5,000 of each kind,
        a hundred at most; the answer's complete is false when rows remain, and
        another call removes them.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [int] $OlderThanDays = -1
    )
    if ($PSCmdlet.ShouldProcess('the DMARC and TLS-RPT reports', 'Prune the old ones')) {
        if ($OlderThanDays -ge 0) {
            Invoke-HmApi -Method POST -Path '/api/v1/domain-reports/expire' -Body @{ older_than_days = $OlderThanDays }
        } else {
            Invoke-HmApi -Method POST -Path '/api/v1/domain-reports/expire'
        }
    }
}

# ---------------------------------------------------------------------------
# Accounts in bulk
# ---------------------------------------------------------------------------

# The columns, in the order the export writes them. Each is a field of
# POST /api/v1/domains/<domain>/accounts under the create's own name, and the
# export reads the same names from the domain's listing, so a file exported and
# given a password column goes back in as it came out. The create refuses any
# other field by name, so a column not in this list is refused before a request
# is made. The same list and the same rules as hmctl's CSV_FIELDS;
# build/check-cli.py holds the two to the same requests.
$script:AccountCsvColumns = @('address', 'password', 'first_name', 'last_name', 'active', 'max_size_mb', 'admin_level')

function Get-HmOpenPgpKey {
    <#
    .SYNOPSIS
        The OpenPGP PUBLIC keys this server holds: a whole domain's, or one
        account's. A domain's listing carries the keys that are NOT being served
        as well as the ones that are, which is what the published column says.
    .EXAMPLE
        Get-HmOpenPgpKey -Domain example.com | Format-Table address, fingerprint, algorithm_name, published
    .EXAMPLE
        Get-HmOpenPgpKey -Account ann@example.com
    #>
    [CmdletBinding(DefaultParameterSetName = 'Domain')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'Domain')] [string] $Domain,
        [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'Account', ValueFromPipelineByPropertyName = $true)] [string] $Account
    )
    process {
        if ($PSCmdlet.ParameterSetName -eq 'Account') {
            Invoke-HmApi -Method GET -Path ('/api/v1/accounts/' + [uri]::EscapeDataString($Account) + '/openpgp')
        }
        else {
            $answer = Invoke-HmApi -Method GET -Path ('/api/v1/domains/' + [uri]::EscapeDataString($Domain) + '/openpgp')
            Get-HmCollection -Answer $answer -Key 'keys'
        }
    }
}

function Set-HmOpenPgpKey {
    <#
    .SYNOPSIS
        Stores an OpenPGP PUBLIC key for an account, from a file or from a
        string. A SECRET key is refused by the server with a sentence saying so;
        this server never holds one.
    .DESCRIPTION
        -Publish switches the account's own publishing on in the same request.
        -AllowAddressMismatch waives the rule that the key's user ids carry the
        account's own address, and is a server administrator's decision.
    .EXAMPLE
        Set-HmOpenPgpKey -Account ann@example.com -Path .\ann.asc -Publish
    .EXAMPLE
        gpg --armor --export ann@example.com | Out-String | Set-HmOpenPgpKey -Account ann@example.com -Key { $_ }
    #>
    [CmdletBinding(SupportsShouldProcess = $true, DefaultParameterSetName = 'Path')]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Account,
        [Parameter(Mandatory = $true, Position = 1, ParameterSetName = 'Path')] [string] $Path,
        [Parameter(Mandatory = $true, ParameterSetName = 'Key')] [string] $Key,
        [switch] $Publish,
        [switch] $AllowAddressMismatch
    )
    $text = if ($PSCmdlet.ParameterSetName -eq 'Path') { Get-Content -Path $Path -Raw } else { $Key }
    if (-not $text -or -not $text.Trim()) { throw 'There is no key here: the upload is empty.' }
    $body = @{ key = $text }
    if ($Publish) { $body['publish'] = $true }
    if ($AllowAddressMismatch) { $body['allow_address_mismatch'] = $true }
    if ($PSCmdlet.ShouldProcess($Account, 'Store an OpenPGP public key')) {
        Invoke-HmApi -Method PUT -Path ('/api/v1/accounts/' + [uri]::EscapeDataString($Account) + '/openpgp') -Body $body
    }
}

function Remove-HmOpenPgpKey {
    <#
    .SYNOPSIS
        Removes one OpenPGP key from an account by its fingerprint. Anybody who
        looks that address up in the Web Key Directory stops finding it.
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Account,
        [Parameter(Mandatory = $true, Position = 1, ValueFromPipelineByPropertyName = $true)] [string] $Fingerprint
    )
    process {
        if ($PSCmdlet.ShouldProcess("$Fingerprint of $Account", 'Remove the OpenPGP key')) {
            Invoke-HmApi -Method DELETE -Path ('/api/v1/accounts/' + [uri]::EscapeDataString($Account) + '/openpgp/' + [uri]::EscapeDataString($Fingerprint)) | Out-Null
        }
    }
}

function Set-HmOpenPgpPublish {
    <#
    .SYNOPSIS
        Publishes an account's OpenPGP keys in the Web Key Directory, or stops.
        The server setting and the domain's own switch have to be on as well.
    .EXAMPLE
        Set-HmOpenPgpPublish -Account ann@example.com -Enabled $true
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Account,
        [Parameter(Mandatory = $true, Position = 1)] [bool] $Enabled
    )
    process {
        if ($PSCmdlet.ShouldProcess($Account, $(if ($Enabled) { 'Publish the OpenPGP keys' } else { 'Stop publishing the OpenPGP keys' }))) {
            Invoke-HmApi -Method PUT -Path ('/api/v1/accounts/' + [uri]::EscapeDataString($Account)) -Body @{ openpgp_publish = $Enabled }
        }
    }
}

function Get-HmMailboxMovePath {
    param([Parameter(Mandatory = $true)] [string] $Address)
    return '/api/v1/accounts/' + [uri]::EscapeDataString($Address) + '/move'
}

function Get-HmMailboxMove {
    <#
    .SYNOPSIS
        Whether a mailbox could be moved at this moment, and what it has been
        through: address, in_use, in_use_reason and last_move. A POP3 session
        holds a mailbox from authentication until its socket closes, an IMAP
        session while it has one of the account's folders selected; while
        something holds it, Move-HmMailbox is refused.
    .EXAMPLE
        Get-HmMailboxMove -Address ann@example.com
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Address
    )
    process {
        Invoke-HmApi -Method GET -Path (Get-HmMailboxMovePath -Address $Address)
    }
}

function Move-HmMailbox {
    <#
    .SYNOPSIS
        Renames a mailbox, or moves it to another domain on this server -
        NewAddress carries the domain, so the two are one command. Everything
        keyed on the account goes with it: its messages, folders, rules,
        filters, application passwords, second factor, contacts, calendars,
        preferences, password history, sender lists, group memberships and
        role. The old address is kept as an alias unless -DropAlias is given.
    .EXAMPLE
        Move-HmMailbox -Address ann@example.com -NewAddress anne@example.com
    .EXAMPLE
        Move-HmMailbox -Address ann@example.com -NewAddress ann@example.net -DropAlias -Confirm:$false
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0, ValueFromPipelineByPropertyName = $true)] [string] $Address,
        [Parameter(Mandatory = $true, Position = 1)] [string] $NewAddress,
        [switch] $DropAlias
    )
    process {
        $body = @{ new_address = $NewAddress; keep_alias = (-not $DropAlias.IsPresent) }
        if ($PSCmdlet.ShouldProcess($Address, "Move to $NewAddress")) {
            Invoke-HmApi -Method POST -Path (Get-HmMailboxMovePath -Address $Address) -Body $body
        }
    }
}

function Read-HmAccountCsv {
    # The file's records as ordered column -> cell dictionaries, the column
    # names trimmed and lower-cased, each with Extra set when the record has
    # more cells than the header has columns. Internal to Import-HmAccount.
    #
    # Read as UTF-8, strictly, as hmctl reads it: Import-Csv in Windows
    # PowerShell 5.1 reads a file without a byte-order mark in the ANSI code
    # page, which turns every accented name into mojibake, and a file that is
    # not UTF-8 at all is refused rather than imported wrong. And parsed by
    # the .NET CSV reader rather than ConvertFrom-Csv, which drops a cell past
    # the header's last column without a word - so "Adams, Ann" written
    # without quotes would have imported as Adams.
    param([Parameter(Mandatory = $true)] [string] $Path)

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath)
    try {
        $text = (New-Object System.Text.UTF8Encoding($false, $true)).GetString($bytes).TrimStart([char] 0xFEFF)
    }
    catch {
        throw "$Path is not UTF-8. Save it as CSV UTF-8 and import it again."
    }

    Add-Type -AssemblyName Microsoft.VisualBasic
    $parser = New-Object Microsoft.VisualBasic.FileIO.TextFieldParser (New-Object System.IO.StringReader $text)
    $parser.TextFieldType = [Microsoft.VisualBasic.FileIO.FieldType]::Delimited
    $parser.SetDelimiters(',')
    $parser.HasFieldsEnclosedInQuotes = $true
    $parser.TrimWhiteSpace = $false

    $header = @()
    $records = New-Object System.Collections.Generic.List[object]
    try {
        if (-not $parser.EndOfData) {
            $header = @($parser.ReadFields() | ForEach-Object { ([string] $_).Trim().ToLowerInvariant() })
        }
        while (-not $parser.EndOfData) {
            $fields = @($parser.ReadFields())
            $cells = [ordered] @{}
            for ($i = 0; $i -lt $header.Count; $i++) {
                $cells[$header[$i]] = if ($i -lt $fields.Count) { [string] $fields[$i] } else { $null }
            }
            $records.Add([pscustomobject] @{ Cells = $cells; Extra = ($fields.Count -gt $header.Count) })
        }
    }
    catch [Microsoft.VisualBasic.FileIO.MalformedLineException] {
        throw "$Path is not CSV this can read, at line $($parser.ErrorLineNumber): $($_.Exception.Message)"
    }
    finally {
        $parser.Close()
    }

    [pscustomobject] @{ Header = $header; Records = $records.ToArray() }
}

function ConvertTo-HmAccountBody {
    # One record as the create's body, or the reason the create would refuse
    # it. A cell is text unless its column is active (a boolean) or max_size_mb
    # (a number): a password of digits alone, or a surname of "True", is a
    # string. An empty cell is not sent, so the server's default applies.
    # Internal to Import-HmAccount.
    param([Parameter(Mandatory = $true)] $Record)

    if ($Record.Extra) {
        return [pscustomobject] @{ Body = $null; Error = 'more cells than the header has columns (a comma inside a value needs the value in quotes)' }
    }

    $body = @{}
    foreach ($column in @($Record.Cells.Keys)) {
        $value = $Record.Cells[$column]
        if ($null -eq $value -or $column -eq '') { continue }
        # Every cell is trimmed but the password's, as hmctl does it: a space at
        # either end of a spreadsheet cell is far more often a slip than part
        # of a name, and trimming one out of a password would make an account
        # whose password is not the one in the file, without a word. So a
        # password with white space at an edge is refused, and -WhatIf shows it.
        $text = ([string] $value).Trim()
        if ($text -eq '') { continue }

        if ($column -eq 'password' -and $text -cne ([string] $value)) {
            return [pscustomobject] @{ Body = $null; Error = 'the password begins or ends with white space, which a spreadsheet cell rarely means; take it out of the cell, or set that password after the import' }
        }

        if ($column -eq 'active') {
            $word = $text.ToLowerInvariant()
            if (@('true', 'yes', 'on', '1') -contains $word) { $body[$column] = $true }
            elseif (@('false', 'no', 'off', '0') -contains $word) { $body[$column] = $false }
            else { return [pscustomobject] @{ Body = $null; Error = "active must be true or false, not '$text'" } }
        }
        elseif ($column -eq 'max_size_mb') {
            [long] $megabytes = 0
            if ($text -notmatch '^[0-9]{1,10}$' -or -not [long]::TryParse($text, [ref] $megabytes) -or $megabytes -gt 2147483647) {
                return [pscustomobject] @{ Body = $null; Error = "max_size_mb must be a whole number of megabytes, 0 for no limit, not '$text'" }
            }
            $body[$column] = $megabytes
        }
        elseif ($column -eq 'admin_level') {
            $level = $text.ToLowerInvariant()
            if (@('user', 'domain', 'server') -notcontains $level) {
                return [pscustomobject] @{ Body = $null; Error = "admin_level must be user, domain or server, not '$text'" }
            }
            $body[$column] = $level
        }
        elseif ($column -eq 'first_name' -or $column -eq 'last_name') {
            # The column's width, in UTF-16 code units - String.Length, which
            # is how the Windows server counts it and how hmctl counts it.
            if ($text.Length -gt 60) {
                return [pscustomobject] @{ Body = $null; Error = "first_name and last_name are at most 60 characters each; this $column has $($text.Length)" }
            }
            $body[$column] = $text
        }
        else {
            $body[$column] = $text
        }
    }

    if (-not $body.ContainsKey('password')) {
        return [pscustomobject] @{ Body = $null; Error = 'address and password are required; this row has no password' }
    }

    [pscustomobject] @{ Body = $body; Error = $null }
}

function Import-HmAccount {
    <#
    .SYNOPSIS
        Makes accounts from a CSV file, with a dry run.
    .DESCRIPTION
        The file is UTF-8, and its header row names the columns: address,
        password, first_name, last_name, active, max_size_mb and admin_level,
        in any order. address and password are required on every row; the rest
        may be left out or left empty, and a column not in that list is refused
        before anything is sent. An address that already exists is left alone
        and counted, because an import that silently overwrote a password
        would lock a hundred people out with one command. A row the server
        would refuse for its cells - no password, an active, max_size_mb or
        admin_level it does not take, a name past 60 characters - is refused
        under -WhatIf as well, and so is a password with white space at its
        start or end. The password policy and the domain's limits are the
        server's to judge, when the row is sent.
    .EXAMPLE
        Import-HmAccount -Path .\new-people.csv -WhatIf
    #>
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Path,
        [string[]] $Domain
    )

    $file = Read-HmAccountCsv -Path $Path
    $records = @($file.Records)
    if ($records.Count -eq 0) { throw "$Path has no rows under its header." }

    if ($file.Header -notcontains 'address') {
        throw "$Path has no 'address' column; its columns are: " + (@($file.Header | Where-Object { $_ }) -join ', ')
    }

    # A header cell left blank over cells left blank is a spreadsheet's
    # trailing comma, not a column.
    $unknown = @()
    foreach ($column in $file.Header) {
        if ($script:AccountCsvColumns -contains $column -or $unknown -contains $column) { continue }
        if ($column -eq '' -and -not ($records | Where-Object { $_.Cells[''] -and ([string] $_.Cells['']).Trim() })) { continue }
        $unknown += $column
    }
    if ($unknown.Count -gt 0) {
        $hint = if ($unknown -contains 'name') { ' A person''s name goes in two columns, first_name and last_name.' } else { '' }
        throw ("$Path has a column the server does not take: " + (($unknown | ForEach-Object { "'$_'" }) -join ', ') +
            '. The columns are ' + ($script:AccountCsvColumns -join ', ') + '.' + $hint)
    }

    $domains = if ($Domain) { $Domain } else {
        $records | ForEach-Object {
            $a = ([string] $_.Cells['address']).Trim()
            if ($a -match '@') { ($a -split '@', 2)[1].ToLowerInvariant() }
        } | Sort-Object -Unique
    }

    $existing = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $domains) {
        try {
            foreach ($account in (Get-HmAccount -Domain $name)) { [void] $existing.Add([string] $account.address) }
        }
        catch {
            Write-Verbose "No accounts read for $name : $_"
        }
    }

    $made = @(); $skipped = @(); $failed = @()

    foreach ($record in $records) {
        $address = ([string] $record.Cells['address']).Trim()
        if (-not $address) { $failed += [pscustomobject] @{ Address = ''; Error = 'no address' }; continue }
        if ($existing.Contains($address)) { $skipped += $address; continue }
        if ($address -notmatch '@') { $failed += [pscustomobject] @{ Address = $address; Error = 'not an e-mail address' }; continue }

        $converted = ConvertTo-HmAccountBody -Record $record
        if ($null -eq $converted.Body) { $failed += [pscustomobject] @{ Address = $address; Error = $converted.Error }; continue }

        if (-not $PSCmdlet.ShouldProcess($address, 'Add account')) { $made += $address; continue }

        try {
            Invoke-HmApi -Method POST -Path ("/api/v1/domains/" + [uri]::EscapeDataString(($address -split '@', 2)[1]) + "/accounts") -Body $converted.Body | Out-Null
            $made += $address
        }
        catch {
            $failed += [pscustomobject] @{ Address = $address; Error = "$_" }
        }
    }

    [pscustomobject] @{
        Made    = $made
        Skipped = $skipped
        Failed  = $failed
    }
}

function Export-HmAccount {
    <#
    .SYNOPSIS
        Writes a domain's accounts as a CSV file, or to the pipeline.
    .DESCRIPTION
        The columns Import-HmAccount reads, less the password: address,
        first_name, last_name, active, max_size_mb and admin_level. No password
        column, because the server does not have the passwords to give, and a
        column of empty passwords re-imported would be a column of accounts
        nobody can sign in to.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Domain,
        [Parameter(Position = 1)] [string] $Path
    )

    $accounts = Get-HmAccount -Domain $Domain |
        Select-Object address, first_name, last_name, active, max_size_mb, admin_level

    if ($Path) {
        # In the file, active is the JSON word, so the file reads the same
        # whichever client wrote it; on the pipeline it stays a boolean, which
        # Where-Object active can filter on.
        $accounts |
            Select-Object address, first_name, last_name,
                @{ Name = 'active'; Expression = { if ($null -eq $_.active) { '' } elseif ($_.active) { 'true' } else { 'false' } } },
                max_size_mb, admin_level |
            Export-Csv -Path $Path -NoTypeInformation -Encoding utf8
    }
    else {
        $accounts
    }
}

# ---------------------------------------------------------------------------
# The whole configuration as one document
# ---------------------------------------------------------------------------

# The sections a document holds, in the order they are applied: a domain before
# the accounts in it. The same list hmctl keeps, and build/check-cli.py holds
# the two to the same requests.
$script:ConfigurationSections = @(
    @{ Name = 'domains';  Collection = '/api/v1/domains';                     Item = '/api/v1/domains/{0}';  Key = 'name';    Parent = $null },
    @{ Name = 'accounts'; Collection = '/api/v1/domains/{1}/accounts';        Item = '/api/v1/accounts/{0}'; Key = 'address'; Parent = 'domain' },
    @{ Name = 'aliases';  Collection = '/api/v1/domains/{1}/aliases';         Item = '/api/v1/aliases/{0}';  Key = 'name';    Parent = 'domain' },
    @{ Name = 'lists';    Collection = '/api/v1/domains/{1}/lists';           Item = '/api/v1/lists/{0}';    Key = 'address'; Parent = 'domain' },
    # NoUpdate: a spam trap is an address and nothing else, so there is nothing
    # about one to change and the server has no PUT route to send a change to.
    @{ Name = 'spam_traps'; Collection = '/api/v1/domains/{1}/spam-traps';    Item = '/api/v1/spam-traps/{0}'; Key = 'address'; Parent = 'domain'; NoUpdate = $true },
    @{ Name = 'groups';   Collection = '/api/v1/groups';                      Item = '/api/v1/groups/{0}';   Key = 'name';    Parent = $null },
    @{ Name = 'rules';    Collection = '/api/v1/rules';                       Item = '/api/v1/rules/{0}';    Key = 'name';    Parent = $null },
    @{ Name = 'routes';   Collection = '/api/v1/routes';                      Item = '/api/v1/routes/{0}';   Key = 'domain_name'; Parent = $null },
    @{ Name = 'ports';    Collection = '/api/v1/ports';                       Item = '/api/v1/ports/{0}';    Key = 'port';    Parent = $null },
    @{ Name = 'ipranges'; Collection = '/api/v1/ipranges';                    Item = '/api/v1/ipranges/{0}'; Key = 'name';    Parent = $null }
)

# What the server allocated or computed rather than what was configured, and a
# piece of another field split out for a reader (a spam trap's local_part, which
# is its address up to the @). None of it travels into a document, and none of it
# is compared - a field here that reached a create would be refused, since the
# write routes take the fields they own and nothing else, and the document would
# not apply to the server it came from. hmctl's list, key for key.
$script:NotConfiguration = @(
    'id', 'account_id', 'domain_id', 'rule_id', 'route_id', 'parent_id',
    'account_count', 'alias_count', 'member_count', 'size_mb', 'size',
    'created', 'modified', 'last_used', 'last_login', 'last_logon',
    'local_part'
)

# A domain's selector and key files while its DKIM key rotation is on: the rotation
# moves them on its own schedule, so they are the server's state and not the
# document's. An export taken before a switch would otherwise differ from the
# server after it, and importing it would ask for the old key back, which the
# server refuses. Left out of the document, out of the comparison and out of what
# is sent whenever the domain has the rotation on - the same rule as hmctl's.
#
# The server holds them for a while with the schedule off too, while a rotation
# started by hand has a next key staged or an old selector still being kept. The
# document keeps them then - they are the domain's configuration once that
# rotation is over - but a comparison that finds them different asks the server
# (Test-HmRotationHoldsKeys), and an update leaves them out while it says so.
$script:RotationOwned = @('dkim_selector', 'dkim_private_key_file', 'dkim_secondary_selector', 'dkim_secondary_private_key_file')

function Test-HmRotationHoldsKeys {
    # Whether the server's DKIM key rotation holds a domain's selector and key
    # files right now: its schedule on, a next key staged, or an old selector still
    # being kept - when the server refuses a change to them with 409. A server
    # without the route, or a key that may not read it, answers no.
    param([Parameter(Mandatory = $true)] [string] $Domain)
    try {
        $status = Invoke-HmApi -Method GET -Path (Get-HmDkimRotationPath -Domain $Domain)
    }
    catch {
        return $false
    }
    if (-not $status) { return $false }
    $names = @($status.PSObject.Properties.Name)
    $enabled = ($names -contains 'policy') -and $status.policy -and
               (@($status.policy.PSObject.Properties.Name) -contains 'enabled') -and $status.policy.enabled -eq $true
    $next = ($names -contains 'next') -and $null -ne $status.next
    $retiring = ($names -contains 'retiring') -and $null -ne $status.retiring
    return [bool] ($enabled -or $next -or $retiring)
}

function Test-HmRotationOwnedDiffers {
    param([Parameter(Mandatory = $true)] $Here, [Parameter(Mandatory = $true)] $There)
    $hereNames = @($Here.PSObject.Properties.Name)
    $thereNames = @($There.PSObject.Properties.Name)
    foreach ($owned in $script:RotationOwned) {
        $before = if ($hereNames -contains $owned) { [string] $Here.$owned } else { '' }
        $after = if ($thereNames -contains $owned) { [string] $There.$owned } else { '' }
        if ($before -ne $after) { return $true }
    }
    return $false
}

function Remove-HmRotationOwned {
    param([Parameter(Mandatory = $true)] $Row, [bool] $RotationOn = $false)
    $on = $RotationOn -or ($Row.PSObject.Properties.Name -contains 'dkim_rotation_enabled' -and $Row.dkim_rotation_enabled -eq $true) -or
          ($Row -is [System.Collections.IDictionary] -and $Row.Contains('dkim_rotation_enabled') -and $Row['dkim_rotation_enabled'] -eq $true)
    if (-not $on) { return $Row }
    $kept = [ordered] @{}
    if ($Row -is [System.Collections.IDictionary]) {
        foreach ($key in $Row.Keys) { if ($script:RotationOwned -notcontains $key) { $kept[$key] = $Row[$key] } }
    }
    else {
        foreach ($property in $Row.PSObject.Properties) { if ($script:RotationOwned -notcontains $property.Name) { $kept[$property.Name] = $property.Value } }
    }
    [pscustomobject] $kept
}

function ConvertTo-HmConfigurationRow {
    param([Parameter(Mandatory = $true)] $Row)
    # <key>_environment says the server's environment sets <key>
    # (docs/EnvironmentConfiguration.md): the value is the deployment's, stored
    # again at every start, and one a file supplied is answered as (set) or null.
    # Neither the key nor the mark is configuration; applied back, both would be
    # refused.
    $managed = @($Row.PSObject.Properties | Where-Object { $_.Name -like '*_environment' } |
        ForEach-Object { $_.Name.Substring(0, $_.Name.Length - '_environment'.Length) })
    $ordered = [ordered] @{}
    foreach ($property in ($Row.PSObject.Properties | Sort-Object Name)) {
        if ($script:NotConfiguration -contains $property.Name) { continue }
        if ($property.Name -like '*_id') { continue }
        # <key>_levels is what the server says about a key (docs/SettingLevels.md),
        # not configuration: applied back, the server would refuse it.
        if ($property.Name -like '*_levels') { continue }
        if ($property.Name -like '*_environment') { continue }
        if ($managed -contains $property.Name) { continue }
        $ordered[$property.Name] = $property.Value
    }
    $ordered
}

function Get-HmChangedSetting {
    <#
    .SYNOPSIS
        Every setting whose value is not the one this version of the server ships
        with, with the shipped value beside it: store, name, value and default.

    .DESCRIPTION
        The first thing to look at when a server behaves oddly and the first thing
        to put in a support message. store is ini for the keys that were the
        [Settings] section of hMailServer.ini and database for hm_settings. A
        setting whose value is a secret is named with its value as (set) and never
        shown. known is false for a setting this build ships no default for - a key
        stored for a script of your own - which is a difference from stock all the
        same, and overridden is true where the [SettingsOverride] section of
        hMailServer.ini is forcing the value over whatever is stored. -Summary
        answers the whole document instead, with the counts.

        Answers to the administrator password and to nothing else, as
        Get-HmIniSetting does: an API key is refused, and so is an account
        holding an administrative role.
    #>
    [CmdletBinding()]
    param([switch] $Summary)
    $answer = Invoke-HmApi -Method GET -Path '/api/v1/settings/changed'
    if ($Summary) { $answer } else { Get-HmCollection -Answer $answer -Key 'settings' }
}

function Export-HmConfiguration {
    <#
    .SYNOPSIS
        The whole configuration as one object, or one file.
    .DESCRIPTION
        The settings groups, the ini keys the server exposes, the domains and
        their accounts, aliases and distribution lists, the groups, rules,
        routes, listeners and IP ranges. Sorted throughout and carrying nothing
        the server allocated, so two exports of an unchanged server are the same
        bytes - which is what makes a document usable in a repository.

        No passwords: the server does not give them out. No certificates and no
        directories: files and paths of one machine. No secret among the ini
        keys: the server lists those as "(set)", and they are left out rather
        than carried as a placeholder an import would write.
    .EXAMPLE
        Export-HmConfiguration -Path .\server.json
    #>
    [CmdletBinding()]
    param([Parameter(Position = 0)] [string] $Path)

    $settings = [ordered] @{}
    foreach ($group in ($script:SettingGroups | Sort-Object)) {
        if ($group -eq 'directories') { continue }
        try {
            $answer = Invoke-HmApi -Method GET -Path (Get-HmSettingPath -Group $group)
        }
        catch {
            continue
        }
        $settings[$group] = ConvertTo-HmConfigurationRow -Row $answer
    }

    $ini = [ordered] @{}
    try {
        # The whole answer, not only its rows: its environment map names every key
        # the server's environment sets, whatever from. As in the groups
        # (ConvertTo-HmConfigurationRow), such a value is the deployment's, stored
        # again at every start, and applied to a server whose environment sets it
        # the PUT would be refused (409) - so it is not in the document.
        $answer = Invoke-HmApi -Method GET -Path '/api/v1/settings/ini'
        $managed = @{}
        if ($answer -and $answer.PSObject.Properties.Name -contains 'environment' -and $answer.environment) {
            foreach ($key in $answer.environment.PSObject.Properties.Name) { $managed[$key.ToLowerInvariant()] = $true }
        }
        foreach ($row in (Get-HmCollection -Answer $answer -Key 'settings' | Sort-Object name)) {
            if ($row.PSObject.Properties.Name -notcontains 'name') { continue }
            if ($row.PSObject.Properties.Name -notcontains 'value') { continue }
            # A secret is listed as "(set)", never as its value, and stays out of
            # the document: applied, the placeholder would overwrite the secret.
            if ($row.PSObject.Properties.Name -contains 'secret' -and $row.secret) { continue }
            if ($managed.ContainsKey(([string] $row.name).ToLowerInvariant())) { continue }
            $ini[[string] $row.name] = $row.value
        }
    }
    catch { }

    $sections = [ordered] @{ settings = $settings; ini = $ini }

    $domains = @()
    foreach ($section in $script:ConfigurationSections) {
        $rows = @()
        if ($section.Parent -eq 'domain') {
            foreach ($domain in $domains) {
                try {
                    $answer = Invoke-HmApi -Method GET -Path ($section.Collection -f '', [uri]::EscapeDataString($domain))
                }
                catch { continue }
                foreach ($row in (Get-HmCollection -Answer $answer -Key $section.Name)) {
                    $entry = ConvertTo-HmConfigurationRow -Row $row
                    $entry['domain'] = $domain
                    $rows += [pscustomobject] $entry
                }
            }
        }
        else {
            try {
                $answer = Invoke-HmApi -Method GET -Path $section.Collection
            }
            catch { continue }
            foreach ($row in (Get-HmCollection -Answer $answer -Key $section.Name)) {
                $entry = [pscustomobject] (ConvertTo-HmConfigurationRow -Row $row)
                if ($section.Name -eq 'domains') { $entry = Remove-HmRotationOwned -Row $entry }
                $rows += $entry
            }
        }

        $rows = @($rows | Sort-Object { $_ | ConvertTo-Json -Depth 10 -Compress })
        $sections[$section.Name] = $rows

        if ($section.Name -eq 'domains') {
            $domains = @($rows | ForEach-Object { [string] $_.name } | Where-Object { $_ } | Sort-Object)
        }
    }

    $document = [ordered] @{ version = 1; sections = $sections }

    if ($Path) {
        ($document | ConvertTo-Json -Depth 20) | Set-Content -Path $Path -Encoding utf8
        return
    }

    $document
}

function Compare-HmConfiguration {
    <#
    .SYNOPSIS
        What would have to change for the server to match a document.
    .DESCRIPTION
        Answers one object per step - Section, Verb, Key, Detail - so that
        Where-Object and Measure-Object work on the plan. Nothing is changed.
    .EXAMPLE
        Compare-HmConfiguration -Path .\server.json | Where-Object Verb -eq delete
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Path)

    $wanted = Get-Content -Path $Path -Raw | ConvertFrom-Json
    $live = Export-HmConfiguration

    $plan = @()

    foreach ($group in $wanted.sections.settings.PSObject.Properties.Name) {
        $here = $live.sections.settings.$group
        $there = $wanted.sections.settings.$group
        foreach ($key in ($there.PSObject.Properties.Name | Sort-Object)) {
            $before = if ($here) { $here.$key } else { $null }
            if ($before -ne $there.$key) {
                $plan += [pscustomobject] @{ Section = "settings/$group"; Verb = 'set'; Key = $key
                                             Detail = "$before -> $($there.$key)" }
            }
        }
    }

    foreach ($key in ($wanted.sections.ini.PSObject.Properties.Name | Sort-Object)) {
        $before = $live.sections.ini.$key
        if ($before -ne $wanted.sections.ini.$key) {
            $plan += [pscustomobject] @{ Section = 'ini'; Verb = 'set'; Key = $key
                                         Detail = "$before -> $($wanted.sections.ini.$key)" }
        }
    }

    foreach ($section in $script:ConfigurationSections) {
        $here = @{}
        foreach ($row in @($live.sections.($section.Name))) {
            if ($row) { $here[[string] $row.($section.Key)] = $row }
        }
        $there = @{}
        foreach ($row in @($wanted.sections.($section.Name))) {
            if (-not $row) { continue }
            $key = [string] $row.($section.Key)
            if ($section.Name -eq 'domains') {
                $liveOn = $here.ContainsKey($key) -and ($here[$key].PSObject.Properties.Name -contains 'dkim_rotation_enabled') -and
                          $here[$key].dkim_rotation_enabled -eq $true
                $documentOn = ($row.PSObject.Properties.Name -contains 'dkim_rotation_enabled') -and $row.dkim_rotation_enabled -eq $true
                if (-not $liveOn -and -not $documentOn -and $here.ContainsKey($key) -and
                    (Test-HmRotationOwnedDiffers -Here $here[$key] -There $row) -and (Test-HmRotationHoldsKeys -Domain $key)) {
                    # A rotation started by hand holds them with the schedule off:
                    # left out of both sides, as they are for a schedule that is on.
                    $liveOn = $true
                    $here[$key] = Remove-HmRotationOwned -Row $here[$key] -RotationOn $true
                }
                $row = Remove-HmRotationOwned -Row $row -RotationOn $liveOn
            }
            $there[$key] = $row
        }

        foreach ($key in ($there.Keys | Sort-Object)) {
            if (-not $here.ContainsKey($key)) {
                $plan += [pscustomobject] @{ Section = $section.Name; Verb = 'create'; Key = $key; Detail = '' }
            }
            elseif (($here[$key] | ConvertTo-Json -Depth 10 -Compress) -ne ($there[$key] | ConvertTo-Json -Depth 10 -Compress)) {
                $plan += [pscustomobject] @{ Section = $section.Name; Verb = 'update'; Key = $key; Detail = '' }
            }
        }

        foreach ($key in ($here.Keys | Sort-Object)) {
            if (-not $there.ContainsKey($key)) {
                $plan += [pscustomobject] @{ Section = $section.Name; Verb = 'delete'; Key = $key; Detail = '' }
            }
        }
    }

    $plan
}

function Import-HmConfiguration {
    <#
    .SYNOPSIS
        Makes the server match a document.
    .DESCRIPTION
        A plan unless -Force, and a deletion needs -AllowDelete on top of that:
        the common case is a document written from one server and applied to
        another that has things of its own, and silently removing them is not a
        thing a configuration tool may do.
    .EXAMPLE
        Import-HmConfiguration -Path .\server.json -WhatIf
    #>
    [CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
    param(
        [Parameter(Mandatory = $true, Position = 0)] [string] $Path,
        [switch] $Force,
        [switch] $AllowDelete
    )

    $wanted = Get-Content -Path $Path -Raw | ConvertFrom-Json
    $plan = @(Compare-HmConfiguration -Path $Path)

    $deletions = @($plan | Where-Object Verb -eq 'delete')
    if (-not $AllowDelete) { $plan = @($plan | Where-Object Verb -ne 'delete') }

    if (-not $Force) {
        if ($deletions -and -not $AllowDelete) {
            Write-Warning ("{0} entr(ies) on the server are not in the document. They are left alone; -AllowDelete removes them." -f $deletions.Count)
        }
        if ($plan) { Write-Warning 'Nothing was changed. Add -Force to apply this plan.' }
        return $plan
    }

    foreach ($step in $plan) {
        if (-not $PSCmdlet.ShouldProcess("$($step.Section) $($step.Key)", $step.Verb)) { continue }

        if ($step.Section -like 'settings/*') {
            $group = $step.Section.Split('/')[1]
            $value = $wanted.sections.settings.$group.($step.Key)
            Invoke-HmApi -Method PUT -Path (Get-HmSettingPath -Group $group) -Body @{ $step.Key = $value } | Out-Null
            continue
        }

        if ($step.Section -eq 'ini') {
            Set-HmIniSetting -Name $step.Key -Value ([string] $wanted.sections.ini.($step.Key)) -Confirm:$false | Out-Null
            continue
        }

        $section = $script:ConfigurationSections | Where-Object Name -eq $step.Section | Select-Object -First 1
        $row = @($wanted.sections.($step.Section)) | Where-Object { [string] $_.($section.Key) -eq $step.Key } | Select-Object -First 1

        if ($step.Verb -eq 'delete') {
            Invoke-HmApi -Method DELETE -Path ($section.Item -f [uri]::EscapeDataString($step.Key)) | Out-Null
            continue
        }

        if ($step.Section -eq 'domains' -and $step.Verb -eq 'update') {
            # Asked of the server at the moment of the update, not of the document.
            $row = Remove-HmRotationOwned -Row $row -RotationOn (Test-HmRotationHoldsKeys -Domain $step.Key)
        }

        $body = @{}
        foreach ($property in $row.PSObject.Properties) {
            if ($property.Name -eq 'domain') { continue }
            $body[$property.Name] = $property.Value
        }

        if ($step.Verb -eq 'create') {
            $path = $section.Collection
            if ($section.Parent -eq 'domain') {
                $path = $section.Collection -f '', [uri]::EscapeDataString([string] $row.domain)
            }
            Invoke-HmApi -Method POST -Path $path -Body $body | Out-Null
        }
        elseif ($section.ContainsKey('NoUpdate') -and $section.NoUpdate) {
            # Said here rather than discovered as a 404: a plan that asks to
            # change a resource that cannot be changed means the two sides
            # disagree about a field that is not configuration.
            throw ("A {0} entry is created or removed, never changed; the document and the server differ about {1} in a way nothing can apply." -f $step.Section, $step.Key)
        }
        else {
            Invoke-HmApi -Method PUT -Path ($section.Item -f [uri]::EscapeDataString($step.Key)) -Body $body | Out-Null
        }
    }

    $plan
}

# ---------------------------------------------------------------------------
# Completion: a domain or an address completes from the server itself
# ---------------------------------------------------------------------------

$script:CompleterDomain = {
    param($commandName, $parameterName, $wordToComplete, $commandAst, $fakeBoundParameters)
    if (-not $script:Session) { return }
    try {
        Get-HmDomain |
            Where-Object { $_.name -like "$wordToComplete*" } |
            ForEach-Object { [System.Management.Automation.CompletionResult]::new($_.name, $_.name, 'ParameterValue', $_.name) }
    }
    catch { }
}

$script:CompleterAddress = {
    param($commandName, $parameterName, $wordToComplete, $commandAst, $fakeBoundParameters)
    if (-not $script:Session) { return }
    try {
        $domains = if ($fakeBoundParameters.ContainsKey('Domain')) { @($fakeBoundParameters['Domain']) } else { (Get-HmDomain).name }
        foreach ($domain in $domains) {
            Get-HmAccount -Domain $domain |
                Where-Object { $_.address -like "$wordToComplete*" } |
                ForEach-Object { [System.Management.Automation.CompletionResult]::new($_.address, $_.address, 'ParameterValue', $_.address) }
        }
    }
    catch { }
}

Register-ArgumentCompleter -CommandName Get-HmAccount, New-HmAccount, Get-HmAlias, New-HmAlias, Export-HmAccount, Get-HmDomain, Set-HmDomain, Remove-HmDomain -ParameterName Domain -ScriptBlock $script:CompleterDomain
Register-ArgumentCompleter -CommandName Get-HmDomain, Set-HmDomain, Remove-HmDomain -ParameterName Name -ScriptBlock $script:CompleterDomain
Register-ArgumentCompleter -CommandName Get-HmDkimRotation, Start-HmDkimRotation, Test-HmDkimRotation, Complete-HmDkimRotation, Remove-HmDkimRetiringKey, Stop-HmDkimRotation -ParameterName Domain -ScriptBlock $script:CompleterDomain
Register-ArgumentCompleter -CommandName Get-HmAccount, Set-HmAccount, Remove-HmAccount, Get-HmMailboxMove, Move-HmMailbox -ParameterName Address -ScriptBlock $script:CompleterAddress
Register-ArgumentCompleter -CommandName Get-HmSpamTrap, Add-HmSpamTrap, Get-HmSpamTrapHit -ParameterName Domain -ScriptBlock $script:CompleterDomain
Register-ArgumentCompleter -CommandName Get-HmDmarcSummary, Get-HmDmarcReport, Get-HmTlsReport -ParameterName Domain -ScriptBlock $script:CompleterDomain

Export-ModuleMember -Function @(
    'Get-HmComplaint', 'Get-HmComplaintRate', 'Invoke-HmComplaintMaintenance',
    'Get-HmDmarcSummary', 'Get-HmDmarcReport', 'Get-HmTlsReport', 'Invoke-HmDomainReportMaintenance',
    'Get-HmDevice', 'Revoke-HmDevice',
    'Connect-HmServer', 'Disconnect-HmServer', 'Get-HmSession', 'Invoke-HmApi', 'Get-HmCollection',
    'Get-HmStatus',
    'Get-HmDomain', 'New-HmDomain', 'Set-HmDomain', 'Remove-HmDomain',
    'Get-HmAccount', 'New-HmAccount', 'Set-HmAccount', 'Remove-HmAccount', 'Export-HmAccountArchive', 'Remove-HmAddressTraces',
    'Get-HmAlias', 'New-HmAlias', 'Remove-HmAlias',
    'Get-HmSpamTrap', 'Add-HmSpamTrap', 'Remove-HmSpamTrap', 'Get-HmSpamTrapHit', 'Clear-HmSpamTrapHit',
    'Get-HmJournalRule', 'New-HmJournalRule', 'Set-HmJournalRule', 'Remove-HmJournalRule',
    'Get-HmGroup', 'New-HmGroup', 'Remove-HmGroup', 'Get-HmGroupMember', 'Add-HmGroupMember', 'Remove-HmGroupMember',
    'Get-HmSetting', 'Set-HmSetting', 'Test-HmBreachCheckEndpoint', 'Get-HmIniSetting', 'Set-HmIniSetting',
    'Get-HmQueue', 'Start-HmQueueDelivery', 'Remove-HmQueueMessage',
    'Get-HmQuarantine', 'Restore-HmQuarantineMessage',
    'Get-HmLog', 'Get-HmBackup', 'Start-HmBackup', 'Restore-HmBackup', 'Set-HmBackupPassphrase',
    'Test-HmRuleCriterion',
    'Get-HmDkimRotation', 'Start-HmDkimRotation', 'Test-HmDkimRotation', 'Complete-HmDkimRotation',
    'Get-HmReputation', 'Invoke-HmReputationCheck',
    'Remove-HmDkimRetiringKey', 'Stop-HmDkimRotation', 'Invoke-HmDkimRotation',
    'Get-HmSuppression', 'Add-HmSuppression', 'Remove-HmSuppression', 'Get-HmBounceCount',
    'Get-HmChangedSetting',
    'Test-HmBounceStatus', 'Invoke-HmBounceMaintenance',
    'Get-HmWebhook', 'New-HmWebhook', 'Set-HmWebhook', 'Remove-HmWebhook',
    'Get-HmWebhookDelivery', 'Invoke-HmWebhookDelivery', 'Test-HmWebhook',
    'Get-HmOpenPgpKey', 'Set-HmOpenPgpKey', 'Remove-HmOpenPgpKey', 'Set-HmOpenPgpPublish',
    'Import-HmAccount', 'Export-HmAccount',
    'Get-HmMailboxMove', 'Move-HmMailbox',
    'Export-HmConfiguration', 'Compare-HmConfiguration', 'Import-HmConfiguration',
    'ConvertTo-HmConfigurationRow', 'Get-HmSettingPath'
)
