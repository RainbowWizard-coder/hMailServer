Governance
==========

This document describes how the hMailServer fork at
[Progressiverobot/hmailserver](https://gitlab.com/Progressiverobot/hmailserver)
is run: who decides what, how decisions are made, and what each role is
expected to do. It describes the project as it actually is today, not as an
aspirational structure.

**Status since 18 September 2026.** GitHub has been unavailable to the project
since 18 September 2026, and its home has been GitLab since 22 September 2026.
Where this document describes the GitHub repository - its ruleset and the
approval it required, repository admin, private vulnerability reporting, and
release signing through GitHub's token service - it describes how the project
ran there. On GitLab, no approval is required (the GitHub ruleset had stopped
requiring one on 14 September 2026), `master` is protected, and a change that
has passed the regression gate lands on it by fast-forward. Security reports go
by the routes in [SECURITY.md](SECURITY.md). The release chain ran on GitHub
Actions; its replacement, the release jobs of `.gitlab-ci.yml`, is written and
has not run yet, so 6.3.4 was built and signed by hand. The installer's fetched
build inputs come from the project's package registry on GitLab. Until a
release has been made by those jobs, *Critical assets* below overstates what is
committed.

The short version
-----------------

This project has **two maintainers**, Christopher Holloway and Zain Ul Abidin,
and is stewarded by Progressive Robot Ltd. Decisions are made by the
maintainers, in the open, on the issue tracker.

Both maintainers work for Progressive Robot Ltd, and until this change this
document named Christopher Holloway as the project's only maintainer.
That is stated plainly here for the same reason the project's size is stated in
[SECURITY.md](SECURITY.md): people relying on a mail server are
entitled to know how much of it rests on how few people, and on which
organisation. The [Continuity](#continuity) section below says what is and is
not currently true about that.

Roles and responsibilities
--------------------------

### Maintainers

**Currently: Christopher Holloway (`Progressiverobot` on GitLab, `chrisholloway5`
on GitHub) and Zain Ul Abidin (`Zainulabidin90` on GitHub).**

The maintainers are accountable for the project. In practice this means:

- **Accepting changes.** Reviewing and merging pull requests, and deciding
  what does and does not belong in the fork.
- **Releasing.** Producing releases according to [RELEASE.md](RELEASE.md),
  including running the full regression suite against the exact binary being
  shipped, and signing every release asset.
- **Security response.** Receiving private vulnerability reports, triaging
  them, and meeting the timetable published in
  [SECURITY.md](SECURITY.md).
- **Direction.** Keeping [Roadmap.md](Roadmap.md) current, and deciding what
  the project will deliberately *not* do.
- **Infrastructure.** Holding and protecting the credentials listed under
  [Critical assets](#critical-assets).

Who holds which today:

| Duty | Christopher Holloway | Zain Ul Abidin |
|---|:-:|:-:|
| Accepting changes | Yes | Yes |
| Releasing | Yes | Not yet ¹ |
| Security response | Yes | Not yet ² |
| Direction | Yes | Yes |
| Infrastructure | Yes | No |

Christopher Holloway owns the GitLab project and holds the signing and release
path. On GitHub, until 18 September 2026, he held admin rights on the
repository, and Zain Ul Abidin held write access, which was enough to review and
merge pull requests and to run the release workflows. Two of the duties above
need more than that, and neither is in place yet:

1. **Releasing** needs Zain Ul Abidin's SSH signing key in
   [.gitlab/allowed_signers](.gitlab/allowed_signers): the `release` job of
   `.gitlab-ci.yml` refuses a tag that does not verify against that file, as
   GitHub's signing workflow did before it. A release also needs
   a machine set up to run [RELEASE.md](RELEASE.md) steps 4 to 11 — the
   regression suite, the fuzz runs and the installer build. Until both are in
   place, releases are tagged by Christopher Holloway.
2. **Security response** needs to see every report. Reports arrive as
   confidential issues on GitLab, opened directly or by mail to the Service
   Desk address ([SECURITY.md](SECURITY.md)), and GitLab shows a confidential
   issue only to its author, its assignees and the project's members with the
   Planner role or above. Until Zain Ul Abidin is a member of the GitLab
   project with such a role, Zain Ul Abidin sees a report only if Christopher
   Holloway assigns it, so security response rests on Christopher Holloway in
   practice. (On GitHub, until 18 September 2026, reports arrived through
   private vulnerability reporting, which showed every advisory only to
   repository admins, security managers and organisation owners.)

### Contributor

Anyone who opens a merge request (a pull request, on GitHub). Contributors are expected to follow
[CONTRIBUTING.md](CONTRIBUTING.md): keep changes focused, keep the
regression suite green, add or update regression tests for behaviour changes,
use parameterised SQL exclusively, and accept that contributions are licensed
under the AGPLv3.

Contributors do not need any prior status. There is no CLA to sign.

### Reporter

Anyone who files an issue or a security report. Reporters are expected to
follow [SUPPORT.md](SUPPORT.md) for defects, and
[SECURITY.md](SECURITY.md) for anything security-relevant — in
particular, **not** to open a public issue for a security problem.

Reporters are credited for security findings unless they ask not to be.

How decisions are made
----------------------

**Ordinary changes.** Either maintainer decides on a contributor's pull
request; a maintainer's own pull request also needs the other's approval (see
Review). The reasoning is recorded where the decision happens — in the pull
request, in the issue, or in the release notes. The project's habit is to
write down *why* rather than just *what*; the release notes are the clearest
example of this and are the intended place to look for the reasoning behind a
behavioural change.

**Review.** A pull request authored by one maintainer, or opened by an agent
on one maintainer's behalf, is approved by the other before it is merged. The
master ruleset enforces this: a merge needs an approving review from someone
other than the last person to push, so a reviewer suggests changes rather than
pushing to the author's branch. A pull request Copilot opens under its own
identity, rather than for one maintainer, needs both maintainers' approval: the
ruleset adds one approval for those. The rule binds both maintainers: while
either is unavailable, the other's own pull requests, release pull requests
included, wait too. When waiting would hold up something urgent — a security
fix inside its disclosure window, or a broken build on master — only an
administrator on the ruleset's bypass list can merge first; Zain Ul Abidin's
write access carries no bypass. A change merged that way is reviewed
afterwards, and its pull request says that it was merged before review, and
why. Reviews are recorded on the pull request rather than anywhere private,
because that record is what shows that a second person has read the change.

**Disagreements.** Raise them on the issue in question. A maintainer will
respond with a reason, not just a verdict. A closed issue is not a closed
conversation — reopening it with new information (especially a reproduction)
is welcome, and has changed outcomes before. If the maintainers disagree, both
positions are written on the issue and Christopher Holloway decides, recording
the reason there. When that decision is to merge a pull request the other
maintainer has not approved, the other maintainer approves it and records the
disagreement in the review.

**Scope.** Whether something belongs in the fork is decided against one
question: does it serve people running this as a mail server in production?
Compatibility with existing hMailServer deployments is a standing constraint,
not a preference — an upgrade must preserve configuration and mail.

**Security.** Security decisions are made under the disclosure timetable in
[SECURITY.md](SECURITY.md), not by consensus. They are the
maintainers' to make, and either maintainer may decide alone when waiting for
the other would miss that timetable. Until the second note under
[Maintainers](#maintainers) is resolved, only Christopher Holloway is notified
of a report and can publish an advisory, so in practice those decisions are
Christopher Holloway's; and a fix authored by Zain Ul Abidin always needs
Christopher Holloway's approval to merge (see Review).

Becoming a maintainer
---------------------

There is no committee and no fixed number of contributions. The path is
demonstrated, sustained judgement: a track record of changes that are correct,
tested, and considerate of existing deployments, plus a willingness to take on
the release and security duties above rather than only the coding.

If you want to help at that level, say so on an issue. The project would
benefit from it — see [Continuity](#continuity).

The second maintainer did not come by that route. Zain Ul Abidin was
appointed in September 2026 from within Progressive Robot Ltd, before building
a track record in this repository; that record is being built through the
review rule under [How decisions are made](#how-decisions-are-made).

Critical assets
---------------

These are the things whose loss would stop the project, and they are listed
so that a successor knows what to look for:

| Asset | What it is for |
|---|---|
| GitLab project owner | Project settings, protected branches and tags, CI/CD variables, confidential issues, and granting access (on GitHub, until 18 September 2026: repository admin, with its rulesets, secrets, private vulnerability reports and security advisories) |
| Release signing path | Sigstore cosign keyless signing of release assets, and the SSH keys listed in `.gitlab/allowed_signers` that sign release tags (one today, Christopher Holloway's) |
| Code-signing material | Signing the Windows installer |
| Build environment recipe | Visual Studio 2026 / v145 plus the external libraries under `hMailServerLibs` |
| Test environment recipe | The database, ClamAV, SpamAssassin and INI configuration the full regression suite needs |

Everything needed to *build, test, release and verify* the product is
committed to this repository and documented — in
[README.md](README.md), [ARCHITECTURE.md](ARCHITECTURE.md),
[RELEASE.md](RELEASE.md), the scripts under `build/`, and the pipeline in
`.gitlab-ci.yml`. Deliberately, nothing about producing a release depends
on knowledge that exists only in one maintainer's head.

Note that release-asset signing is **keyless** (Sigstore, via the CI's OIDC
token: GitHub's for the releases up to 6.3.3, GitLab's from 6.3.5), so
there is no private key behind it to inherit or to lose, which removes the
single most common continuity failure for a small project. Release *tags* are
signed with a maintainer's own SSH key, which must be listed in
[.gitlab/allowed_signers](.gitlab/allowed_signers) — today only Christopher
Holloway's is; a lost key is replaced by adding a new one there, not inherited.

Continuity
----------

**Current status: two maintainers, both at Progressive Robot Ltd; access
continuity is arranged; the record of knowing the codebase is still one
person's.** Christopher Holloway keeps the critical credentials in a managed
arrangement that a trusted person can reach if Christopher Holloway is
confirmed unavailable, together with the legal authority to use them, so the
project could create and close issues, accept changes and publish a release
within days rather than being locked out. A second maintainer with write
access means a contributor's pull request can be reviewed and merged without
that arrangement. On GitHub, Zain Ul Abidin's own pull requests could not: the
master ruleset required an approval from someone other than the last person to
push until 14 September 2026, and GitHub does not let authors approve their own
pull requests. On GitLab no approval is required, so a maintainer who can push
to `master` does not wait on the arrangement.

What remains true, stated as plainly as the single-maintainer position was:

- **The record is one person's.** Every commit on master made by a person
  before 11 September 2026 was authored by Christopher Holloway, and none of
  Christopher Holloway's pull requests had been approved by anyone else, so
  the repository does not yet show a second person who knows the codebase.
  That changes only as both maintainers review and write changes here; the
  review rule under [How decisions are made](#how-decisions-are-made) is what
  builds that record.
- **Administration rests on one person.** Only Christopher Holloway holds
  repository admin and the credentials under
  [Critical assets](#critical-assets); while Christopher Holloway is
  unavailable, settings, rulesets, access and security advisories wait on the
  arrangement above.
- **Two of the duties need more than write access.** Releasing needs a second
  tag-signing key and a release machine, and security response needs access to
  private reports; see the notes under [Maintainers](#maintainers).
- **One organisation.** Both maintainers work for Progressive Robot Ltd, so
  the project still depends on one organisation; only a maintainer from
  outside it would remove that dependence.

The mitigations that were in place for a single maintainer still hold:

- Every build, test and release step is scripted and committed, so the work is
  reproducible by someone else without tacit knowledge.
- Release-asset signing requires no inherited private key, and a tag-signing
  key is replaced rather than inherited.
- The repository is public, and the licence (AGPLv3) permits anyone to
  continue the work — a fork is always available as a last resort, which is
  precisely how this project itself came to exist.

The project is open to further maintainers, and particularly to one from
outside Progressive Robot Ltd; see
[Becoming a maintainer](#becoming-a-maintainer).

Changing this document
----------------------

Propose changes the same way as any other change: open a merge request. The
maintainers decide, and, as everywhere else in this project, give a reason.
