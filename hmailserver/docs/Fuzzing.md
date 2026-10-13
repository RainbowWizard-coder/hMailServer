Fuzzing the parsers
===================

Every message the server accepts is untrusted input, and the code that reads it
runs inside the one process that holds every mailbox on the machine. A parser bug
here is not a bad request that gets a 500 — it is a mail outage, plus whatever
the write path had half-finished when the process went down. That asymmetry is
the whole reason this harness exists.

This document covers the MIME parser, which is done. HTML, iCalendar and vCard
are the same shape of problem and are **not** done; see [What is not covered
yet](#what-is-not-covered-yet) so nobody mistakes a clean MIME run for a clean
parser story. It also covers a differential fuzz of another kind: the server's
reading of a link's host against a browser's ([UTS 46 against
ada](#uts-46-against-ada)).

The harness lives in [`fuzz\`](../../fuzz/) at the repository root.

Why this waited on the crash oracle
-----------------------------------

Read this section before anything else, because it is the reason the fuzzing item
sat at "not started" for so long while nine other prerequisites went in ahead of
it.

**A fuzzer is a machine for producing inputs. It is not a machine for noticing
that something went wrong.** Something else has to be the oracle — the thing that
turns "the program did something bad" into a signal. With no oracle, a fuzzer
runs millions of malformed messages through the parser, the parser corrupts
memory on one of them, the process carries on with corrupted state, the run
reports "no findings", and everyone concludes the parser is fine. That is worse
than not fuzzing, because it produces a false clean bill of health.

This codebase had a specific version of that problem, and it is worth being
precise about it:

* `MimeBody::LoadFromFile` wraps the entire parse in `catch (...)`, copies the
  offending message into `Problematic messages`, reports through `ErrorManager`
  and returns success.
* The server is compiled with `/EHa` (`ExceptionHandling=Async` in
  `hMailServer.vcxproj`). Under `/EHa`, `catch (...)` catches *structured*
  exceptions as well as C++ ones — which means it catches access violations.

Put those two together and an out-of-bounds write in the MIME parser presents, in
production, as a log line saying a message could not be parsed. The process
survives. The message is preserved for inspection. And the memory that got
scribbled on belongs to something else entirely, so the consequence appears
later, somewhere unrelated, as a corrupted mailbox or a crash with a stack that
points at innocent code.

So there were two things to build, in order:

1. **An oracle for the running server.** `Common\Util\CrashOracle.{h,cpp}`
   installs a vectored exception handler, which sees a first-chance exception
   *before* any `catch (...)` gets the chance to swallow it, and records it. The
   regression suite checks the oracle after every test, so a swallowed access
   violation now fails the test that provoked it instead of passing quietly. That
   is what makes the regression suite an oracle rather than just a lot of asserts
   (over 2,000 tests as of September 2026 — a figure that only ever goes up, so treat it
   as a floor rather than a fact to maintain).
2. **An oracle for the fuzz build**, which is AddressSanitizer. ASan is stronger
   than a crash handler: it traps the *first* out-of-bounds byte rather than
   waiting for an access that happens to land on an unmapped page, and it reports
   reads as well as writes. Almost every finding this harness will produce is a
   read one byte past the end of a buffer — something that would never fault in
   production and would never be noticed without it.

The relationship between them is the point. ASan finds the bug in a target that
runs the parser in isolation, thousands of times a second. The crash oracle keeps
the same class of bug visible in the real server, where the parser runs inside a
`catch (...)` that would otherwise hide it. Neither replaces the other, and doing
the fuzzing first would have produced findings nobody could confirm against a
running server.

One consequence worth knowing: the fuzz build is compiled `/EHsc`, not `/EHa`, so
`catch (...)` in the parser does **not** swallow access violations there. ASan
traps before any handler runs anyway. The fuzz build is therefore strictly better
at surfacing memory errors than the shipped build is at surviving them — which is
the right direction for a bug hunt, and another reason a finding here needs a
regression test in the suite rather than only a corpus entry.

Building it
-----------

The one-time prerequisite is a clang toolchain alongside MSVC, with libFuzzer's and
AddressSanitizer's runtime libraries. Two routes; the second is the one in use on
this bench, and it is here first because it is the one that has actually been made
to work end to end.

**Portable LLVM, user-local, no elevation.** This is what the build was verified
against. LLVM publishes its Windows toolchain,
`clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz`, only as a GitHub release asset
(llvm.org's download page links there), and this project fetches nothing from
GitHub, so this route is for a copy of that archive already at hand, such as the
bench's; on a new machine, take the Visual Studio component below:

```powershell
# ~820 MB archive, ~2.5 GB extracted. Windows' own tar handles .xz and .zst.
# $env:TEMP\llvm.tar.xz is the archive named above, already on the machine.
$target = "$env:LOCALAPPDATA\Programs\LLVM-22.1.8"
New-Item -ItemType Directory -Force $target | Out-Null
tar -xf "$env:TEMP\llvm.tar.xz" -C $target --strip-components=1

$env:PATH = "$target\bin;$env:PATH"      # before build-fuzz.ps1
```

Nothing goes into Program Files, nothing is written to the registry, and it is
removed by deleting one folder. That matters more than it sounds: the alternative
below needs administrator rights, and `winget install LLVM.LLVM` is not a substitute
because it has **no user-scope installer at all** (`--scope user` fails with "No
applicable installer found") and its machine scope needs elevation.

**Or the Visual Studio component,** if you have administrator rights on the machine:

> Visual Studio Installer → Modify → **Individual components** → **C++ Clang
> tools for Windows** (component id
> `Microsoft.VisualStudio.Component.VC.Llvm.Clang`)

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools" `
    --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --passive
```

That component ships the sanitizer runtimes too, and `Find-ClangCl.ps1` prefers it
via `vswhere` when it is present. It is second in this list only because it was not
available here.

### Three things that are not optional, each of which costs a build to rediscover

All three are already handled by `build-fuzz.ps1`; they are written down because the
symptoms point somewhere unhelpful.

1. **`/D_DISABLE_STRING_ANNOTATION=1` and `/D_DISABLE_VECTOR_ANNOTATION=1`.** Without
   them the link fails with `lld-link: error: /failifmismatch: mismatch detected for
   'annotate_string'`. The prebuilt `clang_rt.fuzzer` library is compiled with MSVC
   STL's ASan container annotations **off**, and the STL stamps a `/failifmismatch`
   directive into every object recording which way it was built, so any translation
   unit that enables them refuses to link against libFuzzer. The cost is the
   container-overflow checks on `std::string` and `std::vector`; the only way to keep
   them is to rebuild compiler-rt from source to match.
2. **The static CRT (`/MT`) is the default, because `/MD` cannot link at all** —
   `/failifmismatch: mismatch detected for 'RuntimeLibrary'`, since that same library
   is built against the static CRT. Consequence worth knowing when reading a finding:
   the allocator under test is the static CRT's, not the DLL's that the shipped server
   uses.
3. **`clang_rt.asan_dynamic-x86_64.dll` must sit next to the executables even under
   `/MT`.** A `/MT` target still imports it and still dies with `0xc0000135`
   (`STATUS_DLL_NOT_FOUND`) before running a single input. `build-fuzz.ps1` copies it
   for both CRT choices; it used to copy only for `/MD`, on the reasonable-sounding
   but wrong assumption that a static CRT implies a static ASan.

### And why the harness has its own `stdafx.h`

`fuzz\harness\shim\stdafx.h` is first on the include path so that a server source
file's `#include "stdafx.h"` finds it instead of the real one. The real header
`#import`s the ADO type library, which clang rejects outright — *"#import of type
library is an unsupported Microsoft feature"* — and pulls in ATL; a parser needs
neither. The shim also has to include `Util/StdString.h`, because server sources use
`String` and `AnsiString` without including it themselves, relying on the
precompiled header.

**Watch out for a clang that is not this one.** `clang-cl` turns up on PATH from
all sorts of places — a Swift toolchain, an Android NDK, a Chocolatey LLVM — and
those builds compile this code perfectly well but do not ship
`clang_rt.fuzzer-x86_64.lib`. The symptom is `LNK1181` twenty minutes into a
build. `build-fuzz.ps1` prefers the Visual Studio toolchain and checks for the
runtime libraries before it compiles anything, so it fails immediately with the
install instructions instead.

```powershell
.\fuzz\build-fuzz.ps1
```

The build:

* finds clang-cl through `vswhere` and imports the MSVC environment itself, so it
  works from an ordinary PowerShell prompt (clang-cl links with `link.exe` and
  against the MSVC CRT, so that environment is not optional);
* compiles the five real `Common\Mime` sources (`Mime`, `MimeChar`, `MimeCode`,
  `MimeType`, `CodePages`) plus the five utility translation units they need
  (`Charset`, `ByteBuffer`, `Unicode`, `RegularExpression`, `StringParser`), with
  `-fsanitize=fuzzer-no-link,address`;
* links one executable per target with `-fsanitize=fuzzer,address` into
  `fuzz\bin\`;
* copies `clang_rt.asan_dynamic-x86_64.dll` next to the executables **for both CRT
  choices**, because a `/MT` target imports it too — see point 3 above, which is the
  bug this bullet used to repeat;
* regenerates the seed corpus from the real test messages.

It never builds `hMailServer.sln`, never runs its pre/post-build events and never
touches the installed service, so it cannot disturb a regression run in progress.

**Boost**: two of the compiled translation units use header-only Boost
(`boost::regex` in `RegularExpression.cpp`, `boost::tokenizer` and
`boost::lexical_cast` in `StringParser.cpp`). The script picks them up from
`%hMailServerLibs%\boost_1_92_0` like the main build does, or from
`-BoostInclude <dir>`. No Boost library is linked.

Running it
----------

```powershell
# a five-minute smoke test - do this first, and after any parser change
.\fuzz\run-fuzz.ps1 -Target mime_message_fuzzer -Minutes 5

# seconds: replay the seed corpus and everything previous runs kept, then exit
.\fuzz\run-fuzz.ps1 -Target mime_header_fuzzer -Replay

# overnight, four cores
.\fuzz\run-fuzz.ps1 -Target mime_message_fuzzer -Minutes 480 -Jobs 4
```

The three targets, and what each is for:

| Target | Input | Why it is separate |
|---|---|---|
| `mime_message_fuzzer` | a whole message | `MimeBody::Load` is what every spooled message goes through, and the only entry point that recurses (nested `multipart`, nested `message/rfc822`). Also round-trips through `Store()` and re-parses, because the serialiser runs on attacker-controlled structure every time a rule rewrites a header. |
| `mime_header_fuzzer` | a header block | Headers are parsed far more often than bodies — IMAP `FETCH ENVELOPE`, `BODY[HEADER]`, the rule engine and the DKIM signer all load headers alone. Smaller inputs mean a much higher execution rate and a deeper walk into folding and parameter parsing. Runs all three shapes production uses, including `Utilities::GetMimeHeader`'s truncated length. |
| `mime_decode_fuzzer` | selector byte + payload | Spends every execution inside the codecs (base64, quoted-printable, RFC 2047 encoded words, RFC 2231 parameter continuations) instead of first having to produce a plausible header. |

### How long

Fuzzing is not a pass/fail gate you run once; it is a background process with a
long tail. Useful budgets:

* **5 minutes per target** after any change to `Common\Mime` or to something it
  calls. This re-runs the whole corpus and mutates around it, and it is fast
  enough to be habitual. It belongs in the same reflex as running the suite.
* **A few hours** the first time a target is ever run, and after a real change to
  the parser's structure. Watch the `cov:` counter in libFuzzer's output: while it
  is still climbing, the run is still learning. When it has been flat for an hour,
  more of the same is unlikely to help.
* **Overnight with `-Jobs 4`** before a release that changed message handling.
* Between runs, keep the corpus. `fuzz\corpus\<target>\` accumulates every input
  libFuzzer found interesting, and it is the single most valuable thing the
  exercise produces — a later run starts from that coverage instead of from
  scratch. It is machine-local and not committed.

Once a corpus gets large, minimise it before a long run. `-merge=1` keeps only
the inputs needed to hold the same coverage, which makes every subsequent
execution cheaper:

```powershell
mkdir fuzz\corpus\merged
.\fuzz\bin\mime_message_fuzzer.exe -merge=1 fuzz\corpus\merged fuzz\corpus\mime_message_fuzzer
```

### Two practical notes

**Antivirus.** Exclude `fuzz\` from real-time scanning. A run writes corpus
entries and crash artifacts continuously — thousands of small files that each
look like a malformed email — and this project has already lost whole regression
runs to security software interfering with the test environment
(`build\preflight-tests.ps1` warns about the interference sources it can detect).
A scanner that quarantines an artifact mid-run destroys the one thing a finding is
worth: the exact bytes.

**Do not run it on the mail server.** The targets are ordinary user-mode
processes and do not touch the service, but a fuzz run will happily use every
core it is given.

What to do with a finding
-------------------------

libFuzzer stops on the first finding, prints the report, and writes the input
that produced it to `fuzz\artifacts\<target>\crash-<sha1>` (or `timeout-`,
`oom-`, `leak-`). `run-fuzz.ps1` prints the path and the reproduce command. The
exit code is `77` for a finding; anything else non-zero means the target could
not start or a flag was rejected.

**1. Reproduce it, deterministically.**

```powershell
.\fuzz\run-fuzz.ps1 -Target mime_header_fuzzer -Reproduce .\fuzz\artifacts\mime_header_fuzzer\crash-<sha1>
```

One input, one execution, no mutation, no corpus. If it does not reproduce, the
finding depended on state that leaked between executions — which is itself a bug,
in the harness or in the parser's static state, and worth chasing rather than
shrugging at.

**2. Work out what kind of finding it is**, because three of the four kinds are
routinely misread:

* **`heap-buffer-overflow READ` of one or two bytes** — the most common finding in
  this parser and the easiest to dismiss, because in production the byte being
  read usually exists (it is inside the same allocation's slack, or it is the NUL
  terminator's neighbour). Do not dismiss it. It is a real out-of-bounds read;
  whether it faults depends on the allocator's mood, and the value read steers
  the parse.
* **`stack-overflow`** — read the stack trace before filing. If the repeated
  frames are `MimeBody::Load`, that is a genuine finding: the parser recurses once
  per nesting level with no depth limit, so a small message can build a very deep
  tree, and a deep enough one exhausts the stack. If the repeated frames are
  `hm_fuzz::ExerciseBody`, the *harness* ran out of stack on a tree the parser
  survived; raise nothing and lower `kMaxTraversalDepth` instead.
* **`timeout` / `oom`** — a parser that stops making progress or allocates without
  bound is a remote denial of service, which for a mail server is a real
  vulnerability rather than a performance note. `MimeEncodedWord::BEncode` **had** a
  loop of exactly this shape, and it is the example to keep in mind because the
  cause was arithmetic rather than buffer handling: its block size was
  `(MAX_ENCODEDWORD_LEN - charset_length - 7) / 4 * 3`, and since that constant is
  75, a charset name of 69 characters or more drove it to zero or below — a
  zero-size block means the loop makes no progress while appending an encoded-word
  header to its output every iteration, so it never terminated at all. The only
  guard was an `ASSERT`, compiled out of Release. Both `BEncode` and `QEncode` now
  fall back to the raw encoder when no legal encoded word can be built
  (`if (nMaxBlockSize < 3)`), and the reproducer lives in
  `fuzz\regression\mime_decode_fuzzer\out-of-memory-mimeunicodeencoder-encodevalue`,
  so every run re-proves it. Read the shape, not the location: the same "budget goes
  non-positive, loop still appends" pattern is what to look for in the next one.
* **An assertion abort** — only possible if the target was built with
  `-Asserts`. The shipped Release build compiles `ASSERT` to `((void)0)`, so an
  assertion failure is a violated internal invariant, not a crash. Interesting,
  but a different investigation, and not something to fix by "adding a check" at
  the assert.

**3. Minimise the input.** libFuzzer will shrink it for you, which usually turns
4 KB of mutated mail into a dozen bytes and makes the actual defect obvious:

```powershell
.\fuzz\bin\mime_header_fuzzer.exe -minimize_crash=1 -runs=100000 `
    -exact_artifact_path=fuzz\artifacts\minimised .\fuzz\artifacts\mime_header_fuzzer\crash-<sha1>
```

**4. Fix it in `Common\Mime`, and write a suite test if the bug is reachable over
a protocol.** Most of them are: a header value reaches the RFC 2047 decoder
through IMAP `FETCH ENVELOPE`, a `Content-Disposition` filename reaches the
parameter decoder through the attachment API, a nested multipart reaches the
recursion through plain SMTP delivery. A corpus entry only proves the process no
longer dies, only runs when somebody runs the fuzzer, and says nothing about the
parser now doing the *right* thing. The regression suite runs on every release;
the fuzzer does not.

**5. Keep the input forever.** Move the artifact into
`fuzz\regression\<target>\` with a name that says what it was, and commit it.
Every run — including the seconds-long `-Replay` — re-executes it from then on.
The rules are in [`fuzz\regression\README.md`](../../fuzz/regression/README.md);
the important one is that these files are byte-exact and marked binary in
`fuzz\.gitattributes`, because a reproducer whose line endings got normalised
still looks like a reproducer and no longer reproduces anything.

How the harness stays honest
----------------------------

Three decisions in the harness exist to stop it reporting things that are not
bugs. They are worth knowing, because each one looks like a mistake until you
know why:

* **The input is copied and NUL-terminated** before being handed to the parser.
  That is the production contract — `File::ReadTextFile` appends a NUL and
  `LoadFromFile` passes a length that excludes it — and `MimeField::Load` relies
  on it, reading one past the counted length in its folding loop. Handing
  libFuzzer's raw buffer straight in would report a heap overflow on nearly every
  input, all of them false.
* **`ASSERT` is a no-op**, matching Release. `MimeBody::Load` asserts that a
  multipart part has a non-empty boundary, which is one malformed message away;
  with asserts live, that would be the only finding you ever saw. The server's
  own `HM_ASSERT` (the lowercase `assert` sites were renamed to it on 5 September
  2026) maps onto `ASSERT` in the shim, so `-Asserts` governs both spellings.
  The regression-suite counterpart is `build\build.ps1 -Configuration Release
  -Asserts`, where a violated assertion is reported as HM6364 rather than
  aborting, and the test that provoked it is the one that fails.
* **The harness catches what production catches** and nothing more. `catch (...)`
  in `LoadFromFile` means an escaping `std::logic_error` or `std::bad_alloc` is
  not a production crash, and letting it escape the target would call `abort()`
  and be reported as one. Memory errors are traps, not exceptions, so ASan still
  sees them first.

If a change to the harness starts producing large numbers of findings at once,
suspect one of these three before believing the parser suddenly got worse.

The nightly job
---------------

From 15 September 2026 `.github/workflows/fuzz-nightly.yml` ran the three
harnesses every night at 02:10 UTC on a GitHub-hosted Windows runner (and on
demand, with the minutes per harness and the workers as inputs): the native
dependencies restored from their caches, the harnesses built with the runner's
clang-cl, every kept input and every committed reproducer replayed first, then
each harness for forty minutes with two workers. The corpus was kept in the
Actions cache under a key that carried the run id, so every night saved a new
entry and the next night restored the newest - the corpus grew, which is the
point of a job that never stops. A crash, a hang or a violated invariant failed
the job and the reproducer landed as an artifact of the run (`fuzz-findings-<run
id>`, kept thirty days); the section above says what to do with it.

It has not run since GitHub suspended the project's account on 18 September
2026, and it is in the git history at `faca023d4`.

From 25 September 2026 the nightly job is GitLab CI's `linux-fuzz`, on the
project's Linux runner: the same three harnesses, the same dictionary, the same
reproducers and the same libFuzzer flags, built with clang++ by
`build/ci/linux-fuzz.sh` against the POSIX platform header instead of
`<Windows.h>` (the shim's only difference between the two), every kept input
and reproducer replayed first, then twenty minutes a harness with two workers,
on a corpus the runner's cache carries from night to night. A finding fails the
job and its input is the job's artifact for thirty days; reproduce it with one
execution of `fuzz/bin/<target> <file>` on Linux, or `run-fuzz.ps1 -Reproduce` on
Windows. A GitLab pipeline schedule starts it; [ContinuousIntegration.md](ContinuousIntegration.md),
"The fuzz schedule", says how the owner creates it. `run-fuzz.ps1` by hand and
the release checklist's timed run on the release tree stay as they were.

Its first replay found what the Windows build had never been able to: the
committed reproducer `regression/mime_decode_fuzzer/out-of-memory-mimeunicodeencoder-encodevalue`
made `Charset::ToWideChar` read an unterminated heap buffer on Linux, where
iconv refuses a byte sequence the named character set does not have and the
Windows conversion substitutes instead. The buffer is zeroed now, as
`ToMultiByte`'s always was, and a failed conversion gives an empty string.

UTS 46 against ada
------------------

A second kind of harness, which looks for a different answer rather than a
crash. The SURBL test and the sender lists' URL entries read a link's host as a
browser does - the WHATWG URL standard's host parser, whose domain parser is
Unicode's UTS 46 (`Common/Idna`, from 6.3.7) - and a host read otherwise than a
browser reads it is a listed name nobody asks the lists about. So
`fuzz/harness/idna_ada_fuzzer.cpp` puts every input to the server's code and to
[ada](https://www.ada-url.com/), the URL parser of Node.js, and stops at the
first input on which they differ, printing both answers:

1. **The domain parser**: `Idna::DomainToAscii` and ada's `to_ascii` with the
   URL standard's two checks after it both fail, or give one name.
2. **The host of a link's authority**: where the input holds nothing that ends
   an authority, `SURBL::HostOfAuthority` of it is what `HostOfAuthority` makes
   of the host ada's URL parser gives for `http://<input>/`, and empty where ada
   fails the URL.
3. **The link reader in running text**: when that host is one a list can be
   asked about, `SURBL::FindLinkHosts` hands it over for
   `see http://<input>/ now` - unless the input holds white space or an angle
   bracket, where a mail program ends the link before any browser is given it.
4. **The link reader in HTML**: the same for `<a href="http://<input>/">`,
   unless the input holds the `"` that ends the value or the `&` that begins a
   character reference; and again for that link with the input written in
   character references, which a browser reads before its URL parser - `&` and
   `"` as `&amp;` and `&quot;`, and each code point past ASCII that HTML has a
   named reference for as one of its names, picked by its place in the input,
   without its `;` where HTML reads it so and nothing after it would carry the
   name on - so that no input is left out of it, and HTML's whole list of names
   is put to the reader. The names are the harness's own, not the server's
   table: the script writes them from Python's copy of the HTML standard's list
   (`html.entities.html5`), held to the `entities.json` the server's table is
   generated from, so that a name the table lost is a difference.
5. **What 6.3.6 handed over**: every host 6.3.6's reading of the input as a
   link's host part hands over - the name before a character a mail program
   ends a link at, the labels after the last label holding one, an address -
   `SURBL::FindLinkHosts` hands over too, in the text of 3 and both links of 4,
   whatever ada makes of the input. The reading is a copy of 6.3.6's, as the
   `v6.3.6` tag has it (`fuzz/harness/surbl_636_reading.h`), not the server's
   own, which must not be read to judge itself.
6. **Where a mail program ends the link**: the host of 3, when the input is
   followed straight away by a character a mail program ends a link in
   running text at and UTS 46 would take into the host's last label - a
   closing quotation mark, in `see http://<input>” now` and as `&rdquo;` in
   HTML's text, and an ideographic full stop before CJK text - whatever the
   input is written in, ASCII or not, and whatever symbols it holds. The
   reader reads a host part as far as the first character of each kind a
   mail program may end a link at (`LinkEnds_` in `SURBL.cpp`: punctuation
   and symbols past ASCII, each by what UTS 46 maps it to), as far as where
   the punctuation it ends with begins, and as far as the first full stop
   past ASCII before text past ASCII; so the quotation mark is asked of an
   input that holds no punctuation of its kind or does not end in such a
   run, and the full stop of one that holds no full stop past ASCII or none
   before text past ASCII. The kinds are told from the server's own data
   (`Idna::KindOfMapping`, `Idna::CategoryGroupOf`, `Idna::IsDisallowed`),
   which `generate-idna-tables.py --check` and `IdnaTester` hold, so this
   holds the reader to the rule and to reading the part of the host before
   the character as a browser reads it.

It runs on Linux only, with clang's libFuzzer and AddressSanitizer, in the CI
image, as a CMake project of its own (`fuzz/idna/`) that builds the server's
core library and links ada beside it:

```bash
bash build/ci/idna-differential-fuzz.sh          # what check does, then fuzz 30 minutes
bash build/ci/idna-differential-fuzz.sh check    # build, check ada, replay the kept findings
HM_FUZZ_MINUTES=5 HM_FUZZ_JOBS=8 bash build/ci/idna-differential-fuzz.sh
```

**Nothing comes from GitHub.** ada is the amalgamation (`ada.h`, `ada.cpp`) in
its Python package's source distribution on PyPI, `ada_url-4.0.0.tar.gz` from
files.pythonhosted.org, held to the SHA-256 PyPI publishes for it; the Unicode
17.0.0 data its IDNA tables are of, and Unicode 18.0.0's
`NormalizationTest.txt`, come from unicode.org, each held to its SHA-256. A file
that is not the pinned one ends the run before anything is built. ada is only
ever built into the harness, never into the server or a package. The build
directory (`HM_BUILD_DIR`, `/tmp/hm-idna-fuzz`) is kept from
one run to the next: a pinned file already there is not fetched again and only
what changed is rebuilt. It is not a CI job: it is run by hand, on server2,
which built it from nothing in five and a half minutes on 12 October 2026.

### What it sets aside, and why each is ada's

ada is not UTS 46 everywhere, and a differential fuzz that stops at ada's own
departures stops at once, so before it fuzzes the script runs
`idna_ada_conformance.cpp`, which holds the oracle to Unicode's own files:

* the server's NFC to the whole of `NormalizationTest.txt` 18.0.0 (20,171
  lines, and every code point it does not list normalising to itself);
* ada's domain parser to the server's over the source column of every line of
  `IdnaTestV2.txt` 18.0.0 - the file the server's `IdnaTester` passes whole -
  where every difference must be one of the classes below, and on a line the
  file itself fails for that rule; any other difference fails the run.

The classes, each a rule in `fuzz/harness/idna_ada_common.h`:

| Set aside | Why it is ada's | Shown by |
|---|---|---|
| A code point, of the input or of a label as UTS 46 decodes it, whose UTS 46 data differ between Unicode 17.0.0 and 18.0.0 (13,010 code points) | ada's mapping table is of 17.0.0, the server's of 18.0.0 | `generate-idna-tables.py --differences` over the two versions' files |
| A code point whose direction, or whether it is a mark, ada reads otherwise (7,605 code points, 1,441 of them ones a label may hold; most of those - at least 815 - added in Unicode 16.0 or later, 712 in U+1CC00..U+1CEFF alone) | ada's direction and mark data are not of its mapping table's Unicode version: it reads GARAY DIGIT EIGHT (U+10D48, Unicode 16.0, Bidi_Class AN) as left-to-right | six labels put to ada's own `is_label_valid` for every code point, against what they give with the server's data |
| A domain holding U+200C or U+200D, as written or in a label decoded from Punycode | ada's CONTEXTJ looks for a joining character anywhere before and after a ZERO WIDTH NON-JOINER, from lists of its own, where RFC 5892 allows only Joining_Type T between, and once it has taken a joiner it checks nothing more of the label - its bidi rule neither, nor a second joiner: `xn--10cl1a0b660pa0b660p` is Sinhala with two ZERO WIDTH JOINERs, the second after the first and not after a virama | 3 lines of `IdnaTestV2.txt` that the file fails and ada takes, and the label written `xn--`, kept in `fuzz/regression/idna_ada_fuzzer/` |
| A domain the server refuses only for RFC 5893's rule on labels with no R, AL or AN | UTS 46 applies the rule to every label of a bidi domain name; ada only to a label that holds such a character itself | 63 lines the file fails by a B code and ada takes |
| An answer of ada's with a label that is not in NFC | ada skips normalising a name its quick check passes, and the check passes a precomposed character followed by a mark its own decomposition must be reordered with: `U+212B U+0323` comes out `U+00E5 U+0323`, whose NFC is `U+1EA1 U+030A` | the server's NFC held to `NormalizationTest.txt` first |
| A domain the server refuses only for an `xn--` label whose Punycode begins with its only delimiter (`xn---ssm4ef`) | RFC 3492 (section 6.2 and its sample code) consumes the delimiter only after basic code points, so the `-` is read as a digit, which it is not; ICU and the Rust `idna` crate refuse the label too; ada, like Python's codec, skips it | RFC 3492's text and sample code |

Two more limits of the comparison, which are the server's on purpose: a link in
running text ends at white space and the angle brackets, as a mail program ends
it (check 3 leaves those inputs out), and a refused label still hands over the
name before it and the labels after it, where a browser goes nowhere - the link
reader looks for every name a list might know.

### What it found

Two readings of the link reader's that no check could see were found by an
adversarial check of the code on 13 October 2026, before 6.3.7 shipped it, and
are now checks of their own. A link in running text followed by punctuation
past ASCII - a closing quotation mark, a guillemet, CJK brackets, commas or full
stops - named only the host's last label with the character in it, Punycode and
all, where 6.3.6 named the listed host before the character, as the mail
programs that make the link end it there: check 3 asks only for the host a
browser gets from the whole input, which the server did hand over, so check 5
now holds the reader to every host 6.3.6 handed over. And a host written in
HTML's named references to characters UTS 46 maps or takes
(`&eopf;vil.example`, `b&uuml;cher.example`) named nothing, the reader knowing
70 names: check 4 left out every input holding an `&`, so it now writes the
input in references as well.

A second adversarial check, of the repaired code, found two more the same
day. The name before such punctuation was handed over only when the name
itself was written in ASCII: the reading as it is written, which found it,
finds no name in fullwidth letters, named references or Unicode, so
`http://ｅｖｉｌ.example”` named `evil.xn--example-b46c` alone - check 5
holds the reader only to 6.3.6's hosts, and 6.3.6 handed over none for it, so
check 6 now asks for the name before the character. Its first run stopped
after 69 seconds, before the reading to the character was committed, on
`%C2%A9` before a long name and the quotation mark: that reading looked for
where a mail program ends a link in the host percent-decoded, where a mail
program sees `%C2%A9` as three ASCII characters and ends nothing there; it
looks in the host as it is written, and the input is kept. And a host of many
short labels of `U+FDFA`, or of empty ones, each mapped whole, cost up to nine
times the memory and twenty-five times the time of 6.3.6's reading of it; no
input of this fuzz can show that, its inputs being 256 bytes at most, so
`SURBLTester` holds the reader to it by how many code points UTS 46 is given.

One defect of the server's, on 12 October 2026, before 6.3.7 shipped the code:
`0x1.0x000000000000` followed by two parts of over a hundred digits, all zeros
but a final 1, each after a fullwidth full stop, was `1.0.0.1` to ada and
nothing to the server, whose limit on a label's length - there for Punycode's
cost - refused the long ASCII labels of a host that is not ASCII. The limit now
applies only to a label that needs Punycode, and the input is kept in
`fuzz/regression/idna_ada_fuzzer/`, which every run replays before it fuzzes.
Every other stop of the early runs was one of the classes above, found by the
fuzz and confirmed against Unicode's files or RFC 3492 before a rule was written
for it, and each of those inputs replays without a difference against the code
as it is.

That the comparison has teeth was shown the same day by a negative control,
never committed: with 6.3.6's reading of a host put back in `SURBL.cpp`
(percent-decoded, the ignored code points deleted, nothing mapped, no Punycode)
a three-minute run stopped at its first difference, having found 388 inputs
the old reading read otherwise than ada - 380 of its 6,401 seed inputs and 8 of
its first mutations. The one it stopped at, `xn--0ca88g.ßxn--4db`, ada sends to
`xn--0ca88g.xn--xn--4db-zva` and the old reading nowhere; each of the 388
replays without a difference against the code as it is.

The run after those, from a fresh build of the code as it then was, on
server2, 30 minutes with eight processes: no difference in 1,819 seconds, about
47.9 million inputs in 157 jobs, none of them a crash or a timeout under
AddressSanitizer, and 9,263 inputs in the corpus at the end.

The runs of 13 October 2026, after the repair of the link reader the check
above found, each from a fresh copy of the code as it is: the first, 30
minutes from a fresh build, stopped once, at its end, at check 1 -
`vww.xn--10cl1a0b660pa0b660p.` and more, whose second label is Sinhala with two
ZERO WIDTH JOINERs, refused by the server as RFC 5892 has it and taken by ada:
ada's joiner departure, which the rule for it did not yet look for in a label
written `xn--`. It does now, and the input is kept in
`fuzz/regression/idna_ada_fuzzer/`. Two negative controls showed that the new
checks have teeth: with the reading of a host as it is written taken out of the
reader, a run stopped at once, at the replay of a kept finding, at check 5 (6.3.6
handed over `1.0.0.0`); and with 6.3.6's 70 named references put back, at check
4 in references (`--Ä.ututut` named nothing) - once the harness wrote its
references from a list of its own: written from the server's own table, as at
first, they found nothing, a table that lost names reading back alike what it
wrote.

The last run of the first repairs, 30 minutes with eight processes from the
corpus the runs before it had left: no difference in 1,807 seconds, about 36.4
million inputs in 159 jobs, none of them a crash or a timeout under
AddressSanitizer, and 10,228 inputs in the corpus at the end.

The run after the second check's repairs, 30 minutes with eight processes, on
the tree with check 6 from the build directory of a run from nothing an hour
before (the files changed since recompiled) and the corpus the runs before it
had left: the server's NFC held to `NormalizationTest.txt` and ada to
`IdnaTestV2.txt` as before, the three kept findings replayed without a
difference, and then no difference in 1,807 seconds, about 39.9 million
inputs in 160 jobs, none of them a crash or a timeout under AddressSanitizer,
and 10,319 inputs in the corpus at the end - server2 shared with other builds the
while. A negative control showed check 6's teeth: with the reading to the
first punctuation or symbol past ASCII alone, the rule the check proposed, a
run stopped after 117 seconds at check 6, `xn--4ca.b」c--de.f` before the
quotation mark handing over `xn--4ca.b` where a mail program that takes the
punctuation off a link's end gives a browser `xn--4ca.xn--bc--de-wt3e.f`.

A difference a run reports now is
either a defect of the server's - its UTS 46 or its link reader - or a further
departure of ada's, and is not to be set aside until it is shown to be ada's
against Unicode's own data or the RFC it departs from.

What is not covered yet
-----------------------

* **HTML**, reached through the HTML-to-text conversion and message rewriting.
* **iCalendar and vCard**, reached through attachment handling - and, since
  CardDAV (6.3.3) and CalDAV (6.3.4), reached directly
  by an authenticated `PUT` of a card or a calendar object, which is a shorter
  path to `Common/Util/VCard.cpp` and `Common/Util/ICalendar.cpp` than an
  attachment is. `ICalendar` is the more exposed of the two: it parses a
  component tree and then *expands* a recurrence rule, which is arithmetic over
  attacker-chosen numbers rather than only a reader. Its expansion is bounded
  deliberately - a ceiling on instances, on periods scanned, on empty periods in
  a row, and the year 9999 - and one defect of exactly this shape has already
  been found and fixed by hand rather than by a fuzzer: the number reader took
  seven characters at most, so `COUNT=100000000` (nine digits) was misread.
  That is the argument for the target, not against it.
* **`MimeBody::LoadFromFile`'s byte bookkeeping.** The in-memory targets do not
  cover the `body_byte_offset_` / `body_byte_end_` arithmetic or the
  trailing-CRLF trim, which decide which bytes `SaveAllToFile` copies verbatim.
  Getting that wrong does not crash — it invalidates a DKIM body hash, which is
  arguably worse because nothing local notices. It needs `File` to serve bytes
  from memory; see the comment in `fuzz\harness\fuzz_environment.cpp`.
* **Sieve, ManageSieve and the SMTP command parser.** All three are untrusted
  input; none are wired to a target. The SMTP one is the most exposed.

Each of those is a new target next to the three that exist, not new
infrastructure: the shim, the environment stubs and the build script are the part
that was hard, and they are done.

Verified against the code
-------------------------

Checked 13 August 2026. Confirmed present and as described: `/EHa`
(`<ExceptionHandling>Async</ExceptionHandling>`, both configurations in
`hMailServer.vcxproj`) against the fuzz build's `/EHsc`;
`Common\Util\CrashOracle.{h,cpp}`; `MimeBody::LoadFromFile`'s `catch (...)` and its
copy into `Problematic messages`; the three targets and the ten translation units
`build-fuzz.ps1` compiles; `/MT` as the default `-RuntimeLibrary` and the ASan DLL
copied for both; `_DISABLE_STRING_ANNOTATION` / `_DISABLE_VECTOR_ANNOTATION`;
Boost from `%hMailServerLibs%\boost_1_92_0` or `-BoostInclude`; `run-fuzz.ps1`'s
`-Target` / `-Minutes` / `-Jobs` / `-Replay` / `-Reproduce` and `-error_exitcode=77`;
`kMaxTraversalDepth` and `hm_fuzz::ExerciseBody` in `fuzz_mime_common`;
`mime_header_fuzzer`'s three shapes including `Utilities::GetMimeHeader`'s truncated
length; `fuzz\.gitattributes` marking `regression/**`, `corpus/**` and
`artifacts/**` as `-text -diff` with `regression/README.md` put back to text.

One thing was stale and is now corrected in place: the `BEncode` example above was
written in the present tense after the defect had been fixed. If you are updating
this page, that is the failure mode to watch for — a document about finding bugs is
the one most likely to keep describing them after they are gone.
