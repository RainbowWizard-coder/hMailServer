ClamAV: scanning with clamd over TCP or its Unix socket
=======================================================

**The server sends every message it scans to a ClamAV daemon, clamd, with
clamd's INSTREAM command, and reads clamd's verdict. On Windows it reaches
clamd over TCP, at a host and a port. On Linux it reaches clamd over TCP too,
or over the Unix socket that the distributions' clamd listens on by default:
set `ClamAVHost` to the path of that socket, and nothing else is needed on
Debian and Ubuntu. Scanning is off until `ClamAVEnabled` is turned on.**

What the server sends
---------------------

For each message it scans the server opens a connection to clamd, sends
`nINSTREAM`, then the message file in chunks of up to 4,096 bytes, each after
its length in four bytes, then a length of zero, and reads clamd's one-line
answer: `stream: OK`, or `stream: <name> FOUND`, which is a virus called
`<name>`. A message found infected is handled as **Action** says (the message
deleted, or its attachments). A clamd that cannot be reached, or does not
answer within the ClamAV timeouts, is reported in the error log, and the
message is then delivered unscanned unless `AVFailAction=1` holds it
([DiagnosingStalledMail.md](DiagnosingStalledMail.md) says how the timeouts
work).

The connection test sends `nPING`, which a healthy clamd answers `PONG`, then
`nVERSION`, whose answer names the engine and its signatures
(`ClamAV 1.4.3/27800/...`), then a few harmless bytes that must be clean and the
EICAR test string that must not. It is the Control Panel's **Test ClamAV
connection** button, and `AntiVirus.TestClamAVScanner` over COM, on Windows.
The REST API has no route for it yet, so on Linux there is no button: send a
message and read the log, as below.

The settings
------------

| Setting | Over REST (`/api/v1/settings/antivirus`) | What it is |
|---|---|---|
| `ClamAVEnabled` | `clamav_enabled` | Whether messages are scanned by clamd. Off by default. |
| `ClamAVHost` | `clamav_host` | clamd's host name or address, `localhost` by default; on Linux, or the absolute path of clamd's Unix socket. |
| `ClamAVPort` | `clamav_port` | clamd's TCP port, 3310 by default (`TCPSocket` in `clamd.conf`). Not used when `ClamAVHost` is a path. |

All three are server settings in the database, so they are set in the Control
Panel (**Anti-virus settings**, the **ClamAV** tab), in the Control Deck
(**Settings**, the **Anti-virus** card), over REST or with `hmctl`, and take
effect for the next message.

`ClamAVHost` is a path when it starts with `/`; no host name does. A path is
used as it is written, and may be at most 107 bytes long, which is what a Unix
socket's address holds; a longer one is refused when it is saved over REST.
**On Windows a path is refused** - clamd on Windows listens on TCP, and this
server reaches no Unix socket there - with a sentence that names the setting,
over COM (the Control Panel), over REST (the Deck), and by the connection test.
A value either rule refuses that reached the database some other way - a
database moved from a Linux server to Windows, say - is reported with the same
sentence at each scan, and the message is handled as for a clamd that cannot
be reached.

clamd on Linux
--------------

Each distribution's own clamd package listens on a Unix socket, and the server's
user needs write permission on that socket, and permission to pass through the
directories above it, to connect. Under the packages the server runs as the
user `hmailserver`.

| Distribution | Package | The socket (`LocalSocket`) | Who may connect |
|---|---|---|---|
| Debian, Ubuntu | `clamav-daemon` | `/var/run/clamav/clamd.ctl` (`/var/run` is `/run`) | Everybody: `LocalSocketMode 666`, `LocalSocketGroup clamav`. When systemd starts clamd through `clamav-daemon.socket`, the socket is the same, `/run/clamav/clamd.ctl`, made by systemd with its default mode, 0666. |
| Alpine | `clamav-daemon` | `/run/clamav/clamd.sock` | `LocalSocketMode` is not set, which `clamd.conf` describes as "socket is world accessible". |
| Fedora | `clamd`, as the `clamd@scan` service | none until `LocalSocket /run/clamd.scan/clamd.sock` in `/etc/clamd.d/scan.conf` is uncommented | The file's own suggestion beside it is `LocalSocketGroup virusgroup` and `LocalSocketMode 660`, with which the server's user must be in `virusgroup`. |

Read from the packages' own `clamd.conf` (Debian 13's clamav-daemon 1.4.3,
Alpine 3.24's clamav-daemon, Fedora 44's clamd 1.4.6) on 26 October 2026.

So on Debian or Ubuntu:

```sh
sudo apt install clamav-daemon          # clamd starts once clamav-freshclam has fetched the signatures
hmctl settings set antivirus --set-string clamav_host=/var/run/clamav/clamd.ctl --set clamav_enabled=true
```

When the socket's mode leaves others out (`LocalSocketMode 660`, as Fedora's
file suggests and any `clamd.conf` may say), put the server's user in the
socket's group - `LocalSocketGroup`, or else the primary group of the user clamd
runs as - and restart the server, which reads its groups when it starts:

```sh
sudo usermod -aG clamav hmailserver     # Debian, Ubuntu (adduser hmailserver clamav, or addgroup on Alpine)
sudo usermod -aG virusgroup hmailserver # Fedora, with the group its scan.conf names
sudo systemctl restart hmailserver
```

Under its systemd unit the server can reach the socket as it is:
`RestrictAddressFamilies` allows `AF_UNIX`, and `ProtectSystem=strict`, which
makes `/run` read-only to the service, does not stop a connection to a socket
there - connecting writes to the socket, not to the file system it is on, and a
read-only mount refuses writes to files, directories and links only. The one
place that does not work is `/tmp`: the unit has `PrivateTmp=yes`, so a clamd
socket under `/tmp` is not the one the service sees. Every package above keeps
its socket under `/run`.

Over TCP instead: set `TCPSocket 3310` (and `TCPAddr 127.0.0.1`) in
`clamd.conf`, and `ClamAVHost` to the host and `ClamAVPort` to the port.

What the log says
-----------------

| The error log says | What it means |
|---|---|
| `Unable to connect to ClamAV server at <path>: nothing is listening there, as there is no socket at that path.` | No socket at that path: clamd is not running, or `LocalSocket` names another path. |
| `Unable to connect to ClamAV server at <path>: nothing is listening there: the socket is there, and no clamd accepts on it.` | A socket left behind by a clamd that is no longer running. |
| `Unable to connect to ClamAV server at <path>: the socket is there, and this server's user may not connect to it. ...` | The socket's mode or group, or a directory above it, keeps the server's user out: see above. |
| `ClamAVHost is <path>, a socket path of <n> bytes, and the path of a Unix socket can have at most 107.` | The path is too long for a socket address. |
| `ClamAVHost is <path>, the path of a Unix socket, which this server cannot reach on Windows: ...` | A path on Windows. |
| `Unable to connect to ClamAV server at <host>:<port>.` | Over TCP, nothing answered at that host and port. |
| `Timed out after <n> seconds while reading response. Remote server: <path or host:port>.` | clamd took the message and did not answer within the timeout. |

With debug logging on, a scanned message adds `Connecting to ClamAV virus
scanner...` and `No virus detected: Result: stream: OK` to the log; with
application logging on, an infected one adds `Message will be deleted
(contained virus <name>).`

What it does not do
-------------------

INSTREAM is the one scanning command sent, with PING and VERSION for the
connection test. There is no TLS to clamd (clamd has none of its own; a tunnel
belongs outside the server), no STATS, no MULTISCAN (it scans paths on clamd's
machine, not a stream) and no zINSTREAM (compression on a loopback link), and no
named pipe on Windows (Roadmap.md, *Other clamd transports and commands*).

How it is tested
----------------

`clamd-socket-tests`, a tester the Linux build makes and runs
(`platform/clamd_socket_tests_main.cpp`), stands up a fake clamd on a Unix socket
and holds the client to it: clean and infected streams from memory and from a
file of three chunks, PING and VERSION, the connection test, a missing socket and
a socket nothing accepts on reported as nothing listening, a socket the user may
not write to, the deadline and a path too long. Given a real clamd's socket as
its argument (`clamd-socket-tests /var/run/clamav/clamd.ctl`) it runs the
connection test against that clamd as well. The Linux suite's `AntiVirus/ClamdUnixSocket` sends messages
through the running server to a fake clamd of its own, and through a real one
when `HMTEST_CLAMD_SOCKET` names it ([RegressionEnvironment.md](RegressionEnvironment.md),
*On Linux*). On Windows, `AntiVirus/ClamdSocketPathOnWindows` holds the
refusals, and `AntiVirus/ClamAV` the scanner against a real clamd over TCP.
