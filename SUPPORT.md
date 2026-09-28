# Getting help

The project's home is GitLab since 22 September 2026: <https://gitlab.com/hmailserver/hmailserver>. GitHub
has been unavailable to the project since 18 September 2026, so older links to its issues, discussions and
releases answer 404.

**Something is broken** — open an [issue](https://gitlab.com/hmailserver/hmailserver/-/issues) with the
**Bug** template.

The most useful report contains, in order of value:

1. **The relevant log, not a description of it.** Turn on debug logging (Monitoring & troubleshooting → Logging → Debug messages in the Control Panel) and paste the window around the problem. The single most useful bug report this project has received was a log that stopped at a particular line — where it stopped was the whole diagnosis.
2. **The `ERROR_hmailserver_<date>.log`** for the same window, if there is one. Even one entry is often decisive.
3. **Version** (the Control Panel shows it; on Linux, `hmailserver --version`), **the platform** (the Windows version, the Linux distribution and how it was installed, or the container image), and **the database backend**.
4. **What you expected instead**, and whether it is reproducible.

If mail is being accepted but not delivered, or a session appears to hang, say which of those it is — they have different causes and the logs differ.

Redact passwords, keys and real email addresses from anything you paste: an issue is public.

**Something might be a security problem** — do not open a public issue. See [SECURITY.md](SECURITY.md): a
confidential issue, or an email to the project's Service Desk address, which GitLab files as a confidential
issue.

**A question rather than a defect** — open an issue with the **Question** template, which gives it the
`question` label. GitLab has no Discussions, so configuration questions, "is this supposed to work like this" and
"has anyone got X working" are issues too.

**No GitLab account** — email the report or the question to the project's Service Desk address,
[contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com](mailto:contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com).
GitLab files the message as a confidential issue, which only the maintainers see, and their replies reach you by
email.

**The forum** at <https://www.hmailserver.co.uk/>, where this fork's releases are announced, answers questions about running it too: [General support](https://www.hmailserver.co.uk/category/8/general-support) is the place for one that is not a defect. General hMailServer questions not specific to this fork are often answered on the long-running [hMailServer forum](https://www.hmailserver.com/forum/), which has years of accumulated configuration knowledge.

## What to expect

This is a maintained fork, not a commercial product with a support contract. Reports are read and taken seriously — several releases this year exist because someone took the time to report something carefully — but there is no response-time guarantee.

Two commitments in return:

- **You will get a straight answer.** If something is not fixed, it will be said plainly rather than implied to be fixed. An open problem that is honestly labelled stays findable; one that is quietly closed does not.
- **A fix is not claimed without evidence.** "Fixed" means reproduced and covered by a test that fails against the previous build. Anything less is described as what it is — a mitigation, a bound, or added diagnostics.

## Before reporting

- Check the [releases](https://gitlab.com/Progressiverobot/hmailserver/-/releases) — the newest version may already address it.
- Check [Roadmap2.md](Roadmap2.md) for the work planned now, [Roadmap3.md](Roadmap3.md) for the packages, the container image and the places to install from that are still to come, and [Roadmap.md](Roadmap.md) for known gaps and what was deliberately not done.
- Search the existing [issues](https://gitlab.com/Progressiverobot/hmailserver/-/issues). The issues and discussions of the former GitHub project are out of reach while GitHub is; a number written as "GitHub issue N" in this repository refers to them.
