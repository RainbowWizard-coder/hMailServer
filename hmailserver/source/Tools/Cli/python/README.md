hmailserver-cli
===============

`hmctl`, the command-line client of
[Progressive Robot's fork of hMailServer](https://gitlab.com/hmailserver/hmailserver),
for the fork's REST API - not the original hMailServer by Martin Knafve: it
administers a server of the fork - on Windows or on Linux, on this machine or
another - from any shell.

The original hMailServer ([github.com/hmailserver/hmailserver](https://github.com/hmailserver/hmailserver))
is developed separately and has no REST API - programs administer it through its
COM API - so `hmctl` does not work with it.

It is the same `hmctl` the server's Linux packages install as `/usr/bin/hmctl`,
built from the same file, at the server's version. It is one module with nothing
behind it but the Python standard library: installing it fetches nothing else, and
it runs no code when it is installed.

Install
-------

```sh
pipx install hmailserver-cli
```

or `python -m pip install hmailserver-cli` into an environment of your own. Python
3.8 or later.

Use
---

The server's REST listener must be on. It is off until an administrator turns it
on, which `hmctl` cannot do, since it needs the listener itself: on a Linux server,
`sudo hmailserver --set-setting RestApiPort=8045` and a restart
([CommandLine.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/CommandLine.md)
says more).

```sh
export HMAIL_URL=https://mail.example.com:8045
export HMAIL_API_KEY="$(cat ~/.config/hmailserver/api.key)"
hmctl domain list
hmctl account list --domain example.com
hmctl queue list
hmctl api GET /api/v1/status
```

Credentials, in the order they are looked for: `--api-key` or `HMAIL_API_KEY` (an
API key, sent as a bearer token); `--password` or `HMAIL_PASSWORD`, and
`--password-file` or `HMAIL_PASSWORD_FILE` (the administrator password, sent as
HTTP Basic); otherwise a prompt, when there is a console to answer it. An API key is
the better answer for anything unattended: it can be read-only, limited to one
domain, and revoked without changing the administrator password.

`--insecure` accepts a certificate nobody verified, and is refused for any host but
the loopback address. A server on another machine that still has the self-signed
certificate a new installation makes is reached once this machine trusts that
certificate:
[CommandLine.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/CommandLine.md)'s
"Credentials" says how, on Linux and on Windows.

Run `hmctl` with no arguments for the resources, `hmctl <resource> --help` for one
resource's verbs, and `hmctl api` for any method on any path.

More
----

- [Administering the server from a command line](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/CommandLine.md):
  every resource and verb, and the PowerShell module that speaks the same vocabulary.
- [The command-line packages](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/CommandLinePackages.md):
  how this package is built and published.
- Licence: AGPL-3.0-or-later.
