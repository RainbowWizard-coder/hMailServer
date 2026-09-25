# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   Turns the NUnit 3 result file the Windows suite writes (TestResult.xml, from
   nunit3-console under build\run-tests.ps1) into JUnit XML, for GitLab's test
   report (artifacts:reports:junit of the windows-gate job).

   PowerShell and not Python because the bench it runs on has no Python: the
   Microsoft Store's stand-in is all there is, and not on LocalSystem's PATH.

   One <testsuite> per fixture class, one <testcase> per test case:
     Passed, Warning             a plain testcase
     Failed, label Error         <error>, with the message and the stack
     Failed, any other label     <failure>, with the message and the stack
     Skipped, Inconclusive       <skipped>, with the reason
   and one more <testcase>, "(one-time teardown)", for a fixture whose
   OneTimeTearDown failed after its tests passed - NUnit marks only the suite, and
   a report without it would be green while the run is not. Output is kept for
   failures only.

   It prints the run's own counts and exits with them:
     SUMMARY Test Count: T, Passed: P, Failed: F, Inconclusive: I, Skipped: S
     0  nothing failed and at least one test passed
     1  a test, or a fixture's teardown, failed
     2  the file is missing or not an NUnit 3 result, or nothing passed

   Usage: pwsh -NoProfile -File build\ci\nunit-to-junit.ps1 -Path TestResult.xml -Out junit.xml
#>

param(
   [Parameter(Mandatory)][string] $Path,
   [Parameter(Mandatory)][string] $Out
)

$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

$document = [Xml.XmlDocument]::new()
try {
   $document.Load((Resolve-Path -LiteralPath $Path).ProviderPath)
} catch {
   Write-Output "SUMMARY unreadable NUnit result ${Path}: $($_.Exception.Message)"
   exit 2
}
$run = $document.DocumentElement
if ($run.LocalName -ne 'test-run') {
   Write-Output "SUMMARY $Path is not an NUnit 3 result file (its root is $($run.LocalName))"
   exit 2
}

# Characters XML 1.0 does not allow; test output can carry them and GitLab would
# refuse the whole report.
$invalid = [regex]'[\x00-\x08\x0B\x0C\x0E-\x1F\uFFFE\uFFFF]'
function Clean([string] $text, [int] $limit = 65536) {
   if (-not $text) { return '' }
   $text = $invalid.Replace($text, '?')
   if ($text.Length -gt $limit) { $text = $text.Substring(0, $limit) + "`n[... cut at $limit characters]" }
   return $text
}
function FirstLine([string] $text, [string] $otherwise) {
   $line = (($text -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 1)
   if ($line) { return (Clean $line.Trim() 1000) }
   return $otherwise
}
function Inner($node, [string] $xpath) {
   $found = $node.SelectSingleNode($xpath)
   if ($found) { return $found.InnerText }
   return ''
}
function Seconds($node) {
   $value = 0.0
   [void][double]::TryParse([string]$node.GetAttribute('duration'), [Globalization.NumberStyles]::Float, $invariant, [ref]$value)
   return $value
}

$cases = [Collections.Generic.List[object]]::new()

foreach ($node in $run.SelectNodes('//test-case')) {
   $classname = $node.GetAttribute('classname')
   if (-not $classname) { $classname = 'RegressionTests' }
   $name = $node.GetAttribute('name')
   $result = $node.GetAttribute('result')
   $label = $node.GetAttribute('label')
   $case = [ordered]@{
      Class = $classname; Name = $name; Time = (Seconds $node)
      Kind = 'passed'; Message = ''; Text = ''; Output = ''
   }
   switch ($result) {
      'Failed' {
         $message = Inner $node 'failure/message'
         $stack = Inner $node 'failure/stack-trace'
         $case.Kind = if ($label -eq 'Error') { 'error' } else { 'failure' }
         $case.Message = FirstLine $message ("Failed" + $(if ($label) { " ($label)" } else { '' }))
         $case.Text = Clean ($message + $(if ($stack) { "`n" + $stack } else { '' }))
         $case.Output = Clean (Inner $node 'output')
      }
      { $_ -in 'Skipped', 'Inconclusive' } {
         $case.Kind = 'skipped'
         $reason = Inner $node 'reason/message'
         if (-not $reason) { $reason = Inner $node 'failure/message' }
         $case.Message = FirstLine $reason ($result + $(if ($label) { " ($label)" } else { '' }))
      }
   }
   $cases.Add([pscustomobject]$case)
}

# A fixture whose one-time teardown failed: its tests passed and only the suite
# says so.
$teardowns = 0
foreach ($suite in $run.SelectNodes("//test-suite[@result='Failed' and @site='TearDown']")) {
   $teardowns++
   $message = Inner $suite 'failure/message'
   $stack = Inner $suite 'failure/stack-trace'
   $fullname = $suite.GetAttribute('fullname')
   $cases.Add([pscustomobject][ordered]@{
      Class = $(if ($suite.GetAttribute('classname')) { $suite.GetAttribute('classname') } else { $fullname })
      Name = '(one-time teardown)'; Time = 0.0; Kind = 'failure'
      Message = (FirstLine $message 'OneTimeTearDown failed')
      Text = (Clean ($message + $(if ($stack) { "`n" + $stack } else { '' })))
      Output = ''
   })
}

function Count($list, [string] $kind) { @($list | Where-Object { $_.Kind -eq $kind }).Count }

$settings = [Xml.XmlWriterSettings]::new()
$settings.Encoding = [Text.UTF8Encoding]::new($false)
$settings.Indent = $true
$writer = [Xml.XmlWriter]::Create($Out, $settings)
$writer.WriteStartDocument()
$writer.WriteStartElement('testsuites')
$writer.WriteAttributeString('name', 'RegressionTests')
$writer.WriteAttributeString('tests', [string]$cases.Count)
$writer.WriteAttributeString('failures', [string](Count $cases 'failure'))
$writer.WriteAttributeString('errors', [string](Count $cases 'error'))
$writer.WriteAttributeString('skipped', [string](Count $cases 'skipped'))
foreach ($group in ($cases | Group-Object Class | Sort-Object Name)) {
   $members = $group.Group
   $writer.WriteStartElement('testsuite')
   $writer.WriteAttributeString('name', $group.Name)
   $writer.WriteAttributeString('tests', [string]$members.Count)
   $writer.WriteAttributeString('failures', [string](Count $members 'failure'))
   $writer.WriteAttributeString('errors', [string](Count $members 'error'))
   $writer.WriteAttributeString('skipped', [string](Count $members 'skipped'))
   $writer.WriteAttributeString('time', ([double](($members | Measure-Object Time -Sum).Sum)).ToString('0.000', $invariant))
   foreach ($case in $members) {
      $writer.WriteStartElement('testcase')
      $writer.WriteAttributeString('classname', $case.Class)
      $writer.WriteAttributeString('name', (Clean $case.Name 1000))
      $writer.WriteAttributeString('time', ([double]$case.Time).ToString('0.000', $invariant))
      switch ($case.Kind) {
         { $_ -in 'failure', 'error' } {
            $writer.WriteStartElement($case.Kind)
            $writer.WriteAttributeString('message', $case.Message)
            $writer.WriteAttributeString('type', $case.Kind)
            $writer.WriteString($case.Text)
            $writer.WriteEndElement()
            if ($case.Output) { $writer.WriteElementString('system-out', $case.Output) }
         }
         'skipped' {
            $writer.WriteStartElement('skipped')
            $writer.WriteAttributeString('message', $case.Message)
            $writer.WriteEndElement()
         }
      }
      $writer.WriteEndElement()
   }
   $writer.WriteEndElement()
}
$writer.WriteEndElement()
$writer.WriteEndDocument()
$writer.Dispose()

$total = $run.GetAttribute('total'); $passed = $run.GetAttribute('passed'); $failed = $run.GetAttribute('failed')
$inconclusive = $run.GetAttribute('inconclusive'); $skipped = $run.GetAttribute('skipped')
Write-Output "SUMMARY Test Count: $total, Passed: $passed, Failed: $failed, Inconclusive: $inconclusive, Skipped: $skipped"
if ($teardowns) { Write-Output "and $teardowns fixture(s) whose one-time teardown failed" }

if ([int]$failed -gt 0 -or (Count $cases 'failure') -gt 0 -or (Count $cases 'error') -gt 0) { exit 1 }
if ([int]$passed -eq 0) {
   Write-Output 'No test passed: a run of nothing is not a pass.'
   exit 2
}
exit 0
