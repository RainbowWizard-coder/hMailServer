Fuzzing harnesses
=================

Coverage-guided fuzz targets for the parsers that read untrusted input. Today
that means MIME; HTML, iCalendar and vCard are the same shape of problem and are
not done yet. Beside them, a differential target: the server's reading of a
link's host (UTS 46) against ada, the URL parser of Node.js, on Linux.

The operator- and developer-facing documentation is
[hmailserver/docs/Fuzzing.md](../hmailserver/docs/Fuzzing.md): how long to run,
what to do with a finding, and why a fuzzer without the crash oracle is close to
useless. **Read that before starting a long run.** This file is just the map of
the directory.

Quick start
-----------

```powershell
# needs "C++ Clang tools for Windows" from the Visual Studio installer
.\fuzz\build-fuzz.ps1
.\fuzz\run-fuzz.ps1 -Target mime_message_fuzzer -Minutes 5
```

The build never touches `hMailServer.sln`, the installed service or the
regression suite's Release output — see the header comment in `build-fuzz.ps1`.
It is safe to run while the suite is running.

What is here
------------

| Path | What it is |
|---|---|
| `harness\mime_message_fuzzer.cpp` | Target: a whole message through `MimeBody::Load`, then the accessors the server calls on it, then a store/re-parse round trip. The one that reaches the parser's recursion. |
| `harness\mime_header_fuzzer.cpp` | Target: `MimeHeader::Load` alone, in all three shapes production calls it (unfolded, folded, and `Utilities::GetMimeHeader`'s odd truncated length). Small inputs, highest execution rate. |
| `harness\mime_decode_fuzzer.cpp` | Target: the codecs directly — base64, quoted-printable, RFC 2047 encoded words, RFC 2231 parameter continuations, and the encode direction. |
| `harness\fuzz_mime_common.{h,cpp}` | The input-buffer contract, the exception policy, and the shared traversal. Read the WHY blocks before changing it. |
| `harness\fuzz_environment.cpp` | Stubs for `File` and `Formatter`. Environment only — no parser logic is stubbed anywhere. |
| `harness\shim\stdafx.h` | Stand-in for the server's precompiled header, which clang-cl cannot compile because it `#import`s the ADO type library. This file is why the item was possible at all. |
| `build-fuzz.ps1` | Builds the targets with clang-cl and `-fsanitize=fuzzer,address`. Also regenerates the seed corpus. |
| `make-corpus.ps1` | Builds the seed corpora from the real test messages in `hmailserver\test`, byte for byte. |
| `run-fuzz.ps1` | Runs a target, replays a corpus, or reproduces a crash artifact. |
| `Find-ClangCl.ps1` | Locates clang-cl and imports the MSVC environment. Same shape as `build\Find-MsBuild.ps1`. |
| `dict\mime.dict` | MIME token dictionary. Without it the mutator never produces a matching multipart boundary. |
| `harness\idna_ada_fuzzer.cpp` | Differential target: `Idna::DomainToAscii`, `SURBL::HostOfAuthority` and `SURBL::FindLinkHosts` (in text, in HTML, and in HTML written in character references) against what ada makes of the same host, and `FindLinkHosts` against every host 6.3.6 handed over for it, and against the host before a character a mail program ends a link at, put after it. Linux only, built and run by `build/ci/idna-differential-fuzz.sh` ([Fuzzing.md](../hmailserver/docs/Fuzzing.md), "UTS 46 against ada"). |
| `harness\surbl_636_reading.h` | A copy of 6.3.6's reading of a link's host part (`SURBL.cpp` at `v6.3.6`), which the target holds the link reader to handing over every host of. |
| `harness\idna_ada_side.cpp` | ada's half: its domain parser, its URL parser's host, and six probes of its own label check that tell how it reads a code point's direction. Compiled against ada alone. |
| `harness\idna_ada_common.h` | What the target and the check share: UTF-8, the code points whose tables differ, and ada's four departures from UTS 46, each a rule the target sets aside. |
| `harness\idna_ada_conformance.cpp` | Run before the fuzz: the server's NFC held to `NormalizationTest.txt`, and ada held to `IdnaTestV2.txt` beside the server, every difference one of the rules above. |
| `idna\CMakeLists.txt` | The CMake project the two are built by: the server's core library with libFuzzer coverage and AddressSanitizer, and ada from PyPI. |
| `dict\idna.dict` | Tokens for the UTS 46 target: the characters UTS 46 maps, deletes or refuses, in UTF-8, and the shapes of a host. |
| `regression\` | Inputs for findings that have been fixed. Committed, byte-exact, replayed on every run. |

Generated, not committed: `build\`, `bin\`, `corpus\`, `artifacts\`.
