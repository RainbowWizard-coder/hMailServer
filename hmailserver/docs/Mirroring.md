Mirroring and exports
=====================

On 18 September 2026 GitHub suspended the project's account, and with it went the
only other copy of the repository, the issues and the wiki. Since 22 September the
project lives on GitLab alone (<https://gitlab.com/Progressiverobot/hmailserver>,
project 86730559): the 46 issues, the labels, the boards and the wiki exist nowhere
else. This document is how that stops being true. It has two halves:

- **a push mirror** to a second forge, which GitLab keeps up to date after every
  push, for the git history: every protected branch and every tag;
- **a weekly export**, the `forge-export` job, for what a mirror does not carry: the
  issues, the merge requests, the releases and the wiki.

On 26 September 2026 neither exists yet: the project has no push mirror
(`GET /projects/86730559/remote_mirrors` answers `[]`), and the export job waits
for its token and its schedule. Both are the owner's to set up, in
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

The weekly export
-----------------

`forge-export` (`.gitlab/ci/forge-export.yml`, which `.gitlab-ci.yml` includes)
runs `build/ci/forge-export.py` on linix, once a week, in the pipeline of the
schedule whose description is `export`, and in no other pipeline. It writes one
archive, `progressiverobot-hmailserver-export-<UTC time>.tar.gz`:

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
then the `api` token of the second row can change anything the owner can, and it
is readable by any job on a protected branch. Leaving `HM_EXPORT_FULL_TOKEN` unset
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

**The push mirror.**

1. Make an account on the second forge, for example Codeberg (<https://codeberg.org>),
   which takes free, open-source projects; a Forgejo of Progressive Robot's own
   would do as well.
2. Create an empty repository there, `hmailserver`, with no README, licence or
   `.gitignore`, so that the first push is not refused as diverging.
3. Make a token on that forge that can push to it (on Codeberg and Forgejo,
   *Settings*, *Applications*, *Generate new token*, with write access to
   repositories). Note its expiry date.
4. On GitLab: *Settings*, *Repository*, *Mirroring repositories*, *Add new*:
   - Git repository URL: `https://codeberg.org/<account>/hmailserver.git`;
   - Mirror direction: Push;
   - Authentication method: Username and Password; the username is the account on
     the second forge, the password is the token;
   - tick *Mirror only protected branches*; leave *Keep divergent refs* unticked,
     so the mirror is always GitLab's copy.

   Or the same with `glab`, signed in as a Maintainer, with the token read into a
   variable rather than typed on the command line:

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
   shows `last_successful_update_at` and no `last_error`.

**The export.**

6. Make the token for `HM_EXPORT_TOKEN`: in the user settings of the account that
   is to hold it (see [The tokens](#the-tokens)), *Access tokens*, *Add new token*, with
   the scopes `read_api` and `read_repository` and an expiry date. Note the date.
7. Add it on GitLab: *Settings*, *CI/CD*, *Variables*, *Add variable*: key
   `HM_EXPORT_TOKEN`; visibility *Masked and hidden*; flags *Protect variable*
   ticked and *Expand variable reference* unticked.
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
    are in the runner's cache volume on linix, one volume while the runner runs
    one job at a time (`concurrent = 1`; a second slot would get a volume of its
    own and start its own kept set); the first run shows the exact place. As
    `chris` on linix, something like:

    ```sh
    docker volume ls --filter name=project-86730559
    docker run --rm -v <volume>:/cache:ro alpine:3.24 find /cache -path '*forge-export*'
    ```

    The cache is a `cache.zip` that holds `forge-export-kept/`. *Clear runner
    caches* on the project's pipelines page starts the kept set afresh.
12. Put the tokens' expiry dates in the calendar. The job names one in its log
    from 30 days before.

**When the project moves** to the group `hmailserver`, the mirror and the export
move with it: the mirror is a setting of the project, and the job reads the
project's id and address from the pipeline.

Using a copy
------------

- **The repository**: clone it from the mirror.
- **The wiki**: `git clone wiki.bundle wiki`, then push it to the second forge's
  wiki (on Forgejo, `https://codeberg.org/<account>/hmailserver.wiki.git`, once
  the wiki has a first page).
- **The issues, merge requests and releases**: `issues.json`,
  `merge_requests.json` and `releases.json` are GitLab's own API answers. They are
  for reading and for a script to import; while GitLab is reachable, Forgejo's own
  migration from GitLab is the quicker way to carry the issues over.
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
