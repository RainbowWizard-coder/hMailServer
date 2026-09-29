Governance
==========

This document describes how the hMailServer fork at
[hmailserver/hmailserver](https://gitlab.com/hmailserver/hmailserver)
is run: who decides what, how decisions are made, and what each role is
expected to do. It describes the project as it actually is today, not as an
aspirational structure.

**Status since 18 September 2026.** GitHub has been unavailable to the project
since 18 September 2026, and its home has been GitLab since 22 September 2026.
Where this document describes the GitHub repository - its ruleset and the
approval it required, repository admin, private vulnerability reporting, and
release signing through GitHub's token service - it describes how the project
ran there. On GitLab, up to and including 6.3.4, no approval was required (the
GitHub ruleset had stopped requiring one on 14 September 2026): `master` is
protected, and a change that had passed the regression gate landed on it by
fast-forward. From the first change after 6.3.4, every change is to land
through a merge request that Zain Ul Abidin approves (the owner, for Zain's
own) before the owner merges it, by fast-forward, once its pipeline has passed
and every thread on it is resolved; `build/land-via-mr.py` (written on 26
September 2026) lands it that way and refuses a merge request without that
approval (see Review). No batch has landed that way yet: until one has, and the
owner has taken direct pushes off `master`, he can still push a gated tree to
`master` directly, as he did before. Security reports go
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

On GitLab, since 26 September 2026, Christopher Holloway's account
`Progressiverobot` is the project's only Owner among its people and its only
person with the Maintainer role or above. One project access token, `laptop-git`,
which the maintainers' machine works with, holds the Owner role too until it
expires on 27 September 2027; it is the owner's, used on the owner's machine,
and SECURITY.md lists what it can do. Zain Ul Abidin (`zainulabidin1990`) and
Christopher Holloway's older account `chrisholloway5` hold the Developer role;
the owner decided both. `master` and the `v*` tags are protected for
Maintainers, so every change reaches `master` through the owner or that token,
Zain Ul Abidin's included, and only they can create a release tag, which the
`release` job also holds to the one key in `.gitlab/allowed_signers`.

The two maintainers are unassociated: Christopher Holloway is from Progressive
Robot Ltd, and Zain Ul Abidin is independent of it (the owner's statement of
26 September 2026; an earlier version of this document said both worked for
the company, which was wrong). Until September 2026 this document named
Christopher Holloway as the project's only maintainer.
That is stated plainly here for the same reason the project's size is stated in
[SECURITY.md](SECURITY.md): people relying on a mail server are
entitled to know how much of it rests on how few people, and on which
organisation. The [Continuity](#continuity) section below says what is and is
not currently true about that.

Roles and responsibilities
--------------------------

### Maintainers

**Currently: Christopher Holloway (`Progressiverobot` on GitLab, the Owner, and
his older account `chrisholloway5`, a Developer; `chrisholloway5` on GitHub) and
Zain Ul Abidin (`zainulabidin1990` on GitLab, a Developer; `Zainulabidin90` on
GitHub).**

The maintainers are accountable for the project. In practice this means:

- **Accepting changes.** Reviewing and merging merge requests (pull requests,
  on GitHub), and deciding what does and does not belong in the fork.
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
| Accepting changes | Yes | Reviews; lands through the owner ³ |
| Releasing | Yes | No ¹ |
| Security response | Yes | Not yet ² |
| Direction | Yes | Yes |
| Infrastructure | Yes | No |

Christopher Holloway owns the GitLab project and holds the signing and release
path. On GitHub, until 18 September 2026, he held admin rights on the
repository, and Zain Ul Abidin held write access, which was enough to review and
merge pull requests and to run the release workflows. On GitLab, since
26 September 2026, Zain Ul Abidin holds the Developer role, the owner's
decision. Three of the duties above need more than GitLab's Developer role
gives, or a decision not yet made:

1. **Releasing** is the owner's alone. **Release tags are created by
   Christopher Holloway, as the `Progressiverobot` account, alone** (decided on
   26 September 2026), and he is the only member GitLab lets create one: the
   protected tag rule for `v*` allows Maintainers, and he is the only member
   with that role or above. That role is the real control. GitLab's Free tier
   cannot hold tag creation to one account - a protected-tag rule names a
   role - and a Maintainer's `v*` tag runs the tagged commit's own
   `.gitlab-ci.yml`: any pipeline of a protected `v*` tag is given a Sigstore
   certificate naming that tag, which every server built after 6.3.3 accepts.
   The `release` job's own checks - `.gitlab/allowed_signers` read as `master`
   has it, exactly one key in it (his), the tag signed by that key, and the
   tagged commit on `master` - run as the tagged commit has them, so they stop
   a mistake and a tag pushed without his key, not a hostile commit; the Azure
   federated credential made for one tag at a time holds the installer's
   Authenticode signature alone ([RELEASE.md](RELEASE.md), "The release on
   GitLab"). Whoever is given the Maintainer role can therefore sign a release
   the servers accept. A release also needs a machine set up to run
   [RELEASE.md](RELEASE.md) steps 4 to 11 — the regression suite, the fuzz runs
   and the installer build.
2. **Security response** needs to see every report. Reports arrive as
   confidential issues on GitLab, opened directly or by mail to the Service
   Desk address ([SECURITY.md](SECURITY.md)), and GitLab shows a confidential
   issue only to its author, its assignees and the project's members with the
   Planner role or above. Zain Ul Abidin has been a member of the GitLab
   project since 24 September 2026 (`zainulabidin1990`), a Developer since
   26 September 2026, so he can see every report; taking the duty on is the maintainers' decision, and until it is
   made, security response rests on Christopher Holloway. (On GitHub, until 18 September 2026, reports arrived through
   private vulnerability reporting, which showed every advisory only to
   repository admins, security managers and organisation owners.)
3. **Accepting changes** on GitLab needs the Maintainer role: `master` is
   protected, and only Maintainers may merge to it (or push to it, until the
   push rule becomes *No one*; [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
   "Landing on master"). As a Developer, Zain Ul Abidin pushes branches of his
   own, opens merge requests and reviews others'; from the first change after
   6.3.4 he approves each merge request, the owner's batches included, before
   the owner merges it (see Review), and his own changes, and a contributor's
   he has reviewed, reach `master` through the owner, who reviews and lands
   them.

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

**Ordinary changes.** Either maintainer decides on a contributor's merge
request, and from the first change after 6.3.4 every change is approved by a
maintainer other than its author before it lands (see Review). The reasoning is recorded where the decision
happens — in the merge request, in the issue, or in the release notes. The
project's habit is to write down *why* rather than just *what*; the release
notes are the clearest example of this and are the intended place to look for
the reasoning behind a behavioural change. On GitLab, since 26 September 2026,
whoever decides, a change lands on `master` through the owner, the one member
with the Maintainer role (note 3 under [Maintainers](#maintainers)), and, from
the first batch it lands, through a merge request: the owner's own work as a
`batch*` branch that `build/land-via-mr.py` lands
([CONTRIBUTING.md](CONTRIBUTING.md), "Merge requests").

**Review.** From the first change after 6.3.4, every change lands on `master`
through a merge request, and Zain Ul Abidin reviews and approves each one
before Christopher Holloway, the owner, merges it - by fast-forward, with the
merge request's pipeline green (the owner's decision of 26 September 2026).
That covers the owner's own changes, those an agent makes on his behalf,
contributors' and Renovate's. A merge request of Zain Ul Abidin's own is
reviewed and approved by the owner instead. A reviewer asks for changes on the
merge request rather than pushing to the author's branch, and while the
reviewer is unavailable, changes wait. GitLab's Free tier has the Approve
button but no approval rules: anyone with the Developer role or above can
approve, and nothing in GitLab can make an approval a condition of the merge
(no approval rule is configured on the project). What the project's merge
settings do hold is that a merge request merges only by fast-forward, only once
its pipeline has succeeded and only with every thread on it resolved. Whether
an author may approve their own merge request, and whether a new push takes an
approval off, are settings of the paid tiers; the landing script never counts
an author's approval, and counts only one given after the tip it lands reached
the merge request, whatever those settings say. So the approval is held by
`build/land-via-mr.py`, and by the owner, the one member who can merge; it is
recorded on the merge request rather than anywhere private, because that record
is what shows that a second person has read the change. Today `master` still
takes a Maintainer's direct push, which is how 6.3.4 and the batch that carries
it land, gated as before; once the pipeline on `master` is green, `master` is to
be closed to direct pushes ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
"Landing on master"), leaving the merge request as the only way in.

How Zain Ul Abidin, a Developer, reviews one of the owner's batches:

1. Open the merge request (`Land batchN: ...`, opened by `build/land-via-mr.py`),
   and read the **Changes** tab, commit by commit where the batch is large (the
   **Commits** tab).
2. Say what must change as a comment on the line, which opens a thread. GitLab
   will not merge while a thread is unresolved; the author answers, changes the
   batch and pushes it again (a new pipeline), and the reviewer resolves the
   thread when the answer is right.
3. When the change is right, press **Approve**. The approval is recorded on the
   merge request and in its activity. The landing script counts only an
   approval given after the tip it lands reached the merge request, so after a
   new push the reviewer approves again (**Revoke approval**, then **Approve**,
   where the old approval still shows).

`build/land-via-mr.py` reads the approvals just before it merges and records,
in a note on the merge request and in its log, whether somebody other than the
author approved the tip it lands: not the merge request's author, not an
author of one of its commits, not another account of the same person
(`Progressiverobot` and `chrisholloway5` are both Christopher Holloway's), and
not an approval given before that tip reached the merge request (its diff
versions say when). Without such an approval it refuses to merge (exit 7), and
the landing note says so. The owner alone may override that for one landing,
when waiting would hold up something urgent - a security fix inside its
disclosure window, or a broken build on `master` - with
`python3 build/land-via-mr.py batchN --land-without-approval "<reason>"`: the
script refuses the override unless it runs as one of the owner's accounts, and
records the reason in the landing note and its log; a change landed that way is
reviewed afterwards, on its merge request. A contributor's merge request is
approved by a maintainer on the contributor's own merge request before its
commits are put on a batch.

On GitHub, until 14 September 2026, a pull request authored by one maintainer,
or opened by an agent on one maintainer's behalf, was approved by the other
before it was merged, and the master ruleset enforced this:
a merge needed an approving review from someone other than the last person to
push. A pull request Copilot opened under its own identity, rather than for one
maintainer, needed both maintainers' approval: the ruleset added one approval
for those. When waiting would have held up something urgent — a security fix
inside its disclosure window, or a broken build on master — only an
administrator on the ruleset's bypass list could merge first, and a change
merged that way was reviewed afterwards, its pull request saying that it was
merged before review, and why.

**Disagreements.** Raise them on the issue in question. A maintainer will
respond with a reason, not just a verdict. A closed issue is not a closed
conversation — reopening it with new information (especially a reproduction)
is welcome, and has changed outcomes before. If the maintainers disagree, both
positions are written on the issue and Christopher Holloway decides, recording
the reason there. When that decision is to merge a merge request the other
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

The second maintainer did not come by that route. Zain Ul Abidin, who is
independent of Progressive Robot Ltd, was appointed by the owner in September
2026, before building a track record in this repository; that record is being
built through the review rule under [How decisions are made](#how-decisions-are-made).

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
signed with the owner's own SSH key, the one key
[.gitlab/allowed_signers](.gitlab/allowed_signers) may hold, Christopher
Holloway's; a lost key is replaced by putting a new one in its place on
`master`, not inherited.

Continuity
----------

**Current status: two maintainers, one from Progressive Robot Ltd and one
independent of it; access continuity is arranged; the record of knowing the
codebase is still one person's.** Christopher Holloway keeps the critical credentials in a managed
arrangement that a trusted person can reach if Christopher Holloway is
confirmed unavailable, together with the legal authority to use them, so the
project could create and close issues, accept changes and publish a release
within days rather than being locked out. On GitHub, a second maintainer with
write access meant a contributor's pull request could be reviewed and merged
without that arrangement, though Zain Ul Abidin's own pull requests could not
be: the master ruleset required an approval from someone other than the last
person to push until 14 September 2026, and GitHub does not let authors approve
their own pull requests. On GitLab, since 26 September 2026, only the owner can
push or merge to `master`, so while he is unavailable every change, Zain Ul
Abidin's included, waits on the arrangement too; and from the first change
after 6.3.4 every change also waits for the other maintainer's approval (see
Review).

What remains true, stated as plainly as the single-maintainer position was:

- **The record is one person's.** Every commit on master made by a person
  before 11 September 2026 was authored by Christopher Holloway, and none of
  Christopher Holloway's pull requests had been approved by anyone else, so
  the repository does not yet show a second person who knows the codebase.
  That changes only as both maintainers review and write changes here; the
  review rule under [How decisions are made](#how-decisions-are-made), which
  has Zain Ul Abidin approve every merge request from the first change after
  6.3.4, is what builds that record.
- **Administration rests on one person.** Only Christopher Holloway holds
  the Owner role on GitLab, and with it the only Maintainer rights, and the
  credentials under
  [Critical assets](#critical-assets); while Christopher Holloway is
  unavailable, settings, rulesets, access and security advisories wait on the
  arrangement above.
- **Two of the duties need more than the Developer role.** Accepting
  changes and releasing need the Maintainer role, which is the owner's alone,
  and releasing needs a release machine besides. Security response needs
  access to private reports, which a Developer has; taking it on is the
  maintainers' decision. See the notes under [Maintainers](#maintainers).
- **Mostly one organisation, for now.** The maintainers are unassociated -
  Christopher Holloway is from Progressive Robot Ltd, Zain Ul Abidin is
  independent of it - but the administration, the release path and, so far,
  nearly every change are the Progressive Robot maintainer's. The dependence
  on one organisation lessens only as the independent maintainer's reviews
  and changes accumulate here.

The mitigations that were in place for a single maintainer still hold:

- Every build, test and release step is scripted and committed, so the work is
  reproducible by someone else without tacit knowledge.
- Release-asset signing requires no inherited private key, and a tag-signing
  key is replaced rather than inherited.
- The repository is public, and the licence (AGPLv3) permits anyone to
  continue the work — a fork is always available as a last resort, which is
  precisely how this project itself came to exist.

The project is open to further maintainers; see
[Becoming a maintainer](#becoming-a-maintainer).

Changing this document
----------------------

Propose changes the same way as any other change: open a merge request. The
maintainers decide, and, as everywhere else in this project, give a reason.
