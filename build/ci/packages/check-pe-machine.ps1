# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
   Is every program and library an installer carries built for the processor the
   installer is for? The windows-packages-x64 and windows-packages-arm64 jobs
   (.gitlab/ci/packages-windows.yml) run this on the files an Inno Setup script
   packs, after everything is built and placed and before ISCC compiles it.

   Why this and not an installation: the ARM64 installer cannot be run on the
   bench. hMailServerARM64.iss says ArchitecturesAllowed=arm64, so its Setup stops
   on an x64 Windows before it looks at /DIR; GitLab has no hosted Windows ARM
   runner; and an x64 machine cannot start an ARM64 service. What can be checked
   on an x64 machine is what the installer is made of, and that is what this does,
   from the same script ISCC reads:

     -Iss <script>  follows the script as ISPP would - its #define (strings,
                    GetEnv, ReadIni, SourcePath, +), #ifdef, #ifndef, #if with ==
                    and !=, #else, #elif, #endif, #include and #error. A name an
                    #expr or a #for assigns is not known after it: a Source:
                    that uses it is an error, an #if of it undecided. A #sub's
                    body runs where a #for or a call names it, not where it
                    stands: there it may hold #if and the rest, #expr, #for,
                    #pragma and #error, and a line of script (what a call would
                    emit) or any other directive is an error. Under an #if (or
                    #ifexist) it does not evaluate, an #error, #expr, #for,
                    #pragma or a line outside [Files] is passed over, and a
                    [Files] entry, a section, #define, #undef or #include is an
                    error; #emit (#=), #insert, #append, #dim, #redim, #file and
                    a directive it does not know are an error wherever they are
                    compiled in - and takes
                    every Source: line of every [Files] section that is compiled
                    in, with {#...} expanded, its wildcard and recursesubdirs and
                    Excludes applied, relative to the main script's folder as ISCC
                    resolves it. An entry whose DestDir is {tmp} is not checked: it
                    is an installer Setup runs (the .NET Desktop Runtime, SQL
                    Server Compact), each held to its SHA-256 or SHA-512 pin by
                    build\get-dotnet-runtime.ps1 and build\get-installer-binaries.ps1.
                    An entry flagged external is not compiled in and is skipped too.
     -Path <files or folders>  checks those instead, recursively.

   A file is a PE image when it starts with MZ and has a PE header; every other
   file is data and is only counted. For each PE image the COFF header's Machine
   is read, and the CLR header's flags when the image is a .NET assembly:

     native image      must be the -Expect machine: 0x8664 for x64, 0xAA64 for ARM64.
                       An x64 image with CHPE metadata in its load configuration is
                       ARM64EC code, named so, and is not ARM64: an ARM64 process
                       does not load it (vcruntime140_1.dll in the arm64 folder of
                       MSVC 14.51's redistributable is one);
     .NET assembly     the -Expect machine, or AnyCPU - Machine 0x014C, IL only,
                       32BITREQUIRED clear or with 32BITPREFERRED - which the runtime of either processor
                       loads. An assembly marked for another processor (TlbImp
                       /machine:X64, PlatformTarget=x64) is refused: an ARM64 .NET
                       process does not load it.

   -Allow takes file names (wildcards) that are reported and not held to this, for
   a file the first run shows has to be accepted; each is named in the output.

   -List prints what -Iss resolves to - each Source: with its DestDir - without
   needing the files to exist, and checks nothing: what this was checked with on a
   machine that has not built anything.

   Exit codes: 0 every PE image matches; 1 at least one does not; 2 a Source:
   matched no file (and is not flagged skipifsourcedoesntexist), or a -Path does
   not exist; 3 the script could not be followed (an #error, a directive or an
   expression this reader does not know, a missing #include).

   Usage:
     pwsh -NoProfile -File build\ci\packages\check-pe-machine.ps1 -Iss hmailserver\installation\hMailServerARM64.iss -Expect ARM64
     pwsh -NoProfile -File build\ci\packages\check-pe-machine.ps1 -Path <folder> -Expect ARM64
     pwsh -NoProfile -File build\ci\packages\check-pe-machine.ps1 -Iss hmailserver\installation\hMailServer64.iss -Expect x64 -List
     pwsh -NoProfile -File build\ci\packages\check-pe-machine.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Iss')]
param(
   [Parameter(Mandatory, ParameterSetName = 'Iss')]
   [string] $Iss,
   [Parameter(Mandatory, ParameterSetName = 'Path')]
   [string[]] $Path,
   [Parameter(Mandatory, ParameterSetName = 'Iss')]
   [Parameter(Mandatory, ParameterSetName = 'Path')]
   [ValidateSet('x64', 'ARM64')]
   [string] $Expect,
   [Parameter(ParameterSetName = 'Iss')]
   [Parameter(ParameterSetName = 'Path')]
   [string[]] $Allow = @(),
   [Parameter(ParameterSetName = 'Iss')]
   [switch] $List,
   [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
   [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'

$MachineNames = @{
   0x0000 = 'unknown'; 0x014C = 'x86'; 0x01C4 = 'ARM (32-bit)'; 0x8664 = 'x64'; 0xAA64 = 'ARM64'; 0x0200 = 'Itanium'
}
$ExpectedMachine = @{ 'x64' = 0x8664; 'ARM64' = 0xAA64 }

class IssError : System.Exception {
   IssError([string] $message) : base($message) {}
}

# What a name holds once an #expr, a #for or a #sub's body may have set it: not
# known here, so using it stops the reader (or leaves an #if undecided). Where the
# name was not certainly defined before, whether it is now is not known either.
class IssUnknown : IssError {
   [bool] $MaybeUndefined
   IssUnknown([string] $message, [bool] $maybeUndefined) : base($message) { $this.MaybeUndefined = $maybeUndefined }
}

# ---------------------------------------------------------------- the PE reader

function Read-UInt16([byte[]] $b, [int] $o) { return [int]([BitConverter]::ToUInt16($b, $o)) }
function Read-UInt32([byte[]] $b, [int] $o) { return [long]([BitConverter]::ToUInt32($b, $o)) }

# What the headers of one file say: IsPe false for anything that is not a PE image.
function Get-PeInfo([string] $file) {
   $info = [ordered]@{ IsPe = $false; Machine = 0; Magic = 0; Managed = $false; CorFlags = 0; Arm64EC = $false; Note = '' }
   $stream = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
   try {
      $length = $stream.Length
      if ($length -lt 64) { return [pscustomobject]$info }
      $head = New-Object byte[] 64
      [void]$stream.Read($head, 0, 64)
      if ($head[0] -ne 0x4D -or $head[1] -ne 0x5A) { return [pscustomobject]$info }
      $peOffset = Read-UInt32 $head 0x3C
      if ($peOffset -lt 64 -or $peOffset + 24 -gt $length) { $info.Note = 'MZ without a PE header'; return [pscustomobject]$info }
      # The signature, the COFF header and the optional header up to its data
      # directories and the section table: 4 KB past the PE offset covers them in
      # every image a linker writes; a longer header is read whole below.
      $stream.Position = $peOffset
      $size = [int][Math]::Min(4096, $length - $peOffset)
      $pe = New-Object byte[] $size
      [void]$stream.Read($pe, 0, $size)
      if ($pe[0] -ne 0x50 -or $pe[1] -ne 0x45 -or $pe[2] -ne 0 -or $pe[3] -ne 0) { $info.Note = 'MZ without a PE header'; return [pscustomobject]$info }
      $info.IsPe = $true
      $info.Machine = Read-UInt16 $pe 4
      $sections = Read-UInt16 $pe 6
      $optionalSize = Read-UInt16 $pe 20
      $optional = 24
      if ($optionalSize -lt 2) { $info.Note = 'no optional header'; return [pscustomobject]$info }
      $need = [long]($optional + $optionalSize + 40 * $sections)
      if ($need -gt $size) {
         if ($peOffset + $need -gt $length) { $info.Note = 'headers longer than the file'; return [pscustomobject]$info }
         $size = [int]$need
         $pe = New-Object byte[] $size
         $stream.Position = $peOffset
         [void]$stream.Read($pe, 0, $size)
      }
      $info.Magic = Read-UInt16 $pe $optional
      if ($info.Magic -eq 0x10B) { $countAt = $optional + 92; $dirs = $optional + 96 }
      elseif ($info.Magic -eq 0x20B) { $countAt = $optional + 108; $dirs = $optional + 112 }
      else { $info.Note = ('optional header magic 0x{0:X}' -f $info.Magic); return [pscustomobject]$info }
      $count = Read-UInt32 $pe $countAt
      $table = $optional + $optionalSize
      # A file offset from an RVA, through the section that holds it; -1 when none does.
      $toOffset = {
         param([long] $rva)
         for ($i = 0; $i -lt $sections; $i++) {
            $s = $table + 40 * $i
            $virtualSize = Read-UInt32 $pe ($s + 8)
            $virtualAddress = Read-UInt32 $pe ($s + 12)
            $rawSize = Read-UInt32 $pe ($s + 16)
            $rawPointer = Read-UInt32 $pe ($s + 20)
            $span = [Math]::Max($virtualSize, $rawSize)
            if ($rva -ge $virtualAddress -and $rva -lt $virtualAddress + $span) { return [long]($rawPointer + ($rva - $virtualAddress)) }
         }
         return [long]-1
      }
      # An x64 image with CHPE metadata in its load configuration is ARM64EC code
      # (the x64-compatible ARM64 ABI), which an ARM64 process does not load either:
      # IMAGE_LOAD_CONFIG_DIRECTORY64.CHPEMetadataPointer, at 0xC8, when the
      # directory is that long.
      if ($info.Machine -eq 0x8664 -and $info.Magic -eq 0x20B -and $count -gt 10) {
         $lcRva = Read-UInt32 $pe ($dirs + 10 * 8)
         $lcSize = Read-UInt32 $pe ($dirs + 10 * 8 + 4)
         if ($lcRva -ne 0 -and $lcSize -ge 0xD0) {
            $lcOffset = & $toOffset $lcRva
            if ($lcOffset -ge 0 -and $lcOffset + 0xD0 -le $length) {
               $lc = New-Object byte[] 0xD0
               $stream.Position = $lcOffset
               [void]$stream.Read($lc, 0, 0xD0)
               # The structure's own Size field says how much of it there is.
               if ((Read-UInt32 $lc 0) -ge 0xD0 -and [BitConverter]::ToUInt64($lc, 0xC8) -ne 0) { $info.Arm64EC = $true }
            }
         }
      }
      if ($count -lt 15) { return [pscustomobject]$info }
      $clrRva = Read-UInt32 $pe ($dirs + 14 * 8)
      $clrSize = Read-UInt32 $pe ($dirs + 14 * 8 + 4)
      if ($clrRva -eq 0 -or $clrSize -eq 0) { return [pscustomobject]$info }
      $info.Managed = $true
      $clrOffset = & $toOffset $clrRva
      if ($clrOffset -lt 0 -or $clrOffset + 20 -gt $length) { $info.Note = 'CLR header outside every section'; return [pscustomobject]$info }
      $clr = New-Object byte[] 20
      $stream.Position = $clrOffset
      [void]$stream.Read($clr, 0, 20)
      # IMAGE_COR20_HEADER: cb, runtime version, metadata directory, then Flags.
      $info.CorFlags = Read-UInt32 $clr 16
      return [pscustomobject]$info
   } finally {
      $stream.Dispose()
   }
}

# One line's worth: what the image is, and whether it is right for -Expect.
function Test-PeMachine($info, [string] $expect) {
   $want = $ExpectedMachine[$expect]
   $name = $MachineNames[[int]$info.Machine]
   if (-not $name) { $name = 'machine 0x{0:X4}' -f [int]$info.Machine }
   if ($info.Note) { return [pscustomobject]@{ Ok = $false; What = "a PE image this reader could not follow ($($info.Note))" } }
   if ($info.Arm64EC) { $name = 'ARM64EC' }
   if (-not $info.Managed) {
      return [pscustomobject]@{ Ok = ($info.Machine -eq $want -and -not ($info.Arm64EC -and $expect -eq 'ARM64')); What = "$name native" }
   }
   $ilOnly = ($info.CorFlags -band 0x1) -ne 0
   # 32BITREQUIRED alone is x86 only; with 32BITPREFERRED (0x20000) beside it the
   # assembly is AnyCPU that would rather run 32-bit when it starts a process, and
   # a 64-bit process of either processor loads it.
   $required32 = (($info.CorFlags -band 0x2) -ne 0) -and (($info.CorFlags -band 0x20000) -eq 0)
   if ($info.Machine -eq $want) { return [pscustomobject]@{ Ok = $true; What = "$name .NET" } }
   if ($info.Machine -eq 0x014C -and $ilOnly -and -not $required32) { return [pscustomobject]@{ Ok = $true; What = 'AnyCPU .NET' } }
   if ($info.Machine -eq 0x014C -and $required32) { return [pscustomobject]@{ Ok = $false; What = 'x86 .NET (32BITREQUIRED)' } }
   if ($info.Machine -eq 0x014C) { return [pscustomobject]@{ Ok = $false; What = 'x86 .NET (mixed mode, not IL only)' } }
   return [pscustomobject]@{ Ok = $false; What = "$name .NET" }
}

# ---------------------------------------------------------------- the Inno Setup script reader

# An ISPP expression: string literals, names of #defines, SourcePath, the functions
# GetEnv, ReadIni and AddBackslash, +, and == and != for #if. Anything else is an
# IssError: this reader would rather stop than guess what ISCC compiles.
function Get-IssTokens([string] $text) {
   $tokens = New-Object System.Collections.Generic.List[object]
   $i = 0
   while ($i -lt $text.Length) {
      $c = $text[$i]
      if ([char]::IsWhiteSpace($c)) { $i++; continue }
      if ($c -eq '"') {
         $end = $text.IndexOf('"', $i + 1)
         if ($end -lt 0) { throw [IssError]::new("an unterminated string in: $text") }
         $tokens.Add([pscustomobject]@{ Kind = 'string'; Value = $text.Substring($i + 1, $end - $i - 1) })
         $i = $end + 1
         continue
      }
      if ($c -eq '=' -and $i + 1 -lt $text.Length -and $text[$i + 1] -eq '=') { $tokens.Add([pscustomobject]@{ Kind = 'op'; Value = '==' }); $i += 2; continue }
      if ($c -eq '!' -and $i + 1 -lt $text.Length -and $text[$i + 1] -eq '=') { $tokens.Add([pscustomobject]@{ Kind = 'op'; Value = '!=' }); $i += 2; continue }
      if ('+(),'.Contains([string]$c)) { $tokens.Add([pscustomobject]@{ Kind = 'op'; Value = [string]$c }); $i++; continue }
      if ([char]::IsLetter($c) -or $c -eq '_') {
         $start = $i
         while ($i -lt $text.Length -and ([char]::IsLetterOrDigit($text[$i]) -or $text[$i] -eq '_')) { $i++ }
         $tokens.Add([pscustomobject]@{ Kind = 'name'; Value = $text.Substring($start, $i - $start) })
         continue
      }
      throw [IssError]::new("'$c' in an expression this reader does not evaluate: $text")
   }
   return , $tokens
}

function Read-IniValue([string] $file, [string] $section, [string] $key, [string] $default) {
   if (-not (Test-Path -LiteralPath $file)) { return $default }
   $current = ''
   foreach ($line in Get-Content -LiteralPath $file) {
      $t = $line.Trim()
      if ($t -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
      if ($current -ieq $section -and $t -match '^([^=;]+)=(.*)$' -and $Matches[1].Trim() -ieq $key) { return $Matches[2].Trim() }
   }
   return $default
}

function Invoke-IssExpression([string] $text, $state) {
   $tokens = Get-IssTokens $text
   $script:pos = 0
   function Peek { if ($script:pos -lt $tokens.Count) { return $tokens[$script:pos] } return $null }
   function Next { $t = Peek; $script:pos++; return $t }
   function Assert-IssOperator([string] $op) {
      $t = Next
      if (-not $t -or $t.Kind -ne 'op' -or $t.Value -ne $op) { throw [IssError]::new("expected '$op' in: $text") }
   }
   function Get-IssPrimary {
      $t = Next
      if (-not $t) { throw [IssError]::new("an expression ends too early: $text") }
      if ($t.Kind -eq 'string') { return $t.Value }
      if ($t.Kind -eq 'op' -and $t.Value -eq '(') { $v = Get-IssComparison; Assert-IssOperator ')'; return $v }
      if ($t.Kind -ne 'name') { throw [IssError]::new("unexpected '$($t.Value)' in: $text") }
      $after = Peek
      if ($after -and $after.Kind -eq 'op' -and $after.Value -eq '(') {
         [void](Next)
         $arguments = @()
         if (-not ((Peek).Kind -eq 'op' -and (Peek).Value -eq ')')) {
            while ($true) {
               $arguments += , (Get-IssSum)
               $n = Next
               if ($n.Kind -eq 'op' -and $n.Value -eq ')') { break }
               if (-not ($n.Kind -eq 'op' -and $n.Value -eq ',')) { throw [IssError]::new("expected ',' or ')' in: $text") }
            }
         } else { [void](Next) }
         switch ($t.Value) {
            'GetEnv' { $v = [Environment]::GetEnvironmentVariable([string]$arguments[0]); if ($null -eq $v) { return '' } return $v }
            'ReadIni' { return (Read-IniValue ([string]$arguments[0]) ([string]$arguments[1]) ([string]$arguments[2]) ([string]$arguments[3])) }
            'AddBackslash' { $s = [string]$arguments[0]; if ($s.EndsWith('\')) { return $s } return $s + '\' }
            default { throw [IssError]::new("the ISPP function $($t.Value) is not one this reader evaluates: $text") }
         }
      }
      if ($t.Value -eq 'SourcePath') { return $state.SourcePath }
      if ($state.Defines.ContainsKey($t.Value)) {
         $d = $state.Defines[$t.Value]
         if ($d -is [IssError]) { throw $d }
         return $d
      }
      throw [IssError]::new("$($t.Value) is not defined: $text")
   }
   function Get-IssSum {
      $v = Get-IssPrimary
      while ((Peek) -and (Peek).Kind -eq 'op' -and (Peek).Value -eq '+') { [void](Next); $v = [string]$v + [string](Get-IssPrimary) }
      return $v
   }
   function Get-IssComparison {
      $left = Get-IssSum
      $p = Peek
      if ($p -and $p.Kind -eq 'op' -and ($p.Value -eq '==' -or $p.Value -eq '!=')) {
         [void](Next)
         $right = Get-IssSum
         if ($p.Value -eq '==') { return ([string]$left -eq [string]$right) }
         return ([string]$left -ne [string]$right)
      }
      return $left
   }
   $value = Get-IssComparison
   if ($script:pos -ne $tokens.Count) { throw [IssError]::new("more after the expression than this reader evaluates: $text") }
   return $value
}

function Test-Truthy($v) {
   if ($v -is [bool]) { return $v }
   if ($v -is [string]) { return ($v -ne '' -and $v -ne '0') }
   return [bool]$v
}

# {#NAME} and {#expression} in a line, as ISPP's inline directive expands them.
function Expand-IssInline([string] $line, $state) {
   $out = New-Object Text.StringBuilder
   $i = 0
   while ($true) {
      $start = $line.IndexOf('{#', $i)
      if ($start -lt 0) { [void]$out.Append($line.Substring($i)); break }
      $end = $line.IndexOf('}', $start)
      if ($end -lt 0) { throw [IssError]::new("an unterminated {# in: $line") }
      [void]$out.Append($line.Substring($i, $start - $i))
      [void]$out.Append([string](Invoke-IssExpression $line.Substring($start + 2, $end - $start - 2) $state))
      $i = $end + 1
   }
   return $out.ToString()
}

# The parameters of one [Files] entry: Name: value pairs separated by ';', a value
# in double quotes when it holds one.
function Split-IssEntry([string] $line) {
   $entry = @{}
   $i = 0
   while ($i -lt $line.Length) {
      while ($i -lt $line.Length -and ($line[$i] -eq ' ' -or $line[$i] -eq "`t" -or $line[$i] -eq ';')) { $i++ }
      if ($i -ge $line.Length) { break }
      $colon = $line.IndexOf(':', $i)
      if ($colon -lt 0) { break }
      $name = $line.Substring($i, $colon - $i).Trim()
      $i = $colon + 1
      while ($i -lt $line.Length -and $line[$i] -eq ' ') { $i++ }
      if ($i -lt $line.Length -and $line[$i] -eq '"') {
         $value = New-Object Text.StringBuilder
         $i++
         while ($i -lt $line.Length) {
            if ($line[$i] -eq '"') {
               if ($i + 1 -lt $line.Length -and $line[$i + 1] -eq '"') { [void]$value.Append('"'); $i += 2; continue }
               $i++
               break
            }
            [void]$value.Append($line[$i]); $i++
         }
         $entry[$name] = $value.ToString()
         $semi = $line.IndexOf(';', $i)
         $i = if ($semi -lt 0) { $line.Length } else { $semi + 1 }
      } else {
         $semi = $line.IndexOf(';', $i)
         $end = if ($semi -lt 0) { $line.Length } else { $semi }
         $entry[$name] = $line.Substring($i, $end - $i).Trim()
         $i = $end + 1
      }
   }
   return $entry
}

# The names an ISPP expression assigns to - name = ..., a compound name += ...,
# name++ or ++name - its string literals left out. More than it assigns at worst
# (a named argument, a comment), never fewer.
function Get-IssAssignedNames([string] $text) {
   $code = [regex]::Replace($text, '"[^"]*"|''[^'']*''', '""')
   $names = New-Object System.Collections.Generic.List[string]
   foreach ($m in [regex]::Matches($code, '\b([A-Za-z_]\w*)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)')) { $names.Add($m.Groups[1].Value) }
   foreach ($m in [regex]::Matches($code, '\b([A-Za-z_]\w*)\s*(?:\+\+|--)|(?:\+\+|--)\s*([A-Za-z_]\w*)')) {
      $names.Add($(if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }))
   }
   return , $names
}

# Each name an #expr, a #for or a #sub's body assigns: its value no longer known.
function Set-IssUnknown($state, $names, [string] $why) {
   foreach ($name in $names) {
      $old = if ($state.Defines.ContainsKey($name)) { $state.Defines[$name] } else { $null }
      $defined = $state.Defines.ContainsKey($name) -and -not ($old -is [IssUnknown] -and $old.MaybeUndefined)
      $state.Defines[$name] = [IssUnknown]::new("$why sets $name, which this reader does not follow", -not $defined)
   }
}

# Every [Files] entry the script compiles in, in order.
function Read-IssFiles([string] $file, $state) {
   if (-not (Test-Path -LiteralPath $file)) { throw [IssError]::new("#include `"$file`" does not exist") }
   $stack = New-Object System.Collections.Generic.List[object]
   $number = 0
   $inSub = $false
   $subLive = $false
   $subWhere = ''
   # ISPP's short forms: #= is #emit, #! is #expr, #: is #define.
   $short = @{ '=' = 'emit'; '!' = 'expr'; ':' = 'define' }
   foreach ($raw in Get-Content -LiteralPath $file) {
      $number++
      $where = "$(Split-Path -Leaf $file):$number"
      $line = $raw.Trim()
      $isDirective = $line -match '^#\s*(\w+|[=!:])\s*(.*)$'
      if ($isDirective) {
         $directive = $Matches[1].ToLowerInvariant()
         if ($short.ContainsKey($directive)) { $directive = $short[$directive] }
         $rest = $Matches[2].Trim()
      }
      # A #sub's body is what a #for or a call of it runs, not what is read where
      # it stands. hMailServerInnoExtension.iss reads REQUIRED_DB_VERSION with one,
      # whose #expr sets a name and emits nothing. A body may hold what chooses
      # among its own lines (#if and the rest), an #expr or a #for, whose names it
      # sets are then not known here, a #pragma and an #error; a line of script -
      # what a call emits where it is called, as the [Files] entries of ISPP's own
      # AddFile example - or a #define, #include or #emit stops the reader, which
      # does not follow where the sub is called. Not compiled in, it is passed over.
      if ($inSub) {
         if ($isDirective -and $directive -eq 'endsub') { $inSub = $false; continue }
         if (-not $subLive -or $line -eq '' -or $line.StartsWith(';') -or $line.StartsWith('//')) { continue }
         if ($isDirective -and $directive -in @('if', 'elif', 'else', 'endif', 'ifdef', 'ifndef', 'ifexist', 'ifnexist', 'pragma', 'error')) { continue }
         if ($isDirective -and ($directive -eq 'expr' -or $directive -eq 'for')) {
            Set-IssUnknown $state (Get-IssAssignedNames $rest) "${where}: #$directive in the #sub at $subWhere"
            continue
         }
         throw [IssError]::new("${where}: the #sub at $subWhere holds what this reader does not follow where a #for or a call runs it (it reads #if, #expr, #for, #pragma and #error there, and no line of script): $line")
      }
      $active = -not ($stack | Where-Object { -not $_.Active })
      # Under an #if this reader could not evaluate, everything around it compiled
      # in: what lies under it is passed over while it cannot change what this reads
      # (an #error, an #expr or a #for, whose names are then not known, a #pragma,
      # a line outside [Files]), and stops the reader where it could (a [Files]
      # entry, a section, a #define, an #undef, an #include, an #emit...).
      $undecided = @($stack | Where-Object { $_.Unknown -and $_.Outer })
      $live = $active -or $undecided.Count -gt 0
      # (No switch statement here: a continue inside one ends the switch, not this loop.)
      if ($isDirective) {
         if ($directive -eq 'sub') {
            $inSub = $true
            $subLive = $live
            $subWhere = $where
         } elseif ($directive -eq 'endsub') {
            throw [IssError]::new("${where}: #endsub without #sub")
         } elseif ($directive -in @('if', 'ifdef', 'ifndef', 'ifexist', 'ifnexist') -and $undecided.Count -gt 0) {
            $stack.Add([pscustomobject]@{ Active = $false; Taken = $true; Outer = $false; Unknown = $true; Why = '' })
         } elseif ($directive -eq 'ifexist' -or $directive -eq 'ifnexist') {
            # Whether a file exists when ISCC runs: not evaluated here.
            $stack.Add([pscustomobject]@{ Active = $false; Taken = $active; Outer = $active; Unknown = $active; Why = "${where}: #$directive $rest, which this reader does not evaluate" })
         } elseif ($directive -eq 'ifdef' -or $directive -eq 'ifndef') {
            $old = if ($state.Defines.ContainsKey($rest)) { $state.Defines[$rest] } else { $null }
            if ($active -and $old -is [IssUnknown] -and $old.MaybeUndefined) {
               $stack.Add([pscustomobject]@{ Active = $false; Taken = $true; Outer = $true; Unknown = $true; Why = "${where}: #$directive $rest - $($old.Message)" })
            } else {
               $v = $active -and ($state.Defines.ContainsKey($rest) -eq ($directive -eq 'ifdef'))
               $stack.Add([pscustomobject]@{ Active = $v; Taken = $v; Outer = $active; Unknown = $false; Why = '' })
            }
         } elseif ($directive -eq 'if') {
            $v = $false
            $unknown = $false
            $why = ''
            if ($active) { try { $v = Test-Truthy (Invoke-IssExpression $rest $state) } catch [IssError] { $unknown = $true; $why = "${where}: $($_.Exception.Message)" } }
            $stack.Add([pscustomobject]@{ Active = $v; Taken = ($v -or $unknown); Outer = $active; Unknown = $unknown; Why = $why })
         } elseif ($directive -eq 'elif') {
            if ($stack.Count -eq 0) { throw [IssError]::new("${where}: #elif without #if") }
            $top = $stack[$stack.Count - 1]
            if (-not $top.Unknown) {
               $v = $false
               if ($top.Outer -and -not $top.Taken) {
                  try { $v = Test-Truthy (Invoke-IssExpression $rest $state) }
                  catch [IssError] { $top.Unknown = $true; $top.Why = "${where}: $($_.Exception.Message)" }
               }
               $top.Active = $v
               if ($v -or $top.Unknown) { $top.Taken = $true }
            }
         } elseif ($directive -eq 'else') {
            if ($stack.Count -eq 0) { throw [IssError]::new("${where}: #else without #if") }
            $top = $stack[$stack.Count - 1]
            if (-not $top.Unknown) {
               $top.Active = $top.Outer -and -not $top.Taken
               $top.Taken = $true
            }
         } elseif ($directive -eq 'endif') {
            if ($stack.Count -eq 0) { throw [IssError]::new("${where}: #endif without #if") }
            $stack.RemoveAt($stack.Count - 1)
         } elseif (-not $live) {
            # a directive in a part that is not compiled
         } elseif ($directive -eq 'expr' -or $directive -eq 'for') {
            # Evaluated by ISPP, not here: a name either assigns is no longer known.
            Set-IssUnknown $state (Get-IssAssignedNames $rest) "${where}: #$directive"
         } elseif ($directive -eq 'pragma') {
            # A message, a warning or an option: no [Files] entry.
         } elseif ($directive -notin @('define', 'undef', 'include', 'error')) {
            # #emit (and #=), #insert, #append, #dim, #redim, #file, a name ISPP has
            # not: each could write or move a [Files] entry this reader would miss.
            throw [IssError]::new("${where}: #$directive, which this reader does not follow")
         } elseif ($undecided.Count -gt 0) {
            if ($directive -ne 'error') {
               throw [IssError]::new("${where}: #$directive under an #if this reader does not evaluate - $($undecided[0].Why)")
            }
            # an #error under it: ISCC decides
         } elseif ($directive -eq 'define') {
            if ($rest -notmatch '^(\w+)\s*(?:=\s*)?(.*)$') { throw [IssError]::new("${where}: a #define this reader does not follow: $line") }
            $name = $Matches[1]
            $expression = $Matches[2].Trim()
            if ($expression -eq '') { $state.Defines[$name] = '' }
            else {
               # Kept as the error when it cannot be evaluated: it matters only if used.
               try { $state.Defines[$name] = Invoke-IssExpression $expression $state }
               catch [IssError] { $state.Defines[$name] = [IssError]::new("${where}: #define $name = $expression - $($_.Exception.Message)") }
            }
         } elseif ($directive -eq 'undef') {
            $state.Defines.Remove($rest)
         } elseif ($directive -eq 'include') {
            $target = Expand-IssInline ($rest.Trim('"', '<', '>')) $state
            $candidate = Join-Path (Split-Path -Parent $file) $target
            if (-not (Test-Path -LiteralPath $candidate)) { $candidate = Join-Path $state.SourcePath $target }
            Read-IssFiles $candidate $state
         } elseif ($directive -eq 'error') {
            throw [IssError]::new("${where}: #error $rest")
         }
         continue
      }
      if ($undecided.Count -gt 0) {
         if ($line -match '^\[(\w+)\]') { throw [IssError]::new("${where}: a section under an #if this reader does not evaluate - $($undecided[0].Why)") }
         if ($state.Section -eq 'files' -and $line -ne '' -and -not $line.StartsWith(';') -and -not $line.StartsWith('//')) { throw [IssError]::new("${where}: a [Files] entry under an #if this reader does not evaluate - $($undecided[0].Why)") }
         continue
      }
      if (-not $active) { continue }
      if ($line -match '^\[(\w+)\]') { $state.Section = $Matches[1].ToLowerInvariant(); continue }
      if ($state.Section -ne 'files' -or $line -eq '' -or $line.StartsWith(';')) { continue }
      $expanded = try { Expand-IssInline $line $state } catch [IssError] { throw [IssError]::new("${where}: $($_.Exception.Message)") }
      $entry = Split-IssEntry $expanded
      if (-not $entry.ContainsKey('Source')) { continue }
      $state.Entries.Add([pscustomobject]@{
         Where = $where; Source = $entry['Source']; DestDir = [string]$entry['DestDir']
         Flags = @(([string]$entry['Flags']) -split '\s+' | Where-Object { $_ })
         Excludes = @(([string]$entry['Excludes']) -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
      })
   }
   if ($stack.Count -ne 0) { throw [IssError]::new("$(Split-Path -Leaf $file): an #if without its #endif") }
   if ($inSub) { throw [IssError]::new("$(Split-Path -Leaf $file): a #sub without its #endsub") }
}

function Read-IssScript([string] $iss) {
   $full = (Resolve-Path -LiteralPath $iss).ProviderPath
   $state = [pscustomobject]@{
      SourcePath = (Split-Path -Parent $full)
      Defines = @{}
      Section = ''
      Entries = New-Object System.Collections.Generic.List[object]
   }
   Read-IssFiles $full $state
   return $state
}

# The files one entry takes, as ISCC resolves them: relative to the main script's
# folder, the last part of the path a wildcard, recursesubdirs down the tree, Excludes
# matched against the name (or, with a backslash, the end of the path below the base).
function Resolve-IssEntry($entry, [string] $sourcePath) {
   $source = $entry.Source
   $full = if ([IO.Path]::IsPathRooted($source)) { $source } else { Join-Path $sourcePath $source }
   $base = Split-Path -Parent $full
   $pattern = Split-Path -Leaf $full
   $recurse = $entry.Flags -contains 'recursesubdirs'
   if (-not (Test-Path -LiteralPath $base -PathType Container)) { return @() }
   $found = if ($recurse) { @(Get-ChildItem -LiteralPath $base -Filter $pattern -File -Recurse -Force -ErrorAction SilentlyContinue) }
            else { @(Get-ChildItem -LiteralPath $base -Filter $pattern -File -Force -ErrorAction SilentlyContinue) }
   $baseFull = (Resolve-Path -LiteralPath $base).ProviderPath.TrimEnd('\') + '\'
   $kept = foreach ($f in $found) {
      $relative = $f.FullName.Substring([Math]::Min($baseFull.Length, $f.FullName.Length))
      $excluded = $false
      foreach ($x in $entry.Excludes) {
         $x = $x.TrimStart('\')
         if ($x.Contains('\')) { if ($relative -like $x -or $relative -like "*\$x") { $excluded = $true } }
         elseif (($relative -split '\\') | Where-Object { $_ -like $x }) { $excluded = $true }
      }
      if (-not $excluded) { $f }
   }
   return @($kept)
}

# ---------------------------------------------------------------- the check

function Invoke-Check([object[]] $targets, [string] $expect, [string[]] $allow) {
   # $targets: objects with File and Label.
   $pe = 0; $good = 0; $bad = 0; $data = 0; $allowed = 0
   foreach ($t in $targets) {
      $info = Get-PeInfo $t.File
      if (-not $info.IsPe -and -not $info.Note) { $data++; continue }
      $pe++
      $verdict = Test-PeMachine $info $expect
      $leaf = Split-Path -Leaf $t.File
      if (-not $verdict.Ok -and ($allow | Where-Object { $leaf -like $_ })) {
         $allowed++
         "  ALLOWED  {0,-24} {1}" -f $verdict.What, $t.Label
         continue
      }
      if ($verdict.Ok) { $good++; "  ok       {0,-24} {1}" -f $verdict.What, $t.Label }
      else { $bad++; "  WRONG    {0,-24} {1}" -f $verdict.What, $t.Label }
   }
   "PE CHECK ({0}): {1} PE image(s), {2} right, {3} wrong, {4} allowed by -Allow; {5} other file(s)" -f $expect, $pe, $good, $bad, $allowed, $data
   return [pscustomobject]@{ Bad = $bad; Pe = $pe }
}

$parameterSet = $PSCmdlet.ParameterSetName

function Invoke-Main {
   if ($parameterSet -eq 'Path') {
      $targets = @()
      foreach ($p in $Path) {
         if (-not (Test-Path -LiteralPath $p)) { "PE CHECK: $p does not exist"; return 2 }
         if (Test-Path -LiteralPath $p -PathType Container) {
            $targets += Get-ChildItem -LiteralPath $p -File -Recurse -Force | ForEach-Object { [pscustomobject]@{ File = $_.FullName; Label = $_.FullName } }
         } else {
            $targets += [pscustomobject]@{ File = (Resolve-Path -LiteralPath $p).ProviderPath; Label = $p }
         }
      }
      $result = Invoke-Check $targets $Expect $Allow
      $result | Where-Object { $_ -is [string] }
      $r = $result | Where-Object { $_ -isnot [string] }
      if ($r.Bad -gt 0) { return 1 }
      return 0
   }

   try { $state = Read-IssScript $Iss }
   catch [IssError] { "PE CHECK: $Iss could not be followed: $($_.Exception.Message)"; return 3 }
   "== $Iss ($($state.Entries.Count) Source: entries), expected $Expect"
   $targets = @()
   $missing = 0
   foreach ($e in $state.Entries) {
      if ($e.Flags -contains 'external') { "  skipped  external, not compiled in: $($e.Source)"; continue }
      if ($e.DestDir -like '{tmp}*') { "  skipped  run by Setup and held to its pin ({tmp}): $($e.Source)"; continue }
      if ($List) { "  entry    {0} -> {1}{2}" -f $e.Source, $e.DestDir, $(if ($e.Flags) { "  [$($e.Flags -join ' ')]" } else { '' }); continue }
      $files = Resolve-IssEntry $e $state.SourcePath
      if ($files.Count -eq 0) {
         if ($e.Flags -contains 'skipifsourcedoesntexist') { "  absent   (skipifsourcedoesntexist) $($e.Source)"; continue }
         "  MISSING  $($e.Where): $($e.Source) matches no file"
         $missing++
         continue
      }
      foreach ($f in $files) { $targets += [pscustomobject]@{ File = $f.FullName; Label = "$($e.DestDir)\$($f.Name)  <- $($f.FullName)" } }
   }
   if ($List) { return 0 }
   $result = Invoke-Check $targets $Expect $Allow
   $result | Where-Object { $_ -is [string] }
   $r = $result | Where-Object { $_ -isnot [string] }
   if ($missing -gt 0) { "PE CHECK: $missing Source: entr$(if ($missing -eq 1) { 'y' } else { 'ies' }) matched no file - ISCC would stop on it too"; return 2 }
   if ($r.Bad -gt 0) { return 1 }
   return 0
}

# ---------------------------------------------------------------- the self-test

# A PE image made to order: the headers a linker writes, as small as they can be,
# with a CLR header when -Managed, for the reader to be tested without a compiler.
function New-TestPe([string] $file, [int] $machine, [int] $magic, [switch] $Managed, [long] $corFlags = 1, [switch] $Arm64EC) {
   $bytes = New-Object byte[] 1024
   $bytes[0] = 0x4D; $bytes[1] = 0x5A
   [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3C)
   $pe = 0x80
   $bytes[$pe] = 0x50; $bytes[$pe + 1] = 0x45
   [BitConverter]::GetBytes([uint16]$machine).CopyTo($bytes, $pe + 4)
   [BitConverter]::GetBytes([uint16]1).CopyTo($bytes, $pe + 6)
   $optionalSize = if ($magic -eq 0x20B) { 240 } else { 224 }
   [BitConverter]::GetBytes([uint16]$optionalSize).CopyTo($bytes, $pe + 20)
   $optional = $pe + 24
   [BitConverter]::GetBytes([uint16]$magic).CopyTo($bytes, $optional)
   $countAt = if ($magic -eq 0x20B) { $optional + 108 } else { $optional + 92 }
   $dirs = if ($magic -eq 0x20B) { $optional + 112 } else { $optional + 96 }
   [BitConverter]::GetBytes([uint32]16).CopyTo($bytes, $countAt)
   # One section, .text, at RVA 0x2000, raw data at 0x200.
   $section = $optional + $optionalSize
   [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, $section + 8)
   [BitConverter]::GetBytes([uint32]0x2000).CopyTo($bytes, $section + 12)
   [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, $section + 16)
   [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, $section + 20)
   if ($Managed) {
      [BitConverter]::GetBytes([uint32]0x2010).CopyTo($bytes, $dirs + 14 * 8)
      [BitConverter]::GetBytes([uint32]72).CopyTo($bytes, $dirs + 14 * 8 + 4)
      $clr = 0x210
      [BitConverter]::GetBytes([uint32]72).CopyTo($bytes, $clr)
      [BitConverter]::GetBytes([uint32]$corFlags).CopyTo($bytes, $clr + 16)
   }
   if ($Arm64EC) {
      # A load configuration at RVA 0x2100 whose CHPEMetadataPointer is set.
      [BitConverter]::GetBytes([uint32]0x2100).CopyTo($bytes, $dirs + 10 * 8)
      [BitConverter]::GetBytes([uint32]0xD0).CopyTo($bytes, $dirs + 10 * 8 + 4)
      [BitConverter]::GetBytes([uint32]0xD0).CopyTo($bytes, 0x300)
      [BitConverter]::GetBytes([uint64]0x180001000).CopyTo($bytes, 0x300 + 0xC8)
   }
   [IO.File]::WriteAllBytes($file, $bytes)
}

function Invoke-SelfTest {
   function Assert-That([bool] $condition, [string] $what) {
      if ($condition) { "  ok    $what" } else { "  FAIL  $what"; $script:selfTestFailures++ }
   }
   $script:selfTestFailures = 0
   $dir = Join-Path ([IO.Path]::GetTempPath()) ("pe-check-selftest-" + [guid]::NewGuid().ToString('N'))
   New-Item -ItemType Directory -Path $dir | Out-Null
   try {
      "-- the PE reader"
      New-TestPe (Join-Path $dir 'arm64.dll') 0xAA64 0x20B
      New-TestPe (Join-Path $dir 'x64.dll') 0x8664 0x20B
      New-TestPe (Join-Path $dir 'x86.dll') 0x014C 0x10B
      New-TestPe (Join-Path $dir 'anycpu.dll') 0x014C 0x10B -Managed -corFlags 0x1
      New-TestPe (Join-Path $dir 'anycpu32pref.dll') 0x014C 0x10B -Managed -corFlags 0x20003
      New-TestPe (Join-Path $dir 'x86only.dll') 0x014C 0x10B -Managed -corFlags 0x3
      New-TestPe (Join-Path $dir 'managedx64.dll') 0x8664 0x20B -Managed -corFlags 0x1
      New-TestPe (Join-Path $dir 'managedarm64.dll') 0xAA64 0x20B -Managed -corFlags 0x1
      New-TestPe (Join-Path $dir 'arm64ec.dll') 0x8664 0x20B -Arm64EC
      [IO.File]::WriteAllText((Join-Path $dir 'notes.txt'), 'MZ is how this text starts, and it is not a program')
      [IO.File]::WriteAllBytes((Join-Path $dir 'short.bin'), [byte[]](0x4D, 0x5A))
      $v = { param($n, $e) Test-PeMachine (Get-PeInfo (Join-Path $dir $n)) $e }
      Assert-That ((& $v 'arm64.dll' 'ARM64').Ok) 'a native ARM64 image is right for ARM64'
      Assert-That (-not (& $v 'arm64.dll' 'x64').Ok) 'a native ARM64 image is wrong for x64'
      Assert-That (-not (& $v 'x64.dll' 'ARM64').Ok) 'a native x64 image is wrong for ARM64'
      Assert-That ((& $v 'x64.dll' 'x64').Ok) 'a native x64 image is right for x64'
      Assert-That (-not (& $v 'x86.dll' 'ARM64').Ok -and -not (& $v 'x86.dll' 'x64').Ok) 'a native x86 image is wrong for both'
      Assert-That ((& $v 'anycpu.dll' 'ARM64').Ok -and (& $v 'anycpu.dll' 'x64').Ok) 'an AnyCPU assembly is right for both'
      Assert-That ((& $v 'anycpu32pref.dll' 'ARM64').Ok) 'an AnyCPU assembly that prefers 32 bits still loads into a 64-bit process'
      Assert-That (-not (& $v 'x86only.dll' 'ARM64').Ok) 'an assembly that requires 32 bits is wrong'
      Assert-That (-not (& $v 'managedx64.dll' 'ARM64').Ok) 'an assembly marked x64 (TlbImp /machine:X64) is wrong for ARM64'
      Assert-That ((& $v 'managedx64.dll' 'x64').Ok) 'an assembly marked x64 is right for x64'
      Assert-That ((& $v 'managedarm64.dll' 'ARM64').Ok) 'an assembly marked ARM64 is right for ARM64'
      Assert-That (-not (& $v 'arm64ec.dll' 'ARM64').Ok -and (& $v 'arm64ec.dll' 'ARM64').What -eq 'ARM64EC native') 'an ARM64EC image (vcruntime140_1.dll of the arm64 redist) is named and is wrong for ARM64'
      Assert-That ((& $v 'x64.dll' 'ARM64').What -eq 'x64 native') 'an x64 image without CHPE metadata is plain x64'
      Assert-That (-not (Get-PeInfo (Join-Path $dir 'notes.txt')).IsPe) 'a text file that starts with MZ is not a PE image'
      Assert-That (-not (Get-PeInfo (Join-Path $dir 'short.bin')).IsPe) 'two bytes are not a PE image'
      Assert-That ((& $v 'managedx64.dll' 'ARM64').What -eq 'x64 .NET') 'the wrong one is named for what it is'

      "-- the Inno Setup script reader"
      $inst = Join-Path $dir 'installation'
      New-Item -ItemType Directory -Path (Join-Path $inst 'ARM64\bin'), (Join-Path $inst 'x64'), (Join-Path $inst 'tree\sub'), (Join-Path $dir 'libs\ssl-arm64') -Force | Out-Null
      Copy-Item (Join-Path $dir 'arm64.dll') (Join-Path $inst 'ARM64\bin\server.exe')
      Copy-Item (Join-Path $dir 'anycpu.dll') (Join-Path $inst 'ARM64\bin\tool.dll')
      Copy-Item (Join-Path $dir 'x64.dll') (Join-Path $inst 'x64\mysql.dll')
      Copy-Item (Join-Path $dir 'arm64.dll') (Join-Path $inst 'tree\a.dll')
      Copy-Item (Join-Path $dir 'arm64.dll') (Join-Path $inst 'tree\sub\b.dll')
      Copy-Item (Join-Path $dir 'x64.dll') (Join-Path $inst 'tree\sub\skip.dll')
      Copy-Item (Join-Path $dir 'x64.dll') (Join-Path $inst 'runtime.exe')
      Copy-Item (Join-Path $dir 'arm64.dll') (Join-Path $dir 'libs\ssl-arm64\libssl.dll')
      [IO.File]::WriteAllText((Join-Path $inst 'setup.iss'), "[Setup]`r`nAppVersion=9.8.7`r`n")
      $main = @(
         '#define HM_ARM64'
         '#define LIBS = GetEnv("HM_PE_SELFTEST_LIBS")'
         '#define SSL LIBS + "\ssl-arm64"'
         '#define V ReadIni(SourcePath + "\setup.iss", "Setup", "AppVersion", "")'
         '#define NEVER ReadIni(Unknown)'
         '#if V == ""'
         '  #error no version'
         '#endif'
         '#include "files.iss"'
      )
      $files = @(
         '[Files]'
         '; a comment: Source: "nothing.dll"'
         'Source: "ARM64\bin\*"; DestDir: "{app}\Bin"; Flags: ignoreversion'
         '#ifndef HM_ARM64'
         'Source: "x64\mysql.dll"; DestDir: "{app}\Bin"'
         '#else'
         'Source: "tree\*.dll"; DestDir: "{app}\Tree"; Flags: recursesubdirs; Excludes: "skip.dll"'
         '#endif'
         'Source: "{#SSL}\libssl.dll"; DestDir: "{app}\Bin"'
         'Source: "runtime.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall'
         'Source: "absent.dll"; DestDir: "{app}"; Flags: skipifsourcedoesntexist'
      )
      [IO.File]::WriteAllLines((Join-Path $inst 'main.iss'), $main)
      [IO.File]::WriteAllLines((Join-Path $inst 'files.iss'), $files)
      $env:HM_PE_SELFTEST_LIBS = Join-Path $dir 'libs'
      $state = Read-IssScript (Join-Path $inst 'main.iss')
      Assert-That ($state.Defines['V'] -eq '9.8.7') 'ReadIni with SourcePath reads the version, as hMailServerARM64.iss does'
      Assert-That ($state.Entries.Count -eq 5) "the entries compiled in are five, the #ifndef block left out (found $($state.Entries.Count))"
      $all = @($state.Entries | Where-Object { $_.DestDir -notlike '{tmp}*' } | ForEach-Object { Resolve-IssEntry $_ $state.SourcePath } | ForEach-Object Name)
      Assert-That (($all | Sort-Object) -join ',' -eq 'a.dll,b.dll,libssl.dll,server.exe,tool.dll') "wildcards, recursesubdirs, Excludes and {#...} resolve to the right files ($($all -join ','))"
      $targets = @($state.Entries | Where-Object { $_.DestDir -notlike '{tmp}*' } | ForEach-Object { Resolve-IssEntry $_ $state.SourcePath } | ForEach-Object { [pscustomobject]@{ File = $_.FullName; Label = $_.Name } })
      $r = Invoke-Check $targets 'ARM64' @() | Where-Object { $_ -isnot [string] }
      Assert-That ($r.Bad -eq 0 -and $r.Pe -eq 5) 'every file of the ARM64 set is right for ARM64'
      $r = Invoke-Check $targets 'x64' @() | Where-Object { $_ -isnot [string] }
      Assert-That ($r.Bad -eq 4) 'the same set is wrong four times for x64 (the AnyCPU assembly is right for both)'
      $r = Invoke-Check $targets 'x64' @('*.dll', 'server.exe') | Where-Object { $_ -isnot [string] }
      Assert-That ($r.Bad -eq 0) '-Allow accepts what it names'

      [IO.File]::WriteAllLines((Join-Path $inst 'bad.iss'), @('#define X', '#ifdef X', '#error stop here', '#endif'))
      $threw = $false
      try { Read-IssScript (Join-Path $inst 'bad.iss') | Out-Null } catch [IssError] { $threw = $_.Exception.Message -like '*#error stop here*' }
      Assert-That $threw 'an #error in a compiled part stops the reader'
      [IO.File]::WriteAllLines((Join-Path $inst 'unknown.iss'), @('#define NEVER ReadIni(Unknown)', '[Files]', 'Source: "{#NEVER}"; DestDir: "{app}"'))
      $threw = $false
      try { Read-IssScript (Join-Path $inst 'unknown.iss') | Out-Null } catch [IssError] { $threw = $true }
      Assert-That $threw 'a #define this reader cannot evaluate stops it when a Source: uses it'
      [IO.File]::WriteAllLines((Join-Path $inst 'open.iss'), @('#ifdef X', '[Files]'))
      $threw = $false
      try { Read-IssScript (Join-Path $inst 'open.iss') | Out-Null } catch [IssError] { $threw = $true }
      Assert-That $threw 'an #if without its #endif stops it'

      # hMailServerInnoExtension.iss's reading of REQUIRED_DB_VERSION, in its shape:
      # a #sub whose body holds an #if this reader does not evaluate, a #for that
      # runs it, and an #if it does not evaluate around an #error.
      $versionRead = @(
         '#define FILE AddBackslash(SourcePath) + "Constants.h"'
         '#define VERSION ""'
         '#define Handle 0'
         '#define Line ""'
         '#sub ReadLine'
         '  #expr Line = Trim(FileRead(Handle))'
         '  #if Pos("#define REQUIRED_DB_VERSION ", Line) == 1'
         '    #expr VERSION = Trim(Copy(Line, Len("#define REQUIRED_DB_VERSION ") + 1))'
         '  #endif'
         '#endsub'
         '#for {Handle = FileOpen(FILE); Handle && !FileEof(Handle); 0} ReadLine'
         '#if Handle'
         '  #expr FileClose(Handle)'
         '#endif'
         '#if VERSION == "" || Int(VERSION, 0) < 6000'
         '  #error REQUIRED_DB_VERSION could not be read'
         '#else'
         '  #pragma message "read"'
         '#endif'
         '[Files]'
         'Source: "ARM64\bin\server.exe"; DestDir: "{app}\Bin"'
      )
      [IO.File]::WriteAllLines((Join-Path $inst 'version.iss'), $versionRead)
      $read = $null
      $why = ''
      try { $read = Read-IssScript (Join-Path $inst 'version.iss') } catch [IssError] { $why = $_.Exception.Message }
      Assert-That ($read -and $read.Entries.Count -eq 1) "a #sub's body, and an #error under an #if this reader does not evaluate, are passed over, the [Files] entry after them read ($why)"
      [IO.File]::WriteAllLines((Join-Path $inst 'subopen.iss'), @('#sub Never', '#if 1', '#endif'))
      $threw = $false
      try { Read-IssScript (Join-Path $inst 'subopen.iss') | Out-Null } catch [IssError] { $threw = $_.Exception.Message -like '*#sub without its #endsub*' }
      Assert-That $threw 'a #sub without its #endsub stops it'
      foreach ($case in @(
            @{ Lines = @('[Files]', '#if Int("1", 0) < 2', 'Source: "x64\mysql.dll"; DestDir: "{app}"', '#endif'); What = 'a [Files] entry' }
            @{ Lines = @('#if Int("1", 0) < 2', '#define SSL "elsewhere"', '#endif'); What = 'a #define' }
            @{ Lines = @('#if A || B', '#else', '#include "files.iss"', '#endif'); What = 'an #include in its #else' }
            @{ Lines = @('#if Int("1", 0) < 2', '[Files]', '#endif'); What = 'a section' }
            @{ Lines = @('#if Int("1", 0) < 2', '#ifdef X', '#define Y "1"', '#endif', '#endif'); What = 'a #define in an #ifdef inside it' })) {
         [IO.File]::WriteAllLines((Join-Path $inst 'undecided.iss'), $case.Lines)
         $threw = $false
         try { Read-IssScript (Join-Path $inst 'undecided.iss') | Out-Null } catch [IssError] { $threw = $_.Exception.Message -like '*does not evaluate*' }
         Assert-That $threw "$($case.What) under an #if this reader does not evaluate stops it"
      }

      # What a #sub, an #expr, a #for or an #emit could do to [Files] that this
      # reader does not follow: each stops it. ISPP's own example of a #sub emits
      # [Files] entries where a #for calls it.
      foreach ($case in @(
            @{ Lines = @('#define FileName "x64\mysql.dll"', '[Files]', '#sub AddFile', '  Source: "{#FileName}"; DestDir: "{app}"', '#endsub', '#for {i = 0; i < 2; i++} AddFile'); Like = '*#sub at*'; What = "a #sub's body that emits a [Files] entry" }
            @{ Lines = @('#sub Pick', '  #define SSL "elsewhere"', '#endsub'); Like = '*#sub at*'; What = "a #define in a #sub's body" }
            @{ Lines = @('#define DIR "ARM64"', '#expr DIR = "x64"', '[Files]', 'Source: "{#DIR}\mysql.dll"; DestDir: "{app}"'); Like = '*#expr sets DIR*'; What = 'a Source: of a name an #expr assigned' }
            @{ Lines = @('#define DIR "ARM64"', '#sub Pick', '  #expr DIR = "x64"', '#endsub', '[Files]', 'Source: "{#DIR}\mysql.dll"; DestDir: "{app}"'); Like = "*#sub at*sets DIR*"; What = "a Source: of a name a #sub's body assigns" }
            @{ Lines = @('#define N 0', '#for {N = 0; N < 2; N++} Nothing', '[Files]', '#if N == "0"', 'Source: "x64\mysql.dll"; DestDir: "{app}"', '#endif'); Like = '*does not evaluate*'; What = 'a [Files] entry under an #if of a name a #for assigned' }
            @{ Lines = @('#expr Late = "1"', '#ifdef Late', '[Files]', '#endif'); Like = '*does not evaluate*'; What = 'a section under an #ifdef of a name only an #expr defines' }
            @{ Lines = @('#if A || B', '#expr DIR = "x64"', '#endif', '[Files]', 'Source: "{#DIR}\mysql.dll"; DestDir: "{app}"'); Like = '*sets DIR*'; What = 'a Source: of a name an #expr under an undecided #if assigned' }
            @{ Lines = @('[Files]', '#emit ''Source: "x64\mysql.dll"; DestDir: "{app}"'''); Like = '*#emit*'; What = 'an #emit' }
            @{ Lines = @('[Files]', '#= ''Source: "x64\mysql.dll"; DestDir: "{app}"'''); Like = '*#emit*'; What = 'an #= (#emit)' }
            @{ Lines = @('[Files]', '#ifexist "x64\mysql.dll"', 'Source: "x64\mysql.dll"; DestDir: "{app}"', '#endif'); Like = '*#ifexist*'; What = 'a [Files] entry under an #ifexist' }
            @{ Lines = @('#endsub'); Like = '*#endsub without #sub*'; What = 'an #endsub without its #sub' })) {
         [IO.File]::WriteAllLines((Join-Path $inst 'unfollowed.iss'), $case.Lines)
         $threw = $false
         $said = ''
         try { Read-IssScript (Join-Path $inst 'unfollowed.iss') | Out-Null } catch [IssError] { $said = $_.Exception.Message; $threw = $said -like $case.Like }
         Assert-That $threw "$($case.What) stops it ($said)"
      }
      # And what changes nothing it reads goes on: an #ifexist in a part that is not
      # compiled keeps its #endif paired, and an #expr of a name no Source: uses.
      [IO.File]::WriteAllLines((Join-Path $inst 'followed.iss'), @('#ifdef NOPE', '#ifexist "x"', '#endif', '#endif', '#expr Count = 2', '#pragma message "fine"', '[Files]', 'Source: "ARM64\bin\server.exe"; DestDir: "{app}\Bin"'))
      $read = $null
      $why = ''
      try { $read = Read-IssScript (Join-Path $inst 'followed.iss') } catch [IssError] { $why = $_.Exception.Message }
      Assert-That ($read -and $read.Entries.Count -eq 1) "an #ifexist where nothing is compiled, an #expr and a #pragma leave the [Files] entry after them read ($why)"
   } finally {
      Remove-Item Env:\HM_PE_SELFTEST_LIBS -ErrorAction SilentlyContinue
      Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
   }
   if ($script:selfTestFailures -gt 0) { "SELF-TEST: $($script:selfTestFailures) failed"; return 1 }
   "SELF-TEST: every case passed"
   return 0
}

if ($SelfTest) { $code = Invoke-SelfTest } else { $code = Invoke-Main }
$code | Where-Object { $_ -is [string] }
$exit = @($code | Where-Object { $_ -is [int] })[-1]
"PE EXIT=$exit"
exit $exit
