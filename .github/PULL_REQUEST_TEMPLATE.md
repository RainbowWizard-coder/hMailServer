<!--
Thank you. CONTRIBUTING.md has the detail behind every line of this template.
This repository is the project's second home. Its canonical home is GitLab, https://gitlab.com/hmailserver/hmailserver, and a change lands on master there alone, through a GitLab merge request, after review.
A pull request here is read, and if it is accepted a maintainer makes it again on GitLab as a batch* branch and a merge request, keeping your authorship and your sign-off; it is never merged on GitHub, whose master is GitLab's copy (CONTRIBUTING.md, "Merge requests").
Every commit needs the sign-off trailer, Signed-off-by, with the commit's own author email: git commit -s (CONTRIBUTING.md, "Sign-off").
No pipeline of the project's runs for a pull request: GitHub's own CodeQL default setup analyses it, and the checkers, the sign-off check, the builds and the suites run on GitLab once the change is on a batch* branch. So run what applies yourself, and say below what you ran, and on what.
A security fix goes by SECURITY.md's private routes, never a public pull request.
Text between these comment markers is not shown once the pull request is opened.
-->

## What this changes, and why

<!-- One logical change. An issue is GitLab's, since this repository has no Issues: give its address, because a hash and a number here link this repository's pull request or discussion of that number. For one on the former GitHub project, write "GitHub issue N" in words. -->

## Type of change

- [ ] Bug fix
- [ ] New feature, or a Roadmap2 or Roadmap3 row
- [ ] Refactoring or internal
- [ ] Documentation
- [ ] Build, installer or packaging

## How it was tested

<!-- The regression suite: the full run or the subset (the -Where pattern), on which bench, after build\preflight-tests.ps1 passed. The Linux suite, if it applies. Anything else you ran by hand. -->

## Checklist

- [ ] Branched from the current `master`; one logical change
- [ ] Every commit is signed off (`git commit -s`), with the commit's own author email: the Developer Certificate of Origin, required of every commit that reaches `master`, the maintainers' included, and checked by the *sign-off* job (CONTRIBUTING.md, "Sign-off")
- [ ] The server builds warning-free (`/WX`) with `build\build.ps1`; .NET changes build with `-warnaserror`
- [ ] New behaviour is covered by a regression test, and a fix has a test that fails against the build before it
- [ ] A new test file is listed in `RegressionTests.csproj` (and in `LinuxRegressionTests.csproj` if it compiles there)
- [ ] SQL is parameterised only
- [ ] A schema change has `REQUIRED_DB_VERSION`, the four upgrade scripts (MSSQL, MSSQLCE, MySQL, PGSQL), the three create scripts, DBUpdater's `formMain.cs` and a probe in `SchemaVerification.cs`, and passes `build\check-schema-versions.ps1` (pwsh) and `build\check-db-scripts.ps1` (powershell.exe)
- [ ] A new setting is in `hm_settings` (`Property` / `PropertySet`) with a Control Panel editor, and adds no key to `[Settings]` in `hMailServer.ini`
- [ ] A new Control Panel caption has a mnemonic and a translation in all 17 complete languages
- [ ] The documents say what changed: the page under `hmailserver/docs/` for the feature, `CHANGELOG.md`'s Unreleased section, and the wiki page that copies a changed root document
- [ ] A change under `hmailserver/docs/` is followed by `python build/generate-help-pages.py`, with `HelpPagesData.cpp` committed
- [ ] Generated files are regenerated and committed (the `generate-*.py --check` modes pass)
- [ ] New source files carry the licence header (`python build/add-license-headers.py --check`)
- [ ] A Roadmap2 or Roadmap3 row flipped here carries its file's counter line (`python build/check-roadmap2.py --write`)

## Checkers run

<!-- List them, e.g. python build/check-role-choke-point.py, python build/check-help-map.py, ... and say which failed and why, if any did. -->
