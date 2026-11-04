# Accessibility

hMailServer is a mail server that people use through screens: the webmail, the browser Control Deck, the desktop
Control Panel, the installer and the command line. Email is how many people reach work, money and each other, so
the way someone uses a computer should not decide whether they can read their mail or run their server.

**Have you hit a barrier? [Here is how to report it](#how-to-report-a-barrier).** You can open an issue on GitLab or
GitHub, send an email with no account at all, or post on the forum. You do not have to tell us about any disability
or send a screenshot.

This file says what the project aims at, what has been checked and how, what is known not to work, and what happens
when you report something. It covers what this repository ships. It does not cover GitLab's or GitHub's own pages,
the forum's software, or the mail client you connect to the server.

## What we aim at

For the browser pages, the aim is level AA of the Web Content Accessibility Guidelines, WCAG 2.2. On the desktop the
aims are the same: everything can be done from the keyboard, every control has a name a screen reader can read, and
the screens stay usable in high contrast, with large text and in a narrow window.

This is a goal and not a claim. Nobody has carried out a conformance evaluation of any part of hMailServer, though some
criteria have been measured one at a time, so it does not claim to conform to WCAG or to any other accessibility
standard, and the limitations below show where it falls short.

## What works today

Each part says when it was last reviewed and how. A part of the product changes after its review; where a review was
not repeated, this says so.

### The webmail

**Last reviewed 27 September 2026.** Sixty views were walked at desktop and phone sizes, in light and dark, by keyboard
alone and through the accessibility tree in a browser. The webmail was then heard through NVDA 2026.2 in Chromium 154,
in English and German, by reading what NVDA said from its log on a test mailbox, and the defects found were fixed at
the time. The page has changed since, in 18 commits up to 4 November 2026, and that pass has not been repeated.

The webmail has a skip link, landmarks and one main heading for each view. Focus moves to the view's title when the
address changes. Dialogs take focus and give it back. The message list is one tab stop with a full name for each row.
One live region for the whole page says what has changed, and each form has its own status line. Pressing `?` lists the
keyboard shortcuts and `Ctrl+K` opens a command palette. It has a visible focus ring, light and dark themes that can
follow the system, a rule that switches off animation when the system asks for less motion, and icon buttons, buttons
and navigation rows of at least 44 pixels on a narrow screen. It has 23 translations beside English, and Arabic, Hebrew
and Persian are laid out right to left. Signing in asks for no CAPTCHA.

### The pages a link leads to

Some pages open from a link, for people who may have no mailbox on the server: a password-protected message, a
confidential message, a shared file's password, joining or leaving a mailing list, account recovery, the actions in a
held-mail digest, and the sign-in link of a mailbox migration.

**No accessibility review is on record:** the September pass of the webmail did not walk them, and nobody has run a
screen reader on them. Their code has a language, a main landmark, one main heading, and a label on each field but one.
Those with a form have a visible focus ring. Their limitations are below.

### The Control Deck

**No accessibility review is on record:** nobody has walked it with a keyboard alone or a screen reader. Its code has a
skip link, landmarks, named command buttons, a visible focus ring, and notices that carry a word and a mark as well as
a colour. Pressing `?` lists its keys: `/` searches the view, and the arrows and Enter move through a table's rows and
open one. Its limitations are below.

### The Control Panel

**Last reviewed in October 2026.** From 8 to 12 October a review of the Control Panel worked through its keyboard paths,
names, live regions and text size, and checked them in running windows on a Windows test machine: the names of 478
controls on nineteen pages were read back through their automation peers, F6, Ctrl+F, F5 and Escape were driven in a
window, and pages were rendered at 150% text size in three languages. The pass before it, on 5 and 6 September 2026,
read every view in the code and checked names, access keys, focus moves and headings through the UI Automation tree,
which is what a screen reader reads. No screen reader has been run on it. High Contrast was checked with a switch that
simulates it, not on a real High Contrast desktop. The Control Panel has changed since, in 84 commits up to 4 November
2026, and that review has not been repeated.

The Control Panel is built to expose names that begin with the visible caption, Alt access keys on its captions, F6 to
move between the regions of the window, and announcements through live regions. Page titles are headings. A status is
shown by colour, shape and word together, and charts have a legend and a data table. It is built to follow Windows' High
Contrast and Text size settings, and has 17 translations beside English.

### The installer and the older Windows tools

**No accessibility review is on record** for the Inno Setup installer or for DBSetup, DBSetupQuick, DBUpdater,
DataDirectorySynchronizer and ImportTool. The wizards of DBSetup, DataDirectorySynchronizer and ImportTool had their tab
order fixed on 24 October 2026, so that Tab follows the reading order and each label comes before its field, and a test
holds that.

### The command line

**Read in the code on 4 November 2026; not tried with a screen reader or a braille display.** `hmctl` prints no colour
and no animation, prints plain tables, and has a `--json` mode. The PowerShell module returns objects, which your own
shell formats.

### The documents

**Scanned by script on 4 November 2026, the wiki as it stood on 2 November.** Every image in the repository and the wiki
has alternative text, no link is named "here", and the documents in the repository do not skip heading levels.

## Supported environments

The Control Panel, the installer and the older tools are documented for 64-bit Windows 10 1607 or Windows Server 2016
and later. For a browser page as a whole no browser is documented as supported, though [the OpenPGP page's browser
list](hmailserver/docs/OpenPGP.md#which-browsers) names what one feature needs, and no screen reader or other assistive
technology is documented as supported. What follows is what has actually been tried. Anything else is untested rather
than unsupported, and a report about it is as welcome as any other.

- **Webmail:** NVDA 2026.2 with Chromium 154 on Windows, in English and German. A phone-sized window of 390 by 844
  pixels in Chromium, which is not a real phone: no phone screen reader has been used.
- **Control Panel:** the UI Automation tree on a Windows development machine, whose Windows build was not recorded, and
  in October 2026 automation peers and renders in offscreen windows on a Windows 11 Pro test machine.

## Known limitations

No part of this list is a decision that the barrier is acceptable.

### Everywhere

- **Screen readers.** NVDA on the webmail is the only pass on record. Nobody has run a screen reader on the pages a link
  leads to, the Control Deck, the Control Panel, the installer, the older Windows tools or the command line. JAWS,
  VoiceOver, TalkBack, Narrator, Orca and voice control have not been tried on anything.
- **No automated accessibility scan runs in CI.** The page tests run each page's script against a small stand-in for a
  browser. They hold markup and focus, and cannot see contrast, reflow, target size or what a screen reader says. Scans
  in a real browser are a plan for a later release, and that work has not started.

### The webmail

- **Short-lived messages.** A message at the foot of the page goes after 8 seconds, and hovering or focusing it does not
  keep it. If it offers Undo, pressing `z` presses that Undo. How long you have to undo a send is a setting of your own.
- **One-key shortcuts.** Keys such as `e`, `#` and `!` act on a message at once, and no setting switches them off or
  changes them, which can get in the way of voice control. They do nothing while the cursor is in a text field. Moving
  a message shows an Undo for 8 seconds, but `#` on a message already in Trash, or in a folder someone shares with you,
  deletes it at once, with no Undo.
- **Signing out.** By default a session ends after 30 minutes idle or 12 hours in all, with no warning before it does;
  the next action asks you to sign in again. Administrators can set other limits.
- **Dragging in the calendar.** The handle that changes when an event ends works with a mouse alone and has no name of
  its own. The event's editor has date and time fields that do the same. Messages can be moved with "Move to" in the
  palette instead of dragging.
- **Text size.** Text sizes are fixed in pixels, so a browser's default font size setting has no effect. The page does
  not block zoom, but nobody has tested it at 200% or 400%, or in a 320-pixel window.
- **Tick boxes.** A tick box is 18 pixels in a desktop-sized window.
- **High contrast.** Beyond one rule for the focus ring on fields, the webmail relies on the browser's own handling of
  forced colours, and no one has looked at the result.
- **Label colours.** Label text is white on a colour you choose, so contrast depends on that choice. A darker colour
  reads better.
- **Heard and left in the September pass.** NVDA says a dialog's name and form twice as focus enters it, and reads a colour
  field's value and not its name. Sentences from the server, such as "not an e-mail address", are in English on a
  translated page, and the documentation in the help pane is in English. Right-to-left languages have not been heard
  through a screen reader.

### The pages a link leads to

- The password field of a shared file has no label. The sentence just before it says that the sender set a password,
  and the field takes the keyboard when the page opens.
- These pages are in English only, whatever language the webmail is set to.

### The Control Deck

If these stop you, the Control Panel on Windows and `hmctl` on any platform are other routes to most of the same
administration.

- **Most form fields have no label a screen reader can read.** On the Settings, domain, account, alias, list and group
  pages the caption beside a field is not tied to it, and many other fields have a placeholder or nothing. The sign-in
  fields have placeholders and no labels. An error shown under a form is not announced, nor is a failed sign-in.
- Short messages are not announced, and go after 2.6 seconds.
- The current page is not exposed to a screen reader. Changing view moves no focus, announces nothing and leaves the
  window title as it was. The menu's symbols may be read out by name.
- A chart has only a short label, and two sets of bars differ by colour alone: Incoming and Outgoing on Reports, and
  green for passed and red for failed in a domain's DMARC reports. On Reports the table under it has the same values.
  On Metrics and in the DMARC reports the table gives totals, and the figures for each day or sample cannot be reached.
- The dashboard's figures change every 3 seconds, and no setting stops them.
- Icon buttons are 32 pixels with no larger size for phones, nobody has tested it on a phone, and it has no right-to-left
  layout.
- A session ends with no warning, 30 minutes idle by default. Text sizes are fixed in pixels, as in the webmail.
- Beyond one rule for the focus ring on fields, it relies on the browser's own handling of forced colours, and no one has
  looked at the result.
- The rule for less motion covers fields alone: a view's rise as it opens and a short message's slide take no notice of
  the system asking for less motion.
- A tick box is 18 pixels in a desktop-sized window.

### The Control Panel

- Card titles inside a page are not all headings: a settings section's is, a plain card's is not.
- The window cannot be made narrower than 760 pixels or shorter than 520. With the help pane open in a narrow window a
  few controls keep their own width: a filter can run off the edge of its toolbar and the IP ranges page's permission
  boxes are cut, though every list and form can still be scrolled to.
- A row whose tick box is its own caption is a group with no name, though the tick box has one.
- A field's error has been announced since 8 October 2026. Other messages in dialogs have not been audited.
- Messages that appear over the page go by themselves, after 3 seconds, 8 for a failure and 12 with Try again, and are
  not announced. Some failures, such as a save or a page that could not load, are said nowhere else.
- A text box's access key may not reach a screen reader, though the key itself works.
- A page already open keeps its old text size when Windows' Text size setting changes. Restarting the Control Panel
  applies it everywhere. Sizes above 150% have not been looked at.
- The dashboard refreshes on timers, and no setting was found that stops them.
- It has no right-to-left layout.

### The installer and the older Windows tools

- The installer is in English only, and nobody has reviewed its custom pages. Its silent install switches, listed in
  [the README's unattended install notes](README.md#unattended-install), are a way round the wizard.
- The older tools set no accessible names of their own and rely on each label coming before its field. High contrast,
  text size and display scaling have not been examined for them.

### The documents

- In the wiki, 24 pages jump from a level 1 heading to level 3.
- Many tables in the roadmaps show a status as a symbol alone, in a column with no header.

## How to report a barrier

Use whichever suits you. All of them reach the same people.

- **On GitLab**, with the [Accessibility template](https://gitlab.com/hmailserver/hmailserver/-/issues/new?issuable_template=Accessibility).
  This is where the project tracks its work. It needs a GitLab account, and the issue is public.
- **On GitHub**, with the [accessibility form](https://github.com/RainbowWizard-coder/hMailServer/issues/new?template=accessibility.yml).
  It needs a GitHub account, and the issue is public.
- **By email**, to the project's Service Desk address,
  [contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com](mailto:contact-project+hmailserver-hmailserver-86730559-issue-@incoming.gitlab.com).
  No account is needed. GitLab files your message as a confidential issue that only the maintainers can see, and their
  replies reach you by email.
- **On the forum**, in [General support](https://www.hmailserver.co.uk/category/8/general-support). It needs a forum
  account, and the post is public.

These things help. The GitHub form and the GitLab template need only the first of them, and by email or on the forum
nothing is required:

- what you were trying to do, and what happened instead;
- which part you were using: the webmail, a page a link led to, the Control Deck, the Control Panel, the installer, a
  tool, the command line or a document;
- the assistive technology you use and its version, your browser and your system;
- how much it stops you, and anything that gets you round it.

You do not have to disclose a disability or explain why you use what you use, and you do not have to attach a screenshot.
A public report should not hold a password, a key or a real address.

A security problem does not belong in a public issue or on the forum. [SECURITY.md](SECURITY.md) gives the private
routes, the same email address among them.

## What happens next

Every report is read. This is a small project, and as [SUPPORT.md](SUPPORT.md) says there is no response-time
guarantee, so no deadline is promised here either. What is promised:

- **A barrier that stops someone doing a core task comes first.** The core tasks are signing in, reading and sending
  mail, installing or upgrading, and adding a domain or an account.
- **You get a straight answer**, with a workaround where one exists. If something will not be fixed soon, you are told
  so, and an open problem stays open rather than being quietly closed.
- **Barriers that are confirmed are added to the list above** when this file is next reviewed.

Reports are sorted by how much they stop you, in the same four words the forms use:

- **Blocks me:** I cannot do the task at all.
- **Serious:** I can do it only with great effort, or with someone's help.
- **Moderate:** I can do it, but it is slow or confusing.
- **Minor:** it gets in my way, but I can carry on.

## If you change a screen

The checks below hold part of what is above, and they run for you or for a maintainer before a change lands
([CONTRIBUTING.md](CONTRIBUTING.md#the-checkers) says when).

- **The webmail:** `build/check-portal-script.py`, which holds the list, dialogs, announcements and focus to what the
  NVDA pass settled, and `build/check-portal-languages.py`.
- **The Control Deck:** `build/check-deck-script.py`, which holds those keys, the focus ring, and the sort order and check
  states of the quarantine table, and `build/check-deck-languages.py`.
- **The Control Panel:** the `ControlPanel.Tests` project, among them `AccessibleNamesTests`, `AccessKeysTests`,
  `LabelInNameTests`, `LiveRegionTests`, `GridKeysTests`, `ContrastTokensTests`, `ColourVisionTests`,
  `ChartDataTableTests` and `TextScaleTests`, and `build/check-mnemonics.py`, `build/check-localisation.py` and
  `build/check-catalogues.py`.
- **The wizards:** `ReadingOrderTests` in the `DBSetup.Tests` project.

They cannot see how a screen looks or sounds. So check these by hand, and say in your merge request what you did:

- **Keyboard alone:** reach and use everything you changed, see where focus is, and leave it again with Escape.
- **Screen reader spot check:** listen to what you changed with NVDA, or with Narrator, which comes with Windows.
- **High contrast:** a Windows contrast theme, or forced colours in the browser.
- **Large text:** 200% page zoom in a browser; Windows' Text size setting for the Control Panel.
- **Narrow window:** 320 CSS pixels wide for a browser page, with no sideways scrolling of the page.

## This file

Last reviewed on 4 November 2026.
