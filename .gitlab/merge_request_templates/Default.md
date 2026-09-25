<!--
Thank you. CONTRIBUTING.md has the detail behind every line of this template.
GitLab CI runs the checkers, the page harnesses, editorconfig and the sign-off; the builds, the suites and the checks run by hand are recorded only here. Say what you ran, and on what.
Text between these comment markers is not shown once the merge request is saved.
-->

## What this changes, and why

<!-- One logical change. For an issue opened on GitLab, write its number with a hash; for one on the former GitHub project, write "GitHub issue N" in words. -->

## Type of change

- [ ] Bug fix
- [ ] New feature, or a Roadmap2 row
- [ ] Refactoring or internal
- [ ] Documentation
- [ ] Build, installer or packaging

## How it was tested

<!-- The regression suite: the full run or the subset (the -Where pattern), on which bench, after build\preflight-tests.ps1 passed. The Linux suite, if it applies. Anything else you ran by hand. -->

## Checklist

- [ ] Branched from the current `master`; one logical change
- [ ] Every commit is signed off (`git commit -s`), with the commit's own author email
- [ ] The server builds warning-free (`/WX`) with `build\build.ps1`; .NET changes build with `-warnaserror`
- [ ] New behaviour is covered by a regression test, and a fix has a test that fails against the build before it
- [ ] A new test file is listed in `RegressionTests.csproj` (and in `LinuxRegressionTests.csproj` if it compiles there)
- [ ] SQL is parameterised only
- [ ] A schema change has `REQUIRED_DB_VERSION`, the four upgrade scripts (MSSQL, MSSQLCE, MySQL, PGSQL), the three create scripts, DBUpdater's `formMain.cs` and a probe in `SchemaVerification.cs`, and passes `build\check-schema-versions.ps1` (pwsh) and `build\check-db-scripts.ps1` (powershell.exe)
- [ ] A new setting is in `hm_settings` (`Property` / `PropertySet`) with a Control Panel editor, and adds no key to `[Settings]` in `hMailServer.ini`
- [ ] A new Control Panel caption has a mnemonic and a translation in all 17 complete languages
- [ ] A change under `hmailserver/docs/` is followed by `python build/generate-help-pages.py`, with `HelpPagesData.cpp` committed
- [ ] Generated files are regenerated and committed (the `generate-*.py --check` modes pass)
- [ ] New source files carry the licence header (`python build/add-license-headers.py --check`)
- [ ] A Roadmap2 row flipped here carries the counter line (`python build/check-roadmap2.py --write`)

## Checkers run

<!-- List them, e.g. python build/check-role-choke-point.py, python build/check-help-map.py, ... and say which failed and why, if any did. -->
