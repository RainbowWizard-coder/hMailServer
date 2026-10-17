# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The manifest of the HmailServer module (HmailServer.psm1 beside it): what the
# PowerShell Gallery lists and what Import-Module reads first.
#
# ModuleVersion is the server's version, Common/Application/Version.h's, stamped with
# it at a release (RELEASE.md, "Version stamp"). FunctionsToExport is the list the
# module's own Export-ModuleMember names, written out rather than '*', so that
# PowerShell knows the commands without loading the module and the Gallery can list
# them. build/check-cli-packages.py fails when either has drifted from its source
# (hmailserver/docs/CommandLinePackages.md).

@{
    RootModule           = 'HmailServer.psm1'
    ModuleVersion        = '6.3.7'
    GUID                 = '89f008bd-5dc0-4d6d-9a05-4ac1c8dbf8e5'
    Author               = 'Christopher Holloway / Progressive Robot Ltd'
    CompanyName          = 'Progressive Robot Ltd'
    Copyright            = '(c) 2026 Christopher Holloway / Progressive Robot Ltd. AGPL-3.0-or-later.'
    Description          = 'Administer an hMailServer - on Windows or on Linux, on this machine or another - over its REST API from PowerShell: domains, accounts, aliases, lists, settings, the queue, backups, DKIM rotation, webhooks and the rest, as objects. The same vocabulary as hmctl, its command-line twin.'

    # Windows PowerShell 5.1 and PowerShell 7, on any platform PowerShell 7 runs on.
    PowerShellVersion    = '5.1'
    CompatiblePSEditions = @('Desktop', 'Core')

    FunctionsToExport    = @(
        'Add-HmGroupMember',
        'Add-HmSpamTrap',
        'Add-HmSuppression',
        'Clear-HmSpamTrapHit',
        'Compare-HmConfiguration',
        'Complete-HmDkimRotation',
        'Connect-HmServer',
        'ConvertTo-HmConfigurationRow',
        'Disconnect-HmServer',
        'Export-HmAccount',
        'Export-HmAccountArchive',
        'Export-HmConfiguration',
        'Get-HmAccount',
        'Get-HmAlias',
        'Get-HmBackup',
        'Get-HmBounceCount',
        'Get-HmChangedSetting',
        'Get-HmCollection',
        'Get-HmComplaint',
        'Get-HmComplaintRate',
        'Get-HmDevice',
        'Get-HmDkimRotation',
        'Get-HmDmarcReport',
        'Get-HmDmarcSummary',
        'Get-HmDomain',
        'Get-HmGroup',
        'Get-HmGroupMember',
        'Get-HmIniSetting',
        'Get-HmJournalRule',
        'Get-HmLog',
        'Get-HmMailboxMove',
        'Get-HmOpenPgpKey',
        'Get-HmQuarantine',
        'Get-HmQueue',
        'Get-HmReputation',
        'Get-HmSession',
        'Get-HmSetting',
        'Get-HmSettingPath',
        'Get-HmSpamTrap',
        'Get-HmSpamTrapHit',
        'Get-HmStatus',
        'Get-HmSuppression',
        'Get-HmTlsReport',
        'Get-HmWebhook',
        'Get-HmWebhookDelivery',
        'Import-HmAccount',
        'Import-HmConfiguration',
        'Invoke-HmApi',
        'Invoke-HmBounceMaintenance',
        'Invoke-HmComplaintMaintenance',
        'Invoke-HmDkimRotation',
        'Invoke-HmDomainReportMaintenance',
        'Invoke-HmReputationCheck',
        'Invoke-HmWebhookDelivery',
        'Move-HmMailbox',
        'New-HmAccount',
        'New-HmAlias',
        'New-HmDomain',
        'New-HmGroup',
        'New-HmJournalRule',
        'New-HmWebhook',
        'Remove-HmAccount',
        'Remove-HmAddressTraces',
        'Remove-HmAlias',
        'Remove-HmDkimRetiringKey',
        'Remove-HmDomain',
        'Remove-HmGroup',
        'Remove-HmGroupMember',
        'Remove-HmJournalRule',
        'Remove-HmOpenPgpKey',
        'Remove-HmQueueMessage',
        'Remove-HmSpamTrap',
        'Remove-HmSuppression',
        'Remove-HmWebhook',
        'Restore-HmBackup',
        'Restore-HmQuarantineMessage',
        'Revoke-HmDevice',
        'Set-HmAccount',
        'Set-HmBackupPassphrase',
        'Set-HmDomain',
        'Set-HmIniSetting',
        'Set-HmJournalRule',
        'Set-HmOpenPgpKey',
        'Set-HmOpenPgpPublish',
        'Set-HmSetting',
        'Set-HmWebhook',
        'Start-HmBackup',
        'Start-HmDkimRotation',
        'Start-HmQueueDelivery',
        'Stop-HmDkimRotation',
        'Test-HmBounceStatus',
        'Test-HmBreachCheckEndpoint',
        'Test-HmDkimRotation',
        'Test-HmRuleCriterion',
        'Test-HmWebhook'
    )
    CmdletsToExport      = @()
    VariablesToExport    = @()
    AliasesToExport      = @()

    PrivateData          = @{
        PSData = @{
            Tags         = @('hMailServer', 'mail', 'email', 'smtp', 'imap', 'pop3', 'REST', 'administration',
                             'PSEdition_Desktop', 'PSEdition_Core', 'Windows', 'Linux')
            ProjectUri   = 'https://gitlab.com/hmailserver/hmailserver'
            LicenseUri   = 'https://gitlab.com/hmailserver/hmailserver/-/blob/master/LICENSE'
            ReleaseNotes = 'https://gitlab.com/hmailserver/hmailserver/-/blob/master/CHANGELOG.md'
        }
    }
}
