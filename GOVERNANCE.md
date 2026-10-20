Governance
==========

This document describes how the hMailServer fork at
[hmailserver/hmailserver](https://gitlab.com/hmailserver/hmailserver)
is run: who decides what, how decisions are made, and what each role is
expected to do. It describes the project as it actually is today, not as an
aspirational structure.

**Status since 18 September 2026.** The `Progressiverobot` account on GitHub,
the project's home there, was suspended on 18 September 2026 and stays
suspended: every link to github.com/Progressiverobot still answers 404, and the
issues, pull requests and discussions it held are still gone. The project's
home has been GitLab since 22 September 2026, and GitLab is canonical. Since
18 October 2026 the project is published on GitHub again, as a second home, at
<https://github.com/RainbowWizard-coder/hMailServer>, under the account
`RainbowWizard-coder`, as GitLab's mirror: the repository (`master`, the
`batch*` branches and every tag, which GitLab's push mirror has pushed there after
every push since that day), every release so far, 6.0.0-B1 to 6.3.7, with the
same files byte for byte, the project's labels and milestones, the wiki's copy and
Discussions. GitLab is the one place the project is worked on: an issue is opened
there alone, since the GitHub repository has no Issues, and a pull request on
GitHub is read and, if it is accepted, landed through GitLab, as a `batch*` branch
and a merge request, keeping its author and sign-off, so a change reaches `master`
only through GitLab, by the merge request described below. The pipelines run on
GitLab and nowhere else: GitHub Actions runs two workflows of the project's,
`.github/workflows/scorecard.yml`, which scores the repository with OpenSSF
Scorecard, and `.github/workflows/codeql.yml`, which analyses the code there with
CodeQL on each push to `master`, on each pull request and weekly, and whose
findings the maintainers read, nothing more; neither builds, signs or publishes
anything of a release. GitHub's rules there let GitLab's push mirror alone write
`master`, the `batch*` branches and the release tags, and hold a pull request to
two approvals, the code owner's among them (`.github/CODEOWNERS`), and to CodeQL's
check ([Mirroring.md](hmailserver/docs/Mirroring.md#githubs-rules-for-the-copy)).
Where this document describes the
`Progressiverobot` account's GitHub repository - its ruleset and the approval
it required, repository admin, private vulnerability reporting, and release
signing through GitHub's token service - it describes how the project ran
there. On GitLab, up to and including 6.3.4, no approval was required (the
GitHub ruleset had stopped requiring one on 14 September 2026): `master` is
protected, and a change that had passed the regression gate landed on it by
fast-forward. From the first change after 6.3.4, every change is to land
through a merge request that a second person approves - Zain Ul Abidin, or the
owner for Zain's own - before the owner merges it, by fast-forward, once its
pipeline has passed and every thread on it is resolved; `build/land-via-mr.py`
(written on 26 September 2026) lands it that way and refuses a merge request
without that approval unless the owner overrides it (see Review). By 14 October
2026 nine batches had landed that way, the first on 29 September 2026: one,
`batch45` (7 October 2026), with Zain Ul Abidin's approval, and eight on the
owner's override. Since 13 October 2026 Zain Ul Abidin holds no role in the
project, and no other person who could approve is a member of it, so every
landing is on that override until one is ([Maintainers](#maintainers)). The
owner takes direct pushes off `master` once the batch that brings the change
checks has landed through them ([SECURITY.md](SECURITY.md), "Every change"); it
landed on 9 October 2026 (`batch44`), and on 14 October 2026 `master` still let
Maintainers push, so until he does, he can still push a gated tree to `master`
directly, as he did with 6.3.6's release fixes on 5 October 2026. Security
reports go by the routes in [SECURITY.md](SECURITY.md); since 18 October 2026
GitHub's private vulnerability reporting is switched on for the repository
there too, and the maintainers read it. The release chain ran on
GitHub Actions up to 6.3.3; 6.3.4, published on 27 September 2026, was built and
signed by hand; and the chain's replacement, the release jobs of
`.gitlab-ci.yml`, made 6.3.5 on 29 September 2026 and 6.3.6 on 6 October 2026.
A release is built, signed and published on GitLab first and, since
18 October 2026, then copied to GitHub, where a release published after that
day's copies is immutable; the update feed <https://updates.progressiverobot.com>, the package
repositories and the container registry are GitLab's.
The installer's fetched build inputs come from the project's package registry
on GitLab, and no build step downloads from GitHub: the project builds from
GitLab alone, so that no forge's suspension can stop a build again.

The short version
-----------------

This project has **two maintainers**, Christopher Holloway and Zain Ul Abidin,
and is stewarded by Progressive Robot Ltd. Decisions are made by the
maintainers, in the open, on the issue tracker. Since 13 October 2026
Christopher Holloway is the only person with a role in the GitLab project, so
every duty below that needs one is his alone for now ([Maintainers](#maintainers)).

On GitLab, since 26 September 2026, Christopher Holloway's account
`Progressiverobot` is the project's only Owner among its people and its only
person with the Maintainer role or above, and since 13 October 2026 its only
person with any role (its members, read through GitLab's API on 14 October
2026). Zain Ul Abidin (`zainulabidin1990`) held the Developer role from
26 September 2026, the owner's decision; on 13 October 2026 he was made a
Guest, to keep the group's GitLab subscription to its one seat, and then
removed from the project, and Christopher Holloway's older account
`chrisholloway5`, a Developer until then, was removed the same day. The project
access token `laptop-git`, which held the Owner role for work from the owner's
machine, is revoked; its bot user is still listed among the members with that
role, and nothing can act as it. `master` and the `v*` tags are protected for
Maintainers, so every change reaches `master` through the owner, and only he
can create a release tag, which the `release` job also holds to the one key in
`.gitlab/allowed_signers`.

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

**Currently: Christopher Holloway (`Progressiverobot` on GitLab, the Owner;
`RainbowWizard-coder` on GitHub since 18 October 2026, the account the project's
repository there belongs to and its only administrator; `chrisholloway5`, an
earlier GitHub account of his, and on GitLab an older account of his that was a
Developer in the project until 13 October 2026) and Zain Ul Abidin
(`zainulabidin1990` on GitLab, a Developer in the project from 26 September to
13 October 2026 and not a member of it since; `Zainulabidin90` on GitHub, removed
from the project's repository there on 18 October 2026).**

The maintainers are accountable for the project. In practice this means:

- **Accepting changes.** Reviewing merge requests on GitLab and, since
  18 October 2026, pull requests on GitHub, landing them on `master` through a
  GitLab merge request (a pull request as a `batch*` branch, keeping its author
  and sign-off), and deciding what does and does not belong in the fork.
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
| Accepting changes | Yes | No, since 13 October 2026 ³ |
| Releasing | Yes | No ¹ |
| Security response | Yes | No ² |
| Direction | Yes | Yes |
| Infrastructure | Yes | No |

Christopher Holloway owns the GitLab project and holds the signing and release
path. On GitHub, until 18 September 2026, he held admin rights on the
repository, and Zain Ul Abidin held write access, which was enough to review and
merge pull requests and to run the release workflows. On GitLab, Zain Ul Abidin
held the Developer role from 26 September 2026, the owner's decision, and holds
no role in the project since 13 October 2026. On GitHub, since 18 October 2026,
the project's repository, <https://github.com/RainbowWizard-coder/hMailServer>,
belongs to Christopher Holloway's account `RainbowWizard-coder`, which administers
it, and nobody else holds write access there (its collaborators, read through
GitHub's API on 18 October 2026): Zain Ul Abidin's account `Zainulabidin90`, a
collaborator there at first, was removed from it that day, in line with his
holding no role in the project since 13 October 2026. Three of the duties above need
more than GitLab's Developer role gives, or a decision not yet made:

1. **Releasing** is the owner's alone. **Release tags are created by
   Christopher Holloway, as the `Progressiverobot` account, alone** (decided on
   26 September 2026), and he is the only member GitLab lets create one: the
   protected tag rule for `v*` allows Maintainers, and he is the only person
   among the members with that role or above (the one other member listed with
   it, the bot user of the revoked token `laptop-git`, can do nothing). That
   role is the real control. The protected-tag rule names the role, not his
   account, and a Maintainer's `v*` tag runs the tagged commit's own
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
   Planner role or above; since 18 October 2026 a report may also arrive
   through private vulnerability reporting, switched on for the project's
   GitHub repository <https://github.com/RainbowWizard-coder/hMailServer>, which
   the maintainers read. Zain Ul Abidin was a member of the GitLab project
   from 24 September 2026 (`zainulabidin1990`), a Developer from 26 September
   2026, and so could see every report, until 13 October 2026; since then he is
   not a member, and sees none. Security response rests on Christopher
   Holloway, the one person who sees a report; whether Zain Ul Abidin takes the
   duty on, should he be a member again, is the maintainers' decision. (On
   GitHub, until 18 September 2026, reports arrived through
   private vulnerability reporting, which showed every advisory only to
   repository admins, security managers and organisation owners.)
3. **Accepting changes** on GitLab needs the Maintainer role: `master` is
   protected, and only Maintainers may merge to it (or push to it, until the
   push rule becomes *No one*; [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
   "Landing on master"). As a Developer, from 26 September to 13 October 2026,
   Zain Ul Abidin could push branches of his own, open merge requests and
   review others', and from the first change after 6.3.4 he was the second
   person whose approval each merge request needs, the owner's batches
   included, before the owner merges it (see Review); his own changes, and a
   contributor's he had reviewed, reached `master` through the owner, who
   reviews and lands them. Since 13 October 2026 he can do none of that in the
   project; like anyone, he can still comment on its public issues and merge
   requests, and open a merge request from a fork.

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
request, and from the first change after 6.3.4 every change is to be approved by
a maintainer other than its author before it lands. Since 13 October 2026
nobody in the project can approve the owner's batches, in which every change
lands, so each lands on the owner's override (see Review). The reasoning is
recorded where the decision
happens — in the merge request, in the issue, or in the release notes. The
project's habit is to write down *why* rather than just *what*; the release
notes are the clearest example of this and are the intended place to look for
the reasoning behind a behavioural change. On GitLab, since 26 September 2026,
whoever decides, a change lands on `master` through the owner, the one member
with the Maintainer role (note 3 under [Maintainers](#maintainers)), and, from
the first batch it lands, through a merge request: the owner's own work as a
`batch*` branch that `build/land-via-mr.py` lands
([CONTRIBUTING.md](CONTRIBUTING.md), "Merge requests"). Since 18 October 2026 a
pull request on GitHub is accepted too, and lands the same way: a maintainer
puts it on a `batch*` branch, keeping its author and sign-off, and a merge
request lands it, so whichever forge a change is proposed on, it lands on
`master` only through a GitLab merge request.

**Review.** From the first change after 6.3.4, every change lands on `master`
through a merge request, which Christopher Holloway, the owner, merges - by
fast-forward, with the merge request's pipeline green - once a second person has
reviewed and approved it (the owner's decision of 26 September 2026): Zain Ul
Abidin, for the owner's own changes, those an agent makes on his behalf,
contributors' and Renovate's, and the owner for a merge request of Zain Ul
Abidin's own. A reviewer asks for changes on the merge request rather than
pushing to the author's branch. While the reviewer is unavailable, a change
waits, or the owner lands it on his override (below).

That is the rule; this is the record. By 14 October 2026 nine batches had
landed through a merge request, the first on 29 September 2026. One, `batch45`
(!13, 7 October 2026), had Zain Ul Abidin's approval. Eight landed on the
owner's override, each with its reason on its merge request - from 6 October
2026 the owner's standing decision to leave a second person's approval unmet for
the moment - and Zain Ul Abidin reviewed one of them afterwards, `batch43`
(!12), on its merge request (the landing log, `land-via-mr.log`, and the merge
requests' notes). Since 13 October 2026 Zain Ul Abidin holds no role in the
project ([Maintainers](#maintainers)) and no other person who could approve is a
member of it, so until one is, every change lands on the owner's override and
waits for a reviewer to read it afterwards.

No approval rule is configured on the project, so nothing in GitLab makes an
approval a condition of the merge. What the project's merge settings do hold is
that a merge request merges only by fast-forward, only once its pipeline has
succeeded and only with every thread on it resolved. Whether an author may
approve their own merge request, and whether a new push takes an approval off,
are project settings; the landing script never counts an author's approval, and
counts only one given after the tip it lands reached the merge request, whatever
those settings say. So the approval is held by `build/land-via-mr.py`, and by
the owner, the one member who can merge; it is recorded on the merge request
rather than anywhere private, because that record is what shows that a second
person has read the change. `master` takes a Maintainer's direct push until the
owner closes it to direct pushes, which he does once the batch that brings the
change checks has landed through them
([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
"Landing on master"; [SECURITY.md](SECURITY.md), "Every change"), leaving the
merge request as the only way in; that batch, `batch44`, landed on 9 October
2026, and on 14 October 2026 `master` still let Maintainers push.

How a reviewer with the Developer role - Zain Ul Abidin, until 13 October 2026 -
reviews one of the owner's batches:

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
disclosure window, or a broken build on `master` - or, since 6 October 2026,
under his standing decision to leave the approval unmet for the moment, with
`python3 build/land-via-mr.py batchN --land-without-approval "<reason>"`: the
script refuses the override unless it runs as one of the owner's accounts, and
records the reason in the landing note and its log; a change landed that way is
to be reviewed afterwards, on its merge request. A batch that changes the gates' own
files - the change checks' scripts, `.gitlab-ci.yml` and the CI files their
jobs are in or take a part from, the landing script,
`build/check-release-chain.py`, the sign-off check and the secret scan the
landing runs and that scan's rules script, and the source scans' scripts -
lands only with
`--gates-read "<reason>"`, the
word of the person who read that change whole, which the landing note records:
a change to what judges every change is read by a person, not by the gates it
changes. A contributor's merge request on GitLab, or pull request on GitHub, is
approved by a maintainer on that merge request or pull request before its
commits are put on a batch; a fork's pipeline runs in the fork, and no pipeline
of the project's runs on GitHub, so neither result is the gate, and the batch's
pipeline is ([SECURITY.md](SECURITY.md), "Every change").

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
the other would miss that timetable. Only Christopher Holloway sees a report
and can publish an advisory (the second note under
[Maintainers](#maintainers)), so in practice those decisions are
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
2026, before building a track record in this repository; that record was to be
built through the review rule under [How decisions are made](#how-decisions-are-made),
and when he stopped holding a role in the GitLab project, on 13 October 2026,
it held one approval of his (`batch45`, !13) and one review after a landing
(`batch43`, !12).

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
token: GitHub's for the releases from 6.2.19 to 6.3.3, GitLab's from 6.3.5),
so there is no private key behind it to inherit or to lose, which removes the
single most common continuity failure for a small project. Release *tags* are
signed with the owner's own SSH key, the one key
[.gitlab/allowed_signers](.gitlab/allowed_signers) may hold, Christopher
Holloway's; a lost key is replaced by putting a new one in its place on
`master`, not inherited.

Continuity
----------

**Current status: two maintainers, one from Progressive Robot Ltd and one
independent of it, of whom only the first holds a role in the GitLab project
since 13 October 2026; access continuity is arranged; the record of knowing the
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
after 6.3.4 every change is also to wait for the other maintainer's approval,
which since 13 October 2026 nobody in the project can give, so that each lands
on the owner's override (see Review).

What remains true, stated as plainly as the single-maintainer position was:

- **The record is one person's.** Every commit on master made by a person
  before 11 September 2026 was authored by Christopher Holloway, and none of
  Christopher Holloway's pull requests had been approved by anyone else, so
  the repository does not yet show a second person who knows the codebase.
  That changes only as both maintainers review and write changes here; the
  review rule under [How decisions are made](#how-decisions-are-made), which
  has a second person approve every merge request from the first change after
  6.3.4, is what would build that record, and it has built little of it so
  far: one approval and one review after a landing, Zain Ul Abidin's, by
  13 October 2026, when he stopped holding a role in the project.
- **Administration rests on one person.** Only Christopher Holloway, among
  people, holds the Owner role on GitLab, and with it the only Maintainer
  rights, and the credentials under
  [Critical assets](#critical-assets); while Christopher Holloway is
  unavailable, settings, rulesets, access and security advisories wait on the
  arrangement above.
- **Two of the duties need more than the Developer role.** Accepting
  changes and releasing need the Maintainer role, which is the owner's alone,
  and releasing needs a release machine besides. Security response needs
  access to private reports, which a Developer has and somebody who is not a
  member of the project, as Zain Ul Abidin is not since 13 October 2026, does
  not; taking it on is the maintainers' decision. See the notes under
  [Maintainers](#maintainers).
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
- The repository is public, on GitLab and, since 18 October 2026, on GitHub
  too, and the licence (AGPLv3) permits anyone to continue the work — a fork is
  always available as a last resort, which is precisely how this project itself
  came to exist.

The project is open to further maintainers; see
[Becoming a maintainer](#becoming-a-maintainer).

Changing this document
----------------------

Propose changes the same way as any other change: open a merge request on
GitLab, or a pull request on GitHub. The maintainers decide, and, as everywhere
else in this project, give a reason.
