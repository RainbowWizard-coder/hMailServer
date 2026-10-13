#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a translation can lose that a per-string check cannot see.

check-localisation.py judges one string against its English: the placeholders,
the Alt key, whether it is there at all. Three properties are invisible from
there, because seeing them means reading the string against the rest of the
catalogue, or against what the server does at run time:

  literals  Some English in a caption is not language. The server logs its
            accept-pipeline stages under fixed names - spam-protection,
            script/save - and the stalled-mail page lists them so that an
            administrator can match the page against the log; a translated
            copy matches nothing. The same holds for setting and INI names,
            file names, %TOKEN% and <token>, INBOX, the underscore labels of
            the DNS names a reader publishes (_domainkey, _mta-sts), and the
            syntax a user types verbatim. These have to survive translation
            unchanged.

  pages     Where a caption says "on the Transport security page", the
            translation has to name that page the way that page's own title
            is translated. Naming it anything else sends the reader looking
            for a page that is not in the navigation. Only a reference that
            actually points at a page is checked: "the IP ranges page", not
            the ordinary words "no IP ranges are configured", which a
            translation is right to render as ordinary words.

  references
            The English names pages and tabs that exist, by their titles: "the
            X page" ends in a title the navigation map gives, "the X tab" in a
            tab header a view declares, and what it says something is "under"
            is a page or a group of the navigation. "The backup log on the
            Logs page", "the Status page", the account dialog's "Account tab"
            and "a page of their own under Anti-spam" sent the reader to names
            that are not on screen (review findings F048, F280, F126).

  numbers   Every RFC number, port, size and count the English states is
            stated by the translation too. Digit-group separators differ
            between languages, so the comparison is on the digits alone -
            210,000 and 210 000 are the same number. A dropped digit is the
            kind of defect that reads perfectly well.

  untranslated
            In a catalogue written in another script - Japanese, Chinese,
            Russian, Ukrainian - a value equal to its English key is almost
            always a word nobody translated: every Japanese notice read
            "Critical" before a Japanese sentence, and the rule card's heading
            read "THEN", until 6.3.7 (review findings F006, F114, F177). A key
            made of ordinary words is reported there when its value is the
            English itself, unless it is a name every language keeps
            (SAME_IN_EVERY_SCRIPT). An acronym in capitals is a name, but for
            the few English words written in capitals (CAPITAL_WORDS).

  quotations
            A German, Czech or Polish quotation opens with the low mark and is
            closed by its language's own high one (QUOTES) before the next
            opens: 83 German texts closed one with the typewriter's straight
            quote, „Prüfen" beside „Sicherung jetzt starten“ (review finding
            F038).

  punctuation
            One dash, one ellipsis and no padding (review findings F037, F299):
            the English writes the spaced hyphen, the ellipsis character, and
            three dots only for omitted literal text; a translation writes its
            catalogue's dash (OWN_DASH), a range with the hyphen, and three
            dots only where its English has them; and no text pads itself with
            spaces to line up with the next (PADDING) - "Type:       TXT"
            under "Host/Name:  {0}" lined up in no translation.

The Czech, Polish, Russian and Ukrainian plural forms ("<English>|few", which
Loc.Plural reads for counts such as 2 to 4) are held to the English they stand in for, by the
same rules.

Exits 1 with the language, the catalogue key and what is wrong.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESOURCES = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Resources")

# The pages a caption points at. Each is a catalogue key in its own right, so
# the translated title is looked up rather than written here.
PAGE_TITLES = [
   "Anti-spam settings", "Anti-virus settings", "Blocked attachments",
   "Delivery of e-mail", "Transport security", "TCP/IP ports", "IP ranges",
   "SSL certificates", "Live logs", "Setup checklist", "Event scripts",
   "Web services & autoconfiguration", "Auto-ban", "Logging", "Domains",
   "API & monitoring", "Authentication", "App passwords", "Advanced",
   "REST API keys", "Message trace", "Quarantine", "Delivery queue",
   "Public folders", "External accounts", "Routes", "Rules", "Diagnostics",
   "Backup", "Groups",
]


def navigation_titles():
   """Every page title the navigation map gives, so that a page renamed or added is
   held to its references too: the list above named thirty, and the German "on the
   Suppression list page" kept the old title when the page was renamed (review
   finding F284)."""
   path = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Services", "NavigationMap.cs")
   try:
      with open(path, encoding="utf-8-sig") as handle:
         source = handle.read()
   except OSError:
      return []
   return re.findall(r'\bPage\("[a-z0-9]+",\s*N\("((?:[^"\\]|\\.)*)"\)', source)


PAGE_TITLES = sorted(set(PAGE_TITLES) | set(navigation_titles()))


def navigation_groups():
   """Every group title the navigation map gives - the sidebar's headings, Spam & virus
   filtering, Sign-in & access - which a text may name as the place a page is under."""
   path = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Services", "NavigationMap.cs")
   try:
      with open(path, encoding="utf-8-sig") as handle:
         source = handle.read()
   except OSError:
      return []
   return re.findall(r'\bGroup\("[A-Za-z0-9]+",\s*N\("((?:[^"\\]|\\.)*)"\)', source)


def tab_headers():
   """Every tab header the Control Panel's views declare - Tab(L("...")) on the settings
   pages, Header = L("...") on the dialogs' TabItems, Header="{loc:L '...'}" in XAML -
   with the Alt-key underscore taken out, so that "the X tab" is held to a tab that
   exists: the account dialog's hint sent the reader to an "Account tab" where the
   password is on General, and the directory pages to its "Directory tab", which is
   Windows directory (review finding F126)."""
   views = os.path.join(ROOT, "hmailserver", "source", "Tools", "ControlPanel", "Views")
   found = set()
   # Only a TabItem's header: a grid column's Header = L("Account") is not a tab.
   patterns = (r'\bTab\(L\("((?:[^"\\]|\\.)*)"\)', r'\bnew TabItem\s*\{\s*Header\s*=\s*L\("((?:[^"\\]|\\.)*)"\)',
               r"<TabItem\b[^>]*\bHeader=\"\{loc:L '((?:[^'\\]|\\.)*)'\}\"")
   for dirpath, _dirs, files in os.walk(views):
      for name in files:
         if not name.endswith((".cs", ".xaml")):
            continue
         try:
            with open(os.path.join(dirpath, name), encoding="utf-8-sig") as handle:
               source = handle.read()
         except OSError:
            continue
         for pattern in patterns:
            found.update(header.replace("_", "") for header in re.findall(pattern, source))
   return found


# A page or a tab the English names: "the Live logs page", "on the Anti-spam settings
# page", "the Protocols page's SMTP tab", "its Windows directory tab". The name is a
# capitalised run of at most five words (four for a tab) before the word, which ends in
# a title the navigation map gives (or a tab header a view declares); "the Logs page",
# "the Status page" and "the Anti-spam page" named pages that do not exist (review
# findings F048, F280). The Control Deck's pages are its own and are not checked.
PAGE_REFERENCE = re.compile(r"\b[Tt]he ([A-Z][\w&'/().,-]*(?: [\w&'/().,-]+){0,4}) page(?![\w-])")
TAB_REFERENCE = re.compile(r"(?:\b[Tt]he|\bits|'s) ([A-Z][\w&'/().,-]*(?: [\w&'/().,-]+){0,3}) tab(?![\w-])")
NOT_A_CONTROL_PANEL_PAGE = ("Control Deck", "Deck's")

# A place the English says something is under: "Allowed and blocked domains have a page
# of their own under Anti-spam" sent the reader to a group the sidebar does not have -
# the page is under Spam filtering (review finding F280, found re-checking
# 4a53ffb35, whose page rule reads only "the X page"). A capitalised run of at most five
# words after "under" that ends its clause has to be a page title or a group title the
# navigation map gives. The archive's Inbound and Error folders and the registry's HKLM
# are names of their own and not places in the Control Panel.
UNDER_REFERENCE = re.compile(r"\bunder ([A-Z][\w&'/-]*(?: [\w&'/-]+){0,4})(?=[.,;:]|$)")
NOT_A_NAVIGATION_PLACE = ("HKLM", "Inbound", "Error")


def reference_findings(english, titles, tabs, groups=()):
   """Findings for the English texts that name a page or a tab the Control Panel does not have."""
   def known(name, names):
      return any(name == known_name or name.endswith(" " + known_name) for known_name in names)

   findings = []
   for text in sorted(english):
      for match in UNDER_REFERENCE.finditer(text):
         name = match.group(1)
         if name in titles or name in groups or name in NOT_A_NAVIGATION_PLACE:
            continue
         findings.append((text, "says it is under %r, which is not a page or a group of the navigation" % name))
      for match in PAGE_REFERENCE.finditer(text):
         name = match.group(1)
         if any(word in name for word in NOT_A_CONTROL_PANEL_PAGE) or known(name, titles):
            continue
         findings.append((text, "names a page %r the navigation does not have; use the page's title" % name))
      for match in TAB_REFERENCE.finditer(text):
         name = match.group(1)
         if known(name, tabs):
            continue
         findings.append((text, "names a tab %r no view declares; use the tab's header" % name))
   return findings

# What the server writes, and what the user types back verbatim.
LITERALS = [
   "spam-protection", "done spam-protection", "script/save", "header-parsing",
   "message-modifications", "AEAD-ONLY", ":hours", "HMBackup",
   "hm_metricsamples", "v=DMARC1", "v=spf1", "v=STSv1", "v=TLSRPTv1",
   "multipart/mixed", "multipart/alternative", "rsa-sha1", "rsa-sha256",
   "objectCategory", "sAMAccountName", "strongAuthRequired", "/api/v1",
   "/metrics", "/livez", "/readyz", "/healthz", "/portal", ".well-known",
   "127.0.0.1", "INBOX", "identd",
   # The breach check's public corpus, and the token its endpoint setting reads.
   "api.pwnedpasswords.com", "{prefix}",
]

# An INI or setting name: two or more CamelCase words run together.
SETTING = re.compile(r"\b(?:[A-Z][a-z0-9]+){2,}\b")
FILENAME = re.compile(r"\b[\w.\\-]+\.(?:INI|ini|exe|log|xml|dll|py|ps1|md|resx)\b")
PERCENT = re.compile(r"%[A-Za-z]+%")
ANGLED = re.compile(r"<[a-z][a-z0-9\\_.-]*>")
# The underscore labels of the DNS names a text tells the reader to publish or look up:
# the Japanese text of DKIM signing's item named <selector>.domainkey.<domain>, its
# underscore read as an Alt key and moved to the end as "(_D)", a record no receiver
# looks up (found rendering the setup checklist for review finding F040).
DNS_LABEL = re.compile(r"(?<![A-Za-z0-9])_(?:domainkey|dmarc|mta-sts|smtp|tls|tcp|submissions?|imaps?|pop3s?|autodiscover|caldavs?|carddavs?|report|bimi)(?![A-Za-z0-9-])")
PLACEHOLDER = re.compile(r"\{\d+(?:[:,][^}]*)?\}")
# Digits, optionally in groups of three. "451. 0" is two numbers, not 4510.
NUMBER = re.compile(r"\d+(?:[ ,. ]\d{3})*")

# CamelCase that is a product or a class rather than a setting a user types,
# and that a translation may reasonably render in its own words.
SETTING_SKIP = {"ControlPanel", "OpenSSL", "OpenTelemetry", "SpamAssassin",
                "ClamWin", "PowerShell", "LiveCharts", "SkiaSharp", "MimeBody",
                "MessageData", "ManageSieve", "LocalSystem", "ChaCha",
                "Postfix", "HAProxy", "AutoDiscover", "AutoConfig",
                "MailServer", "ProgressiveRobot"}


# The catalogues in a script other than Latin, where an English word left as it
# is stands out as untranslated.
# The suffix of a catalogue key that holds a plural form of the English key
# before it (Loc.Plural; check-localisation.py requires them).
FEW = "|few"

OTHER_SCRIPTS = {"ja", "zh-Hans", "ru", "uk"}

# Keys of ordinary words that are names in every language and script: products,
# formats, the DKIM canonicalisation names, an IMAP right, the Control Panel's
# own name. A new key belongs here only when it is a name. The keys the keyboard
# shortcuts name - Del, Enter, Esc and Ins - are printed so on the keycaps of a
# Japanese, Chinese, Russian and Ukrainian keyboard, and are named so in the
# shortcut lists of those languages' own software.
SAME_IN_EVERY_SCRIPT = {
   "Azure Blob Storage", "NCSA Common Log Format", "Control Panel", "hMailServer Control Panel", "Control Deck",
   "Relaxed", "Simple", "Sieve", "Expunge", "Web  ", "OK",
   "Del", "Enter", "Esc", "Ins",
}

# English words written in capitals, which are words and not acronyms: the rule
# card's IF and THEN.
CAPITAL_WORDS = {"IF", "THEN", "AND", "OR", "NOT", "ELSE"}

WORDS = re.compile(r"[A-Za-z]+(?:[ -][A-Za-z]+)*")

# The quotation marks of the catalogues that open a quotation low: German and Czech
# close it with the high reversed mark, Polish with the high one. A German quotation
# closed by the typewriter's straight quote - „Prüfen", „wird geprüft", „v=DMARC1",
# the palette's „Spam stoppen" - is two styles in one pair (review finding F038): 83
# German texts closed so until 6.3.7.
QUOTES = {"de": ("„", "“"), "cs": ("„", "“"), "pl": ("„", "”")}


# One dash and one ellipsis (review findings F037, F046, F090, F091, F134, F299; the
# owner's choice of 9 October 2026: the spaced hyphen). The English writes its dash as
# the spaced hyphen, " - ", and no em or en dash at all: 479 texts did, 45 the em dash
# and 3 the en dash, sometimes two kinds in one sentence. A translation writes the
# hyphen too, but for the three catalogues whose typography and whose bulk write a dash
# of their own - Russian and Ukrainian the em dash, Czech the en dash - which may write
# that dash or the hyphen, never both in one text and never the other dash; a range
# between two numbers is the hyphen everywhere, as English writes it. A caption that
# opens a dialog, progress text and the rest of a list end in the ellipsis character;
# three dots stand only for omitted literal text - p=..., ip=..., https://host/...,
# Bearer ..., hmapi_... - which the reader types or reads as it is, and a translation
# writes three dots only where its English does.
EM, EN, ELLIPSIS = "\u2014", "\u2013", "\u2026"
OWN_DASH = {"ru": EM, "uk": EM, "cs": EN}
LITERAL_DOTS = re.compile(r"(?:[=/_]|Bearer |; )\.\.\.")
RANGE_DASH = re.compile(r"(?<=[\d}])[\u2013\u2014](?=[\d{])")
# A text padded with spaces to line up with the next - a run of them after a colon,
# or before a dash or a bracket: "Type:       TXT" under "Host/Name:  {0}", "Version
# {0}  -  .NET {1}", "{0}  (not subscribed)" (review finding F299). No translation
# kept the alignment, its words being longer or shorter and a CJK character two
# columns wide; the layout aligns, and a text has one space there. Indentation after
# a line break, a list's "1.  " and the palette's spaced-out key hints are not padding.
PADDING = re.compile(r"(?<=[:\uff1a]) {2,}(?=\S)|(?<=\S) {2,}(?=[-(\u2013\u2014])")
PADDING_FINDING = "spaces padding it to line up (%r); one space, and let the layout align"


def padding_findings(text):
   """The runs of spaces a text uses to line itself up with another."""
   return [PADDING_FINDING % text[max(0, m.start() - 8):m.end() + 4] for m in PADDING.finditer(text)][:1]


def english_punctuation_findings(text):
   """The dashes, three dots and padding an English text should not have."""
   findings = []
   for char, name in ((EM, "em"), (EN, "en")):
      if char in text:
         findings.append("an %s dash; the Control Panel's dash is the spaced hyphen (\" - \")" % name)
   for match in re.finditer(r"\.\.\.", text):
      if not LITERAL_DOTS.search(text[max(0, match.start() - 7):match.end()]):
         findings.append("three dots; a caption that opens a dialog, progress text and the rest of a list end in "
                         "the ellipsis character, and three dots stand only for omitted literal text (p=...)")
         break
   findings += padding_findings(text)
   return findings


def dash_findings(tag, source, target):
   """The dashes and three dots a translation should not have."""
   findings = []
   own = OWN_DASH.get(tag)
   for match in RANGE_DASH.finditer(target):
      findings.append("the range %r is written with the hyphen, as the English writes it"
                      % target[max(0, match.start() - 4):match.end() + 4])
   bare = RANGE_DASH.sub("-", target)
   for char, name in ((EM, "em"), (EN, "en")):
      if char in bare and char != own:
         findings.append("an %s dash; this catalogue's dash is the spaced hyphen%s"
                         % (name, "" if own is None else " or the %s dash" % ("em" if own == EM else "en")))
   if own is not None and " - " in bare and (" " + own + " ") in bare:
      findings.append("both the hyphen and the %s dash as dashes in one text; use one" % ("em" if own == EM else "en"))
   if "..." in target and "..." not in source:
      findings.append("three dots where the English has none; the ellipsis character is " + ELLIPSIS)
   findings += padding_findings(target)
   return findings


def quotation_findings(tag, target):
   """What is wrong with the quotation marks of one translation: a quotation that its
   language's closing mark does not close before the next one opens or the text ends,
   and a closing mark that closes nothing."""
   marks = QUOTES.get(tag)
   if marks is None:
      return []
   opening, closing = marks
   findings = []
   open_at = None
   for index, char in enumerate(target):
      if char == opening:
         if open_at is not None:
            findings.append("the quotation at %r is not closed by %s before the next one opens"
                            % (target[open_at:open_at + 24], closing))
         open_at = index
      elif char == closing:
         if open_at is None:
            findings.append("%s at %r closes no quotation" % (closing, target[max(0, index - 20):index + 1]))
         open_at = None
   if open_at is not None:
      findings.append("the quotation at %r is not closed by %s" % (target[open_at:open_at + 24], closing))
   return findings


def untranslated(tag, source, target):
   """True when a catalogue of another script gives an English key of ordinary words back as it is."""
   if tag not in OTHER_SCRIPTS or target != source or source in SAME_IN_EVERY_SCRIPT:
      return False
   core = without_mnemonic(source).strip().strip(".:…")
   if not WORDS.fullmatch(core):
      return False
   words = re.split(r"[ -]", core)
   # An acronym or a name in capitals (SMTP, HELO, MTA-STS) is the same in every
   # script; a CamelCase word (ClamAV, OpenPGP) is a product.
   if all(w.isupper() and w not in CAPITAL_WORDS for w in words):
      return False
   if any(re.search(r"[a-z][A-Z]", w) for w in words):
      return False
   return True


def read_resx(path):
   """{key: value} for one catalogue; a missing file is an empty one."""
   if not os.path.exists(path):
      return {}
   out = {}
   for data in ET.parse(path).getroot().findall("data"):
      value = data.find("value")
      out[data.get("name")] = (value.text or "") if value is not None else ""
   return out


def without_mnemonic(text):
   """The caption as it is read, with the Alt-key underscore taken out.

   The underscore may land inside an identifier - "Timeo_utSeconds" is how a
   collision in that scope was resolved - and the setting name is still on
   screen, so the literal checks accept either form.
   """
   index = 0
   while index < len(text):
      if text[index] != "_":
         index += 1
         continue
      if index + 1 < len(text) and text[index + 1] == "_":
         index += 2
         continue
      return text[:index] + text[index + 1:]
   return text


def digits(text):
   return re.sub(r"[^0-9]", "", text)


def check(tag, catalogue, pages=None):
   """Findings for {English: translation}; pages, when given, is the whole catalogue
   the page titles are read from (the plural forms are checked on their own)."""
   pages = catalogue if pages is None else pages
   titles = {}
   for title in PAGE_TITLES:
      if title in pages and pages[title]:
         titles[title] = pages[title]

   findings = []
   for source, target in sorted(catalogue.items()):
      if not source or not target:
         continue

      plain = without_mnemonic(target)
      bare = PLACEHOLDER.sub(" ", source)

      if untranslated(tag, source, target):
         findings.append((source, "the value is the English itself; translate it, or add it to SAME_IN_EVERY_SCRIPT "
                                  "if it is a name every language keeps"))

      for problem in quotation_findings(tag, target):
         findings.append((source, problem))

      for problem in dash_findings(tag, source, target):
         findings.append((source, problem))

      wanted = {digits(m.group(0)) for m in NUMBER.finditer(bare)}
      have = {digits(m.group(0)) for m in NUMBER.finditer(PLACEHOLDER.sub(" ", target))}
      for number in sorted(w for w in wanted - have if len(w) >= 2):
         findings.append((source, "the number %s is not in the translation" % number))

      for literal in LITERALS:
         if literal in source and literal not in target and literal not in plain:
            findings.append((source, "%r is what the server writes; the translation changed it" % literal))

      for name in sorted(set(SETTING.findall(source)) - SETTING_SKIP):
         if name not in target and name not in plain:
            findings.append((source, "the setting name %r is not in the translation" % name))

      for pattern in (FILENAME, PERCENT, ANGLED):
         for match in sorted(set(pattern.findall(source))):
            if match not in target and match not in plain:
               findings.append((source, "%r is a literal; the translation changed it" % match))

      # A DNS name's label is held to the value as it is, not with an Alt key taken out.
      for match in sorted(set(DNS_LABEL.findall(source))):
         if match not in target:
            findings.append((source, "the DNS label %r is not in the translation" % match))

      if len(source) > 40:
         for title, translated in sorted(titles.items()):
            if source == title or translated in target:
               continue
            pointer = r"(?:the |on the |see the |Open )" + re.escape(title) + r"(?: page| tab)(?![\w-])"
            if re.search(pointer, source):
               findings.append((source, "names the %r page as something other than %r" % (title, translated)))

   return findings


def self_test():
   """The untranslated rule against values made to pass and to fail it: each failing
   case is a defect the Control Panel shipped."""
   failures = 0
   cases = {
      # (language, English, value): reported as untranslated
      ("ja", "Critical", "Critical"): True,
      ("ja", "Normal", "Normal"): True,
      ("ja", "THEN", "THEN"): True,
      ("ru", "_Delete selected", "_Delete selected"): True,
      ("ja", "Critical", "重大"): False,
      ("ja", "THEN", "アクション"): False,
      ("ja", "SMTP", "SMTP"): False,
      ("ru", "MTA-STS", "MTA-STS"): False,
      ("zh-Hans", "ClamAV", "ClamAV"): False,
      ("uk", "Azure Blob Storage", "Azure Blob Storage"): False,
      ("ja", "TLS 1.2", "TLS 1.2"): False,
      ("de", "Normal", "Normal"): False,
      ("fr", "Information", "Information"): False,
   }
   for (tag, source, target), expected in cases.items():
      found = untranslated(tag, source, target)
      if found != expected:
         print("FAIL: [%s] %r -> %r is %s as untranslated" % (tag, source, target, "reported" if found else "not reported"))
         failures += 1

   # The quotation rule: a German, Czech or Polish quotation is closed by its
   # language's own mark, and only a quotation is.
   quotations = {
      ("de", "Klicken Sie auf „Prüfen\", um den Eintrag zu lesen."): True,
      ("de", "Klicken Sie auf „Prüfen”."): True,
      ("de", "Klicken Sie auf „Prüfen“."): False,
      ("de", "„Ein“ und „Aus“"): False,
      ("de", "„Ein und „Aus“"): True,
      ("de", "Ein“"): True,
      ("de", "Ohne Anführungszeichen."): False,
      ("cs", "Klikněte na „Zkontrolovat“."): False,
      ("cs", "Klikněte na „Zkontrolovat\"."): True,
      ("pl", "Kliknij „Sprawdź”."): False,
      ("pl", "Kliknij „Sprawdź“."): True,
      ("fr", "Cliquez sur «Vérifier»."): False,
   }
   for (tag, value), expected in quotations.items():
      found = bool(quotation_findings(tag, value))
      if found != expected:
         print("FAIL: [%s] %r is %s as a quotation closed wrongly" % (tag, value, "reported" if found else "not reported"))
         failures += 1

   # The dash and ellipsis rules: the English writes the spaced hyphen and the ellipsis
   # character, and three dots only for literal text; a translation writes the hyphen,
   # or its own dash where its catalogue keeps one, one kind to a text.
   english_cases = {
      "Public " + EM + " anyone can send": True,
      "Weak password " + EN + " add {0}.": True,
      "Public - anyone can send": False,
      "Applying...": True,
      "Open Domains...": True,
      "Applying" + ELLIPSIS: False,
      "Address, domain or URL (https://host/...)": False,
      "Publish it (v=STSv1; id=...) - or wait.": False,
      "Send it as Authorization: Bearer hmapi_...": False,
      "Trust anchor override (tag alg digesttype hex; ...)": False,
      "Type:       TXT": True,
      "Host/Name:  {0}": True,
      "Version {0}  -  .NET {1}": True,
      "{0}  (not subscribed)": True,
      "Type: TXT": False,
      "Could not read the list: ": False,
      "The file\r\n   {0}\r\ncould not be loaded.": False,
      "1.  Scan this QR code with an authenticator app.": False,
      "↑↓ move    Enter open    Esc close": False,
   }
   for text, expected in english_cases.items():
      found = bool(english_punctuation_findings(text))
      if found != expected:
         print("FAIL: English %r is %s for its dash or its dots" % (text, "reported" if found else "not reported"))
         failures += 1
   translation_cases = {
      ("de", "Public - anyone can send", "Öffentlich " + EM + " jeder kann senden"): True,
      ("de", "Public - anyone can send", "Öffentlich - jeder kann senden"): False,
      ("zh-Hans", "A - B", "甲" + EM + EM + "乙"): True,
      ("fi", "Values 1-14", "Arvot 1" + EN + "14"): True,
      ("fi", "Values 1-14", "Arvot 1-14"): False,
      ("ru", "Public - anyone can send", "Открытый " + EM + " любой"): False,
      ("ru", "Public - anyone can send", "Открытый - любой"): False,
      ("ru", "Public - anyone can send", "Открытый " + EN + " любой"): True,
      ("ru", "A - b - c", "А " + EM + " б - в"): True,
      ("ru", "{0}-{1} of {2}", "{0}" + EN + "{1} из {2}"): True,
      ("cs", "Public - anyone can send", "Veřejný " + EN + " kdokoli"): False,
      ("cs", "Public - anyone can send", "Veřejný " + EM + " kdokoli"): True,
      ("de", "Applying" + ELLIPSIS, "Wird angewendet..."): True,
      ("de", "Applying" + ELLIPSIS, "Wird angewendet" + ELLIPSIS): False,
      ("de", "Address (https://host/...)", "Adresse (https://host/...)"): False,
      ("fr", "Value: {0}", "Valeur :    {0}"): True,
      ("fr", "Value: {0}", "Valeur : {0}"): False,
      ("zh-Hans", "Host/Name: {0}", "主机/名称：  {0}"): True,
      ("zh-Hans", "Host/Name: {0}", "主机/名称：{0}"): False,
   }
   for (tag, source, value), expected in translation_cases.items():
      found = bool(dash_findings(tag, source, value))
      if found != expected:
         print("FAIL: [%s] %r is %s for its dash or its dots" % (tag, value, "reported" if found else "not reported"))
         failures += 1

   # The page rule reads every navigation title: a page outside the old list of
   # thirty, renamed in a catalogue, is held to the references to it.
   for title in ("Suppression list", "Spam exemptions", "Allowed and blocked domains", "DNS block lists (DNSBL)"):
      if title not in PAGE_TITLES:
         print("FAIL: the page %r is not among the titles the page rule reads" % title)
         failures += 1
   reference = "Reads the refusals and delivery reports for this server's senders. The list is on the Suppression list page."
   for value, expected in (("Die Liste steht auf der Seite Sperrliste.", True), ("Die Liste steht auf der Seite Unterdrückungsliste.", False)):
      found = bool(check("de", {"Suppression list": "Unterdrückungsliste", reference: value}))
      if found != expected:
         print("FAIL: %r is %s as naming the page wrongly" % (value, "reported" if found else "not reported"))
         failures += 1

   # The DNS label rule: the shipped Japanese DKIM text is refused, the mended one and a
   # caption's Alt key are not.
   dkim = "At your DNS host, publish a TXT record named <selector>._domainkey.<domain>."
   for (source, value), expected in (((dkim, "DNS で <selector>.domainkey.<domain> を公開します。(_D)"), True),
                                     ((dkim, "DNS で <selector>._domainkey.<domain> を公開します。"), False),
                                     (("Publish _dmarc.<domain> and _mta-sts.<domain>.", "_dmarc.<domain> を公開します。"), True),
                                     (("_Domains", "ドメイン(_D)"), False)):
      found = any("DNS label" in why for _, why in check("ja", {source: value}))
      if found != expected:
         print("FAIL: %r is %s as losing a DNS label" % (value, "reported" if found else "not reported"))
         failures += 1

   # The English reference rule: a page or a tab named in the text exists by that name.
   titles = {"Live logs", "Server status", "Anti-spam settings", "Protocols", "Directory authentication (LDAP)"}
   tabs = {"General", "SMTP", "Windows directory", "Sender auth"}
   references = {
      "Check the folder on the server, or in the backup log on the Logs page.": True,
      "Check the folder on the server, or in the backup log on the Live logs page.": False,
      "The outcome goes to the log and the Status page.": True,
      "The outcome goes to the log and the Server status page.": False,
      "It is configured on the Anti-spam page, under sender authentication.": True,
      "It is configured on the Anti-spam settings page's Sender auth tab.": False,
      "Use LDAP directory authentication on the Directory authentication (LDAP) page instead.": False,
      "Leave it off and the account uses the password on the Account tab.": True,
      "Leave it off and the account uses the password on the General tab.": False,
      "The same value the account's Directory tab holds.": True,
      "The same value the account's Windows directory tab holds.": False,
      "Host name on the Protocols page's SMTP tab is empty.": False,
      "Look for it on the Deck's queue page.": False,
      "No profile has been applied, so this page shows the shipped values.": False,
   }
   for text, expected in references.items():
      found = bool(reference_findings({text}, titles, tabs))
      if found != expected:
         print("FAIL: %r is %s as naming a page or a tab that does not exist" % (text, "reported" if found else "not reported"))
         failures += 1
   if "General" not in tab_headers() or "Windows directory" not in tab_headers():
      print("FAIL: the tab rule does not read the account dialog's tab headers")
      failures += 1

   # The place rule: what a text says something is under is a page or a group.
   pages = {"Anti-spam settings", "Advanced", "Allowed and blocked domains"}
   groups = {"Spam filtering", "Sign-in & access"}
   places = {
      "Allowed and blocked domains have a page of their own under Anti-spam.": True,
      "Domains are allowed or blocked outright on the Allowed and blocked domains page, under Spam filtering.": False,
      "Until it is turned on under Anti-spam settings, spam over the threshold is refused.": False,
      "The engine itself is switched on under Advanced.": False,
      "It is switched on under Anti-spam.": True,
      "Mail from elsewhere is filed under Inbound, and reports under Error, with a copy for each recipient.": False,
      "The secret is stored per machine under HKLM, so enabling it needs local administrator rights.": False,
   }
   for text, expected in places.items():
      found = bool(reference_findings({text}, pages, set(), groups))
      if found != expected:
         print("FAIL: %r is %s as naming a place the navigation does not have" % (text, "reported" if found else "not reported"))
         failures += 1
   if "Spam filtering" not in navigation_groups():
      print("FAIL: the place rule does not read the navigation map's group titles")
      failures += 1
   print("self-test: %d failure(s)" % failures)
   return 1 if failures else 0


def main():
   if hasattr(sys.stdout, "reconfigure"):
      sys.stdout.reconfigure(encoding="utf-8")
   if "--self-test" in sys.argv[1:]:
      return self_test()
   if not os.path.isdir(RESOURCES):
      print("No Resources directory at %s" % RESOURCES)
      return 1

   english = read_resx(os.path.join(RESOURCES, "Strings.resx"))
   if not english:
      print("Strings.resx is empty or missing; run check-localisation.py --write first.")
      return 1

   # The English itself: every page and tab it names is one the Control Panel has.
   references = reference_findings(english, set(PAGE_TITLES), tab_headers(), set(navigation_groups()))
   references += [(text, what) for text in sorted(english) for what in english_punctuation_findings(text)]
   for source, what in references:
      print("  [en] %s\n        in %r" % (what, source[:110]))
   total = len(references)

   for name in sorted(os.listdir(RESOURCES)):
      match = re.match(r"^Strings\.([A-Za-z-]+)\.resx$", name)
      if not match:
         continue
      tag = match.group(1)
      raw = read_resx(os.path.join(RESOURCES, name))
      # Only the keys this catalogue actually translates, keyed by the English.
      catalogue = {key: value for key, value in raw.items() if key in english and value}
      findings = check(tag, catalogue)
      # A Czech, Polish, Russian or Ukrainian plural form ("<English>|few", see
      # Loc.Plural) is held to the English of the form it stands in for.
      few = {key[:-len(FEW)]: value for key, value in raw.items()
             if key.endswith(FEW) and key[:-len(FEW)] in english and value}
      findings += [(source + FEW, what) for source, what in check(tag, few, pages=catalogue)]
      total += len(findings)
      for source, what in findings:
         print("  [%s] %s\n        in %r" % (tag, what, source[:110]))

   if total:
      print("FAIL: %d problem(s) across the catalogues" % total)
      return 1

   print("OK    every catalogue keeps the server's own words, the numbers and the page names")
   return 0


if __name__ == "__main__":
   sys.exit(main())
