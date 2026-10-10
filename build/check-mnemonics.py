#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every static caption in the Control Panel carries an Alt-key mnemonic, and no
two captions that can be on screen together share one.

Half a set of access keys is worse than none - a keyboard user learns that Alt
does something on one page and then finds it does nothing on the next - so this
is checked rather than hoped. The rule is WPF's own: a single underscore in a
caption marks the key ("_Save changes" is Alt+S), a doubled underscore is a
literal one. Buttons and check boxes read that through their templates; the
captions above editors go through Views/Mnemonic.cs, which underlines the
letter and registers the key for the editor.

What is checked
  * XAML views: every Button, CheckBox and RadioButton with a literal Content,
    and every FieldRow's Label - the caption above an editor, whose key puts
    the keyboard in the editor - outside a DataTemplate (a caption repeated
    per row cannot have a unique key; the rows are reached with the arrow
    keys, and their verbs with Enter, Delete and a context menu, whose
    captions do carry keys - DomainsRowVerbsTests holds the domains page's).
    The scope for uniqueness is the file: a view is one page, all of it on
    screen at once.
  * A page whose root says AccessKeyScope.ByCard="True" (Views/AccessKeyScope)
    answers a key first in the card that holds the keyboard: each of its
    cards is a scope of its own, and a caption outside every card - the
    page header's - must not share a key with any card's, as a tabbed
    dialog's constructor must not share one with any tab. The Backup &
    restore page needs it: its five cards hold more captions than there are
    letters.
  * Code-built views and dialogs: every Button/CheckBox/RadioButton initialised
    with a literal Content, every MakeButton("..."), every Label("...", editor)
    and Labelled*_("...", editor) caption, every FieldRow initialised with a
    literal Label and every caption a field helper puts on a FieldRow
    (DialogFields.Field, FormField_, Captioned), and every "<something>.Content
    = "..."" assignment. The scope is the method that places the control,
    because a dialog's tabs are built one per method and only one tab is
    visible at a time; a field is placed by the method that first mentions
    it. Keys in the constructor (the Save button under every tab) must not
    collide with any method's keys. A caption the code behind a XAML page
    sets on one of the page's named buttons - the server status page's Pause
    and Resume - joins the XAML page's scope, where it is on screen.
  * The button a confirmation names - Dialogs.Confirm(text, title, "Re_move")
    - and the fixed answers of Dialogs' own questions (new Choice("_Save", ...)
    for unsaved changes). A confirmation is a window of its own, its button
    beside Cancel, so each is a scope of its own; it is held to one verb, one
    key, which is where a page's Re_move opened a box whose Remove was Alt+R.
  * Exempt: Cancel, OK and Close (Escape and Enter are their keys, by platform
    convention), a message box's Yes and No, the "…" browse buttons, and
    captions that come from data.
  * A read-only row - DialogFields.ReadOnlyField("...", value), the account
    dialog's Last sign-in and Zero-access mailbox - labels a value, not an
    editor, so there is nothing for a key to put the keyboard in: its caption
    carries no key, in English or in any catalogue (review finding F292, where
    the two were "Last l_ogon" and "_Zero-access mailbox").
  * One verb, one key: a button, check box or radio button carries the same
    key wherever its caption appears in English - Refresh is Alt+R on every
    page, never Alt+F on the pages where R was taken - so that a key learnt
    on one page works on the next. (Every language then follows: one English
    caption is one catalogue entry.) A field's caption names a field of its
    own form, and its key is chosen among that form's.
  * A caption may be wrapped for translation - L("_Save changes") in code,
    {loc:L '_Save changes'} in XAML - and is read through the wrapper. Every
    language catalogue (Resources/Strings.<culture>.resx) is then checked the
    same way: each translated caption carries its own key, chosen for that
    language, and no two that share a scope share one.
  * A shared helper that places a caption on every page that calls it
    (Services/ReadFailure's Try again button) is scanned once for coverage, and
    its captions join the scope of every calling page - the class the call is
    in, and the view file as well for a XAML page - so a key it carries is
    checked against each of those pages. Such a caption is on screen with
    pages it never knows about, and the one in the tree carries no key at all.
  * A bar of the window's own (MainWindow.xaml's pending bar) is on screen
    under every page the window shows, in the same access-key scope, so its
    captions join the scope of every one of those pages - the classes
    MainWindow.xaml.cs's pageFactories_ make, in each of their files - as a
    shared helper's do. The pending bar's two carry no key.

Usage: python3 build/check-mnemonics.py [--list]
Exit status 0 when every caption is covered and every scope is collision-free.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hmailserver", "source", "Tools", "ControlPanel")
EXEMPT = {"Cancel", "OK", "Close", "…", "...",
          # A message box's answers (Views/Dialogs), Enter and Escape as for OK and
          # Cancel; they carry no key in any language.
          "Yes", "No",
          # The one action in a read-failure notice (Services/ReadFailure), on some
          # twenty pages and reached by Tab. No letter is free of a collision on every
          # page that shows it - in English T is Require TLS for auth, Add action, Take
          # the role away, Restore settings and Test connection - and WPF, given two
          # enabled targets for one key, moves the focus between them and presses
          # neither, so the caption carries no key in any language.
          "Try again",
          # The pending bar's two buttons (MainWindow.xaml), on screen with every
          # page until the settings it counts are applied - a restart's for as long
          # as the service is not restarted. No letter of either is free on every
          # page: in English R, E, S, T, V and C of Restart the service are each on
          # six pages or more and A of Apply now on eighteen, and the bar is in the
          # window's one access-key scope (WINDOW_BARS), so neither carries a key in
          # any language and both are reached by Tab.
          "Apply now", "Restart the service…",
          # The paused server's bar (MainWindow.xaml, review finding F019), on screen
          # with every page while the server is paused - Server status among them,
          # whose own R_esume the mail server takes E - and in the window's one
          # access-key scope (WINDOW_BARS), so its one button carries no key either:
          # it is Server status's caption, which does the same, without its key.
          "Resume the mail server"}

# Shared helpers, each with the call that places its captions on a page: every
# caption of the helper joins the scope of every page whose code makes the call.
SHARED = {os.path.join("Services", "ReadFailure.cs"): "ReadFailure.Show("}

# The window's own bars, each the element (by x:Name) of MainWindow.xaml that
# holds one: shown under whichever page the window shows, and in the same
# access-key scope, since nothing in the window sets one of its own - WPF, given
# two enabled targets for one key, moves the focus between them and presses
# neither. A bar's captions join the scope of every page MainWindow.xaml.cs
# makes (its pageFactories_), as a shared helper's join the pages that call it;
# a dialog is a window of its own and is not one of them.
WINDOW_BARS = {"MainWindow.xaml": ["PendingBar", "PausedBar"]}


def parse(caption):
    """The display text and the key index, by the rule MnemonicText.Parse follows."""
    text, key = [], -1
    i = 0
    while i < len(caption):
        c = caption[i]
        if c != "_":
            text.append(c)
            i += 1
            continue
        if i + 1 < len(caption) and caption[i + 1] == "_":
            text.append("_")
            i += 2
            continue
        if key < 0 and i + 1 < len(caption) and not caption[i + 1].isspace():
            key = len(text)
        else:
            text.append("_")
        i += 1
    shown = "".join(text)
    return shown, (shown[key].upper() if key >= 0 else None)


def unescape_cs(literal):
    return literal.replace("\\u2026", "…").replace('\\"', '"')


class Caption:
    def __init__(self, file, line, scope, raw, class_name=None, page=None, via=None, target=None, field=False):
        self.file, self.line, self.scope, self.raw = file, line, scope, raw
        self.class_name = class_name
        # The control a "<target>.Content = ..." assignment sets the caption on.
        self.target = target
        # A field's caption (a FieldRow's label), as opposed to a verb or an option.
        self.field = field
        # The file whose scope the caption joins: its own, or for a shared helper's
        # caption the page the helper put it on, with the call that did - (file,
        # line, call) - in via.
        self.page = page or file
        self.via = via
        self.text, self.key = parse(raw)


def location(c):
    """Where a problem with the caption is reported: the caption, or for a shared
    helper's caption the call that put it on the page."""
    if c.via:
        return f"{os.path.relpath(c.via[0], ROOT)}:{c.via[1]}"
    return f"{os.path.relpath(c.file, ROOT)}:{c.line}"


def describe(c):
    """The caption in a problem, with where it comes from when that is a shared helper."""
    if c.via:
        return f"\"{c.raw}\" ({os.path.relpath(c.file, ROOT)} line {c.line}, placed by {c.via[2]})"
    return f"\"{c.raw}\" (line {c.line})"


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def innermost(ranges, pos):
    """The smallest (name, start, end) range around pos, or None."""
    hits = [(name, a, b) for name, a, b in ranges if a <= pos <= b]
    return min(hits, key=lambda r: r[2] - r[1]) if hits else None


def xaml_captions(path):
    with open(path, encoding="utf-8") as handle:
        src = handle.read()
    # DataTemplate ranges
    inside = []
    depth = 0
    for m in re.finditer(r"<(/?)DataTemplate\b", src):
        if m.group(1):
            depth -= 1
            if depth == 0:
                inside[-1] = (inside[-1][0], m.end())
        else:
            if depth == 0:
                inside.append((m.start(), None))
            depth += 1

    def templated(pos):
        return any(a <= pos < (b or len(src)) for a, b in inside)

    out = []
    for m in re.finditer(r'<(?:ui:)?(?:Button|CheckBox|RadioButton|ToggleButton)\b[^>]*?\bContent="([^"]*)"', src, re.S):
        if templated(m.start()):
            continue
        scope, class_name = xaml_scope(path, src, m.start())
        out.append(Caption(path, line_of(src, m.start()), scope, unwrap_xaml(m.group(1)), class_name))
    # the caption above an editor, whose key puts the keyboard in the editor
    for m in re.finditer(r'<scaffold:FieldRow\b[^>]*?\bLabel="([^"]*)"', src, re.S):
        if templated(m.start()):
            continue
        scope, class_name = xaml_scope(path, src, m.start())
        out.append(Caption(path, line_of(src, m.start()), scope, unwrap_xaml(m.group(1)), class_name, field=True))
    return out


def card_spans(src):
    """(start, end) of every scaffold:Card element of a XAML page whose root says
    AccessKeyScope.ByCard="True", by counting its opening and closing tags; none
    for any other page."""
    if not re.search(r'\bAccessKeyScope\.ByCard="True"', src):
        return []
    spans, stack = [], []
    for t in re.finditer(r"<(/?)scaffold:Card\b[^>]*?(/?)>", src, re.S):
        if t.group(1):
            if stack:
                spans.append((stack.pop(), t.end()))
        elif not t.group(2):
            stack.append(t.start())
    return spans


def xaml_scope(path, src, pos):
    """The scope of a XAML caption, and the scope it must not collide with: the
    file's for a page that is one access-key scope; on a page whose cards answer
    their own keys (AccessKeyScope.ByCard), the innermost card's, checked against
    the file's captions outside every card as a tabbed class's method is against
    its constructor."""
    base = os.path.basename(path)
    cards = [(a, b) for a, b in card_spans(src) if a <= pos < b]
    if not cards:
        return base, None
    a, _ = max(cards)
    return f"{base}#card@{line_of(src, a)}", base


def unwrap_xaml(value):
    """The English text inside {loc:L '...'}, or the value itself when it is not wrapped."""
    m = re.fullmatch(r"\{loc:L\s+'((?:[^'\\]|\\.)*)'\s*\}", value)
    return re.sub(r"\\(.)", r"\1", m.group(1)) if m else value


def body_ranges(src, header):
    """(name, start, end) for every body introduced by a header match, by brace counting."""
    ranges = []
    for m in re.finditer(header, src, re.M):
        name = m.group(1)
        depth, i = 0, m.end() - 1
        while i < len(src):
            c = src[i]
            if c == '"':  # skip string literals
                i += 1
                while i < len(src) and src[i] != '"':
                    i += 2 if src[i] == "\\" else 1
            elif c == "'":
                i += 1
                while i < len(src) and src[i] != "'":
                    i += 2 if src[i] == "\\" else 1
            elif src.startswith("//", i):
                i = src.find("\n", i)
                if i < 0:
                    i = len(src)
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    ranges.append((name, m.start(), i))
                    break
            i += 1
    return ranges


METHOD = r"^\s+(?:public|private|protected|internal)[^\n;=]*?\b(\w+)\s*\([^;{]*?\)\s*(?:where[^{]*)?\n\s*\{"
CLASS = r"^\s*(?:public|private|protected|internal|static|sealed|partial|abstract|\s)*class\s+(\w+)[^\n{]*\n\s*\{"
LITERAL = r"(?:L\()?\"((?:[^\"\\]|\\.)*)\"\)?"


def call_args(src, start):
    """The top-level arguments of the call whose opening parenthesis ends at start."""
    depth, args, current, i = 1, [], "", start
    while i < len(src) and depth > 0:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < len(src) and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            current += src[i:j + 1]
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                break
        elif c == "," and depth == 1:
            args.append(current.strip())
            current = ""
            i += 1
            continue
        current += c
        i += 1
    args.append(current.strip())
    return args


def cs_captions(path):
    with open(path, encoding="utf-8") as handle:
        src = handle.read()
    classes = body_ranges(src, CLASS)
    methods = body_ranges(src, METHOD)
    lines = src.split("\n")

    def scope_for(pos, field=None):
        """A class shown as a whole is one scope; a class with tabs is one scope per
        method, its constructor global to all of them. A field belongs to the method
        that first mentions it."""
        cls = innermost(classes, pos)
        class_name = cls[0] if cls else ""
        tabbed = bool(cls) and "new TabItem" in src[cls[1]:cls[2]]
        if not tabbed:
            return class_name, class_name
        method = innermost(methods, pos)
        if method is None and field:
            users = [(name, a, b) for name, a, b in methods
                     if cls[1] <= a <= cls[2] and re.search(r"\b" + re.escape(field) + r"\b", src[a:b])]
            # the method that builds the tab, when one does; otherwise the first that mentions it
            builders = [u for u in users if u[0].startswith("Build")]
            method = (builders or users or [None])[0]
        return (method[0] if method else class_name), class_name

    def add(out, pos, literal, field=None, end=None, target=None, labels=False, window=None):
        line = line_of(src, pos)
        # a caption repeated per row, card, record or setting cannot have a unique key, and says so
        # in a comment on the initialiser or on the literal; the rows are reached with the arrow keys
        span = "\n".join(lines[line - 1:line_of(src, end if end is not None else pos)])
        if re.search(r"//.*\bper (row|card|record|setting)\b", span):
            return
        if window:
            # a window of its own: a scope of its own, apart from the class that opens it
            scope, class_name = window, None
        else:
            scope, class_name = scope_for(pos, field)
        out.append(Caption(path, line, scope, unescape_cs(literal), class_name, target=target, field=labels))

    out = []
    kinds = r"(?:Button|CheckBox|RadioButton|ToggleButton)"
    # new Type { Content = "..." }
    for m in re.finditer(r"new\s+(?:Wpf\.Ui\.Controls\.)?" + kinds + r"\b(?:\(\))?\s*\{(?:[^{}]|\{[^{}]*\})*?\bContent\s*=\s*" + LITERAL, src, re.S):
        add(out, m.start(), m.group(1), end=m.end())
    # Type name = new() { Content = "..." }
    for m in re.finditer(r"\b" + kinds + r"\s+(\w+)\s*=\s*new\s*\(\)\s*\{(?:[^{}]|\{[^{}]*\})*?\bContent\s*=\s*" + LITERAL, src, re.S):
        add(out, m.start(), m.group(2), field=m.group(1), end=m.end())
    for m in re.finditer(r"\bMakeButton\(\s*" + LITERAL, src):
        add(out, m.start(), m.group(1))
    for m in re.finditer(r"\bLabel\(\s*" + LITERAL + r"\s*,\s*\w+\s*\)", src):
        add(out, m.start(), m.group(1), labels=True)
    for m in re.finditer(r"\bLabelled(?:Box|Combo)_\(\s*" + LITERAL, src):
        add(out, m.start(), m.group(1), labels=True)
    for m in re.finditer(r"\bLabelledCheck_\(\s*\w+\s*,\s*" + LITERAL, src):
        add(out, m.start(), m.group(1))
    # new FieldRow { Label = "..." }, and the helpers that build one from a caption
    for m in re.finditer(r"new\s+(?:Scaffold\.)?FieldRow\b(?:\(\))?\s*\{(?:[^{}]|\{[^{}]*\})*?\bLabel\s*=\s*" + LITERAL, src, re.S):
        add(out, m.start(), m.group(1), end=m.end(), labels=True)
    for m in re.finditer(r"\b(?:FormField_|Captioned|Field)\(\s*" + LITERAL + r"\s*,", src):
        add(out, m.start(), m.group(1), labels=True)
    # The button a confirmation names: Confirm's third argument, in the box's own window.
    for m in re.finditer(r"\b(?:Dialogs|MessageBox)\.Confirm\(", src):
        args = call_args(src, m.end())
        literal = re.fullmatch(LITERAL, args[2]) if len(args) > 2 else None
        if literal:
            add(out, m.start(), literal.group(1), window=f"Confirm@{line_of(src, m.start())}")
    # The fixed answers of Dialogs' own questions, one scope per method that asks one.
    for m in re.finditer(r"\bnew\s+Choice\(\s*" + LITERAL, src):
        method = innermost(methods, m.start())
        add(out, m.start(), m.group(1), window="Choice@" + (method[0] if method else "?"))
    # something.Content = "..." (a button or check box whose caption changes with state)
    for m in re.finditer(r"\b(\w+)\.Content\s*=\s*(?:[^;\"]*\?\s*)?" + LITERAL + r"(?:\s*:\s*" + LITERAL + r")?", src):
        for g in (m.group(2), m.group(3)):
            if g is not None:
                add(out, m.start(), g, target=m.group(1))
    return out


READ_ONLY_ROW = r"\bDialogFields\.ReadOnlyField\(\s*" + LITERAL


def read_only_captions(path):
    """The captions of a code-built view's read-only rows (DialogFields.ReadOnlyField),
    which label a value and so carry no key."""
    with open(path, encoding="utf-8") as handle:
        src = handle.read()
    return [Caption(path, line_of(src, m.start()), "read-only", unescape_cs(m.group(1)))
            for m in re.finditer(READ_ONLY_ROW, src)]


def read_only_problems(captions):
    """A read-only row's caption with a key, in English or in a catalogue."""
    import xml.etree.ElementTree as ET
    problems = [f"{location(c)}: \"{c.raw}\" captions a read-only row, which has no editor for a key to reach; "
                "take its mnemonic out" for c in captions if c.key is not None]
    resources = os.path.join(ROOT, "Resources")
    if not os.path.isdir(resources):
        return problems
    for name in sorted(os.listdir(resources)):
        m = re.fullmatch(r"Strings\.([A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*)\.resx", name)
        if not m:
            continue
        catalogue = {}
        for data in ET.parse(os.path.join(resources, name)).getroot().findall("data"):
            value = data.find("value")
            if value is not None and value.text:
                catalogue[data.get("name")] = value.text
        for c in captions:
            raw = catalogue.get(c.raw)
            if raw is not None and parse(raw)[1] is not None:
                problems.append(f"{location(c)}: [{m.group(1)}] \"{raw}\" captions a read-only row (translation of "
                                f"\"{c.raw}\"); take its mnemonic out")
    return problems


def placed_captions(own, call, files):
    """A shared helper's captions, copied onto every page whose code makes the call
    that places them: into the scope of the class the call is in, and for a XAML
    page into the view file's scope as well, since its captions are scoped by file.
    One copy per page and scope, however many calls the page makes."""
    out = []
    seen = set()
    for path in files:
        if not path.endswith(".cs"):
            continue
        with open(path, encoding="utf-8") as handle:
            src = handle.read()
        classes = body_ranges(src, CLASS)
        for m in re.finditer(re.escape(call), src):
            cls = innermost(classes, m.start())
            class_name = cls[0] if cls else ""
            via = (path, line_of(src, m.start()), call.rstrip("("))
            targets = [(path, class_name, class_name)]
            if path.endswith(".xaml.cs") and os.path.exists(path[:-3]):
                targets.append((path[:-3], os.path.basename(path[:-3]), None))
            for page, scope, scoped_class in targets:
                if (page, scope) in seen:
                    continue
                seen.add((page, scope))
                for c in own:
                    out.append(Caption(c.file, c.line, scope, c.raw, scoped_class, page=page, via=via))
    return out


def element_span(src, name):
    """(start, end) of the XAML element whose x:Name is name, by counting the
    opening and closing tags of its own type; None when there is none."""
    m = re.search(r'<([\w:.]+)\b[^>]*?\bx:Name="' + re.escape(name) + '"', src)
    if not m:
        return None
    tag, depth = re.escape(m.group(1)), 0
    for t in re.finditer(r"<(/?)" + tag + r"\b[^>]*?(/?)>", src[m.start():]):
        if t.group(1):
            depth -= 1
        elif not t.group(2):
            depth += 1
        if depth == 0:
            return m.start(), m.start() + t.end()
    return m.start(), len(src)


def window_pages():
    """The classes of the pages MainWindow shows: the class each pageFactories_
    entry makes with new, or the class the helper method it calls returns."""
    with open(os.path.join(ROOT, "MainWindow.xaml.cs"), encoding="utf-8") as handle:
        src = handle.read()
    names = set()
    for m in re.finditer(r"pageFactories_\[[^\]]+\]\s*=\s*\(\)\s*=>\s*(?:new\s+(\w+)\s*\(|(\w+)\.(\w+)\s*\()", src):
        if m.group(1):
            names.add(m.group(1))
            continue
        helper = os.path.join(ROOT, "Views", m.group(2) + ".cs")
        if os.path.exists(helper):
            # The class the method's body makes when it is "=> new X(", else the
            # class it returns: Groups() returns a UserControl and makes a GroupsPageView.
            with open(helper, encoding="utf-8") as handle:
                made = re.search(r"\bstatic\s+(\w+)\s+" + re.escape(m.group(3)) + r"\s*\([^)]*\)\s*(?:=>\s*new\s+(\w*)\s*\()?", handle.read())
            if made:
                names.add(made.group(2) or made.group(1))
    return names


def bar_captions(files):
    """Each window bar's captions, copied onto every page the window shows: into
    the scope of the page's class in each of its code files, and for a XAML page
    into the view file's scope as well. The bars' own captions are MainWindow.xaml's,
    checked for coverage with the rest of that file."""
    pages = window_pages()
    out = []
    for window, bars in WINDOW_BARS.items():
        path = os.path.join(ROOT, window)
        with open(path, encoding="utf-8") as handle:
            src = handle.read()
        for bar in bars:
            span = element_span(src, bar)
            if span is None:
                continue
            captions = [c for c in xaml_captions(path) if span[0] <= line_start(src, c.line) < span[1]]
            targets = []
            for page in files:
                base = os.path.basename(page)
                if page.endswith(".xaml") and base[:-5] in pages and page != path:
                    targets.append((page, base, None))
                elif page.endswith(".cs"):
                    with open(page, encoding="utf-8") as handle:
                        code = handle.read()
                    for name, _, _ in body_ranges(code, CLASS):
                        if name in pages:
                            targets.append((page, name, name))
            for c in captions:
                via = (path, c.line, f"MainWindow's {bar}")
                for page, scope, scoped_class in targets:
                    out.append(Caption(c.file, c.line, scope, c.raw, scoped_class, page=page, via=via))
    return out


def line_start(src, line):
    """The offset of the first character of a 1-based line."""
    pos = 0
    for _ in range(line - 1):
        pos = src.index("\n", pos) + 1
    return pos


def collisions(captions, tag=None):
    """No two captions in one scope share a key; a tabbed class's constructor scope
    is global to its methods, and a page's captions outside its cards to each of
    its cards (AccessKeyScope.ByCard). One problem line per collision, for the
    caption that came second in its scope, after the constructor's or the page's."""
    problems = []
    by_scope = {}
    for c in captions:
        if c.key:
            by_scope.setdefault((c.page, c.scope), []).append(c)
    language = f"[{tag}] " if tag else ""
    for (page, scope), items in by_scope.items():
        ctor = by_scope.get((page, items[0].class_name), []) if items[0].class_name and items[0].class_name != scope else []
        seen = {}
        for c in ctor + items:
            if c.key in seen:
                other = seen[c.key]
                if c in items:
                    problems.append(f"{location(c)}: {language}Alt+{c.key} in {describe(c)} collides with {describe(other)} (scope {scope})")
            else:
                seen[c.key] = c
    return problems


def language_problems(captions):
    """Every translated caption must carry a key, and no two in one scope may share
    one - the rule above, applied to each catalogue in Resources with the captions
    mapped through it. A caption the catalogue does not translate is shown in
    English, so it keeps the English key and takes part in the English check only."""
    import xml.etree.ElementTree as ET
    resources = os.path.join(ROOT, "Resources")
    if not os.path.isdir(resources):
        return []
    problems = []
    for name in sorted(os.listdir(resources)):
        m = re.fullmatch(r"Strings\.([A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*)\.resx", name)
        if not m:
            continue
        tag = m.group(1)
        catalogue = {}
        for data in ET.parse(os.path.join(resources, name)).getroot().findall("data"):
            value = data.find("value")
            if value is not None and value.text:
                catalogue[data.get("name")] = value.text
        translated = []
        for c in captions:
            raw = catalogue.get(c.raw)
            if raw is None or c.text in EXEMPT:
                continue
            t = Caption(c.file, c.line, c.scope, raw, c.class_name, page=c.page, via=c.via)
            if c.key is not None and t.key is None and not c.via:
                problems.append(f"{location(c)}: [{tag}] no mnemonic in \"{raw}\" (translation of \"{c.raw}\")")
            translated.append(t)
        problems += collisions(translated, tag)
    return problems


def onto_xaml_pages(captions):
    """A caption the code behind a XAML page sets on one of the page's named
    controls is on screen with the page's XAML captions: it joins the XAML file's
    scope. One the XAML already gives the control - Pause, put back after Resume -
    is the same caption and not a second one. On a page whose cards answer their
    own keys, the caption joins the scope of the card the control is in."""
    by_file = {}
    for c in captions:
        by_file.setdefault(c.file, []).append(c)
    out = []
    for c in captions:
        if c.target and c.file.endswith(".xaml.cs") and os.path.exists(c.file[:-3]):
            xaml = c.file[:-3]
            with open(xaml, encoding="utf-8") as handle:
                page_src = handle.read()
            names = {m.group(1): m.start() for m in re.finditer(r'x:Name="(\w+)"', page_src)}
            if c.target in names:
                if any(x.raw == c.raw for x in by_file.get(xaml, [])):
                    continue
                scope, class_name = xaml_scope(xaml, page_src, names[c.target])
                c = Caption(c.file, c.line, scope, c.raw, class_name, page=xaml)
        out.append(c)
    return out


def one_key_per_caption(captions):
    """One verb, one key: a button, check box or radio button with the same caption
    carries the same key wherever it is. A field's caption is not held to it."""
    seen = {}
    for c in captions:
        if c.text in EXEMPT or c.key is None or c.via or c.field:
            continue
        seen.setdefault(c.text, {}).setdefault(c.key, c)
    problems = []
    for text, by_key in seen.items():
        if len(by_key) < 2:
            continue
        ordered = sorted(by_key.values(), key=lambda c: (c.file, c.line))
        first = ordered[0]
        for c in ordered[1:]:
            problems.append(f"{location(c)}: one verb, one key: \"{text}\" is Alt+{c.key} in {describe(c)} and Alt+{first.key} at {location(first)}")
    return problems


def self_test():
    """The rules against pages made to pass and to fail them, each a defect the
    Control Panel shipped (review findings F071 and F287): a field's caption with
    no key, a caption set in code on a XAML page's button, a dialog tab's key that
    is the Save button's under every tab, a verb with two keys, and the cards of a
    page that answer their own keys."""
    import tempfile
    failures = 0

    def expect(what, problems, wanted):
        nonlocal failures
        found = bool(problems)
        if found != wanted:
            print(f"FAIL: {what}: {'reported' if found else 'not reported'}: {problems}")
            failures += 1

    with tempfile.TemporaryDirectory() as root:
        def page(name, text):
            path = os.path.join(root, name)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            return path

        head = '<UserControl x:Class="T.P" xmlns:local="clr-namespace:T"{0}>\n'
        cards = ('<ui:Button Content="{{loc:L \'_Refresh\'}}" />\n'
                 '<scaffold:Card Title="A">\n'
                 '<scaffold:FieldRow Label="{{loc:L \'_Port\'}}"><ui:TextBox /></scaffold:FieldRow>\n'
                 '<scaffold:FieldRow Label="{{loc:L \'_Reply to\'}}"><ui:TextBox /></scaffold:FieldRow>\n'
                 '</scaffold:Card>\n'
                 '<scaffold:Card Title="B">\n'
                 '<ui:Button Content="{{loc:L \'_Pause\'}}" />\n'
                 '<scaffold:FieldRow Label="{{loc:L \'Host\'}}"><ui:TextBox /></scaffold:FieldRow>\n'
                 '</scaffold:Card>\n'
                 '</UserControl>\n')
        one = xaml_captions(page("One.xaml", head.format("") + cards.format()))
        by_card = xaml_captions(page("ByCard.xaml", head.format(' local:AccessKeyScope.ByCard="True"') + cards.format()))

        # a field's caption is read, and one without a key is reported
        expect("a FieldRow caption with no key", [c for c in one if c.field and c.key is None], True)
        # one page is one scope: card A's P and card B's P collide
        expect("_Port and _Pause on a page that is one scope", [p for p in collisions(one) if "_Pause" in p], True)
        # a page by card: two cards may share a key, a card and the page may not
        expect("_Port and _Pause in two cards", [p for p in collisions(by_card) if "_Pause" in p], False)
        expect("_Refresh outside the cards and _Reply to in a card", [p for p in collisions(by_card) if "_Reply to" in p], True)

        # a caption set in code on a named button is on its page, in its card
        page("Status.xaml", head.format("") + '<ui:Button x:Name="PauseButton" Content="{loc:L \'_Pause\'}" />\n'
             '<ui:Button Content="{loc:L \'_Refresh\'}" />\n</UserControl>\n')
        code = page("Status.xaml.cs", "namespace T\n{\n   public partial class StatusView\n   {\n"
                    "      private void Show_()\n      {\n"
                    "         PauseButton.Content = L(\"_Resume\");\n      }\n   }\n}\n")
        status = onto_xaml_pages(xaml_captions(os.path.join(root, "Status.xaml")) + cs_captions(code))
        expect("_Resume set in code beside _Refresh", collisions(status), True)

        # a tab's key that is the Save button's, under every tab
        dialog = page("Dialog.cs", "namespace T\n{\n   public class RouteDialog : Window\n   {\n"
                      "      public RouteDialog()\n      {\n"
                      "         var save = new Button { Content = L(\"_Save\") };\n"
                      "         tabs.Items.Add(new TabItem { Content = BuildAuth() });\n      }\n\n"
                      "      private FrameworkElement BuildAuth()\n      {\n"
                      "         return Label(L(\"_Subject\"), subject_);\n      }\n   }\n}\n")
        expect("a tab's _Subject and the dialog's _Save", collisions(cs_captions(dialog)), True)

        # one verb, one key; a field's caption is its form's
        verbs = page("Verbs.xaml", head.format("") + '<ui:Button Content="{loc:L \'Re_fresh\'}" />\n'
                     '<scaffold:FieldRow Label="{loc:L \'Na_me\'}"><ui:TextBox /></scaffold:FieldRow>\n</UserControl>\n')
        names = page("Names.xaml", head.format("") + '<ui:Button Content="{loc:L \'_Refresh\'}" />\n'
                     '<scaffold:FieldRow Label="{loc:L \'_Name\'}"><ui:TextBox /></scaffold:FieldRow>\n</UserControl>\n')
        both = xaml_captions(verbs) + xaml_captions(names)
        expect("Refresh as Alt+F and Alt+R", [p for p in one_key_per_caption(both) if '"Refresh"' in p], True)
        expect("the field Name as Alt+M and Alt+N", [p for p in one_key_per_caption(both) if '"Name"' in p], False)

        # a confirmation's button is held to the verb's key, in a window of its own
        members = page("Members.cs", "namespace T\n{\n   public class MembersDialog : Window\n   {\n"
                       "      public MembersDialog()\n      {\n"
                       "         var remove = new Button { Content = L(\"Re_move\") };\n      }\n\n"
                       "      private void Remove_()\n      {\n"
                       "         if (!Dialogs.Confirm(F(\"Remove {0}?\", name), L(\"Control Panel\"), L(\"_Remove\")))\n"
                       "            return;\n      }\n   }\n}\n")
        confirmed = cs_captions(members)
        expect("a confirmation's _Remove beside the page's Re_move", [p for p in one_key_per_caption(confirmed) if '"Remove"' in p], True)
        fixed = page("Fixed.cs", open(members, encoding="utf-8").read().replace('L(\"_Remove\")))', 'L(\"Re_move\")))'))
        expect("a confirmation's Re_move beside the page's Re_move", one_key_per_caption(cs_captions(fixed)) + collisions(cs_captions(fixed)), False)

        # a read-only row carries no key, and is not a field that must have one (F292)
        account = page("Account.cs", "namespace T\n{\n   public class AccountDialog : Window\n   {\n"
                       "      private ScrollViewer BuildGeneral()\n      {\n"
                       "         panel.Children.Add(DialogFields.ReadOnlyField(L(\"Last l_ogon\"), lastLogon_));\n"
                       "         panel.Children.Add(DialogFields.ReadOnlyField(L(\"Zero-access mailbox\"), zeroAccess_));\n"
                       "         return null;\n      }\n   }\n}\n")
        rows = read_only_captions(account)
        expect("the read-only rows are read", [c for c in rows if c.text in ("Last logon", "Zero-access mailbox")] if len(rows) == 2 else [], True)
        expect("a read-only row's Last l_ogon", [p for p in read_only_problems(rows) if "Last l_ogon" in p], True)
        expect("a read-only row's Zero-access mailbox", [p for p in read_only_problems(rows) if "Zero-access" in p], False)
        expect("a read-only row as a field with no key", [c for c in cs_captions(account) if c.key is None], False)

    print("self-test: " + ("FAIL" if failures else "OK"))
    return 1 if failures else 0


def main():
    listing = "--list" in sys.argv
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if "--self-test" in sys.argv:
        return self_test()
    problems = []
    all_captions = []
    views = os.path.join(ROOT, "Views")
    files = sorted(os.path.join(views, f) for f in os.listdir(views)) + [os.path.join(ROOT, "MainWindow.xaml")]
    for path in files:
        if path.endswith(".xaml"):
            all_captions += xaml_captions(path)
        elif path.endswith(".cs"):
            all_captions += cs_captions(path)
    # Captions the code behind a XAML page sets on its named controls: on its page.
    all_captions = onto_xaml_pages(all_captions)

    # A shared helper's captions: scanned once, where they are, for coverage, and
    # copied onto every page that calls the helper for the collision check.
    placed = []
    for helper, call in SHARED.items():
        own = cs_captions(os.path.join(ROOT, helper))
        all_captions += own
        placed += placed_captions(own, call, files)
    # The window's bars: on screen with every page the window shows.
    placed += bar_captions(files)
    # coverage
    for c in all_captions:
        if c.text in EXEMPT:
            continue
        if c.key is None:
            problems.append(f"{location(c)}: no mnemonic in \"{c.raw}\"")
    # uniqueness per scope, with the shared helpers' captions on the pages that call them
    problems += collisions(all_captions + placed)
    problems += one_key_per_caption(all_captions)
    problems += language_problems(all_captions + placed)
    # read-only rows: no key, in English or in any catalogue
    read_only = [c for path in files if path.endswith(".cs") for c in read_only_captions(path)]
    problems += read_only_problems(read_only)
    if listing:
        for c in all_captions + placed:
            print(f"{location(c)}: [{c.scope}] {'Alt+' + c.key if c.key else '   -'}  {c.text}" + (f"  (from {os.path.relpath(c.file, ROOT)})" if c.via else ""))
    covered = sum(1 for c in all_captions if c.key)
    exempt = sum(1 for c in all_captions if c.text in EXEMPT)
    print(f"{len(all_captions)} static captions: {covered} with a mnemonic, {exempt} exempt, {len(all_captions) - covered - exempt} missing")
    if problems:
        # one line per problem, deduplicated, in file order
        for p in sorted(set(problems), key=lambda s: (s.split(":")[0], int(s.split(":")[1]))):
            print("  " + p)
        print(f"FAIL: {len(set(problems))} problem(s)")
        return 1
    print("OK    every static caption carries a mnemonic and no scope has a collision")
    return 0


if __name__ == "__main__":
    sys.exit(main())
