#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Control Panel's translation catalogues agree with its source, and every
language that claims to be complete is.

The English text is the key (Services/Loc.cs). A caption is marked where it is
used - L("_Save changes"), F("Deleted {0} messages", n) or N("Welcome") in C#,
{loc:L '_Save changes'} in XAML - and Resources/Strings.resx, the English
catalogue, is generated from those marks so that it can never drift from them.
Resources/Strings.<culture>.resx holds one language each, keyed by the same
English text.

What is checked
  * Strings.resx holds exactly the set of marked texts, in sorted order, each
    mapped to itself. Run this script with --write after adding or changing a
    marked text; CI fails when the committed file is behind.
  * Every language catalogue: no key that the English catalogue lacks (an
    orphan is a translation of text that no longer exists); every value keeps
    the same {n} placeholders as its English (a lost placeholder is a sentence
    with no subject, an extra one is a FormatException at run time); a caption
    with an Alt-key mnemonic in English carries one in the translation too,
    because the keyboard cannot lose a key by changing language; and a value
    keeps the white space its English starts or ends with, because the source
    joins that text to another there ("Could not copy: " + the reason) - in
    Japanese and Chinese, which put no space between words, it may go where
    the translation ends (or starts) with a character of those scripts - and
    adds none at an edge where its English has none.
  * A language listed in COMPLETE has a translation for every key. Others are
    reported, so a new language can be brought in a page at a time.
  * A source file listed in LOCALISED carries no untranslated caption: no
    literal text reaching a caption, title, header, tool tip, placeholder,
    message box or accessible name without a marker. Files not yet listed are
    reported with their counts - that list is the work remaining, and a file
    joins LOCALISED when its count reaches zero. A literal that is not for
    translation (a protocol name, a brand glyph, an example address) is either
    in EXEMPT below or on a line ending in "// no-loc".
  * No C# source splits a sentence around a value with its closing
    punctuation left as a literal - L("... followed by @") + domain + "." -
    since its translator cannot place the value; F("... @{0}.", domain).
  * No C# source joins a count to a text with a space - rows.Count + " " +
    noun, or n + " " + (n == 1 ? one : many) - since the noun is translated
    alone and the code then inflects it in English; a sentence written whole
    for each form, Plural(n, "{0} item", "{0} items"), lets each language
    count its own way.
  * Plurals: the two forms of every Loc.Plural or Loc.PluralF call are string
    literals, catalogue keys like any marked text, and carry the same
    placeholders (a language's "one" is not always 1: Russian says 21 that
    way); the catalogues of Czech, Polish, Russian and Ukrainian hold a third
    form for each pair, the second key followed by "|few", for two to four of
    a thing, and no catalogue holds one the source does not ask for; no marked
    text puts an English plural ending in brackets ("message(s)"), and no
    translation puts one of its own language's there ("{0} expiré(s)") where
    the English has a bare count to inflect around; and no C#
    source writes a count's forms as n == 1 ? L(...) : F(...), which gives
    those four languages one plural for 2 and for 5.
  * Whole texts: no marked text begins with white space, or ends with it
    after anything but the colon of a label ("Scope: " + value). A text with
    white space at an edge is glued to a value or to another text -
    L("MX - which server takes the mail for ") + domain, L(" (inactive)"),
    L(" and the policy verifies nothing.") - and its translator sees a piece
    of a sentence and cannot move the rest; F("... {0}", value) can be, and
    whole sentences are joined by Loc.Sentences. In a localised file no
    translated text is joined to another with a space literal - said + " " +
    F(...), L(...) + " " + reason, (n > 0 ? " " : "") + Plural(...) - since
    Japanese and Chinese end a sentence with a full stop that needs none.

Usage: python3 build/check-localisation.py [--write] [--inventory] [--self-test]
  --write      regenerate Strings.resx and bring every language catalogue up
               to date (orphans dropped, new keys added empty, values kept)
  --inventory  list every untranslated caption in every file, localised or not
  --self-test  hold the caption, white-space and split-sentence rules to
               source lines and translations made to pass and to fail them
Exit status 0 when every check above passes.
"""
import html
import os
import re
import sys
import unicodedata
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "hmailserver", "source", "Tools", "ControlPanel"))
RESOURCES = os.path.join(ROOT, "Resources")
NEUTRAL = os.path.join(RESOURCES, "Strings.resx")

# Languages whose catalogue must translate every key.
COMPLETE = {"cs", "da", "de", "es", "fi", "fr", "it", "ja", "nb", "nl", "pl", "pt-BR", "ru", "sv", "tr", "uk", "zh-Hans"}

# Files (relative to ROOT, forward slashes) in which every caption is marked.
LOCALISED = {
   "App.xaml",
   "App.xaml.cs",
   "MainWindow.xaml",
   "MainWindow.xaml.cs",
   "Services/NavigationMap.cs",
   "Services/Toast.cs",
   "Views/BackupView.xaml",
   "Views/BackupView.xaml.cs",
   "Views/ConnectView.xaml",
   "Views/ConnectView.xaml.cs",
   "Views/DashboardView.xaml",
   "Views/DashboardView.xaml.cs",
   "Views/Dialogs.cs",
   "Views/DomainsView.xaml",
   "Views/DomainsView.xaml.cs",
   "Views/FeatureSettingsView.Updates.cs",
   "Views/FeatureSettingsView.xaml",
   "Views/FeatureSettingsView.xaml.cs",
   "Views/IPRangesView.xaml",
   "Views/IPRangesView.xaml.cs",
   "Views/LogsView.xaml",
   "Views/LogsView.xaml.cs",
   "Views/QueueView.xaml",
   "Views/QueueView.xaml.cs",
   "Views/RolesView.cs",
   "Views/RoutesView.xaml",
   "Views/RoutesView.xaml.cs",
   "Views/RulesView.xaml",
   "Views/RulesView.xaml.cs",
   "Views/ServerSettingsView.xaml",
   "Views/ServerSettingsView.xaml.cs",
   "Views/ServerSettingsView.NeverBanNames.cs",
   "Services/NeverBanNames.cs",
   "Services/SettingClaims.cs",
   "Views/SslCertificatesView.xaml",
   "Views/SslCertificatesView.xaml.cs",
   "Views/StatusView.xaml",
   "Views/StatusView.xaml.cs",
   "Views/UpdateInstallWindow.cs",
   "Views/UpdateNotice.cs",
   "Services/AdministratorUpdate.cs",
   "Services/InstallationPermissions.cs",
   "Services/AfterUpdateHandoff.cs",
   "Services/UpdateStatusText.cs",
   "Views/TcpIpPortsView.xaml",
   "Views/TcpIpPortsView.xaml.cs",
   "Views/DnsRecordsView.cs",
   "Views/LdapSettingsView.cs",
   "Views/DirectorySyncView.cs",
   "Views/ApiKeysView.cs",
   "Services/ExternalSetupChecks.cs",
   "Services/TlsPosture.cs",
   "Services/SpamPipeline.cs",
   "Services/VirusPipeline.cs",
   "Services/CertificateInspector.cs",
   "Services/ApiKeyStore.cs",
   "Services/ListenerProbe.cs",
   "Services/PaletteSearch.cs",
   "Services/StatusSemantics.cs",
   "Services/ChartCatalog.cs",
   "Services/ChartDataTable.cs",
   "Services/NumericField.cs",
   "Services/WindowsServiceInfo.cs",
   "Services/ActiveDirectoryService.cs",
   "Services/DnsTxtLookup.cs",
   "Services/PathPicker.cs",
   "Services/PasswordStrength.cs",
   "Services/ServerSession.cs",
   "Services/SettingApplies.cs",
   "Services/SettingLevels.cs",
   "Services/HostReachability.cs",
   "Services/WelcomeIntents.cs",
   "Services/IntentIndex.cs",
   "Services/Typography.cs",
   "Services/IniFeatureStore.cs",
   "Services/MessageStoreConsistencyReport.cs",
   "Services/DirectorySyncReport.cs",
   "Services/DiscoveryReport.cs",
   "Services/DkimKeyGenerator.cs",
   "Services/LanguageChoice.cs",
   "Services/PasswordGenerator.cs",
   "Services/SettingsSearchIndex.cs",
   "Views/StalledMailView.cs",
   "Views/ControlDeckLink.cs",
   "Views/ExamplePlaceholder.cs",
   "Services/ControlDeck.cs",
   "Views/ProfilesView.cs",
   "Views/UtilityViews.cs",
   "Views/TlsOverviewView.cs",
   "Views/VirusOverviewView.cs",
   "Views/SpamOverviewView.cs",
   "Views/MessageTraceView.cs",
   "Views/AboutView.cs",
   "Views/QuarantineView.cs",
   "Views/QuarantineListDialog.cs",
   "Services/QuarantineReview.cs",
   "Services/ChartWindowWords.cs",
   "Views/SuppressionListView.cs",
   "Views/DiscoveryView.cs",
   "Views/ScriptsView.cs",
   "Views/AccessibleChartCard.cs",
   "Views/PublicFoldersView.cs",
   "Views/WelcomeView.cs",
   "Views/ExternalSetupView.cs",
   "Views/NavigationPalette.cs",
   "Views/Mnemonic.cs",
   "Views/DomainDialog.cs",
   "Views/AccountDialog.cs",
   "Views/CollectionSpecs.cs",
   "Views/FolderPermissionsDialog.cs",
   "Views/TcpIpPortDialog.cs",
   "Views/RouteDialog.cs",
   "Views/IPRangeDialog.cs",
   "Views/GroupMembersDialog.cs",
   "Views/RuleActionDialog.cs",
   "Views/DistributionListDialog.cs",
   "Views/RuleCriteriaDialog.cs",
   "Views/AppPasswordsPanel.cs",
   "Views/RecipientsDialog.cs",
   "Views/ActiveDirectoryPickerDialog.cs",
   "Views/TotpSetupDialog.cs",
   "Views/AdministratorTwoFactorDialog.cs",
   "Views/CollectionEditorView.cs",
   "Views/AccountRecoveryPanel.cs",
   "Views/SettingLevelsPanel.cs",
   "Views/AccountTwoFactorPanel.cs",
   "Views/MessageViewerDialog.cs",
   "Views/TotpPromptDialog.cs",
   "Views/FieldDialog.cs",
   "Views/Scaffold/Scaffold.xaml",
   "Views/Scaffold/Tokens.xaml",
   "Views/Scaffold/ScaffoldControl.cs",
   "Views/Scaffold/PageHeader.cs",
   "Views/Scaffold/Card.cs",
   "Views/Scaffold/Toolbar.cs",
   "Views/Scaffold/EmptyState.cs",
   "Views/Scaffold/LoadingState.cs",
   "Views/Scaffold/InlineNotice.cs",
   "Views/Scaffold/StatusPill.cs",
   "Views/Scaffold/FieldRow.cs",
   "Views/Scaffold/SettingsSection.cs",
   "Views/Scaffold/DialogFrame.cs",
   "Views/FluentDialogWindow.cs",
   "Views/Scaffold/TourOverlay.cs",
   "Services/Tour.cs",
   "Services/TourSession.cs",
   "Services/TourProgress.cs",
   "MainWindow.Tour.cs",
   "Services/DesignTokens.cs",
   "Services/ConnectionStatus.cs",
   "Services/SettingsPageStatus.cs",
   "Services/ThemeTokens.cs",
   "Services/SelectableText.cs",
   "Services/FrameworkWords.cs",
   "Services/ChartPalette.cs",
   "Views/HelpPane.cs",
}

# Literal texts that are the same in every language and are not captions to
# translate: protocol and product names, the brand glyph, keyboard chords.
EXEMPT = {
   "hM", "hMailServer", "hMailServer.ini", "Ctrl+K", "localhost", "0.0.0.0",
   "SMTP", "POP3", "IMAP", "SSL/TLS", "TLS", "STARTTLS", "DNS", "ID", "#", "…", "...",
   "—", "-", "OK", "Light", "Dark", "live",
}

CS_LITERAL = r'"((?:[^"\\\n]|\\.)*)"'
CS_MARK = re.compile(r"(?<![\w.])[LNF]\(\s*" + CS_LITERAL)
XAML_MARK = re.compile(r"\{loc:L\s+'((?:[^'\\]|\\.)*)'\s*\}")
XAML_CAPTION_ATTR = re.compile(
   r'\b(?:Text|Header|Content|ToolTip|Title|PlaceholderText|AutomationProperties\.Name|AutomationProperties\.HelpText)="([^"]*)"')
CS_SINK = re.compile(
   r"(\.Show\(|\.Text\s*=|\bTitle\s*=|\bHeader\s*=|\bContent\s*=|\bToolTip\s*=|\bPlaceholderText\s*="
   r"|\bLabel\(|\bMakeButton\(|\bCard\(|\bLabelled\w*\(|\bLabel\s*=|\bBlurb\s*=|\bNote\s*=|\bHint\s*="
   r"|new TextBlock\b|\bSetName\(|\bSetHelpText\(|\bFail_\(|\bSetBusy_\(|\bToast\.\w+\()")
PLACEHOLDER = re.compile(r"\{(\d+)(?:[:,][^}]*)?\}")

# Languages written without spaces between words: there a space at a joint may go
# where the translation's own character at that edge is one of the script's.
UNSPACED = {"ja", "zh-Hans"}


def lost_edge_space(name, value, tag):
   """The edges ("start", "end") where the English has white space and the translation lost it.

   The source joins such a text to another at that edge - L("Could not copy: ") +
   the reason, F(" (published {0})", date) after a sentence - so a translation
   without the space ran the two together: 290 values in sixteen languages had
   lost a trailing space by 1 October 2026 ("Kopieren nicht möglich:Zugriff
   verweigert"). In Japanese and Chinese a space between two sentences, or after
   a word of their own script, is not written, so a translation that ends (or
   starts) with a wide character there keeps none (13 such values)."""
   lost = []
   for edge, english, translated, letter in (
         ("start", name[:1], value[:1], value.lstrip()[:1]),
         ("end", name[-1:], value[-1:], value.rstrip()[-1:])):
      if not english.isspace() or translated.isspace() or not letter:
         continue
      if tag in UNSPACED and unicodedata.east_asian_width(letter) in ("W", "F"):
         continue
      lost.append(edge)
   return lost


def added_edge_space(name, value):
   """The edges ("start", "end") where the translation has white space and the English none.

   The source joins the next text to the English as it is, so a space the translation
   adds there is a space the program never meant: Finnish counted "3  ulkoisella
   POP3-noutotilillä on" with two, and French opened a group's name with "« " that
   the source closed with "'."."""
   return [edge for edge, english, translated in (("start", name[:1], value[:1]), ("end", name[-1:], value[-1:]))
           if translated.isspace() and not english.isspace()]


def unescape_cs(literal):
   out, i = [], 0
   while i < len(literal):
      c = literal[i]
      if c == "\\" and i + 1 < len(literal):
         n = literal[i + 1]
         if n == "u" and i + 5 < len(literal):
            out.append(chr(int(literal[i + 2:i + 6], 16)))
            i += 6
            continue
         out.append({"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'", "0": "\0"}.get(n, n))
         i += 2
         continue
      out.append(c)
      i += 1
   return "".join(out)


def unescape_xaml(literal):
   # inside {loc:L '...'}: XML entities first (the attribute), then the
   # markup-extension escapes (\' and \\)
   text = html.unescape(literal)
   return re.sub(r"\\(.)", r"\1", text)


def source_files():
   for base, dirs, files in os.walk(ROOT):
      dirs[:] = [d for d in dirs if d not in ("bin", "obj", "publish", "Resources")]
      for name in sorted(files):
         if name.endswith((".cs", ".xaml")) and not name.endswith(".g.cs"):
            yield os.path.join(base, name)


def rel(path):
   return os.path.relpath(path, ROOT).replace("\\", "/")


def read(path):
   with open(path, encoding="utf-8-sig") as handle:
      return handle.read()


def marked_texts():
   """{english: [locations]} for every marker in the source, the forms of every Plural call among them."""
   found = {}
   for path in source_files():
      src = read(path)
      if path.endswith(".xaml"):
         hits = ((m.start(), unescape_xaml(m.group(1))) for m in XAML_MARK.finditer(src))
      else:
         hits = [(m.start(), unescape_cs(m.group(1))) for m in CS_MARK.finditer(src)]
         for call in plural_calls(src):
            hits.extend((pos, text) for pos, text in call["forms"] if text is not None)
      for pos, text in hits:
         if text:
            found.setdefault(text, []).append(f"{rel(path)}:{src.count(chr(10), 0, pos) + 1}")
   return found


# --- plurals -------------------------------------------------------------------------

# Loc.Plural(count, one, other) and Loc.PluralF(count, one, other, args...): the two
# forms are catalogue keys, read here from the call's second and third arguments,
# which have to be literals so that the pair can be checked. The language's CLDR
# category of the count picks the form at run time (Services/Loc.cs); a catalogue
# whose language says two to four of a thing apart from five holds a third form,
# the key of the second literal followed by "|few".
PLURAL_CALL = re.compile(r"(?<![\w.])(?:Loc\.)?Plural(F?)\(")
PLURAL_DECLARATION = re.compile(r"\b(?:string|static|public|private|internal)\s+$")
FEW = "|few"
FEW_LANGUAGES = {"cs", "pl", "ru", "uk"}


def call_arguments(src, start):
   """[(offset, text)] of the top-level arguments of the call whose "(" is at start - 1, or None if it never closes."""
   args, depth, i, begin = [], 0, start, start
   while i < len(src):
      c = src[i]
      if c == '"' or (c in "@$" and src[i + 1:i + 2] == '"'):
         verbatim = c == "@" or src[i:i + 3] in ('$@"', '@$"')
         i = src.index('"', i)
         i += 1
         while i < len(src):
            if verbatim and src[i] == '"' and src[i + 1:i + 2] == '"':
               i += 2
               continue
            if not verbatim and src[i] == "\\":
               i += 2
               continue
            if src[i] == '"':
               break
            i += 1
      elif c == "'":
         # a char literal: 'x', '\'', 'A'
         i = src.index("'", i + (3 if src[i + 1:i + 2] == "\\" else 2))
      elif c in "([{":
         depth += 1
      elif c in ")]}":
         if depth == 0:
            args.append((begin, src[begin:i]))
            return args
         depth -= 1
      elif c == "," and depth == 0:
         args.append((begin, src[begin:i]))
         begin = i + 1
      i += 1
   return None


def plural_calls(src):
   """Each Plural and PluralF call: {"line", "forms": [(offset, text or None), (offset, text or None)]}.

   A form that is not one plain string literal is None, and reported."""
   calls = []
   for m in PLURAL_CALL.finditer(src):
      if PLURAL_DECLARATION.search(src[max(0, m.start() - 30):m.start()]):
         continue
      if src[src.rfind("\n", 0, m.start()) + 1:m.start()].lstrip().startswith(("//", "*")):
         continue   # an example in a comment
      args = call_arguments(src, m.end())
      if args is None or len(args) < 3:
         continue
      forms = []
      for offset, text in args[1:3]:
         literal = re.fullmatch(r"\s*" + CS_LITERAL + r"\s*", text)
         forms.append((offset + len(text) - len(text.lstrip()), unescape_cs(literal.group(1)) if literal else None))
      calls.append({"line": src.count("\n", 0, m.start()) + 1, "forms": forms})
   return calls


def plural_pairs():
   """{other: one} for every Plural call in the source, and the problems of the calls themselves."""
   pairs, problems = {}, []
   for path in source_files():
      if not path.endswith(".cs"):
         continue
      src = read(path)
      for call in plural_calls(src):
         (_, one), (_, other) = call["forms"]
         where = f"{rel(path)}:{call['line']}"
         if one is None or other is None:
            problems.append(f"{where}: a Plural call whose forms are not two string literals; write both forms in "
                            f"the call, so that the pair can be read as catalogue keys and checked")
            continue
         if set(PLACEHOLDER.findall(one)) != set(PLACEHOLDER.findall(other)):
            problems.append(f"{where}: the two forms of a Plural call carry different placeholders, {one!r} and "
                            f"{other!r}; both carry the count, since a language's \"one\" is not always 1")
         if pairs.get(other, one) != one:
            problems.append(f"{where}: {other!r} is the plural of two texts, {pairs[other]!r} and {one!r}")
         pairs[other] = one
   return pairs, problems


# An English plural ending in brackets - "message(s)", "address(es)" - which leaves every
# language an English plural to translate: German read "Einstellung(en)" and "Regel(n)",
# and Polish has three forms for it. Loc.Plural writes each form whole. A unit after a
# space, "timeout (s)", is not this.
PAREN_PLURAL = re.compile(r"[A-Za-z]\((?:s|es)\)")

# The same in a translation: French "{0} expiré(s) et désormais refusé(s)", Spanish
# "{0} caducada(s)", Polish "kont(a)" - the English key has a bare count, "{0} expired and
# now refused", that the language has to inflect around, and its translator wrote both
# forms at once. The key is to be written as Plural(n, one, other), each form whole. The
# endings are the plural endings of the seventeen languages; a gender ending on a past
# tense - Czech "Otevřel(a)", Polish "Otworzył(a)" for "Opened by" - says who did it,
# not how many, and is allowed by name.
CATALOGUE_PAREN_PLURAL = re.compile(r"[^\W\d_]\((?:s|e|es|en|n|er|r|x|i|a|y|t|ów|ů|lar|ler|а|я|ы|и|і|ов|ів|ей)\)")
GENDER_ENDINGS = {"Otevřel(a)", "Otworzył(a)"}


def catalogue_paren_plurals(value, key=""):
   """The words of a translation that put a plural ending in brackets, other than a gender ending or a word its key has."""
   found = []
   for m in CATALOGUE_PAREN_PLURAL.finditer(value):
      word = re.search(r"[^\W\d_]*$", value[:m.start() + 1]).group(0) + value[m.start() + 1:m.end()]
      if word not in GENDER_ENDINGS and word not in key:
         found.append(word)
   return found

# A count's two forms written as a choice, n == 1 ? L("1 item") : F("{0} items", n): one
# plural for 2 and for 5, which Polish, Czech, Russian and Ukrainian do not have.
PAIR_CHOICE = re.compile(r"==\s*1\s*\)?\s*\?\s*L\(\s*" + CS_LITERAL + r"\s*\)\s*:\s*F\(\s*\"")


# A piece of a sentence: a marked text with white space at an edge, glued in the source
# to a value or another text. German read "... die Mail annimmt für example.com" for
# L("MX - which server takes the mail for ") + domain, the Japanese of a sentence added
# with a leading space kept the space after its 。, and " (inactive)" put the English
# order of a name and its state in every language (review finding F283). The colon of a
# label, "Scope: " + value, is the one separator a text may end with.
def glued_at_edge(text):
   """True for a marked text with white space at its start, or at its end after anything but a colon."""
   return text != text.lstrip() or (text != text.rstrip() and not text.endswith(": "))


def cs_pair_choices(src):
   """(line, code) for every count written as n == 1 ? L(...) : F(...)."""
   return [(src.count("\n", 0, m.start()) + 1, " ".join(src[m.start():m.end() + 30].split()))
           for m in PAIR_CHOICE.finditer(src)]


# --- resx ---------------------------------------------------------------------

# The licence header exactly as build/add-license-headers.py writes one into an
# XML file: the three lines, each on its own, inside one comment element under
# the declaration. It was the older two-line wording until 15 September 2026,
# which meant that --write left all eighteen catalogues failing the SPDX check
# and the person who had only added a caption had to find that out from CI.
RESX_HEAD = """<?xml version="1.0" encoding="utf-8"?>
<!--
  https://www.progressiverobot.com
  Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
  SPDX-License-Identifier: AGPL-3.0-or-later
-->
<root>
  <!-- {comment} -->
  <resheader name="resmimetype">
    <value>text/microsoft-resx</value>
  </resheader>
  <resheader name="version">
    <value>2.0</value>
  </resheader>
  <resheader name="reader">
    <value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
  <resheader name="writer">
    <value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
"""

NEUTRAL_COMMENT = ("GENERATED by build/check-localisation.py from the L(...), F(...), N(...) and {loc:L '...'} "
                   "marks in the Control Panel source. Do not edit by hand: change the source and run the "
                   "script with its write option. The English text is the key; a language catalogue beside this file "
                   "(Strings.<culture>.resx) maps each key to its translation.")


def attr(text):
   return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")
           .replace("\r", "&#13;").replace("\n", "&#10;").replace("\t", "&#9;"))


def elem(text):
   # a carriage return as a character reference: an XML parser normalises a
   # literal CR LF in element text to LF, and a message box that says CR LF
   # would come back one character shorter than its key
   return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
           .replace("\r", "&#13;"))


def write_resx(path, items, comment):
   # Written the way the file was read: a catalogue that carried a UTF-8 byte
   # order mark keeps it. read() strips the mark on the way in (utf-8-sig), and
   # writing plain utf-8 on the way out silently dropped it from sixteen
   # catalogues on 23 September 2026 - one byte of churn per file that every
   # later diff would have carried.
   had_bom = False
   try:
      with open(path, "rb") as handle:
         had_bom = handle.read(3) == b"\xef\xbb\xbf"
   except OSError:
      pass
   lines = [RESX_HEAD.replace("{comment}", comment)]
   for name, value in items:
      lines.append(f'  <data name="{attr(name)}" xml:space="preserve">\n    <value>{elem(value)}</value>\n  </data>\n')
   lines.append("</root>\n")
   with open(path, "w", encoding="utf-8-sig" if had_bom else "utf-8", newline="\n") as handle:
      handle.write("".join(lines))


def read_resx(path):
   """[(name, value)] in file order; a missing file is an empty catalogue."""
   if not os.path.exists(path):
      return []
   tree = ET.parse(path)
   out = []
   for data in tree.getroot().findall("data"):
      value = data.find("value")
      out.append((data.get("name"), (value.text or "") if value is not None else ""))
   return out


def language_files():
   if not os.path.isdir(RESOURCES):
      return []
   out = []
   for name in sorted(os.listdir(RESOURCES)):
      m = re.fullmatch(r"Strings\.([A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*)\.resx", name)
      if m:
         out.append((m.group(1), os.path.join(RESOURCES, name)))
   return out


# --- untranslated captions ------------------------------------------------------

# One word that ends a sentence - "Copied.", "Deleted.", "Applying..." - is a caption,
# though a dot and no space otherwise reads as a host or a file name. AppPasswordsPanel
# said "Copied." in English in every language until 6.3.6, and two more were found
# with it.
SENTENCE_WORD = re.compile(r"[A-Z][a-z]+(?:\.\.\.|[.!?\u2026])")

# A piece of a sentence joined to others: lowercase words with white space at an edge,
# as in "in " + page or a " and " between two names. The palette said "in" before every
# page's place in every language until 6.3.6.
JOINED_PIECE = re.compile(r"\(?[A-Za-z]?[a-z]{2,}(?: [a-z]{2,})*[:.,;)]?")

# A constant such as ApiKeyStore's "Key." is the start of a dotted name, not a sentence.
CONST_DECLARATION = re.compile(r"\bconst\s+string\s+\w+\s*=\s*$")


def is_caption(text):
   if SENTENCE_WORD.fullmatch(text.strip()):
      return text.strip() not in EXEMPT
   return re.search(r"[A-Za-z]{2,}", text) is not None and text.strip() not in EXEMPT \
      and not (("@" in text or "." in text) and " " not in text)


NON_UI_CALL = re.compile(
   r"\b(?:GetMetricHistory|TryGetProperty|GetProperty|ReadString|WriteString|ReadBool|WriteBool|ReadInt|WriteInt"
   r"|GetValue|SetValue|ReadFrom|WriteTo|ReadValue|Read|IniRead_|IniWrite_|IniReadInt_|IniWriteInt_|IniReadBool_|IniWriteBool_|LiveBool_|LiveText_|LiveInt_|SecretConfigured_|OpenSubKey|CreateSubKey|SetResourceReference|SetAutomationId|Contains|StartsWith|EndsWith"
   r"|Split|Replace|IndexOf|TryParse|TryParseExact|ParseExact|GetFiles|Path\.Join|Path\.Combine|nameof|Debug\.Fail"
   r"|Debug\.Assert|LogException|RunUpdateAction_|NavigateTo|Slug|GetString|Equals|Compare|Regex|Match|\w+Exception)\s*\(")
NON_UI_CONTEXT = re.compile(r"(?:\bcase\s|==|!=|\bis\s|\[|\bTag\s*=|\bKey\s*=|\bPath\s*=|\bProp\s*=|\bconst\s)\s*$")
# named arguments whose value is a search term list or an icon name, not a caption
NON_UI_ARGUMENT = re.compile(r"\b(?:aliases|seeAlso|icon):")


# The access key's underscore in a caption, which splits a one-word caption so that
# it no longer holds two letters together: "U_p", the THEN card's Up button, was
# never translated because the scan did not see a word in it (review finding F178).
# Only a one-word caption is read this way, its underscore first or between a letter
# and a small one ("_Save", "U_p", "Dow_n", "Re_fresh..."): PART_Search, MAX_SIZE and
# max_age are names, and a caption of several words is seen by its space already.
ONE_WORD_WITH_MNEMONIC = re.compile(r"_?[A-Za-z]+(?:_[a-z][A-Za-z]*)?(?:\.\.\.|\u2026)?")


def without_mnemonic(text):
   """A one-word caption as it reads on screen, its access-key underscore removed; any other text as it is."""
   if text.count("_") != 1 or not ONE_WORD_WITH_MNEMONIC.fullmatch(text):
      return text
   return text.replace("_", "", 1)


def cs_untranslated(src):
   """(line, literal) for every literal that reads as a caption and carries no marker.

   A literal is a caption when it holds two consecutive letters and either
   contains a space or begins with a capital letter and looks nothing like an
   identifier, a path or a key - or is one word ending a sentence ("Copied."),
   or a piece of a sentence joined to others ("in ", " and "). Lines that call
   something that takes a key, a pattern or a path rather than a caption are
   left out, as are comparisons, case labels, indexers, the Tag/Key/Path
   properties that carry keys, and a constant's one word ending in a dot."""
   out = []
   lines = src.split("\n")
   # the forms of a Plural call are catalogue keys, read by plural_calls
   plural_forms = {offset for call in plural_calls(src) for offset, _ in call["forms"]}
   for m in re.finditer(r'\$?"((?:[^"\\\n]|\\.)*)"', src):
      if m.start() in plural_forms:
         continue
      raw = m.group(1)
      interpolated = m.group(0).startswith("$")
      text = without_mnemonic(unescape_cs(re.sub(r"\{[^}]*\}", "", raw) if interpolated else raw))
      if not re.search(r"[A-Za-z]{2,}", text) or text.strip() in EXEMPT:
         continue
      sentence_word = SENTENCE_WORD.fullmatch(text.strip()) is not None
      joined_piece = text != text.strip() and JOINED_PIECE.fullmatch(text.strip()) is not None
      if " " not in text.strip() and not sentence_word and not joined_piece \
            and not (text[:1].isupper() and not re.search(r"[_./\\:={}<>\[\]@|*]", text)
                     and not re.search(r"[a-z][A-Z]|\d$", text)):
         continue
      if not is_caption(text) or text.count("|") >= 2:   # a|b|c is a list of search terms
         continue
      line = src.count("\n", 0, m.start()) + 1
      source_line = lines[line - 1]
      stripped = source_line.strip()
      if "// no-loc" in source_line or stripped.startswith(("//", "///", "*", "[")):
         continue
      column = m.start() - (src.rfind("\n", 0, m.start()) + 1)
      head = source_line[:column]
      if "//" in head and head[:head.index("//")].count('"') % 2 == 0:
         continue   # inside a trailing comment
      # a literal continuing a statement from the lines above belongs to the
      # call on the first of them
      statement = source_line
      back = line - 2
      while back >= 0 and statement.lstrip().startswith(('"', "+", "?", ":")) and line - back <= 5:
         statement = lines[back] + "\n" + statement
         back -= 1
      if NON_UI_CALL.search(statement) or NON_UI_ARGUMENT.search(statement):
         continue
      before = src[max(0, m.start() - 40):m.start()]
      if NON_UI_CONTEXT.search(before):
         continue
      if sentence_word and CONST_DECLARATION.search(before):
         continue
      if re.search(r"\b[LNF]\(\s*$", before):
         continue
      out.append((line, ("$" if interpolated else "") + unescape_cs(raw)))
   return out


def xaml_untranslated(src):
   out = []
   lines = src.split("\n")
   for m in XAML_CAPTION_ATTR.finditer(src):
      value = html.unescape(m.group(1))
      if value.startswith("{") or not is_caption(without_mnemonic(value)):
         continue
      line = src.count("\n", 0, m.start()) + 1
      if "<!-- no-loc -->" in lines[line - 1]:
         continue
      out.append((line, value))
   return out


def untranslated_by_file():
   result = {}
   for path in source_files():
      src = read(path)
      hits = xaml_untranslated(src) if path.endswith(".xaml") else cs_untranslated(src)
      if hits:
         result[rel(path)] = hits
   return result


# A sentence split around a value: a translated start, the value, and the sentence's
# own closing punctuation as a literal - L("The address must be a name followed by @")
# + domain + ".". Its translator sees the start alone and cannot move the value: the
# German put its verb last and read "... gefolgt von @ seinexample.com.", and the
# quote that closed a group's name stayed English where French and Russian opened it
# with «. Written whole - F("The address must be a name followed by @{0}.", domain) -
# it can be. A literal followed by more text (L("Next step") + ". " + step) joins two
# texts and is not this.
SPLIT_SENTENCE = re.compile(
   r"(?<![\w.])[LF]\(\s*" + CS_LITERAL + r"[^;]*?\)\s*\+\s*[^;\"+]+?\s*\+\s*\"([^\"\\\n\w\s]+)\"(?=\s*[);,])")


def cs_split_sentences(src):
   """(line, code) for every translated text joined to a value and then to its closing punctuation."""
   return [(src.count("\n", 0, m.start()) + 1, " ".join(m.group(0).split())) for m in SPLIT_SENTENCE.finditer(src)]


# A count joined to a text with a space: rows_.Count + " " + noun, or a helper's
# n + " " + (n == 1 ? one : many). The noun reaches its translator alone, and what
# the code then does to it is English: the count pill read "3 Sicherheitsgruppes"
# in German and "5 ルールs" in Japanese until 6.3.7, and "2 Scanner" lost its
# verb in the anti-virus verdict. A whole text for each form - Plural(n,
# "{0} item", "{0} items") - lets each language count its own way. A name joined
# to a number (protocol + " " + port) is not a count and is not this.
COUNT_JOINED = re.compile(r"(?:\.Count|\.Length|\b(?:count|total|n))\s*\+\s*\"\s\"\s*\+")


def cs_counts_joined(src):
   """(line, code) for every count joined to a text with a space."""
   return [(src.count("\n", 0, m.start()) + 1, " ".join(src[m.start():m.end() + 40].split()))
           for m in COUNT_JOINED.finditer(src)]


# A translated text joined to another with a space literal: said + " " + F("Left where
# they were: ..."), F("Windows could not start the install: {0}", reason) + " " +
# L("Nothing was changed."), " " + F("Found: {0}") added to a verdict, or
# (note.Length > 0 ? " " : "") + Plural(...). Japanese and Chinese end a sentence with
# a full stop that carries its own room, and showed a space after it - "MX。 ホスト/名前"
# on the DNS records page (review finding F283). Loc.Sentences joins whole sentences the
# way each language does. A name joined to a number (protocol + " " + port) touches no
# translated text and is not this.
SPACE_JOINED_CALL = r"(?:Loc\.)?(?:L|F|Plural|PluralF)\("
SPACE_BEFORE_CALL = re.compile(r'"\s"\s*\+\s*(?:\(\s*[^;()?"]*\?\s*)?' + SPACE_JOINED_CALL
                               + r'|"\s"\s*:\s*""\s*\)\s*\+\s*' + SPACE_JOINED_CALL)
TRANSLATING_CALL = re.compile(r"(?<![\w.])" + SPACE_JOINED_CALL)
SPACE_AFTER_CALL = re.compile(r'(?:\s*\))*\s*\+\s*"\s"')


def cs_space_joins(src):
   """(line, code) for every translated text joined to another with a space literal."""
   starts = [m.start() for m in SPACE_BEFORE_CALL.finditer(src)]
   for m in TRANSLATING_CALL.finditer(src):
      args = call_arguments(src, m.end())
      if args and SPACE_AFTER_CALL.match(src, args[-1][0] + len(args[-1][1]) + 1):
         starts.append(m.start())
   found = []
   for at in sorted(set(starts)):
      begin = src.rfind("\n", 0, at) + 1
      end = src.find("\n", at)
      line = src[begin:len(src) if end < 0 else end]
      if "// no-loc" in line or line.lstrip().startswith(("//", "*")):
         continue
      found.append((src.count("\n", 0, at) + 1, " ".join(line.split())))
   return found


# --- main -------------------------------------------------------------------------

def mnemonic_key(caption):
   """The Alt key a caption carries, by the rule MnemonicText.Parse follows, or None."""
   i, shown = 0, []
   key = None
   while i < len(caption):
      c = caption[i]
      if c != "_":
         shown.append(c)
         i += 1
         continue
      if i + 1 < len(caption) and caption[i + 1] == "_":
         shown.append("_")
         i += 2
         continue
      if key is None and i + 1 < len(caption) and not caption[i + 1].isspace():
         key = caption[i + 1].upper()
      else:
         shown.append("_")
      i += 1
   return key



# The controls whose Text the user edits and the program then USES. A caption on
# one of these is not a caption; it is a starting value, and a translated one is
# a value no server ever meant. TextBlock and Label are absent on purpose: their
# Text is the caption.
VALUE_CONTROLS = ("TextBox", "PasswordBox", "PasswordField", "NumberBox",
                  "AutoSuggestBox", "ComboBox")

# Text="{loc:L '...'}" on one of those, in XAML.
XAML_VALUE = re.compile(
   r"<(?:[A-Za-z0-9_]+:)?(" + "|".join(VALUE_CONTROLS) + r")\b[^>]*?\bText\s*=\s*\"\{loc:L\b",
   re.S)

# An object initialiser that sets Text, with whatever type the declaration or the
# new expression names. C#'s target-typed `new()` carries no type of its own, so
# the declaration on its left is the only place the type appears:
#     private readonly TextBox box_ = new() { Text = ... };
CS_INITIALISER = re.compile(
   r"(?P<declared>[A-Za-z_][A-Za-z0-9_.]*)\s+[A-Za-z_]\w*\s*=\s*"
   r"new(?:\s+(?P<constructed>[A-Za-z_][A-Za-z0-9_.]*))?\s*(?:\([^()]*\))?\s*"
   r"\{(?P<initialiser>[^{}]*)\}",
   re.S)
CS_TEXT_FROM_CATALOGUE = re.compile(r"\bText\s*=\s*[LF]\(")


def names_value_control(*type_names):
   """True when any of these names IS one of the value controls.

   The comparison is on the last segment, so Wpf.Ui.Controls.TextBox counts and
   so does a bare TextBox. RichTextBox and TextBlock do not, and must not: the
   first has no Text to set and the second is a caption.
   """
   for name in type_names:
      if not name:
         continue
      if name.split(".")[-1] in VALUE_CONTROLS:
         return True
   return False


def translated_values():
   """Every place a VALUE is filled from the catalogue: (file, line, snippet).

   Issue #156: ConnectView.xaml set the user-name box's Text from the catalogue,
   so choosing Chinese put 管理员 where an account name belongs and the sign-in
   failed. The catalogues were right and the binding was wrong, which is why this
   is checked in the source rather than in the translations.
   """
   found = []
   for path in source_files():
      body = read(path)

      if path.lower().endswith(".xaml"):
         for match in XAML_VALUE.finditer(body):
            line = body.count("\n", 0, match.start()) + 1
            snippet = " ".join(body[match.start():match.start() + 90].split())
            found.append((rel(path), line, snippet))
         continue

      for match in CS_INITIALISER.finditer(body):
         if not CS_TEXT_FROM_CATALOGUE.search(match.group("initialiser")):
            continue
         if not names_value_control(match.group("declared"), match.group("constructed")):
            continue
         line = body.count("\n", 0, match.start()) + 1
         snippet = " ".join(body[match.start():match.start() + 90].split())
         found.append((rel(path), line, snippet))

   return found


def self_test():
   """The caption rules and the white-space rule against lines and values made to
   pass and to fail them: each failing case is a defect the Control Panel shipped."""
   failures = 0
   captions = {
      # (source, caption expected): an unmarked caption is reported, a marked one,
      # a key and a non-caption are not
      'status_.Text = "Copied.";': True,
      'status_.Text = L("Copied.");': False,
      'Say_(StatusLevel.Good, "Deleted.");': True,
      'Summary_(StatusLevel.Information, apply ? "Applying..." : L("Reading the directory..."));': True,
      'public const string SectionPrefix = "Key.";': False,
      'private const string Prefix = "Help.";': False,
      'string host = "example.com";': False,
      'location = location.Length > 0 ? "in " + location : "";': True,
      'location = location.Length > 0 ? F("in {0}", location) : "";': False,
      'return F("Sent to {0}.", string.Join(" and ", told));': True,
      'AutomationProperties.SetName(name, style.Name + ", " + style.PatternText + " line");': True,
      'int position = diagnostic.IndexOf("data ", StringComparison.Ordinal);': False,
      'string id = "case " + Id + ":"; // no-loc': False,
      'title_.Text = "Help";': True,
      'var up = new Button { Content = "U_p" };': True,
      'var up = new Button { Content = L("U_p") };': False,
      'string key = "MAX_SIZE";': False,
      'string key = "Log_File";': False,
   }
   for line, expected in captions.items():
      found = bool(cs_untranslated(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as an untranslated caption")
         failures += 1

   xaml = {
      '<ui:Button x:Name="ActionUpButton" Content="U_p" />': True,
      '<ui:Button Content="{loc:L \'U_p\'}" />': False,
      '<TextBlock Text="example.com" />': False,
   }
   for line, expected in xaml.items():
      found = bool(xaml_untranslated(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as an untranslated XAML caption")
         failures += 1

   edges = {
      # (English, translation, language): the edges reported as lost
      ("Could not copy: ", "Kopieren nicht möglich:", "de"): ["end"],
      ("Could not copy: ", "Kopieren nicht möglich: ", "de"): [],
      ("Could not copy: ", "Impossible de copier :", "fr"): ["end"],
      (" (published {0})", "(veröffentlicht {0})", "de"): ["start"],
      (" (published {0})", " (veröffentlicht {0})", "de"): [],
      ("Last update: {0}. ", "最終更新: {0}。", "ja"): [],
      ("Searching ", "正在搜索", "zh-Hans"): [],
      ("Store: ", "ストア:", "ja"): ["end"],
      ("Members - ", "メンバー -", "ja"): ["end"],
      ("Web  ", "Web", "zh-Hans"): ["end"],
      ("Copy", "Kopiera", "sv"): [],
   }
   for (name, value, tag), expected in edges.items():
      found = lost_edge_space(name, value, tag)
      if found != expected:
         print(f"FAIL: {tag} {name!r} -> {value!r}: lost {found}, expected {expected}")
         failures += 1

   added = {
      # (English, translation): the edges where the translation adds white space
      ("external POP3 fetch account has", " ulkoisella POP3-noutotilillä on"): ["start"],
      ("Open the member list of '", "Ouvrir la liste des membres de « "): ["end"],
      ("Open the member list of '", "Ouvrir la liste des membres de «\u00a0"): ["end"],
      ("Name", " Nom "): ["start", "end"],
      ("Could not copy: ", "Kopieren nicht möglich: "): [],
      (" (published {0})", " (veröffentlicht {0})"): [],
      ("Copy", "Kopiera"): [],
   }
   for (name, value), expected in added.items():
      found = added_edge_space(name, value)
      if found != expected:
         print(f"FAIL: {name!r} -> {value!r}: added {found}, expected {expected}")
         failures += 1

   splits = {
      # (source, reported): a sentence split around a value and closed by a literal is
      # reported; a sentence written whole, and two texts joined, are not
      'DialogFields.ShowError(row_, L("The address must be a name followed by @") + domainName_ + ".");': True,
      'status_.Text = L("Searching ") + domain + "…";': True,
      'membersHint_.Text = L("Open the member list of \'") + selectedGroupName_ + "\'.";': True,
      'content.Children.Add(Paragraph(L("... claiming to be from ") + domain.Name + ".",\n   Typography.Caption));': True,
      'text = F("{0} of {1} in ", a, b) + place + ")";': True,
      'membersHint_.Text = F("Open the member list of \'{0}\'.", selectedGroupName_);': False,
      'status_.Text = L("Could not copy: ") + error;': False,
      'spoken = L("Next step") + ". " + step;': False,
      'notes.Add(L("Set by the environment") + " (" + environment + ")");': False,
      'line = L("Total") + ": " + count;': False,
      'ratio = L("Delivered ") + done + "/" + total;': False,
      'status_.Text = L("Last run: ") + when + " ";': False,
      'path = folder + "\\\\" + name + ".";': False,
   }
   for line, expected in splits.items():
      found = bool(cs_split_sentences(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as a sentence split around a value")
         failures += 1

   counts = {
      # (source, reported): a count joined to a text with a space is reported; one
      # text per number, and a name joined to a number, are not
      'countBadge_.Text = rows_.Count + " " + Pluralize(spec_.ItemNoun);': True,
      'private static string Plural_(int count, string one, string many) => count + " " + (count == 1 ? one : many);': True,
      'private static string Count(int n, string singular, string plural) =>\n   n + " " + (n == 1 ? singular : plural);': True,
      'pill.Text = total + " " + L("messages");': True,
      'countBadge_.Text = rows_.Count == 1 ? L(spec_.CountOne) : F(spec_.CountMany, rows_.Count);': False,
      'count_.Text = count == 1 ? L("1 public folder.") : F("{0} public folders.", count);': False,
      'string label = ProtocolName(protocol) + " " + address + ":" + port;': False,
      'line = L("Total") + ": " + count;': False,
      'return v.Listener.Protocol + " " + v.Listener.Port;': False,
   }
   for line, expected in counts.items():
      found = bool(cs_counts_joined(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as a count joined to a text")
         failures += 1

   joins = {
      'Status_(level, said + " " + F("Left where they were: {0}. The first reason: {1}", failed, why));': True,
      'return F("Windows could not start the install: {0}", reason) + " " + L("Nothing was changed.");': True,
      'text += " " + F("The permission is inherited from {0}.", from);': True,
      'return " " + (records.Count > 1 ? F("Found: {0} (and {1} more)", first, n) : F("Found: {0}", first));': True,
      'note += (note.Length > 0 ? " " : "") + Plural(n, "{0} value was not read.", "{0} values were not read.");': True,
      'x = y + (next != null ? " " + F("Next step: {0}.", title) : "");': True,
      'Label((mx ? L("Record type: MX.") : L("Record type: TXT.")) + " " + note);': True,
      'x = Text(L("Name"), value) + " " + rest;': False,
      'Label(L("Record type: MX. Host/Name") + " " + note);': True,
      'x = Loc.F("Saved at {0}.", when) + " " + rest;': True,
      'policyNote.Text = Sentences(caveat, mxNote, extra);': False,
      'Status_(level, Sentences(said, F("Left where they were: {0}.", failed)));': False,
      'string label = ProtocolName(protocol) + " " + address + ":" + port;': False,
      'line = L("Total") + ": " + count;': False,
      'string id = L("x") + " " + y; // no-loc': False,
      '// said + " " + F("Left where they were: {0}.", failed)': False,
   }
   for line, expected in joins.items():
      found = bool(cs_space_joins(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as a translated text joined with a space")
         failures += 1

   plurals = {
      # (source, the forms read): a form that is not a literal reads as None
      'Status_(Plural(rows.Count, "{0} message held.", "{0} messages held."));':
         [("{0} message held.", "{0} messages held.")],
      'x = PluralF(Math.Max(a, b), "{0} of {1} change failed.", "{0} of {1} changes failed.", a, F("x {0}", b));':
         [("{0} of {1} change failed.", "{0} of {1} changes failed.")],
      'x = Loc.Plural(list[i].Count, "Say \\"{0}\\" once.", "Say \\"{0}\\" twice.");':
         [('Say "{0}" once.', 'Say "{0}" twice.')],
      'x = Plural(n, spec_.CountOne, "{0} items");': [(None, "{0} items")],
      'public static string Plural(long count, string one, string other)': [],
      'string f = PluralForm(n, one, other);': [],
      'x = Plural(c == \'(\' ? 1 : 2, "{0} bracket", "{0} brackets");': [("{0} bracket", "{0} brackets")],
   }
   for line, expected in plurals.items():
      found = [tuple(text for _, text in call["forms"]) for call in plural_calls(line + "\n")]
      if found != expected:
         print(f"FAIL: {line!r} reads as the plural calls {found}, expected {expected}")
         failures += 1
   for line in ('Status_(Plural(rows.Count, "{0} message held.", "{0} messages held."));',
                'x = PluralF(n, "Saved {1}: {0} setting waits.", "Saved {1}: {0} settings wait.", n, time);'):
      if cs_untranslated(line + "\n"):
         print(f"FAIL: the forms of {line!r} are reported as untranslated captions")
         failures += 1

   for text, expected in {"{0} message(s) waiting for delivery.": True, "{0} blocked address(es)": True,
                          "SMTP server minimum timeout (s)": False, "{0} messages waiting.": False}.items():
      if bool(PAREN_PLURAL.search(text)) != expected:
         print(f"FAIL: {text!r} is {'not ' if expected else ''}an English plural in brackets")
         failures += 1
   for text, expected in {"{0} expiré(s) et désormais refusé(s)": ["expiré(s)", "refusé(s)"],
                          "{0} caducada(s) y ahora rechazada(s)": ["caducada(s)", "rechazada(s)"],
                          "{0}. {1} configuré(s), aucun actif, la vérification ne s'exécute donc pas.": ["configuré(s)"],
                          "jej {0} kont(a)": ["kont(a)"], "{0} Regel(n) und {1} Einstellung(en)": ["Regel(n)", "Einstellung(en)"],
                          "{0} правил(а)": ["правил(а)"], "{0} записей": [], "{0} expirées et désormais refusées": [],
                          "Otevřel(a)": [], "Otworzył(a)": [], "出荷時の設定を保持(_S)": [], "Délai minimal (s)": []}.items():
      if catalogue_paren_plurals(text) != expected:
         print(f"FAIL: {text!r} reads as the plural endings in brackets {catalogue_paren_plurals(text)}, expected {expected}")
         failures += 1

   for text, expected in {"MX - which server takes the mail for ": True, " (inactive)": True,
                          " and the policy verifies nothing.": True, "Before you copy - ": True,
                          "\n\nSave this weak password anyway?": True, "Web  ": True,
                          "Could not read the list: ": False, "Scope: ": False,
                          "MX - which server takes the mail for {0}": False, "It is safe to enable signing.": False}.items():
      if glued_at_edge(text) != expected:
         print(f"FAIL: {text!r} is {'not ' if expected else ''}reported as glued at an edge")
         failures += 1

   choices = {
      'Status_(total == 1 ? L("1 message held.") : F("{0} messages held.", total));': True,
      'count_.Text = (count == 1) ? L("1 public folder.") : F("{0} public folders.", count);': True,
      'countBadge_.Text = rows_.Count == 1 ? L(spec_.CountOne) : F(spec_.CountMany, rows_.Count);': False,
      'return seconds == 1 ? L("last second") : Plural(seconds, "last {0} second", "last {0} seconds");': False,
      'Status_(Plural(total, "{0} message held.", "{0} messages held."));': False,
   }
   for line, expected in choices.items():
      found = bool(cs_pair_choices(line + "\n"))
      if found != expected:
         print(f"FAIL: {line!r} is {'reported' if found else 'not reported'} as a count's forms written as a choice")
         failures += 1

   print(f"self-test: {failures} failure(s)")
   return 1 if failures else 0


def main():
   if hasattr(sys.stdout, "reconfigure"):
      sys.stdout.reconfigure(encoding="utf-8")
   if "--self-test" in sys.argv[1:]:
      return self_test()
   write = "--write" in sys.argv
   inventory = "--inventory" in sys.argv
   problems = []

   texts = marked_texts()
   pairs, plural_problems = plural_pairs()
   problems.extend(plural_problems)

   for path, line, snippet in translated_values():
      problems.append(
         f"{path}:{line}: this fills an input control's VALUE from the catalogue, so every "
         f"language starts it with a different string: {snippet!r}. A value is not a caption - "
         f"issue #156 was a user name translated to 管理员, which is no account's name. Put the "
         f"hint in PlaceholderText and leave Text a literal.")

   wanted = sorted(texts)
   # MSBuild's resource compiler treats names case-insensitively (MSB3568), so
   # two marked texts that differ only in case would be one resource
   folded = {}
   for t in wanted:
      folded.setdefault(t.lower(), []).append(t)
   for group in folded.values():
      if len(group) > 1:
         problems.append("these marked texts differ only in case and the resource compiler treats them as one; reword one: "
                         + ", ".join(repr(t) + " (" + texts[t][0] + ")" for t in group))
   os.makedirs(RESOURCES, exist_ok=True)

   if write:
      write_resx(NEUTRAL, [(t, t) for t in wanted], NEUTRAL_COMMENT)
      for tag, path in language_files():
         existing = dict(read_resx(path))
         comment = read(path)
         m = re.search(r"<!-- (?!Copyright)(.*?) -->", comment, re.S)
         items = []
         for t in wanted:
            items.append((t, existing.get(t, "")))
            if tag in FEW_LANGUAGES and t in pairs:
               items.append((t + FEW, existing.get(t + FEW, "")))
         write_resx(path, items,
                    (m.group(1).strip() if m else f"The {tag} catalogue. Keys are the English text; values are the translation.").replace("--", "-"))

   neutral = read_resx(NEUTRAL)
   if [n for n, _ in neutral] != wanted or any(n != v for n, v in neutral):
      missing = [t for t in wanted if t not in dict(neutral)]
      stale = [n for n, _ in neutral if n not in texts]
      problems.append(f"Resources/Strings.resx is behind the source ({len(missing)} marked text(s) missing, "
                      f"{len(stale)} stale). Run build/check-localisation.py --write and commit the result.")
      for t in missing[:10]:
         problems.append(f"  missing: {t!r} ({texts[t][0]})")
      for t in stale[:10]:
         problems.append(f"  stale:   {t!r}")

   for text in wanted:
      if PAREN_PLURAL.search(text):
         problems.append(f"{texts[text][0]}: {text!r} puts an English plural ending in brackets, which every language "
                         f"then has to translate; write both forms whole with Plural(n, one, other)")
      if glued_at_edge(text):
         problems.append(f"{texts[text][0]}: {text!r} has white space at an edge, where the source glues it to another "
                         f"text, so its translator sees a piece of a sentence; write it whole, F(\"... {{0}} ...\", value), "
                         f"or join whole sentences with Sentences(...)")

   coverage = {}
   for tag, path in language_files():
      entries = read_resx(path)
      # a plural's form for two to four, in the languages that have one
      forms = {p + FEW: p for p in pairs} if tag in FEW_LANGUAGES else {}
      known = set(wanted) | set(forms)
      required = len(known)
      translated = 0
      for name, value in entries:
         if name not in known:
            problems.append(f"Strings.{tag}.resx: {name!r} is not a text the source marks any more; drop it (--write does)")
            continue
         if not value.strip():
            continue
         translated += 1
         if set(PLACEHOLDER.findall(forms.get(name, name))) != set(PLACEHOLDER.findall(value)):
            problems.append(f"Strings.{tag}.resx: the placeholders of {name!r} differ in {value!r}")
         for word in catalogue_paren_plurals(value, name):
            problems.append(f"Strings.{tag}.resx: {name!r} is translated {value!r}, whose {word!r} puts a plural ending in "
                            f"brackets; write each form whole, the English as Plural(n, one, other) where it has a bare count")
         if mnemonic_key(name) is not None and mnemonic_key(value) is None:
            problems.append(f"Strings.{tag}.resx: {name!r} carries an Alt key and its translation {value!r} does not")
         for edge in lost_edge_space(name, value, tag):
            problems.append(f"Strings.{tag}.resx: {name!r} has white space at its {edge}, where the source joins it to "
                            f"another text, and its translation {value!r} does not")
         for edge in added_edge_space(name, value):
            problems.append(f"Strings.{tag}.resx: {name!r} has no white space at its {edge} and its translation {value!r} "
                            f"has: the source joins the next text to it as it is, so the space is one too many")
      coverage[tag] = (translated, required)
      if tag in COMPLETE and translated < required:
         untranslated = [n for n in sorted(known) if not dict(entries).get(n, "").strip()]
         problems.append(f"Strings.{tag}.resx claims to be complete and lacks {len(untranslated)} translation(s):")
         for n in untranslated[:20]:
            problems.append(f"  {n!r}")

   remaining = untranslated_by_file()
   for path in sorted(remaining):
      if path in LOCALISED:
         for line, text in remaining[path]:
            problems.append(f"{path}:{line}: untranslated caption {text!r} in a localised file")
   for path in source_files():
      if path.endswith(".cs"):
         for line, code in cs_split_sentences(read(path)):
            problems.append(f"{rel(path)}:{line}: a sentence split around a value, its end a literal: {code!r}; "
                            f"write it whole, F(\"... {{0}}.\", value), so that a translator can place the value")
         for line, code in cs_counts_joined(read(path)):
            problems.append(f"{rel(path)}:{line}: a count joined to a text with a space: {code!r}; write the sentence "
                            f"whole for each form, Plural(n, \"{{0}} item\", \"{{0}} items\"), so that each language counts its own way")
         if rel(path) in LOCALISED:
            for line, code in cs_space_joins(read(path)):
               problems.append(f"{rel(path)}:{line}: a translated text joined to another with a space literal: {code!r}; "
                               f"join whole sentences with Sentences(a, b), which puts no space after a Japanese or "
                               f"Chinese full stop, or write the text whole, F(\"... {{0}} ...\", value)")
         for line, code in cs_pair_choices(read(path)):
            problems.append(f"{rel(path)}:{line}: a count's forms written as a choice of two: {code!r}; Polish, Czech, "
                            f"Russian and Ukrainian say 2 and 5 apart, so write Plural(n, \"{{0}} item\", \"{{0}} items\")")
   for path in sorted(LOCALISED):
      if not os.path.exists(os.path.join(ROOT, path)):
         problems.append(f"LOCALISED names {path}, which does not exist")

   left = {p: len(h) for p, h in remaining.items() if p not in LOCALISED}
   print(f"{len(wanted)} texts in the catalogue; " +
         ", ".join(f"{tag}: {done}/{total} translated" for tag, (done, total) in sorted(coverage.items())))
   print(f"{len(LOCALISED)} file(s) fully localised; {sum(left.values())} untranslated caption(s) left in {len(left)} file(s)")
   if inventory:
      for path in sorted(remaining, key=lambda p: (-len(remaining[p]), p)):
         print(f"  {path}: {len(remaining[path])}" + ("  (localised)" if path in LOCALISED else ""))
         for line, text in remaining[path]:
            print(f"    {line}: {text[:110]!r}")
   elif left:
      for path in sorted(left, key=lambda p: (-left[p], p))[:12]:
         print(f"  {left[path]:4d} {path}")

   if problems:
      for p in problems:
         print("  " + p)
      print(f"FAIL: {len(problems)} problem(s)")
      return 1
   print("OK    the catalogues match the source and every localised file is fully marked")
   return 0


if __name__ == "__main__":
   sys.exit(main())
