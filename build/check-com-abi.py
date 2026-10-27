# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every COM interface keeps the members the last release gave it, in their places.

Every interface of hMailServer.idl is dual, and a dual interface's vtable follows
the order its members are DECLARED in, not their dispatch ids. A member declared
above one that shipped moves every member below it a slot down while the IID
stays the same, and an early-bound client built against the released type
library - .NET through the Interop.hMailServer.dll an install puts in Bin, C++
through a header made from the type library - then calls another method than the
one it names, with its arguments, rather than failing to bind. Late binding by
name and dispatch id, which scripts and the Control Panel use, does not notice,
nor does a build, a test of this tree or a diff that shows two added lines:
Utilities.RunAcmeCheck and GetAcmeStatus were declared in the middle of
IInterfaceUtilities after 6.3.7 and shifted 63 of its members, and the same was
put right before 6.2.22 shipped (Roadmap.md, "COM interface members were inserted
rather than appended").

build/com-abi-baseline.txt is what the last release's hMailServer.idl says of
each interface: its IID, the interface it derives from, the attributes a client
binds by (dual, local, object, oleautomation), and each member in its order,
with its kind (method, propget, propput, propputref), dispatch id, name, return
type and parameters - their direction ([in], [out], [retval], [lcid]) and type,
but not their names, which no binding reads by position, nor their defaultvalue
or help text. This holds the tree's hMailServer.idl to it: every interface the
release had is there, with its IID, base and those attributes, and its members
as the tree declares them begin with the release's, one for one - so a member
is only ever added after the last one the release had. A member inserted,
removed, moved, given another dispatch id or kind, or given another type or
direction of a parameter is refused, naming the interface, the member's place
and what the release had there, and so is dual or another of those attributes
taken off or put on. The type library's own identity - the library's uuid and
version, and each coclass's uuid and default interface - is held the same way,
and so is each enumerator's value, which an early-bound client compiles in: an
enumerator the release had is in its enum with its value. A new interface,
coclass, enum or enumerator is free, and so is anything added at an
interface's end. Every interface of the tree, new ones too, must derive from
IDispatch or IUnknown: one derived from another of the file's interfaces has
its base's members first in its vtable, so a member appended to the base, which
this lets through, would move each of its own, and the check, which counts an
interface's members from the slot after IDispatch's or IUnknown's, refuses such
a base rather than pass it. None derives from another today.

What it does not hold: an interface's other attributes (helpstring,
nonextensible, pointer_default, hidden), a member's attributes other than its
kind and dispatch id, an enum's own uuid, the parameters' names and defaultvalues, and
whatever hMailServer.idl does not declare, such as the coclasses' interfaces
other than the default one and the registry entries an install writes.

The baseline is written from a release tag, never from the tree:

  python build/check-com-abi.py --write-baseline v6.3.8

after a release is tagged (RELEASE.md says when), which reads that tag's
hMailServer.idl through git. A clone that has the baseline's tag is also held to
the baseline being what that tag's hMailServer.idl says, so that the file cannot
be edited to let a change through, and the tree is held to each newer release
tag it has as well, so that a baseline not yet rewritten after a release still
holds that release. The CI's checkouts are a commit deep and have no tag: in a
CI job (GITLAB_CI=true) the check fetches the baseline's tag and every newer
release tag from origin, a commit deep each, and holds the baseline and the
tree to them as in a clone that has them, and the job fails when they cannot be
listed or fetched - so a change of hMailServer.idl that edits the baseline to
match, or names an older release in it, is refused there. Elsewhere a clone
without the tag holds the tree to the baseline as committed, and says so.

The check proves its negative controls on every run (--self-test runs only
those), against the tree's hMailServer.idl changed in memory: a member inserted
above the release's (the two ACME methods put back after GetReputationReport),
one removed, two swapped, a dispatch id changed, a parameter's type and its
direction changed, a propget made a method, an interface's IID changed, an
interface taken away, a new interface derived from a released one, dual taken
off an interface, an enumerator's value changed and an enumerator removed must
each be refused, and a member appended, a member written over two lines, a
member commented out where it would have been inserted, a new interface derived
from IDispatch and an enumerator appended must each pass. The parse is held to
see every HRESULT of every interface body, so a member it stopped reading is
not one it silently passes.

The text is read as MIDL reads it or not at all. Its tokens are string
literals, comments of both kinds, words and single characters; MIDL runs the C
preprocessor over hMailServer.idl first, and reads character literals, which
this does not. So it refuses, naming the line, a file in which MIDL would read
otherwise: a preprocessor line (#if 0 ... #endif around a member takes it out of
the vtable, an #include in a body brings members in, each moving every member
below), a line ending in a backslash (which joins the next line on, so a //
comment can swallow the member under it), a ' (a character literal, which MIDL
takes as an attribute's argument: in custom(<uuid>, '"') a reader that knew no
such literal would take the " to open a string, and a member written between
two such literals on one line would be text to it where MIDL reads a member) and
a " that opens no string on its line. hMailServer.idl has none of them; a #, a '
or a " inside a string or a comment is text. And it reads an enumerator only
with its value written out, as each is, refusing one whose value C or MIDL
would work out. A released member inside #if 0 ... #endif, an #include at the
top of a body, a // comment ending in a backslash above a member, a member
between two '"' on one line in custom() and in helpcontext(), the same with
L'"', and an enumerator without its value are seven more of the controls, and
each must stop the parse with its refusal.

Usage:  python build/check-com-abi.py [--self-test] [--write-baseline TAG]
Exit 0 when every interface keeps the release's members, 1 when one does not
or, in CI, the baseline's tag cannot be fetched, 2 for a usage or git error or
an hMailServer.idl this does not read, such as one with a preprocessor line.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IDL_PATH = "hmailserver/source/Server/hMailServer/hMailServer.idl"
IDL = os.path.join(ROOT, *IDL_PATH.split("/"))
BASELINE = os.path.join(HERE, "com-abi-baseline.txt")
TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")

# A string literal (helpstring text holds any punctuation: semicolons, brackets,
# "//" in a URL), a comment of either kind, a preprocessor line (which tokens()
# refuses), a word (an identifier, a number, a piece of a uuid), or any other
# single character - a # among them, refused too, and a ' or a " that opens no
# string on its line, refused as this does not read character literals.
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|//[^\n]*|/\*.*?\*/|^[ \t]*#[^\n]*|[A-Za-z0-9_.]+|\S',
                   re.DOTALL | re.MULTILINE)
WORD = re.compile(r"^[A-Za-z0-9_.]+$")
KINDS = ("propget", "propput", "propputref")
# What a parameter's attributes say that the vtable and the marshalling read.
DIRECTIONS = ("in", "out", "retval", "lcid")
VTABLE_BASE = {"IDispatch": 7, "IUnknown": 3}
# An interface's attributes that say how a client binds to it and how its calls are
# marshalled: dual (the vtable a type library's early-bound client calls),
# oleautomation (marshalled from the type library), object (a COM interface) and
# local (no proxy at all). Held as the release had them; the rest (helpstring,
# nonextensible, pointer_default, hidden) are free.
BINDING = ("dual", "local", "object", "oleautomation")
ENUM_VALUE = re.compile(r"^-?(?:0[xX][0-9A-Fa-f]+|0|[1-9][0-9]*)$")


class IdlError(Exception):
    pass


def line_number(text, offset):
    return text.count("\n", 0, offset) + 1


def tokens(text):
    """(text, start, end) of each token, comments left out.

    MIDL runs the C preprocessor over the file before it reads it, and this reads
    the text as it is written, so anything the preprocessor would change is
    refused rather than read past: a directive, which can take a member out
    (#if 0 ... #endif) or bring one in (#include) and move every member below it,
    and a backslash at the end of a line, which splices the next line on and so
    makes a // comment swallow the member written below it. hMailServer.idl has
    neither; a # inside a string or a comment is text and stays allowed. A
    character literal is refused too: TOKEN knows string literals and comments
    but not '...', so in '"' the " would open a string for this check running to
    the next " on the line, and a member written between two such literals would
    be text to it where MIDL reads it. So is a " that opens no string on its line.
    """
    splice = re.search(r"\\[ \t]*(?:\n|$)", text)
    if splice:
        raise IdlError("line %d ends in a backslash, which the C preprocessor MIDL runs first joins to the next line "
                       "(after a // comment, the next line is part of the comment): write it on one line"
                       % line_number(text, splice.start()))
    found = []
    for match in TOKEN.finditer(text):
        value = match.group(0)
        if value.startswith("//") or value.startswith("/*"):
            continue
        if value in ("'", '"'):
            raise IdlError("line %d has %s: this check does not read C character "
                           "literals, which MIDL takes as an attribute's argument ('\"' or L'\"' in custom() or "
                           "helpcontext()), and a \" inside one would open a string for this check that hides the rest "
                           "of the line, a member among it; hMailServer.idl holds none - give the argument as a number "
                           "or a string" % (line_number(text, match.start()),
                                            "a ' outside a string or a comment" if value == "'"
                                            else "a \" that opens no string on its line"))
        if value.lstrip().startswith("#"):
            line_start = text.rfind("\n", 0, match.start()) + 1
            line_end = text.find("\n", match.start())
            line = text[line_start:line_end if line_end >= 0 else len(text)].strip()
            raise IdlError("line %d is a preprocessor line (%s): MIDL runs the C preprocessor first, so it can take "
                           "members out of an interface or bring them in where this check reads the text as written; "
                           "hMailServer.idl holds none - write the members out"
                           % (line_number(text, match.start()), line[:60]))
        found.append((value, match.start(), match.end()))
    return found


def closing(toks, index, opening, close):
    """The index of the token that closes toks[index]."""
    depth = 0
    for position in range(index, len(toks)):
        value = toks[position][0]
        if value == opening:
            depth += 1
        elif value == close:
            depth -= 1
            if depth == 0:
                return position
    raise IdlError("a %s at offset %d is never closed" % (opening, toks[index][1]))


def split_top(toks, separator=","):
    """toks split at separators outside any (), [] or {}."""
    parts, current, depth = [], [], 0
    for token in toks:
        value = token[0]
        if value in "([{":
            depth += 1
        elif value in ")]}":
            depth -= 1
        if value == separator and depth == 0:
            parts.append(current)
            current = []
        else:
            current.append(token)
    if current:
        parts.append(current)
    return parts


def attributes(toks):
    """{name: argument text or None} of the tokens between [ and ]."""
    found = {}
    for part in split_top(toks):
        if not part:
            continue
        name = part[0][0]
        argument = None
        if len(part) > 1 and part[1][0] == "(":
            argument = "".join(token[0] for token in part[2:-1])
        found[name] = argument
    return found


def render(toks):
    """Type tokens as one string: a space only between two words."""
    out = ""
    previous_word = False
    for value, _, _ in toks:
        word = bool(WORD.match(value))
        if word and previous_word:
            out += " "
        out += value
        previous_word = word
    return out


def parameter(toks):
    direction = []
    if toks and toks[0][0] == "[":
        end = closing(toks, 0, "[", "]")
        attrs = attributes(toks[1:end])
        direction = [name for name in DIRECTIONS if name in attrs]
        toks = toks[end + 1:]
    # The last word is the parameter's name, unless the type is all there is.
    if len(toks) > 1 and WORD.match(toks[-1][0]):
        toks = toks[:-1]
    text = render(toks)
    return ("[%s] " % ",".join(direction) if direction else "") + text


def member(toks, interface):
    attrs = {}
    if toks and toks[0][0] == "[":
        end = closing(toks, 0, "[", "]")
        attrs = attributes(toks[1:end])
        toks = toks[end + 1:]
    opening = next((i for i, token in enumerate(toks) if token[0] == "("), None)
    if opening is None or opening < 2:
        raise IdlError("%s: a member at offset %d is not <type> <name>(...)" % (interface, toks[0][1] if toks else -1))
    end = closing(toks, opening, "(", ")")
    name = toks[opening - 1][0]
    returns = render(toks[:opening - 1])
    parameters = [parameter(part) for part in split_top(toks[opening + 1:end])]
    if parameters == ["void"]:
        parameters = []
    kind = next((k for k in KINDS if k in attrs), "method")
    dispid = attrs.get("id")
    return "%s id(%s) %s %s(%s)" % (kind, dispid if dispid is not None else "-", returns, name, ", ".join(parameters))


def enumerators(toks, text):
    """"<name> = <value>" of each enumerator between an enum's braces. Only a value written out is read: C
    gives an enumerator without one its predecessor's plus one, and MIDL takes an expression, neither of which
    this computes, so either is refused; every enumerator of hMailServer.idl has an integer of its own."""
    found = []
    for part in split_top(toks):
        while part and part[0][0] == "[":
            part = part[closing(part, 0, "[", "]") + 1:]
        if not part:
            continue
        values = [token[0] for token in part]
        literal = "".join(values[2:])
        if len(values) < 3 or values[1] != "=" or not WORD.match(values[0]) or not ENUM_VALUE.match(literal):
            raise IdlError("line %d: the enumerator %s is not written <name> = <integer>: this check reads only an "
                           "enumerator's value written out, as every one in hMailServer.idl is, and not one C or MIDL "
                           "would work out - write its value" % (line_number(text, part[0][1]), render(part)[:60]))
        found.append("%s = %d" % (values[0], int(literal, 0)))
    return found


def parse(text):
    """{"interfaces": {name: (iid, base, [member, ...], [binding attribute, ...])}, "types": [line, ...],
    "spans": {...}}"""
    toks = tokens(text)
    interfaces, spans, types = {}, {}, []
    pending = {}
    index = 0
    hresults = 0
    while index < len(toks):
        value = toks[index][0]
        if value == "[":
            end = closing(toks, index, "[", "]")
            pending = attributes(toks[index + 1:end])
            index = end + 1
            continue
        if value == "interface" and index + 2 < len(toks) and toks[index + 2][0] == ":":
            name, base = toks[index + 1][0], toks[index + 3][0]
            brace = index + 4
            if brace >= len(toks) or toks[brace][0] != "{":
                raise IdlError("interface %s: no { after its base" % name)
            end = closing(toks, brace, "{", "}")
            body = toks[brace + 1:end]
            hresults += sum(1 for token in body if token[0] == "HRESULT")
            members, member_spans = [], []
            for part in split_top(body, ";"):
                if not part:
                    continue
                members.append(member(part, name))
                member_spans.append((part[0][1], part[-1][2] + 1))   # with its ;
            if name in interfaces:
                raise IdlError("interface %s is defined twice" % name)
            interfaces[name] = ((pending.get("uuid") or "").upper(), base, members,
                                [flag for flag in BINDING if flag in pending])
            spans[name] = member_spans
            pending = {}
            index = end + 1
            continue
        if value == "enum":
            brace = index + 1
            tag = None
            if brace < len(toks) and WORD.match(toks[brace][0]):
                tag = toks[brace][0]
                brace += 1
            if brace < len(toks) and toks[brace][0] == "{":
                end = closing(toks, brace, "{", "}")
                # typedef enum [tag] { ... } name; - the typedef's name is the one the type library gives it.
                after = toks[end + 1][0] if end + 1 < len(toks) else ""
                name = after if WORD.match(after) else tag
                if not name:
                    raise IdlError("line %d: an enum with no name" % line_number(text, toks[index][1]))
                types += ["enum %s %s" % (name, entry) for entry in enumerators(toks[brace + 1:end], text)]
                pending = {}
                index = end + 1
                continue
        if value == "library" and index + 2 < len(toks) and toks[index + 2][0] == "{":
            types.append("library %s uuid(%s) version(%s)"
                         % (toks[index + 1][0], (pending.get("uuid") or "").upper(), pending.get("version")))
            pending = {}
            index += 3
            continue
        if value == "coclass" and index + 2 < len(toks) and toks[index + 2][0] == "{":
            name = toks[index + 1][0]
            end = closing(toks, index + 2, "{", "}")
            default = "-"
            for part in split_top(toks[index + 3:end], ";"):
                if part and part[0][0] == "[":
                    close = closing(part, 0, "[", "]")
                    if "default" in attributes(part[1:close]):
                        default = part[-1][0]
            types.append("coclass %s uuid(%s) default %s" % (name, (pending.get("uuid") or "").upper(), default))
            pending = {}
            index = end + 1
            continue
        if value not in ("interface", "dispinterface"):
            pending = {} if value in (";", "}", "{") else pending
        index += 1
    counted = sum(len(entry[2]) for entry in interfaces.values())
    if counted != hresults:
        raise IdlError("read %d members in the interfaces' bodies, where they hold %d HRESULTs" % (counted, hresults))
    return {"interfaces": interfaces, "types": types, "spans": spans}


def baseline_text(parsed, tag, commit):
    lines = [
        "# The COM interfaces of hmailserver/source/Server/hMailServer/hMailServer.idl at %s (commit %s)," % (tag, commit),
        "# written by python build/check-com-abi.py --write-baseline %s, which reads that tag through git." % tag,
        "# build/check-com-abi.py holds the tree to it: each interface's members, in this order, come first in",
        "# the tree's, and each enumerator keeps its value, so that an early-bound client built against that",
        "# release's type library calls the member it names. Not edited by hand; rewritten from the next",
        "# release's tag.",
        "release %s %s" % (tag, commit),
    ]
    lines += parsed["types"]
    for name in sorted(parsed["interfaces"]):
        iid, base, members, binding = parsed["interfaces"][name]
        lines.append("interface %s uuid(%s) : %s%s" % (name, iid, base, "".join(" " + flag for flag in binding)))
        lines += ["  " + entry for entry in members]
    return "\n".join(lines) + "\n"


def read_baseline(text):
    release, types, interfaces, current = None, [], {}, None
    for number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        if line.startswith("  "):
            if current is None:
                raise IdlError("baseline line %d: a member before any interface" % number)
            interfaces[current][2].append(line[2:])
            continue
        found = re.match(r"^interface (\w+) uuid\(([0-9A-F-]*)\) : (\w+)((?: [a-z]+)*)$", line)
        if found:
            current = found.group(1)
            binding = found.group(4).split()
            if any(flag not in BINDING for flag in binding):
                raise IdlError("baseline line %d names an attribute this does not hold: %s" % (number, line[:80]))
            interfaces[current] = (found.group(2), found.group(3), [], binding)
            continue
        found = re.match(r"^release (v\d+\.\d+\.\d+) ([0-9a-f]{40})$", line)
        if found and release is None:
            release = (found.group(1), found.group(2))
            continue
        if line.startswith("library ") or line.startswith("coclass ") or re.match(r"^enum \w+ \w+ = -?\d+$", line):
            types.append(line)
            continue
        raise IdlError("baseline line %d is not one this reads: %s" % (number, line[:80]))
    if release is None:
        raise IdlError("the baseline names no release")
    return {"release": release, "types": types, "interfaces": interfaces}


def slot(base, number):
    first = VTABLE_BASE.get(base)
    return (", vtable slot %d" % (first + number - 1)) if first is not None else ""


def problems(release, tree, label):
    """What the tree changed of the release's interfaces and type identities."""
    found = []
    for line in release["types"]:
        if line not in tree["types"]:
            kind, name = line.split(" ")[:2]
            # An enumerator by its enum and its own name; a library or coclass by its name.
            key = 3 if kind == "enum" else 2
            now = [entry for entry in tree["types"] if entry.split(" ")[:key] == line.split(" ")[:key]]
            found.append("%s %s: %s had \"%s\", the tree has %s" % (kind, name, label, line,
                         ("\"%s\"" % now[0]) if now else "none"))
    for name in sorted(release["interfaces"]):
        iid, base, members, binding = release["interfaces"][name]
        if name not in tree["interfaces"]:
            found.append("%s: %s had it with %d member(s), the tree has no such interface" % (name, label, len(members)))
            continue
        tree_iid, tree_base, tree_members, tree_binding = tree["interfaces"][name]
        if tree_iid != iid:
            found.append("%s: its IID is %s, where %s had %s" % (name, tree_iid or "missing", label, iid))
        if tree_base != base:
            found.append("%s: it derives from %s, where %s had %s" % (name, tree_base, label, base))
        if tree_binding != binding:
            found.append("%s: its attributes say %s, where %s's said %s: a client binds to it and its calls are "
                         "marshalled as these say" % (name, ", ".join(tree_binding) or "none of " + "/".join(BINDING),
                                                      label, ", ".join(binding) or "none of " + "/".join(BINDING)))
        for number, entry in enumerate(members, 1):
            have = tree_members[number - 1] if number <= len(tree_members) else None
            if have == entry:
                continue
            if have is None:
                what = "the tree has %d member(s) - one was removed" % len(tree_members)
            elif entry in tree_members[number:]:
                what = ("the tree has %s there, and %s further down: a member was inserted above it or the members "
                        "were reordered - add a member after the last one %s had" % (have, entry, label))
            elif entry in tree_members[:number - 1]:
                what = "the tree has %s there, and %s higher up: the members were reordered" % (have, entry)
            else:
                what = ("the tree has %s there, and %s nowhere: it was changed or removed - its dispatch id, kind, "
                        "return type and its parameters' types and directions are the release's" % (have, entry))
            found.append("%s: member %d%s: %s had %s; %s" % (name, number, slot(base, number), label, entry, what))
            break
    return found


def base_problems(tree):
    """Every interface of the tree derives from IDispatch or IUnknown. One derived from another of the file's
    interfaces has its own members after its base's in its vtable, so a member appended to the base, which
    problems() lets through, would move each of them; this holds each interface's members from the slot after
    IDispatch's or IUnknown's, and so refuses a base it cannot count from."""
    return ["%s: it derives from %s: an interface's vtable begins with its base's members, so one appended to %s "
            "would move each of %s's, and this check holds an interface's members only from the slot after "
            "IDispatch's or IUnknown's - derive it from IDispatch" % (name, entry[1], entry[1], name)
            for name, entry in sorted(tree["interfaces"].items()) if entry[1] not in VTABLE_BASE]


def git(*arguments, timeout=120):
    try:
        done = subprocess.run(["git", "-C", ROOT] + list(arguments), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    return done.stdout


def tag_idl(tag):
    """(commit, text) of hMailServer.idl at a release tag, or None when this clone does not have the tag."""
    if not TAG.match(tag):
        raise IdlError("%s is not a release tag (vX.Y.Z)" % tag)
    commit = git("rev-parse", "--verify", "--quiet", "refs/tags/%s^{commit}" % tag)
    if not commit:
        return None
    commit = commit.decode("ascii").strip()
    blob = git("show", "%s:%s" % (commit, IDL_PATH))
    if blob is None:
        raise IdlError("%s has no %s" % (tag, IDL_PATH))
    return commit, blob.decode("utf-8-sig", errors="replace").replace("\r\n", "\n")


def newer_tags(release):
    listed = git("tag", "--list", "v*")
    if listed is None:
        return []
    own = tuple(int(part) for part in TAG.match(release).groups())
    found = []
    for name in listed.decode("ascii", errors="replace").split():
        match = TAG.match(name)
        if match and tuple(int(part) for part in match.groups()) > own:
            found.append((tuple(int(part) for part in match.groups()), name))
    return [name for _, name in sorted(found)]


def fetch_release_tags(release):
    """In a CI job, whose checkout is a commit deep and has no tag: fetch the baseline's release tag and every
    newer release tag from origin, a commit deep each, so that the baseline is held to what its tag says and
    the tree to every release after it, as in a clone that has them. None when they are here, or a problem."""
    listed = git("ls-remote", "--tags", "--refs", "origin", timeout=300)
    if listed is None:
        return "origin's tags could not be listed (git ls-remote --tags origin)"
    names = set()
    for line in listed.decode("ascii", errors="replace").splitlines():
        found = re.match(r"^[0-9a-f]{40}\trefs/tags/(v\d+\.\d+\.\d+)$", line)
        if found:
            names.add(found.group(1))
    if release not in names:
        return "origin has no tag %s, the release the baseline names" % release
    own = tuple(int(part) for part in TAG.match(release).groups())
    wanted = [release] + sorted((n for n in names if tuple(int(p) for p in TAG.match(n).groups()) > own),
                                key=lambda n: tuple(int(p) for p in TAG.match(n).groups()))
    if git("fetch", "--quiet", "--no-tags", "--depth=1", "origin",
           *["+refs/tags/%s:refs/tags/%s" % (name, name) for name in wanted], timeout=600) is None:
        return "origin's tag(s) %s could not be fetched" % ", ".join(wanted)
    return None


def read_tree():
    with open(IDL, encoding="utf-8-sig", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def cut(text, span):
    return text[:span[0]] + text[span[1]:]


def make_controls(text, release, tree):
    """Each control: (what, the IDL text changed, whether it must be refused - True, False, or ("unread", words
    of the refusal) for a text the parse must refuse to read, as MIDL would read it otherwise than this does -
    and the interface it is about)."""
    spans = tree["spans"]

    def member_span(interface, name):
        for index, entry in enumerate(tree["interfaces"][interface][2]):
            if re.search(r" %s\(" % re.escape(name), entry):
                return spans[interface][index]
        raise IdlError("self-test: %s has no member %s" % (interface, name))

    def line_of(span):
        start = text.rfind("\n", 0, span[0]) + 1
        end = text.find("\n", span[1])
        return start, (end + 1 if end >= 0 else len(text))

    controls = []
    utilities = "IInterfaceUtilities"
    # The defect this check was written for: the two ACME methods where they were first declared.
    run, status = line_of(member_span(utilities, "RunAcmeCheck")), line_of(member_span(utilities, "GetAcmeStatus"))
    report = line_of(member_span(utilities, "GetReputationReport"))
    acme = text[run[0]:run[1]] + text[status[0]:status[1]]
    moved = cut(cut(text, status), run)
    moved = moved[:report[1]] + acme + moved[report[1]:]
    controls.append(("the ACME methods after GetReputationReport", moved, True, utilities))

    # The rest on an interface of the release with at least three members, the first such in order.
    name = next(n for n in sorted(release["interfaces"]) if len(release["interfaces"][n][2]) >= 3)
    first, second = (line_of(span) for span in spans[name][:2])
    one, two = text[first[0]:first[1]], text[second[0]:second[1]]
    fresh = "   [id(9999), helpstring(\"x\")] HRESULT SelfTestMember([out, retval] BSTR *Value);\n"
    controls.append(("a member inserted first", text[:first[0]] + fresh + text[first[0]:], True, name))
    controls.append(("the first member removed", cut(text, first), True, name))
    controls.append(("the first two members swapped", text[:first[0]] + two + text[first[1]:second[0]] + one + text[second[1]:], True, name))
    renumbered = re.sub(r"\bid\(\d+\)", "id(9999)", one, count=1)
    controls.append(("the first member's dispatch id changed", text[:first[0]] + renumbered + text[first[1]:], True, name))
    last = line_of(spans[name][-1])
    controls.append(("a member appended", text[:last[1]] + fresh + text[last[1]:], False, name))
    commented = "   // " + fresh.strip() + "\n"
    controls.append(("a member commented out at the top", text[:first[0]] + commented + text[first[0]:], False, name))
    wrapped = re.sub(r"\)\]\s*HRESULT", ")]\n      HRESULT", one, count=1)
    if wrapped == one:
        wrapped = one.replace(" HRESULT ", "\n      HRESULT ", 1)
    controls.append(("the first member written over two lines", text[:first[0]] + wrapped + text[first[1]:], False, name))
    # What MIDL's preprocessor would change: each must stop the parse, not be read past.
    controls.append(("the first member inside #if 0 ... #endif", text[:first[0]] + "#if 0\n" + one + "#endif\n"
                     + text[first[1]:], ("unread", "is a preprocessor line"), name))
    controls.append(("an #include at the top of a body", text[:first[0]] + "   #include \"self-test.idl\"\n"
                     + text[first[0]:], ("unread", "is a preprocessor line"), name))
    controls.append(("a // comment ending in a backslash above the first member", text[:first[0]] + "   // note \\\n"
                     + text[first[0]:], ("unread", "ends in a backslash"), name))
    # A member between two character literals on one line: to a reader that took the
    # " in '"' to open a string, the member and the first one's attributes are one
    # string, where MIDL reads two members. In custom(), in helpcontext() and wide.
    for literal, attribute in (("'\"'", "custom(D3A9C1F0-0000-4000-8000-000000000001, %s)"),
                               ("'\"'", "helpcontext(%s)"),
                               ("L'\"'", "custom(D3A9C1F0-0000-4000-8000-000000000001, %s)")):
        given = attribute % literal
        hidden = ("   [%s, id(9999)] HRESULT SelfTestMember([out, retval] long *Value); %s"
                  % (given, one.lstrip().replace("[", "[%s, " % given, 1)))
        controls.append(("a member between two %s on one line, in %s" % (literal, attribute.split("(")[0] + "()"),
                         text[:first[0]] + hidden + text[first[1]:], ("unread", "character literals"), name))

    # A parameter's type, a parameter's direction and a property's kind, wherever the tree first has each.
    def first_line(pattern):
        for interface in sorted(release["interfaces"]):
            for span in spans[interface][:len(release["interfaces"][interface][2])]:
                start, end = line_of(span)
                if re.search(pattern, text[start:end]):
                    return interface, start, end
        raise IdlError("self-test: no member of the release matches %s" % pattern)

    interface, start, end = first_line(r"\[in\] BSTR ")
    controls.append(("an [in] BSTR made an [in] long", text[:start] + text[start:end].replace("[in] BSTR ", "[in] long ", 1)
                     + text[end:], True, interface))
    interface, start, end = first_line(r"\[in\] BSTR ")
    controls.append(("an [in] BSTR made an [out] BSTR", text[:start] + text[start:end].replace("[in] BSTR ", "[out] BSTR *", 1)
                     + text[end:], True, interface))
    interface, start, end = first_line(r"\[propget, ")
    controls.append(("a propget made a method", text[:start] + text[start:end].replace("[propget, ", "[", 1) + text[end:],
                     True, interface))
    iid = tree["interfaces"][name][0]
    changed = re.sub(r"uuid\(%s\)" % re.escape(iid), "uuid(00000000-0000-0000-0000-000000000000)", text, count=1, flags=re.I)
    controls.append(("an interface's IID changed", changed, True, name))
    controls.append(("an interface taken away", re.sub(r"\binterface %s\b(?=\s*:)" % name, "interface SelfTestGone", text,
                                                       count=1), True, name))

    # What a client binds by or compiles in besides the members: an interface's base, its binding attributes
    # and the enumerators' values.
    def new_interface(interface, base, uuid):
        return ("\n[\n   object,\n   uuid(%s),\n   dual\n]\ninterface %s : %s {\n"
                "   [id(1)] HRESULT SelfTestMember([out, retval] BSTR *Value);\n};\n" % (uuid, interface, base))
    controls.append(("a new interface derived from IDispatch", text + new_interface(
        "SelfTestNew", "IDispatch", "D3A9C1F0-0000-4000-8000-000000000002"), False, "SelfTestNew"))
    controls.append(("a new interface derived from %s" % name, text + new_interface(
        "SelfTestDerived", name, "D3A9C1F0-0000-4000-8000-000000000003"), True, "SelfTestDerived"))
    position = re.search(r"\binterface %s\s*:" % name, text).start()
    head = text.rfind("\n[", 0, position)
    undual = re.sub(r"\bdual\s*,\s*", "", text[head:position], count=1)
    if head < 0 or undual == text[head:position]:
        raise IdlError("self-test: %s's attributes have no dual" % name)
    controls.append(("dual taken off an interface", text[:head] + undual + text[position:], True, name))
    released = [line for line in release["types"] if line.startswith("enum ")]
    if not released:
        raise IdlError("self-test: the baseline holds no enumerator")
    _, enum, enumerator, _, value = released[0].split(" ")
    written = re.search(r"\b%s\s*=\s*%s\b" % (re.escape(enumerator), re.escape(value)), text)
    removed = re.search(r"(?:\[[^\]\n]*\]\s*)?\b%s\s*=\s*%s\s*,\s*" % (re.escape(enumerator), re.escape(value)), text)
    brace = text.find("}", written.end()) if written else -1
    if not written or not removed or brace < 0:
        raise IdlError("self-test: %s's %s = %s is not written so in the tree" % (enum, enumerator, value))
    controls.append(("an enumerator's value changed", text[:written.start()] + "%s = 9999" % enumerator
                     + text[written.end():], True, "enum " + enum))
    controls.append(("an enumerator removed", text[:removed.start()] + text[removed.end():], True, "enum " + enum))
    controls.append(("an enumerator appended", text[:brace] + "   SelfTestValue = 9999,\n" + text[brace:], False,
                     "enum " + enum))
    controls.append(("an enumerator without a value of its own", text[:brace] + "   SelfTestValue,\n" + text[brace:],
                     ("unread", "is not written <name> = <integer>"), "enum " + enum))
    return controls


def self_test(text, release):
    """The negative controls judged: (failures, how many controls)."""
    tree = parse(text)
    controls = make_controls(text, release, tree)
    # Judged beside what the tree as it is already breaks, so that a tree that fails
    # is reported for itself, not as a control that went wrong.
    already = set(problems(release, tree, "the release") + base_problems(tree))
    failures = []
    for what, changed_text, refused, interface in controls:
        unread = isinstance(refused, tuple)
        try:
            changed_tree = parse(changed_text)
        except IdlError as error:
            if not unread or refused[1] not in str(error):
                failures.append("self-test: %s: the parse failed: %s" % (what, error))
            continue
        if unread:
            failures.append("self-test: %s (%s) was read past rather than refused" % (what, interface))
            continue
        found = [p for p in problems(release, changed_tree, "the release") + base_problems(changed_tree)
                 if p not in already]
        named = [p for p in found if p.startswith(interface + ":")]
        if refused and not named:
            failures.append("self-test: %s (%s) was not refused%s" % (what, interface,
                            (": only " + "; ".join(found)) if found else ""))
        if not refused and found:
            failures.append("self-test: %s (%s) was refused: %s" % (what, interface, "; ".join(found)))
    return failures, len(controls)


def main(arguments):
    try:
        if arguments[:1] == ["--write-baseline"]:
            if len(arguments) != 2:
                sys.stderr.write("usage: python build/check-com-abi.py --write-baseline vX.Y.Z\n")
                return 2
            found = tag_idl(arguments[1])
            if found is None:
                sys.stderr.write("check-com-abi: this clone has no tag %s\n" % arguments[1])
                return 2
            with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
                f.write(baseline_text(parse(found[1]), arguments[1], found[0]))
            sys.stdout.write("wrote %s from %s (%s)\n" % (os.path.relpath(BASELINE, ROOT), arguments[1], found[0]))
            return 0
        only_self_test = arguments == ["--self-test"]
        if arguments and not only_self_test:
            sys.stderr.write("usage: python build/check-com-abi.py [--self-test] [--write-baseline TAG]\n")
            return 2

        with open(BASELINE, encoding="utf-8") as f:
            release = read_baseline(f.read())
        text = read_tree()
        failures, controls = self_test(text, release)
        if only_self_test:
            for failure in failures:
                sys.stdout.write("  FAIL  %s\n" % failure)
            if failures:
                return 1
            sys.stdout.write("  OK    check-com-abi --self-test: %d controls, each refused or passed as it should\n" % controls)
            return 0

        tag, commit = release["release"]
        tree = parse(text)
        found = ["against %s: %s" % (tag, p) for p in problems(release, tree, tag)]
        found += base_problems(tree)
        found += failures
        notes = []
        at_tag = tag_idl(tag)
        if at_tag is None and os.environ.get("GITLAB_CI") == "true":
            # A CI checkout has no tag: there the baseline is held to its tag all the same, fetched, and the
            # job fails when it cannot be, so that a change of hMailServer.idl and the baseline together
            # cannot pass where nothing reads the tag.
            fetched = fetch_release_tags(tag)
            if fetched:
                found.append("in CI the baseline is held to the tag it names, and %s: it is not held" % fetched)
            else:
                notes.append("fetched %s and the release tags after it from origin" % tag)
                at_tag = tag_idl(tag)
                if at_tag is None:
                    found.append("fetched %s from origin, and this clone still has no such tag" % tag)
        if at_tag is None:
            if os.environ.get("GITLAB_CI") != "true":
                notes.append("this clone has no tag %s: the committed baseline is held as it is" % tag)
        else:
            if at_tag[0] != commit:
                found.append("build/com-abi-baseline.txt names %s as commit %s, where the tag is %s" % (tag, commit, at_tag[0]))
            tagged = parse(at_tag[1])
            if baseline_text(tagged, tag, at_tag[0]) != baseline_text(
                    {"interfaces": release["interfaces"], "types": release["types"]}, tag, at_tag[0]):
                found.append("build/com-abi-baseline.txt is not what %s's hMailServer.idl says: write it again with "
                             "python build/check-com-abi.py --write-baseline %s" % (tag, tag))
                # And the tree against what the tag says, so that what the edited baseline let through is named.
                found += ["against %s's own hMailServer.idl: %s" % (tag, p) for p in problems(
                    {"types": tagged["types"], "interfaces": tagged["interfaces"]}, tree, tag)]
            for newer in newer_tags(tag):
                newer_idl = tag_idl(newer)
                if newer_idl is None:
                    continue
                parsed = parse(newer_idl[1])
                found += ["against %s: %s" % (newer, p) for p in problems(
                    {"types": parsed["types"], "interfaces": parsed["interfaces"]}, tree, newer)]
                notes.append("%s is newer than the baseline's %s: python build/check-com-abi.py --write-baseline %s"
                             % (newer, tag, newer))
    except (IdlError, OSError) as error:
        sys.stdout.write("  FAIL  check-com-abi: %s\n" % error)
        return 2

    for note in notes:
        sys.stdout.write("  NOTE  %s\n" % note)
    if found:
        for problem in found:
            sys.stdout.write("  FAIL  %s\n" % problem)
        sys.stdout.write("\n%d problem(s): the tree does not keep %s's COM members in their places, or the check could "
                         "not show that it would see it. A dual interface's vtable follows the order its members are "
                         "declared in: declare a new member after the last one the release had, and never move, remove "
                         "or change one it had.\n" % (len(found), tag))
        return 1
    members = sum(len(entry[2]) for entry in release["interfaces"].values())
    values = sum(1 for line in release["types"] if line.startswith("enum "))
    sys.stdout.write("  OK    %d interface(s) and %d member(s) of %s are in the tree in their places, with %d type "
                     "identities and %d enumerators' values; %d negative controls refused or passed as they should\n"
                     % (len(release["interfaces"]), members, tag, len(release["types"]) - values, values, controls))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
