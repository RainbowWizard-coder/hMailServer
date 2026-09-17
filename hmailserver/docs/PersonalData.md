# A person's data: the export and the erasure

The two halves of a data-subject request, as the server does them.

## Everything the server holds, as one archive

`Account.ExportArchive(file)` over COM, `POST /api/v1/accounts/{address}/export` with `archive` over REST, `hmctl privacy export <address> --archive <file>` and `Export-HmAccountArchive` write one .zip on the server's disk; `GET /api/v1/accounts/{address}/export.zip` (the administrator password's alone) and the Deck's "Everything (.zip)" on the account row download the same archive, and `GET /api/v1/me/export.zip` - the "Your data" card on the webmail's Settings page - hands it to the person with no administrator involved. A download is held in memory once, so past 500 MB it is refused with 413 and the operator writes it to disk instead; the disk form has no ceiling but the format's 4 GB.

Inside:

| Entry | What it is |
| --- | --- |
| `manifest.json` | the address, the moment, and the counts of what follows |
| `mail/<folder path>.mbox` | every folder with mail, one mbox each: a `From_` line before each message, body lines beginning `From ` quoted, as the folder download of the self-service API writes them |
| `account.xml` | what the backup carries for the account, in the backup's own form: the settings, rules, fetch accounts, app passwords, sender list, the address book, the calendars, the webmail's preferences, the S/MIME keys, the files sent as links, scheduled mail, the password history |
| `contacts.vcf` | the address book as vCard 3.0, the card a client sent where there is one |
| `calendars/<calendar>/<object>` | every calendar object as the client sent it |
| `preferences.json` | the webmail's preferences as stored |
| `filters.sieve` | the active Sieve script, when there is one |
| `logs.txt` | the lines of the server's own log files that name the address, up to 50 MB |

`Account.ExportMessages(directory)` and the `directory` form of the POST stay: a directory of `.eml` files mirroring the folder tree, for the operator who wants exactly that.

## Every trace of an address

`Utilities.EraseAddressTraces(address, includeArchive)` over COM, `POST /api/v1/addresses/erase` over REST, `hmctl privacy erase`, `Remove-HmAddressTraces` and the Deck's "Erase traces" on the account row remove what deleting the account deliberately leaves: quarantine entries, message-trace rows, greylisting triplets, distribution-list memberships, aliases on either side, the bounce and complaint records, and now the rows naming the address in other people's address books (`hm_contacts`) and first-contact memory (`hm_knownsenders`). With the archive included, the person's archive tree is swept file by file: a copy under legal hold keeps its file as well as its index row, and the directories left empty go.

The logs are the half that is asked for by name: `Utilities.EraseAddressLogLines(address)` over COM, `include_logs` on the REST body, `--logs` and `-IncludeLogs`. Every occurrence of the address in `hmailserver_*.log` and `ERROR_hmailserver_*.log` becomes `[erased]`, so the line says it was changed rather than pretending nothing was there; the count answered is the occurrences rewritten. A log past 256 MB is not rewritten in memory and is reported instead - log retention is the answer for it. The Control Panel's account dialog does both halves in one go from "Erase traces...".

## What is tested

`API/RestApiPersonExport.cs` (Windows): the archive's entries and counts over COM, the same archive downloaded by the operator and by the person, the key refused, and an erasure reaching the other person's address book, the first-contact memory and the log. `API/RestApiArchive.cs`: the held copy that survives the sweep. `Infrastructure/PrivacyTools.cs`: the `.eml` export and the stores the erasure has always reached. The Deck's harness drives the row's link and the confirmation; the webmail's harness finds the card's link.
