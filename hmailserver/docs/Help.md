Help where the question is
==========================

Every page of the three administration surfaces - the Windows Control Panel,
the browser Control Deck and the webmail - has a help button that opens what
the documentation says about that page, beside the page rather than instead
of it, and the same text is in the binary, so a server with no route to the
Internet is not a server with no documentation. The Ctrl+K palette matches a
page's purpose sentence to find the page; this is the other direction, from
the page to its text.

Where the text comes from
-------------------------

The text is this directory: `hmailserver/docs/*.md`, the operator
documentation, which many of the wiki's pages are written from. It is in the
repository, so CI holds it (a document the checkers can read is one that
cannot drift from the code it describes), and it can be embedded. The wiki
itself lives in another repository and cannot be built in, and its pages are
its own: some are a document's copy, some were written from several
documents, the manual's chapters were brought up against them, and every page
has a name of the wiki's own - `Backup-and-Restore`, not `Backup`. So the help
pane shows the documents, and *Open on the wiki* takes you to the wiki's page
made from the document showing, where the wiki has one and the server can
reach it ([The wiki link](#the-wiki-link)).

`build/generate-help-pages.py` embeds every document except `README.md` (the
index of the others) and `DeckParity.md` (a census a checker rewrites) as
`hmailserver/source/Server/Common/Util/HelpPagesData.cpp`: the text with LF
line endings, in pieces of at most four thousand characters, with a table of
the file name without its extension, the byte length of the text, the
document's level-one and level-two headings with the line each starts on, and
the wiki's page made from the document, or none, read from
`build/help-wiki-pages.txt` ([The wiki link](#the-wiki-link)).
The table ends with a build tag, the first sixteen hex digits of the SHA-256
of every page's text and wiki page, which the routes serve as the ETag. Today that is 71
documents and 1.1 MB of text, which is 1.2 MB of generated C++ and about
1.1 MB in the binary; `--check` fails when the unit, or the Control Panel's
copy of the wiki table below, is behind the documents or the table, and
GitLab CI's checkers job runs it on every push and merge request.

The Control Panel talks to the server over COM - what has no COM surface, the
Discovery page and the settings no COM property carries, it reaches over the REST
API while that listener is on - and must work with no REST listener, so it
carries its own copy: the same files, embedded as resources straight from this directory by its project file
(`ControlPanel.csproj`, `EmbeddedResource Include="..\..\..\docs\*.md"`, the
same two files excluded, each named `Help.<Name>.md`). Nothing is copied
into the Control Panel's tree, so its copy cannot be behind the documents.
The wiki table is the one thing written into it: `Services/HelpWikiPages.g.cs`,
by the same generator from the same file, which its `--check` holds.

The routes
----------

Both are unauthenticated, as `/deck-lang/` is: the text is the repository's
own public documentation and nothing of this server's but the wiki base URL,
so a credential would protect nothing and would keep the Deck's sign-in card
from offering help.

| Route | Answers |
|---|---|
| `GET /api/v1/help` | `{wiki, tag, pages:[{name, title, bytes, wiki_page, headings:[{text, level, line}]}]}`. `wiki` is the `HelpWikiBaseUrl` setting, empty when the administrator hid the link; `tag` is the build tag; each page's `name` is what the page route takes, its `title` its first heading, its `wiki_page` the project wiki's page made from the document (`Backup-and-Restore` for `Backup`), which a pane adds to `wiki` whatever its address, or empty when the project's wiki has none and there is no link, and its `headings` the level-one and level-two ones with the line each starts on, counted from 0. The wiki value rides in it, so it is answered `Cache-Control: no-cache` with an `ETag` of the build tag and a digest of that value, and `304` to an `If-None-Match` naming it: a change to the setting reaches a browser on its next page load, not a day later. |
| `GET /api/v1/help/{page}` | The page's markdown, `text/markdown; charset=utf-8`, with `Cache-Control: public, max-age=86400` and an `ETag` of the build tag and the page name; a request whose `If-None-Match` names that tag is answered `304`. `{page}` is a file name without its extension and without a path - `Backup` for `Backup.md` - matched without regard to case. A name carrying a dot, a separator or anything outside letters and digits, and a name that is not a page, are one and the same `404`, so a request that tries the path for a file learns nothing. |

The answers are the same bytes for every reader, so they are on the list of
what the listener compresses (see [ResponseCompression.md](ResponseCompression.md)),
once per distinct answer.

How a page is mapped
--------------------

One table per surface, in the surface's own code, each entry a page, a
document and an optional heading:

| Surface | Table | Keyed by |
|---|---|---|
| Control Panel | `Services/HelpMap.cs` | the navigation key of every page in `Services/NavigationMap.cs`, the nodes the palette indexes |
| Control Deck | `HELP_MAP` in `hmailserver/installation/WebAdmin/index.html` | every `data-view` section |
| Webmail | `HELP_MAP` in `Common/Util/Portal.js` | every route head the router distinguishes (`f` for a folder, `m` for a message, `compose`, `search`, `settings`, ...) |

The heading, where one is named, is a level-one or level-two heading of the
document, and the pane scrolls to it. A Control Panel page whose subject has no
document of its own maps to `HelpTarget.NoGuide`, and its pane shows the page's
title and purpose, says that no guide covers the page yet, and lists every
document; when a document is written for such a page, its entry is the one line
that changes. Until October 2026 such a page was mapped to "the nearest"
document, which on some twenty pages was one about something else - the groups
and public folders opened what a backup carries, the five certificate and TLS
pages one remote domain's outbound TLS, LDAP the webmail's OpenID Connect, the
virus pages what an encrypted message gets.

A Control Panel page's tabs can have documents of their own: `HelpMap.Tabs`,
keyed `page#Tab` by the navigation key and the tab's English header as the page
builds it (`logging#SNMP` is `Snmp.md`). While the help is open it follows the
tab as well as the page, the tab's document while that tab is the one showing
and the page's on the others; a tab whose subject the page's document does not
cover, and no document does, is `NoGuide` there. The documents no page maps to
are listed as reference-only in `HelpMap.cs` and are reachable from every pane's
*All documentation* list, as every document is.

The checker
-----------

`build/check-help-map.py` reads the three tables and holds them to the
documents: every page of each surface has an entry, every entry names a
document that exists, every heading named is in that document (by the same
parser the generator lists headings with), and no document is unreachable
from every surface without being listed as reference-only - and nothing
listed as reference-only is mapped. A tab's entry names a page of the Control
Panel and a tab that page builds, read from `ServerSettingsView`'s builder for
the page's section. And a reference-only document whose title or name is the
heading of a tab or a card of a Control Panel page fails: it is that page's help,
which the pane cannot reach from it, and its tab is mapped to it instead - but
for a document whose name only looks like a heading (`NOT_THE_PAGES`: the
project's repository mirror is not the server's), and one that the document of
the tab holding its card links (`REACHED_FROM`: account recovery, from the
webmail's sign-in document), which the checker holds to being mapped and to
carrying the link. `--self-test` is its negative control: the same check over a
copy of the maps with an entry naming a heading that is not there, another naming
a document that is not, a tab entry naming a tab its page does not build, and a
reference-only document named as a tab is named, each of which must fail.
GitLab CI's checkers job runs both, beside `generate-help-pages.py --check`.

It holds the wiki table too, `build/help-wiki-pages.txt`, through the
generator's own reader: every page of the wiki named once and in the form a
wiki page's name takes, every document listed under a page one the help
carries, and no document under two pages. A pipeline has no copy of the wiki,
whose repository is another; given one, `--wiki <clone of hmailserver.wiki>`
also holds the table to the wiki itself: its pages are exactly the table's,
and each names in its text - in its opening note, or in the section that
links it - every document listed under it. A page the wiki renames or drops,
and a document a page no longer covers, then fail there, not in a pane as
GitLab's "page does not exist". The table is held to the branch the wiki
serves, the one its remote's HEAD names, which GitLab renders the pages from,
and while that is another branch to `master` as well, where the wiki's edits
are pushed: on 15 October 2026 GitLab served `main`, eight commits behind
`master`, so a page added on `master` alone would have opened "page does not
exist" in every pane while a check of `master` passed. The remote is the
clone's `gitlab`, or `origin` in a plain clone, and it is asked with
`git ls-remote`, since a clone's own record of its remote's HEAD is written
when the clone is made and no fetch moves it; each branch must be in the
clone where the remote has it, so fetch first. `--ref <ref>` holds the table
to that one ref of the clone instead. The self-test's negative controls
include the table's: a document the help does not carry, a document under two
pages, a page the wiki does not have, a page of the wiki the table leaves out
and a page that does not name a document listed under it, each of which must
fail, and the link as it was made up to October 2026, the document's own
name, which the wiki the table describes refuses for every document but two.
They include the branch's too, over wikis the self-test makes with git: a
page on `master` alone while the wiki serves `main`, a remote whose HEAD has
moved since its clone was made, a clone behind its remote and a clone with no
remote to ask, each of which must fail, and a plain clone, which must pass.

The panes
---------

The same shape in all three, in the register of the tours
([ControlPanelDesign.md](ControlPanelDesign.md)): a right-hand pane, never a
window and never a dialog, that follows the page as the navigation changes,
with the document's headings as a list at the top, a search box over the
open document (the two browser pages mark every match and Enter moves to
the next; the Control Panel finds the next match), the text, and two links
at the foot - *Open on the wiki*, where the project's wiki has a page, and
*All documentation*, the second listing every document for opening in the
same pane. A link in a document to another document of the set opens that
document in the pane; a link to the web opens in a new tab. Its rendering is
a small converter of each surface's own - headings, paragraphs, emphasis,
inline code, code blocks, bulleted and numbered lists, links, block quotes,
rules and tables as a grid; nothing else, and anything else as its text -
so the text can carry nothing the page runs: the two browser pages build DOM
nodes and never assign markup, the Control Panel builds a `FlowDocument`.
The pane scrolls to a heading by the line it starts on (the map names it by
its markdown text, and a heading with inline markup renders without it), and
remembers what it last followed, so a page drawn again - the Deck's
dashboard poll, a save, a message opened from the list - moves nothing.

* **The Control Panel:** the `?` button in the window's command bar, and
  `F1`. The text comes from its own embedded copy over no route at all. The
  pane's width and whether it is open are kept in
  `HKCU\Software\hMailServer\ControlPanel` under `HelpPaneWidth` and
  `HelpPaneOpen`, beside the window bounds, the palette's history and the
  tour record. The pane follows the page's tab as well as the page
  (`MainWindow.HelpTabs.cs`). `Services/MarkdownBlocks.cs` is the parser (compiled into
  `ControlPanel.Core` and tested without WPF), `Services/MarkdownFlow.cs`
  the rendering into a `FlowDocument`, `Services/HelpPages.cs` the embedded
  copy, `Views/HelpPane.cs` the pane.
* **The Deck:** the `?` button in the command bar, and `F1` from anywhere;
  `Escape` closes it, except in the search box, whose own `Escape` comes
  first. The keyboard shortcuts
  list, which had the `?` button, keeps the `?` *key* and has a keyboard
  glyph for its button. The document is fetched from `/api/v1/help/{page}`
  and rendered by a copy of the webmail's renderer inline in the page (its
  policy allows no script but its own). Whether the pane is open is kept in
  `localStorage` under `hmsHelp`, beside the theme and the language.
* **The webmail:** the `?` button in the toolbar, `F1`, and *Help for this
  page* in the Ctrl+K palette; `Escape` closes it from the page or from
  inside the pane, and a field of the page keeps its own. The keyboard
  shortcuts button has a keyboard glyph for the same reason. The renderer
  is `/portal-markdown.js`
  (`Common/Util/PortalMarkdown.js`, served beside the S/MIME module and
  carried by the service worker as part of the shell). Whether the pane is
  open is kept in `localStorage` under `hmPortalHelp`.

The wiki link
-------------

`HelpWikiBaseUrl`, a server setting in `hm_settings` (never an ini key),
shipped as `https://gitlab.com/hmailserver/hmailserver/-/wikis/`, the
project's wiki. Up to 6.3.5 it was shipped as
`https://gitlab.com/Progressiverobot/hmailserver/-/wikis/`, the project's path
before it moved into the group `hmailserver` on 29 September 2026, and a
database set up then keeps that value through an upgrade, as it keeps any
setting; GitLab redirects the old address to the new one, page by page, so the
link still works, and setting the new value is the administrator's choice.

Each pane's *Open on the wiki* is that URL with the name of the project
wiki's page made from the document: `.../-/wikis/Backup-and-Restore` for
`Backup.md`, `.../-/wikis/The-Webmail` for the webmail's own documents. The
wiki names its pages its own way, and up to October 2026, while the link was
the URL with the document's own name, it opened GitLab's "page does not
exist" for every document but `Fuzzing.md` and `Migration.md`. A document no
page of the wiki was made from has no link at all - about half of them, the
settings pages' reasons and the first-hour checklist among them - since there
is nothing to open. The names are the project wiki's whatever the address:
an address of the administrator's own - a copy of the wiki on an intranet,
say - gets the same page names added, and a link only for the documents the
project's wiki has a page for, so a wiki of one's own has to name its pages
as the project's does; up to October 2026 such an address got the
document's own name. The pages come from one table,
`build/help-wiki-pages.txt`, which lists every page of the wiki with the
documents each was made from:

* a page that is a document's copy, whose opening says so ("Source of truth:
  `hmailserver/docs/X.md`, kept in the repository and copied here");
* a page written from documents, whose opening names them, or for *The
  Webmail* the documents its sections link;
* the manual's chapter brought up against a document (*Backup and Restore*,
  against `Backup.md`).

A document a page names for a part of its own subject, or points at, is not
that page's: `RemoteDomains.md` on *Encryption for People*, which takes its
gateway encryption, or `ActiveActive.md` on the *High Availability Runbook*.
Where two pages were made from one document, its copy is the one, and
otherwise the page about the document's own subject: `Upgrading.md` opens the
*Upgrading* guide, its copy, not the chapter *Upgrading hMailServer*.
`generate-help-pages.py` writes the table into the
server, where `GET /api/v1/help` gives each document's page as its
`wiki_page` to the Deck and the webmail, and into the Control Panel as
`Services/HelpWikiPages.g.cs`, so that it adds the wiki's names to the base of
a server of any version. When the wiki gains, loses or renames a page, the
table changes with it: `check-help-map.py --wiki <clone>` says which lines no
longer hold, and the generator writes both copies again.

The setting is `help_wiki_base_url` on `GET` and `PUT /api/v1/settings`, so it is on the Deck's Settings page; on the Control
Panel's Advanced page under General; and `Settings.HelpWikiBaseUrl` over COM.
A value that is not empty and not an absolute `http://` or `https://` URL is
refused in the same words on every door; empty hides the link everywhere.
The two browser pages read it from `GET /api/v1/help` once per page load,
the Control Panel over COM at sign-in, and the Control Panel keeps only a
value that is an absolute `http://` or `https://` URL before it hands one
to the shell - a row written any other way is not the server's to have
checked.

What is thinner than the row asks
---------------------------------

* The text is the repository's documentation, not the wiki's own pages. The
  wiki cannot be built in; the same text can, and *Open on the wiki* is the
  bridge - for the documents the wiki made a page from, about half of them.
  The others have no wiki link until the wiki has a page made from them.
* There is no search across all documents in the pane - only within the open
  one. The Ctrl+K palette still finds pages by their purpose sentence, and
  *All documentation* lists every document by its title.
* Some twenty Control Panel pages have no document of their own yet, and say so
  in the pane, said above.
* The first-run walk gains no stop for the help button; the button is in the
  command bar of every page, where the walk's later stops already point.

What is tested
--------------

* `build/check-help-map.py`, with `--self-test` as its negative control, and
  `build/generate-help-pages.py --check`, both in GitLab CI's checkers job;
  `check-help-map.py --wiki` against a fetched clone of the wiki, at the
  branch it serves (and `master`), by hand, whenever the wiki or the table
  changes.
* `RegressionTests/API/RestApiHelp.cs`: the list answers every document with
  its headings, as the repository's files have them, and with the wiki page
  `build/help-wiki-pages.txt` gives it, or none; a page answers its
  markdown byte for byte with the file in the tree, as `text/markdown` with
  the cache headers and the ETag, and `304` to a request carrying it; a path,
  a dot-dot, an unknown name and an extension are `404`; no credential is
  asked; the wiki setting round-trips over REST and COM, an empty one is
  allowed, and a value that is not a URL is refused on both doors with
  nothing changed.
* `ControlPanel.Tests`: the markdown parser over a fixed document (headings
  of both kinds, a list, a code block, a table, a link) and over every document
  of the set, the map's lookup for pages and for tabs, its coverage of every
  navigation page, every tab entry against the tabs its page builds, the pages
  with no guide, the pane's heading scroll target, the embedded copy against the
  directory (the names, the two exclusions, the bytes), the wiki URL rule, and
  the wiki link: the project wiki's page for a document, never its name, an
  address of the administrator's own given the same names, no link for a
  document the project's wiki has no page for, every document held to a page
  of the table's or to none, the program's copy of the table to the file, and
  the texts that describe the setting - the Advanced page's sentence, the COM
  property's helpstrings, the REST row and "The wiki link" here - to saying
  so.
* `build/deck-script-test.js` and `build/portal-script-test.js`: the button,
  the fetch path, the rendering of a heading and a list, the map lookup for
  three views, and *Open on the wiki* as the wiki's page for the document
  (`Backup-and-Restore`, `The-Webmail`), or no link for a document it has none
  for.
