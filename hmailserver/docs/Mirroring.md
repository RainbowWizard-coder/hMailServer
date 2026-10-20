Mirroring and exports
=====================

On 18 September 2026 GitHub suspended the project's account, and with it went the
only other copy of the repository, the issues and the wiki. From 22 September the
project lived on GitLab alone (<https://gitlab.com/hmailserver/hmailserver>,
project 86730559): the 46 issues, the labels, the boards and the wiki existed
nowhere else. Since 18 October 2026 the project is published on GitHub again, as a
second home, under the account `RainbowWizard-coder`
(<https://github.com/RainbowWizard-coder/hMailServer>). GitLab stays canonical: the
pipelines run there and nowhere else, a release is built, signed and published there
first and then copied to GitHub, and a change lands on `master` only through a
GitLab merge request ([GOVERNANCE.md](../../GOVERNANCE.md)). GitLab is the one
place the project is worked on, and GitHub is its mirror. The repository -
`master` and every tag - was first pushed there from a maintainer's clone; since
the first sync of GitLab's push mirror to GitHub finished, at 17:30 UTC that day,
GitLab has pushed `master`, the `batch*` branches and the tags there after every
push, and the two forges carry the same 53 tags (one that only that clone had was
deleted from GitHub). Nothing else GitHub holds came through the mirror: every
release from
6.0.0-B1 to 6.3.7, with the same files byte for byte, was copied by a script that
checked each against the SHA-256 that GitLab's package registry records for it (the
notes of 6.3.6 and 6.3.7 are cut at GitHub's limit of 125,000 characters and
attached whole as a file, and GitHub writes "." where a file name has "~", so the
per-distribution .deb files are renamed there); the labels and the milestones are
there as well; Discussions - announcements, and questions in Q&A - are GitHub's
alone; the wiki is there too, pushed whole - its 74 pages, 59 images and every
commit - to GitHub's wiki on 18 October 2026 from GitLab's, which stays the one
that is edited; and so is a copy of the container image
([ContainerImage.md](ContainerImage.md)). The issues and the boards are GitLab's
alone: the repository on GitHub has no Issues. This document is how the second
copy is kept. It has two halves:

- **a push mirror** to GitHub, which GitLab keeps up to date after every push, for
  the git history: every protected branch and every tag;
- **a weekly export**, the `forge-export` job, for what a mirror does not carry: the
  issues, the merge requests, the releases and the wiki.

On 26 September 2026 neither existed: the project had no push mirror
(`GET /projects/86730559/remote_mirrors` answered `[]`), and the export job waited
for its token and its schedule. **The push mirror has run since 18 October
2026:** GitLab's remote mirror 4137207, to
`https://github.com/RainbowWizard-coder/hMailServer.git`, enabled, for the
protected branches alone - `master` and `batch*` - and keeping no divergent ref
(see the checklist's step 4), so that GitHub carries GitLab's refs. Its first sync
finished at 17:30 UTC that day, and `GET
/projects/86730559/remote_mirrors` shows its `last_successful_update_at` and no
`last_error`; `master`, moved on GitLab at 18:32:09 UTC, was on GitHub at
18:32:11. The export still waits for its token and its schedule: on 18 October
2026 the project has neither `HM_EXPORT_TOKEN` nor a schedule described `export`.
Both halves are the owner's to set up, in
[The owner's checklist](#the-owners-checklist).

What a push mirror carries, and what it does not
------------------------------------------------

GitLab's push mirroring is in the Free tier. After a push to the project, GitLab
pushes the project's repository on to the mirror.

| Carried | Not carried |
|---|---|
| Branches: every protected one (`master`, `batch*`) with *Mirror only protected branches*, or all of them | Issues, their comments and their attachments |
| Tags, the signed release tags included, and so every commit they point at | Merge requests and their discussions |
| | The wiki, which is a git repository of its own |
| | Releases: the notes and the links GitLab keeps beside a tag (the tag itself is carried) |
| | The packages: every release's files in the generic package registry |
| | CI/CD variables, pipeline schedules, runners, the project's settings and members |
| | Labels, milestones, boards, the Service Desk address, the Pages site |

So a mirror is enough to build every release again from its tag on another
forge, and nothing else. The rest is the export's.

The mirror carries nothing else of what GitHub holds - the labels, milestones and
releases it was given for the return of 18 October 2026, the copies of the wiki
and the container image, its Discussions and whatever is opened there since. An
issue is opened on GitLab alone, since the repository on GitHub has no Issues. A
pull request is read on GitHub and, if it is accepted, landed through GitLab, as a
`batch*` branch and a merge request that keep its author and sign-off, never by a
merge on GitHub, whose `master` is GitLab's copy and nothing else. No pipeline of
the project's runs on GitHub. GitHub Actions runs two workflows of the project's
there, and nothing of a build or a release: `.github/workflows/scorecard.yml`,
which scores the repository with OpenSSF Scorecard and publishes the result, and
`.github/workflows/codeql.yml`, which analyses the code with CodeQL on each push
to `master`, on each pull request and weekly, and whose findings the maintainers
read, nothing more ([ContinuousIntegration.md](ContinuousIntegration.md#openssf-scorecard)).
The CodeQL workflow takes the place of GitHub's CodeQL default setup, which
analysed the copy in the same way from 18 October 2026 with no workflow file, and
which OpenSSF Scorecard's SAST check did not count: it sees the default setup only
through the check runs of merged pull requests, and the copy has none. GitHub
takes no workflow's CodeQL analysis while the default setup is on, so the default
setup is switched off when the workflow reaches `master`. The repository's Actions
settings allow GitHub's own actions and `ossf/scorecard-action` alone, each pinned
by a full commit. The two old release workflows that ran there, `sbom.yml` and
`sign-release.yml`, still in the trees of the tags up to `v6.3.3`, have been
disabled since 18 October 2026 ([ASSURANCE-CASE.md](../../ASSURANCE-CASE.md)).

GitHub's rules for the copy
---------------------------

GitHub's rulesets keep the copy GitLab's: only the repository's administrator
role, as which the push mirror pushes, writes `master`, the `batch*` branches and
the release tags, and nobody deletes `master` or a tag, moves a tag or
force-pushes `master`. As read through GitHub's API on 20
October 2026 (*Settings*, *Rules*, *Rulesets*):

| Ruleset | On | What it holds | Who may bypass it |
|---|---|---|---|
| 25332414, "master: no force push, no deletion, linear history" | `master` | No deletion, no force push, a linear history | Nobody |
| 25352233, "master: updated by the GitLab mirror alone" | `master` | No update | The repository's administrator role |
| 25445910, "master: a pull request needs two reviews, a code owner's and CodeQL" (since 20 October 2026) | `master` | A change only through a pull request, with two approvals, the code owner's among them (`.github/CODEOWNERS`), and one more for a pull request Copilot opens unattributed to a person (GitHub's default), the last push approved by someone other than its author, approvals dismissed by a new push and every review thread resolved; and CodeQL's check, `CodeQL`, passed on a branch up to date with `master` | The repository's administrator role |
| 25352238, "batch branches: the mirror's copies, not edited here" | `batch*` | No creation, update, deletion or force push | The repository's administrator role |
| 25352236, "release tags: created by the GitLab mirror alone" | Tags `v*` and `build-inputs-*` | No creation | The repository's administrator role |
| 25332415, "release tags: no deletion, no moving" | Tags `v*` and `build-inputs-*` | No deletion, update or force push | Nobody |

The mirror pushes with the owner's token, as the repository's administrator, the
role the bypasses name, and the owner's account is the one that holds it
([GOVERNANCE.md](../../GOVERNANCE.md), "Maintainers"). The pull-request ruleset
holds what a merge on GitHub would need; none is made, since a pull request there
is landed through GitLab, and the mirror's push passes it as it passes the others.
GitHub's documentation names the check that holds a pull request's code-scanning
results, in every configuration of code scanning, *Code scanning results*, with
each tool's results shown separately
(*Triaging code scanning alerts in pull requests*); the tool's own check run is
named for the tool, `CodeQL`, which is the context the ruleset requires. GitHub's
API showed a check run of that name, from the GitHub Advanced Security app, on the
pull requests of two public repositories on 20 October 2026, one with a CodeQL
workflow and one under the default setup; the copy has had no pull request yet, so
the check has not been seen there. OpenSSF Scorecard's Branch-Protection check
reads these rules, and reads the administrator's bypass, which the mirror needs,
as rules that do not apply to administrators
([ContinuousIntegration.md](ContinuousIntegration.md#openssf-scorecard)).

The weekly export
-----------------

`forge-export` (`.gitlab/ci/forge-export.yml`, which `.gitlab-ci.yml` includes)
runs `build/ci/forge-export.py` on linix, once a week, in the pipeline of the
schedule whose description is `export`, and in no other pipeline. It writes one
archive, `hmailserver-hmailserver-export-<UTC time>.tar.gz` (the project's path slug,
`CI_PROJECT_PATH_SLUG`, since the move into the group on 29 September 2026):

| File | What it holds |
|---|---|
| `issues.json` | Every issue, open and closed, the confidential ones included, each with its notes |
| `merge_requests.json` | Every merge request, each with its notes |
| `labels.json`, `milestones.json`, `boards.json` | As the API gives them |
| `releases.json` | Every release: its notes, and the links to its files |
| `wiki.bundle` | The wiki's git repository, all of its history and the files attached to its pages; `git clone wiki.bundle wiki` reads it |
| `project.json` | The project's settings as the API gives them |
| `MANIFEST.txt` | The counts, and each file's SHA-256 |

Run by hand against the project on 26 September 2026, it took 21 seconds and wrote
6.1 MiB: 46 issues (one confidential) with 63 notes, no merge request, 40 labels,
2 milestones, 2 boards, 42 releases and the wiki's 79 commits.

The export is of the GitLab project alone: a pull request or a discussion opened on
GitHub is not in it.

**GitLab's own project export**, when the second token is set: the job asks
GitLab for it (`POST /projects/86730559/export`), waits for it and downloads it.
It is the complete copy: the repository, the uploads attached to issues, the
issues and merge requests with their events, and more; GitLab's documentation
lists what it leaves out (CI/CD variables, job logs and artifacts, packages and
the container registry among them). It is also the copy another GitLab can
import. It needs a token with the `api` scope from a Maintainer, which is why it
is optional. Without that token the job skips it and says so in its log. When
GitLab refuses the token or the export fails, the job fails so that the schedule's
owner hears of it, but the week's archive is still kept and still the job's
artifact: the artifact and the cache are saved when the job fails too, and the
script renames an archive into place only once it is whole.

**What neither carries:** the release files themselves (2,854 MiB in the package
registry on 26 September 2026; `releases.json` holds their links), CI/CD variables
(secrets, on purpose), and the pipeline schedules.

### Where the archives are kept

- **On linix, off GitLab.** The last four archives of each kind, the project
  exports among them, in the job's cache (`forge-export-kept/`, cache key
  `forge-export`). The runner keeps its cache in a volume on linix, so these copies
  survive whatever happens to GitLab. They are there only while linix and its
  runner are: copy one somewhere else now and then (the checklist says how).
  GitLab's project export is kept only here.
- **On GitLab, for 13 weeks.** Each week's archive of the issues, releases and
  wiki, but not the project export, is the job's artifact. It can be downloaded by
  Developers and above only (`artifacts:access: developer`): the project is
  public, its artifacts would otherwise be too, and the archive holds the
  confidential issues. The job's log is public as well, so the script writes counts
  to it and never an issue's title or text.

**Why not the package registry, and why the project export stays off GitLab.**
Anyone can download from the package registry of a public project, and nothing
in it expires. On 26 September 2026 the namespace may hold 5,000 MiB
(`storageSizeLimit`) and held 3,111 MiB, of which the release files were 2,854
MiB and the repository 248 MiB: thirteen weekly archives at about 6 MiB fit, a
project export (with the repository in it) every week would not.

### The tokens

The job reads two CI/CD variables of the project. Neither is in any file of the
tree, and the script sends each as a header, to the API and to git, never on a
command line and never to the log. `build/check-forge-export.py` fails on a CI
file that gives either a value.

| Variable | Needed for | What it is |
|---|---|---|
| `HM_EXPORT_TOKEN` | the job at all | A personal access token with the scopes `read_api` and `read_repository`, of an account with the Reporter role or above in the project, so that it sees the confidential issues (the job refuses a lower role) |
| `HM_EXPORT_FULL_TOKEN` | GitLab's project export | A personal access token with the scope `api`, of an account with the Maintainer role |

Personal access tokens, because project and group access tokens are a Premium
feature on GitLab.com. A personal access token acts for its account across every
project that account can reach, so the safest holder is an account made for this
alone and given the role in this project only. The owner's own account works too;
then the `api` token of the second row can change anything the owner can. Each
variable is scoped to the environment `forge-export`, which `forge-export` alone
names, so that no other job of a protected branch's pipeline is given it - one that
a switch sends to GitLab's hosted runners among them ([ContinuousIntegration.md](ContinuousIntegration.md),
"GitLab's hosted runners beside the project's"). Leaving `HM_EXPORT_FULL_TOKEN` unset
and making the project export by hand from time to time (*Settings*, *General*,
*Advanced*, *Export project*) keeps no such token anywhere.

**While `HM_EXPORT_TOKEN` is not set the job refuses.** It fails before it asks
GitLab anything, saying the variable is missing and pointing here, and GitLab tells
the schedule's owner of the failed pipeline. A token GitLab refuses, one without
`read_repository`, and one whose account cannot see a confidential issue fail the
job the same way, each with its own sentence. A token that expires within 30 days
is named in the log every week until it is replaced.

The job does not use the pipeline's own `CI_JOB_TOKEN`. What that token may read
through the API is a fixed list of endpoints, and it was not tried here for the
issues or the wiki.

The owner's checklist
---------------------

**The push mirror.** Done on 18 October 2026: steps 1 to 5 say how it was made,
and how to make it again.

1. The account on the second forge: `RainbowWizard-coder` on GitHub, since
   18 October 2026. Done.
2. The repository there: <https://github.com/RainbowWizard-coder/hMailServer>, since
   18 October 2026, first pushed from a maintainer's clone with `master` and every
   tag as GitLab has them; the protected `batch*` branches followed when the mirror
   ran, since it carries every protected branch. Done; the stub branch `main` the
   repository had at first was deleted at 17:27 UTC, before the mirror's first
   sync.
3. Make a token on GitHub that can push to it: *Settings*, *Developer settings*,
   *Personal access tokens*, *Fine-grained tokens*, *Generate new token*, for that
   repository alone, with *Contents* and *Workflows* read and write (*Workflows*
   because `master` carries `.github/workflows/scorecard.yml`, and GitHub refuses a
   push that adds or changes a workflow file from a token without it). Note its
   expiry date. Mirror 4137207 was made on 18 October 2026 with the owner's
   classic token, which acts for his whole account and which `gh` holds (its
   scopes include `repo` and `workflow`); replacing it with a token like this one,
   *Workflows* included, is the owner's.
4. On GitLab: *Settings*, *Repository*, *Mirroring repositories*, *Add new*:
   - Git repository URL: `https://github.com/RainbowWizard-coder/hMailServer.git`;
   - Mirror direction: Push;
   - Authentication method: Username and Password; the username is
     `RainbowWizard-coder`, the password is the token;
   - tick *Mirror only protected branches*; leave *Keep divergent refs* unticked,
     so the mirror is always GitLab's copy: a ref changed on GitHub is overwritten
     with GitLab's at the next push, and a `batch*` branch deleted on GitLab once
     it has landed on `master` is deleted from GitHub at the next push (GitLab
     keeps on the mirror a deleted branch with commits `master` does not have).
     The mirror made on 18 October 2026, 4137207, was made with the box ticked;
     it has been unticked since about 19:00 UTC that day, through the remote
     mirrors API, the one way to change it once a mirror exists (`GET
     projects/86730559/remote_mirrors` shows `keep_divergent_refs` false and
     `only_protected_branches` true), after the one branch it had left on GitHub
     was deleted there by hand. Done.

   Or the same with `glab`, signed in as a Maintainer, with the token read into a
   variable rather than typed on the command line. The command was written for a
   Codeberg repository; for GitHub, the repository URL and the username named above
   go in place of its placeholders - `RainbowWizard-coder` for the `<account>` before
   the token, and `github.com/RainbowWizard-coder/hMailServer.git` for
   `codeberg.org/<account>/hmailserver.git`:

   ```sh
   read -rs MIRROR_TOKEN
   glab api --method POST projects/86730559/remote_mirrors \
     -f url="https://<account>:${MIRROR_TOKEN}@codeberg.org/<account>/hmailserver.git" \
     -F enabled=true -F only_protected_branches=true -F keep_divergent_refs=false
   unset MIRROR_TOKEN
   ```

5. Press *Update now* beside the mirror (or `POST
   projects/86730559/remote_mirrors/<id>/sync`), then check that the second forge
   has `master` and the `v*` tags, and that `GET projects/86730559/remote_mirrors`
   shows `last_successful_update_at` and no `last_error`. Done on 18 October 2026:
   the first sync finished at 17:30 UTC with no error, and GitHub has had `master`
   and the same 53 tags as GitLab since a tag that only a maintainer's clone had,
   pushed with the rest, was deleted from it at 18:22 UTC.

**The copies on GitHub that the mirror does not make**, each made by hand:

- the wiki, pushed whole from a clone of GitLab's over the first page GitHub's
  wiki needs ([Using a copy](#using-a-copy)). Done on 18 October 2026;
- the releases, each copied once GitLab has published it ([RELEASE.md](../../RELEASE.md),
  "The copy on GitHub"). Done for all 47 on 18 October 2026: every one from 6.0.0-B1
  to 6.3.7 (45) and `build-inputs-1` and `build-inputs-2`;
- the container image, copied to `ghcr.io/rainbowwizard-coder/hmailserver` with
  its signatures and attestations ([ContainerImage.md](ContainerImage.md)). Done on
  18 October 2026 for 6.3.7 and 6.3.6; the package is still private there, and the
  owner makes it public in GitHub's package settings (*Change visibility*), which no
  API changes.

**The export.**

6. Make the token for `HM_EXPORT_TOKEN`: in the user settings of the account that
   is to hold it (see [The tokens](#the-tokens)), *Access tokens*, *Add new token*, with
   the scopes `read_api` and `read_repository` and an expiry date. Note the date.
7. Add it on GitLab: *Settings*, *CI/CD*, *Variables*, *Add variable*: key
   `HM_EXPORT_TOKEN`; visibility *Masked and hidden*; flags *Protect variable*
   ticked and *Expand variable reference* unticked; *Environments* `forge-export`.
8. Optionally, the same for `HM_EXPORT_FULL_TOKEN`, with the scope `api`, from an
   account with the Maintainer role.
9. Make the schedule: *Build*, *Pipeline schedules*, *New schedule*: description
   exactly `export`, interval `23 4 * * 0` (Sundays at 04:23 UTC), time zone UTC,
   target branch `master`, no variable, activated. Or:

   ```sh
   glab api --method POST projects/86730559/pipeline_schedules \
     -f description=export -f ref=master -f cron="23 4 * * 0" -f cron_timezone=UTC
   ```

10. Start it once from the schedule's *Run* button. `forge-export` should pass
    with the counts in its log and an archive in its artifact; open the archive
    and check `MANIFEST.txt`.
11. Copy the kept archives off linix now and then, and after the first run. They
    are in the runner's cache volumes on linix: the docker executor keeps one for
    each of the three places a job can run in (`concurrent = 3` since 28 September
    2026, [ContinuousIntegration.md](ContinuousIntegration.md), linix), so each
    place that has run the job holds a kept set of its own, and GitLab Runner 19
    names the volumes by a hash rather than by the project. As `chris` on linix,
    something like:

    ```sh
    for volume in $(docker volume ls -q --filter name=-cache-); do
      docker run --rm -v "$volume":/cache:ro alpine:3.24 find /cache -path '*forge-export*'
    done
    ```

    The cache is a `cache.zip` that holds `forge-export-kept/`. *Clear runner
    caches* on the project's pipelines page starts the kept set afresh.
12. Put the tokens' expiry dates in the calendar. The job names one in its log
    from 30 days before.

**When the project moves**, as it did to the group `hmailserver` on 29 September
2026, the mirror and the export move with it: the mirror is a setting of the
project, and the job reads the project's id and address from the pipeline.

Using a copy
------------

- **The repository**: clone it from GitHub,
  <https://github.com/RainbowWizard-coder/hMailServer>.
- **The wiki**: `git clone wiki.bundle wiki`, then push it to the second forge's
  wiki (on GitHub, `https://github.com/RainbowWizard-coder/hMailServer.wiki.git`,
  once the wiki there has a first page, which GitHub makes the repository from;
  done on 18 October 2026, when GitLab's wiki was pushed over that page whole, with
  `git push --force <that address> HEAD:master` from a clone of GitLab's wiki).
- **The issues, merge requests and releases**: `issues.json`,
  `merge_requests.json` and `releases.json` are GitLab's own API answers. They are
  for reading and for a script to import.
- **All of it at once**: GitLab's project export imports into any GitLab
  (*New project*, *Import project*, *GitLab export*), a self-managed one included.

What checks it
--------------

`build/check-forge-export.py`, which the `checkers` job runs with every other
checker, holds the job to this document: it runs for the `export` schedule on a
protected ref alone, selected by the schedule's description and never by a
variable; on linix; with its image pinned by digest; with an artifact for
Developers and above that expires; keeping what the script writes; and with no
token in any CI file. It breaks each of those rules in a copy and must catch it,
and it runs `build/ci/forge-export.py --self-test`, which exports from an API in
memory: the refusals, the pages of a long list, the notes, the wiki bundle, the
project export waited for through queued and started, the keeping of the last
four, and no token in the log or the archive.
