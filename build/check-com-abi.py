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
as written, but not their names, which no binding reads by position, nor their
defaultvalue or help text. A parameter's name is its last word unless that word
is one a type is written with (TYPE_WORDS, and every word that begins with _):
MIDL builds "[in] long long", "[in] long double", "[in] long unsigned" and
"BSTR * __ptr32", parameters with no name, as other types than "long" and
"BSTR *", so their last word is read as part of the type. This holds the
tree's hMailServer.idl to it: every interface the release had is there, with
its IID, base and those attributes, and its members as the tree declares them
begin with the release's, one for one - so a member is only ever added after
the last one the release had. A member inserted, removed, moved, given another
dispatch id or kind, or given another type or direction of a parameter is
refused, naming the interface, the member's place and what the release had
there, and so is dual taken off. A parameter's type is held as it is written,
and so a declaration that could give the words it is written with another
meaning is refused: MIDL takes hMailServer.idl's own typedef, enum, enumerator,
interface, coclass or library of a name for the type of every member written
with that name after it, one the Windows SDK defines too among them (typedef
hyper LONG; after the imports gave 49 parameters of 36 released members I8 in
the type library for I4, while every member's text stayed the release's), so a
typedef of anything but an enum stops the parse, and a name the tree declares
that the release's members are written with as a type (LONG, BSTR,
VARIANT_BOOL, IDispatch) is refused unless the release declared it so too. The
type library's own identity - the library's uuid and version, and each
coclass's uuid and default interface - is held the same way, and so is each
enumerator's value, which an early-bound client compiles in: an enumerator the
release had is in its enum with its value. A new interface, coclass, enum or
enumerator is free, and so is anything added at an interface's end. Every
interface of the tree, new ones too, must be declared interface <name> : <base>
{ ... }; with IDispatch or IUnknown its base: one derived from another of the
file's interfaces has its base's members first in its vtable, so a member
appended to the base, which this lets through, would move each of its own, and
the check, which counts an interface's members from the slot after IDispatch's
or IUnknown's, refuses such a base rather than pass it. None derives from
another today.

The file is read only in the forms it is written with, each release's since
6.0.0 the same, and every other form is refused, naming the line, rather than
taught to the reader: at file level the two imports, import "oaidl.idl"; and
import "ocidl.idl";, an interface's forward declaration interface <name>;, an
interface's definition [<attributes>] interface <name> : <base> { ... };, an
enum typedef [<attributes>] enum { ... } <name>; (with no tag) and one library
[<attributes>] library <name> { ... };; in the library importlib("stdole2.tlb");
and [<attributes>] coclass <name> { [default] interface <name>; };; in an
interface's body [<attributes>] HRESULT <name>(<parameters>); alone, each
parameter [<attributes>] followed by words and *s; in an enum's body
[<attributes>] <name> = <integer>. Each attribute list is one list on one of
these, and gives the attributes hMailServer.idl gives that place and no other
(ATTRIBUTES): an interface object, uuid, dual, nonextensible, helpstring and
pointer_default(unique); a member propget, propput, id, helpstring and
defaultbind; a parameter in, out, retval and defaultvalue; an enum uuid and
helpstring; an enumerator hidden and helpstring; the library uuid, version and
helpstring; a coclass uuid and helpstring; a coclass's part default - each
argument in the form the file writes it (a uuid, a string literal, an integer,
<major>.<minor>, unique), never a name the build's or the compiler's own
definitions could give a value. So MIDL's other statements are refused at file
level (cpp_quote, midl_pragma, a typedef of a structure, a union or a type,
const, struct, union, module, dispinterface, a coclass or an importlib outside
the library, declare_guid, _Pragma and __pragma, an attribute list on anything
but an interface or the library), in the library (an interface, a typedef,
cpp_quote, another importlib) and in an interface's body (a member without its
attribute list, a cpp_quote, a word after a member's parameters, a parameter
such as SAFEARRAY(BSTR)), and so are MIDL's other attributes, wherever they
stand: a member's call_as() and local, by which MIDL fills a slot otherwise
(call_as(X) puts the [local] method X, with X's parameters, in the member's
slot of the header the server's classes implement), an interface's local,
oleautomation and async_uuid, a member's vararg, restricted and propputref, a
parameter's optional, lcid, size_is, iid_is and string, an enum's v1_enum, a
coclass's noncreatable and a part's source or restricted (a coclass has one
part, so a second [default] is refused too), and two attribute lists in a row,
which MIDL merges where a reader of one list would read either ([default]
[source] on a coclass's part made another interface its default; [local] as a
list of its own made an interface local). A member that returns other than
HRESULT stops the parse as well. A change of hMailServer.idl that needs a form,
a statement or an attribute the file does not have yet changes this check with
it, which says then what it holds of it.

What it does not hold: an interface's other attributes (helpstring,
nonextensible, pointer_default), a member's attributes other than its kind and
dispatch id, an enum's own uuid, the parameters' names, defaultvalues and other
attributes, what the Windows SDK's oaidl.idl and ocidl.idl declare, which the
file imports (an import of any other file is refused, and so is a file beside
hMailServer.idl that MIDL, which looks in the directory it runs in first, would
read in place of the SDK's: another .idl, an .acf, a type library or one of
SDK_HEADERS), the options the build runs MIDL with (the vcxproj's Midl settings
and what MSBuild adds to them), and whatever hMailServer.idl does not declare,
such as the registry entries an install writes.

The baseline is written from a release tag, never from the tree:

  python build/check-com-abi.py --write-baseline v6.3.8

after a release is tagged (RELEASE.md says when), which reads that tag's
hMailServer.idl through git. A clone that has the baseline's tag is also held to
the baseline being what that tag's hMailServer.idl says, so that the file cannot
be edited to let a change through, and the tree is held to each newer release
tag it has as well, so that a baseline not yet rewritten after a release still
holds that release. In a CI job (GITLAB_CI=true) the tags are not taken from
the clone, which may have none (a checkout a commit deep), the tags its fork was
made with, or tags that something run in a clone the runner keeps between jobs
made or moved: every run lists the release tags of the project the releases are
tagged in (PROJECT_URL, gitlab.com's hmailserver/hmailserver, whatever project
the job runs in, so that a fork's merge request is held to the project's
releases), fetches the baseline's tag and every newer one from it, a commit deep
each, over the clone's own, and holds the baseline and the tree to them; a tag
the clone had that pointed elsewhere than the project's is a problem, and so is
a tag that cannot be listed or fetched - so a change of hMailServer.idl that
edits the baseline to match, or names an older release in it, is refused there,
whatever the clone held. Elsewhere a clone without the tag holds the tree to the
baseline as committed, and says so.

The check proves its negative controls on every run (--self-test runs only
those), against the tree's hMailServer.idl changed in memory: a member inserted
above the release's (the two ACME methods put back after GetReputationReport),
one removed, two swapped, a dispatch id changed, a parameter's type and its
direction changed, a propget made a method, a released [in] long given no name
as [in] long long, [in] long double and [in] long unsigned, a released
[out, retval] BSTR * given none as BSTR * __ptr32, an interface's IID changed,
an interface taken away, a new interface derived from a released one, dual
taken off an interface, an enumerator's value changed, an enumerator removed,
a new enum named LONG and one named VARIANT_BOOL, an interface declared as
BSTR and an enumerator named LONG, and a file named oaidl.idl, wtypesbase.idl,
hMailServer.acf, stdole2.tlb or basetsd.h beside hMailServer.idl must each be
refused, and a member appended, a member written over two lines, a member
commented out where it would have been inserted, a new interface derived from
IDispatch and an enumerator appended must each pass. Every HRESULT of the file
must be a member of an interface body the parse read, so a member it stopped
reading is not one it silently passes. And each of the forms above that the
file does not use is a control that must stop the parse with its refusal: at
file level typedef hyper LONG; alone and between cpp_quote("#if 0") and
cpp_quote("#endif"), a typedef of a structure, midl_pragma, const, struct,
union, declare_guid, _Pragma, __pragma, an importlib, a module, a coclass, an
attribute list on nothing and a dispinterface; in the library a typedef, a
cpp_quote, an interface and another importlib; in an interface's body a member
without its attribute list, a cpp_quote, a word after a member's parameters and
a SAFEARRAY(BSTR) parameter; a coclass's part given [default] [source], [local]
as a list of its own above an interface's, an interface given local and given
oleautomation, a member given vararg, an enum v1_enum, an enumerator
restricted, the library lcid(0), a coclass noncreatable, a parameter optional
and a coclass's part restricted, a member's id given as a name and its
helpstring as a wide string, a member's id taken off, an enum given a tag; a
released member made call_as() of a [local] method appended with other
parameters and one made [local], a coclass given a [default, source], a
[source] or a second [default] interface, and a new interface derived from
IUnknown with a member that returns void.

The text is read as MIDL reads it or not at all. Its tokens are string
literals, comments of both kinds, words and single characters; MIDL runs the C
preprocessor over hMailServer.idl first, and reads character literals, which
this does not. So it reads, outside strings and comments, words and the single
characters ( ) * , - : ; = [ ] { } alone, which are all hMailServer.idl has
been written with since 6.0.0, with only spaces, tabs and line feeds between
them, and refuses, naming the line, a file in which MIDL could read otherwise:
a preprocessor line (#if 0 ... #endif around a member takes it out of the
vtable, an #include in a body brings members in, each moving every member
below), the same directives spelt %:, which the preprocessor obeys as it does
# (a %:define can declare a member where the macro is used, or make
VARIANT_BOOL a long), and any other character - a < or > of the digraphs <: :>
<% %>, a ', a " that opens no string on its line, a character not ASCII, a
form feed; a line ending in a backslash (which joins the next line on, so a //
comment can swallow the member under it); a carriage return not followed by a
line feed, anywhere (the preprocessor takes it as part of the line, so after a
// comment the rest of the line, a member among it, is comment, where a reader
that split lines at it would read the member - the file is read from its bytes,
a carriage return and a line feed taken for a line feed and nothing else); and
an import of a file other than the Windows SDK's two. A ' is a character
literal, which MIDL takes as an attribute's argument: in custom(<uuid>, '"') a
reader that knew no such literal would take the " to open a string, and a
member written between two such literals on one line would be text to it where
MIDL reads a member. hMailServer.idl has none of them; a #, a %, a ' or a "
inside a string or a comment is text. And it reads an enumerator only with its
value written out, as each is, refusing one whose value C or MIDL would work
out. A released member inside #if 0 ... #endif, an #include at the top of a
body, a // comment ending in a backslash above a member, a member between two
'"' on one line in custom() and in helpcontext(), the same with L'"', a
%:define of a member used in a member's attributes, a released member inside
%:if 0 ... %:endif in the middle of an attribute's argument, a %:include at the
top of a body, a // comment followed by a carriage return and a released
member, read from bytes with CRLF line ends, a no-break space between two
tokens, an import of another file, a new interface with no base and an
enumerator without its value are more of the controls, and each must stop the
parse with its refusal.

Usage:  python build/check-com-abi.py [--self-test] [--write-baseline TAG]
Exit 0 when every interface keeps the release's members, 1 when one does not
or, in CI, the baseline's tag cannot be listed or fetched from the project or a
tag the clone had is not the project's, 2 for a usage or git error or an
hMailServer.idl this does not read, such as one with a preprocessor line or a
statement or an attribute the file is not written with.
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
# The project the releases are tagged in, which a CI job fetches the release tags from: not the clone's origin,
# which in a fork's pipeline is the fork, and not a variable, which a fork's settings could give another value.
PROJECT_URL = "https://gitlab.com/hmailserver/hmailserver.git"

# A string literal (helpstring text holds any punctuation: semicolons, brackets,
# "//" in a URL), a comment of either kind, a preprocessor line (which tokens()
# refuses), a word (an identifier, a number, a piece of a uuid), or any other
# single character, of which tokens() reads only SINGLES and refuses the rest.
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"|//[^\n]*|/\*.*?\*/|^[ \t]*#[^\n]*|[A-Za-z0-9_.]+|\S',
                   re.DOTALL | re.MULTILINE)
WORD = re.compile(r"^[A-Za-z0-9_.]+$")
# The single characters hMailServer.idl is written with outside its strings and comments, each release's since
# 6.0.0 as well; any other is refused, # (a directive) and % (%: is the preprocessor's other spelling of #, and
# <: :> <% %> of [ ] { }) among them. Between tokens only spaces, tabs and line feeds.
SINGLES = frozenset("()*,-:;=[]{}")
GAP = re.compile(r"[^ \t\n]")
# The files hMailServer.idl imports, the Windows SDK's: MIDL reads an imported file too, and this does not.
IMPORTS = ("oaidl.idl", "ocidl.idl")
# What MIDL reads through those imports and the library's importlib("stdole2.tlb"), in the Windows SDK
# 10.0.26100: it looks for each file in the directory it runs in, hMailServer.idl's, before the SDK's, and
# reads <name>.acf beside each <name>.idl, so a file there of one of these names, another .idl, an .acf or a
# type library would be read in place of the SDK's, where this check reads hMailServer.idl alone.
SDK_HEADERS = frozenset((
    "basetsd.h", "corecrt.h", "corecrt_memcpy_s.h", "corecrt_memory.h", "corecrt_wstring.h", "errno.h", "guiddef.h",
    "idispids.h", "msxmldid.h", "string.h", "vcruntime.h", "vcruntime_string.h", "winapifamily.h",
    "winpackagefamily.h", "xmldomdid.h", "xmldsodid.h"))
SHADOWING = re.compile(r"\.(?:idl|acf|tlb|olb)$", re.I)
KINDS = ("propget", "propput", "propputref")
# What a parameter's attributes say that the vtable and the marshalling read.
DIRECTIONS = ("in", "out", "retval", "lcid")
# The words a parameter's type is written with that are never its name, so that a parameter given no name
# ("[in] long long", as CriteriaMatch's "[in] eRuleMatchType" has none) is read with its whole type: C's and
# MIDL's type specifiers and qualifiers, and the older modifiers without a _ (type_word() takes every word
# that begins with _ as one too).
TYPE_WORDS = frozenset((
    "long", "short", "int", "char", "small", "hyper", "double", "float", "signed", "unsigned", "wchar_t", "byte",
    "boolean", "void", "const", "volatile", "far", "near", "huge", "pascal", "cdecl", "struct", "union", "enum",
    "register", "auto", "static", "extern", "inline", "interface", "handle_t", "error_status_t"))
# The attributes of a member by which MIDL fills its vtable slot otherwise than as the member is declared:
# call_as(X) puts the [local] method X there, with X's parameters, in the header the server's classes
# implement, while the type library keeps the member; local makes a member such an X. hMailServer.idl has
# neither, and a member with one is refused rather than read.
SLOT_ATTRIBUTES = ("call_as", "local")
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
    Outside strings and comments only words and SINGLES are read, so a % is refused
    as a # is (%:define, %:if 0 and %:include are directives to the preprocessor),
    and so is any other character the file is not written with; between tokens only
    spaces, tabs and line feeds. A carriage return that ends no line is refused
    wherever it is: a reader that splits lines at it would end a // comment there,
    where for the preprocessor the rest of the line, a member perhaps, is comment.
    """
    carriage = text.find("\r")
    if carriage >= 0:
        raise IdlError("line %d has a carriage return that is not followed by a line feed: the C preprocessor MIDL "
                       "runs takes it as part of the line, so after a // comment the text after it is comment too, "
                       "a member among it - end each line with a line feed (or a carriage return and a line feed)"
                       % line_number(text, carriage))
    splice = re.search(r"\\[ \t]*(?:\n|$)", text)
    if splice:
        raise IdlError("line %d ends in a backslash, which the C preprocessor MIDL runs first joins to the next line "
                       "(after a // comment, the next line is part of the comment): write it on one line"
                       % line_number(text, splice.start()))
    found = []
    previous = 0
    for match in TOKEN.finditer(text):
        odd = GAP.search(text, previous, match.start())
        if odd:
            raise IdlError("line %d has U+%04X between tokens: this check reads spaces, tabs and line feeds there "
                           "and nothing else, and hMailServer.idl holds nothing else" % (
                               line_number(text, odd.start()), ord(odd.group(0))))
        previous = match.end()
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
        if not value.startswith('"') and not WORD.match(value) and value not in SINGLES:
            raise IdlError("line %d has %s outside a string or a comment, where hMailServer.idl is written with words "
                           "and %s alone%s" % (
                               line_number(text, match.start()),
                               ("a %s" % value) if " " < value < "\x7f" else "U+%04X" % ord(value),
                               " ".join(sorted(SINGLES)),
                               (": %: is the C preprocessor's other spelling of #, a directive MIDL obeys (%:define, "
                                "%:if 0, %:include), and <: :> <% %> are its spellings of [ ] { }" if value in "%<>"
                                else ": this check reads no other, as MIDL might read it otherwise")))
        found.append((value, match.start(), match.end()))
    odd = GAP.search(text, previous)
    if odd:
        raise IdlError("line %d has U+%04X after the last token: this check reads spaces, tabs and line feeds there "
                       "and nothing else" % (line_number(text, odd.start()), ord(odd.group(0))))
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


def type_word(word):
    """Whether a word is one a parameter's type is written with and never its name: C's and MIDL's type
    specifiers and qualifiers (TYPE_WORDS), and every word that begins with _, as Microsoft's modifiers and
    sized types do (__ptr32, __int64, __RPC_FAR); no parameter of hMailServer.idl is named so."""
    return word in TYPE_WORDS or word.startswith("_")


# The statements hMailServer.idl is written with, at each level, and in each the attributes, which are all this
# reads - each release's since 6.0.0 has the same and no other: any other MIDL statement (cpp_quote, midl_pragma,
# a typedef of anything but an enum, const, struct, union, module, dispinterface, coclass outside the library,
# declare_guid, _Pragma and the rest) or attribute (local, oleautomation, async_uuid, vararg, optional, lcid,
# size_is, iid_is, string, v1_enum, source, restricted and the rest) is refused, naming the line, rather than read
# past, so that nothing MIDL reads is taken for nothing; a change of the file that needs another form changes this
# check with it.
FILE_FORMS = ('import "oaidl.idl"; and import "ocidl.idl";, interface <name>;, [<attributes>] interface <name> : '
              '<base> { <members> };, typedef [<attributes>] enum { <enumerators> } <name>; and [<attributes>] '
              'library <name> { ... };')
FILE_OTHERS = ("cpp_quote, midl_pragma, a typedef of anything but an enum, const, struct, union, module, "
               "dispinterface, a coclass or an importlib outside the library, declare_guid, _Pragma, __pragma")
LIBRARY_FORMS = 'importlib("stdole2.tlb"); and [<attributes>] coclass <name> { [default] interface <name>; };'
LIBRARY_OTHERS = "an interface, a typedef, cpp_quote, a module, a dispinterface, another importlib"
MEMBER_FORM = "[<attributes>] HRESULT <name>(<parameters>);"
IMPORTLIB = '"stdole2.tlb"'
# The attributes each place is given, with the form of each one's argument: None for none, "uuid", "string" (one
# string literal), "integer" (a dispatch id written out), "unique", "version" (<major>.<minor>) and "literal" (an
# integer or a string written out) - so that no attribute's argument is a name, which the build's own definitions
# (/D NDEBUG) or the compiler's could give a value.
ATTRIBUTES = {
    "interface": {"object": None, "uuid": "uuid", "dual": None, "nonextensible": None, "helpstring": "string",
                  "pointer_default": "unique"},
    "member": {"propget": None, "propput": None, "id": "integer", "helpstring": "string", "defaultbind": None},
    "parameter": {"in": None, "out": None, "retval": None, "defaultvalue": "literal"},
    "enum": {"uuid": "uuid", "helpstring": "string"},
    "enumerator": {"hidden": None, "helpstring": "string"},
    "library": {"uuid": "uuid", "version": "version", "helpstring": "string"},
    "coclass": {"uuid": "uuid", "helpstring": "string"},
    "coclass part": {"default": None},
}
# Those a place must have, as each has in hMailServer.idl.
REQUIRED_ATTRIBUTES = {"interface": ("object", "uuid"), "member": ("id",), "library": ("uuid", "version"),
                       "coclass": ("uuid",), "coclass part": ("default",)}
ARGUMENT_FORMS = {
    "uuid": re.compile(r"^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$"),
    "integer": re.compile(r"^(?:0[xX][0-9A-Fa-f]+|0|[1-9][0-9]*)$"),
    "unique": re.compile(r"^unique$"),
    "version": re.compile(r"^[0-9]+\.[0-9]+$"),
    "literal": re.compile(r'^(?:-?(?:0[xX][0-9A-Fa-f]+|0|[1-9][0-9]*)|"(?:[^"\\\n]|\\.)*")$'),
    "string": re.compile(r'^"(?:[^"\\\n]|\\.)*"$'),
}


def attribute_list(toks, index, place, text, owner):
    """({name: argument text or None}, the index after its ]) of the attribute list that opens at toks[index],
    read as the attributes hMailServer.idl gives a <place> (ATTRIBUTES), or IdlError: any other attribute, one
    given twice, an argument of another form, one REQUIRED_ATTRIBUTES names missing, and a second list right
    after it, which MIDL merges with it where this would read one."""
    where = line_number(text, toks[index][1])
    end = closing(toks, index, "[", "]")
    allowed = ATTRIBUTES[place]
    found = {}
    for part in split_top(toks[index + 1:end]):
        name = part[0][0] if part else ""
        line = line_number(text, part[0][1]) if part else where
        if place == "member" and name in SLOT_ATTRIBUTES:
            raise IdlError("line %d: %s's member has [%s]: MIDL fills a vtable slot by it otherwise than as the member "
                           "is declared (call_as(X) puts the [local] method X in the member's slot, with X's "
                           "parameters, in the header the server's classes implement), which this check does not "
                           "read; hMailServer.idl has neither" % (line, owner, name))
        if place == "coclass part" and name == "source":
            raise IdlError("line %d: %s has a source interface: MIDL takes the [default] part that is not a source for "
                           "the default interface a client creates it by, which this check reads only where there is "
                           "one [default] and no source, as in every coclass of hMailServer.idl" % (line, owner))
        if not WORD.match(name) or name not in allowed:
            raise IdlError("line %d: %s has [%s], which this check does not read: a %s of hMailServer.idl is given %s "
                           "alone, and any other attribute MIDL knows is refused rather than read past, as MIDL may "
                           "build the %s otherwise by it - a change that needs it changes build/check-com-abi.py "
                           "with it" % (line, owner, render(part)[:50] if part else "", place,
                                        ", ".join(sorted(allowed)), place))
        if name in found:
            raise IdlError("line %d: %s has [%s] twice" % (line, owner, name))
        argument = None
        if len(part) > 1:
            if part[1][0] != "(" or closing(part, 1, "(", ")") != len(part) - 1 or len(part) < 4:
                raise IdlError("line %d: %s's [%s] is not written %s(<argument>) or %s" % (line, owner, name, name,
                                                                                         name))
            argument = part[2:-1]
        form = allowed[name]
        if (form is None) != (argument is None) or form and (
                not ARGUMENT_FORMS[form].match(text[argument[0][1]:argument[-1][2]])
                or form == "string" and len(argument) != 1):
            raise IdlError("line %d: %s's [%s] is not written as hMailServer.idl writes it (%s): an argument of "
                           "another form, a name among them, is one this check does not read"
                           % (line, owner, render(part)[:50], ("%s(<%s>)" % (name, form)) if form else name))
        found[name] = "".join(token[0] for token in argument) if argument else None
    for name in REQUIRED_ATTRIBUTES.get(place, ()):
        if name not in found:
            raise IdlError("line %d: %s's attributes have no %s, which every %s of hMailServer.idl has"
                           % (where, owner, name, place))
    if end + 1 < len(toks) and toks[end + 1][0] == "[":
        raise IdlError("line %d: %s has two attribute lists in a row: MIDL merges them, where this check reads "
                       "one list a place - write them as one" % (line_number(text, toks[end + 1][1]), owner))
    return found, end + 1


def parameter(toks, text, owner):
    direction = []
    if toks and toks[0][0] == "[":
        attrs, after = attribute_list(toks, 0, "parameter", text, owner)
        direction = [name for name in DIRECTIONS if name in attrs]
        toks = toks[after:]
    if not toks or not WORD.match(toks[0][0]) or any(not WORD.match(value) and value != "*" for value, _, _ in toks):
        raise IdlError("line %d: %s has a parameter written %s, which this check does not read: every parameter of "
                       "hMailServer.idl is [<attributes>] <type words and *> [<name>], or void"
                       % (line_number(text, toks[0][1]) if toks else 0, owner, render(toks)[:50] or "with no type"))
    # The last word is the parameter's name, unless the type is all there is or the word is one a type is
    # written with: "[in] long long", "[in] long double" and "BSTR * __ptr32" name no parameter, and MIDL
    # builds each as another type than "long" or "BSTR *".
    if len(toks) > 1 and WORD.match(toks[-1][0]) and not type_word(toks[-1][0]):
        toks = toks[:-1]
    text_of = render(toks)
    return ("[%s] " % ",".join(direction) if direction else "") + text_of


def member(toks, interface, text):
    where = line_number(text, toks[0][1])
    if toks[0][0] != "[":
        raise IdlError("line %d: %s has %s where hMailServer.idl writes a member %s: a member without its attribute "
                       "list, a cpp_quote or another statement in an interface's body is one this check does not read"
                       % (where, interface, render(toks)[:50], MEMBER_FORM))
    attrs, after = attribute_list(toks, 0, "member", text, interface)
    toks = toks[after:]
    opening = next((i for i, token in enumerate(toks) if token[0] == "("), None)
    if opening is None or opening < 2:
        raise IdlError("line %d: %s's member %s is not written %s" % (where, interface, render(toks)[:50], MEMBER_FORM))
    end = closing(toks, opening, "(", ")")
    name = toks[opening - 1][0]
    returns = render(toks[:opening - 1])
    if returns != "HRESULT":
        raise IdlError("line %d: %s's member %s returns %s: this check reads members that return HRESULT, as every "
                       "member of hMailServer.idl does, and counts the file's HRESULTs against them"
                       % (where, interface, name, returns or "nothing"))
    if not WORD.match(name) or end != len(toks) - 1:
        raise IdlError("line %d: %s's member %s is not written %s: a member with anything after its parameters, or "
                       "not named by a word, is one this check does not read"
                       % (where, interface, render(toks)[:50], MEMBER_FORM))
    parameters = [parameter(part, text, "%s's member %s" % (interface, name))
                  for part in split_top(toks[opening + 1:end])]
    if parameters == ["void"]:
        parameters = []
    kind = next((k for k in KINDS if k in attrs), "method")
    dispid = attrs.get("id")
    return "%s id(%s) %s %s(%s)" % (kind, dispid if dispid is not None else "-", returns, name, ", ".join(parameters))


def enumerators(toks, text, owner):
    """(["<name> = <value>", ...], [name, ...]) of the enumerators between an enum's braces. Only a value written
    out is read: C gives an enumerator without one its predecessor's plus one, and MIDL takes an expression,
    neither of which this computes, so either is refused; every enumerator of hMailServer.idl has an integer of
    its own."""
    found, names = [], []
    for part in split_top(toks):
        if not part:
            raise IdlError("%s has two commas with no enumerator between them" % owner)
        if part[0][0] == "[":
            _, after = attribute_list(part, 0, "enumerator", text, "%s's enumerator" % owner)
            part = part[after:]
        values = [token[0] for token in part]
        literal = "".join(values[2:])
        if len(values) < 3 or values[1] != "=" or not WORD.match(values[0]) or not ENUM_VALUE.match(literal):
            raise IdlError("line %d: the enumerator %s is not written <name> = <integer>: this check reads only an "
                           "enumerator's value written out, as every one in hMailServer.idl is, and not one C or MIDL "
                           "would work out - write its value" % (line_number(text, part[0][1]) if part else 0,
                                                                 render(part)[:60]))
        found.append("%s = %d" % (values[0], int(literal, 0)))
        names.append((values[0], part[0][1]))
    return found, names


def parse(text):
    """{"interfaces": {name: (iid, base, [member, ...], [binding attribute, ...])}, "types": [line, ...],
    "spans": {...}, "defined": {name: kind}}, read from the statements of FILE_FORMS and LIBRARY_FORMS alone."""
    toks = tokens(text)
    interfaces, spans, types, defined = {}, {}, [], {}
    libraries = []
    # Every HRESULT of the file, so that one outside the interface bodies read below stops the parse rather than
    # being passed unread.
    hresults = sum(1 for token in toks if token[0] == "HRESULT")

    def token(index):
        return toks[index][0] if 0 <= index < len(toks) else ""

    def at(index):
        return line_number(text, toks[min(index, len(toks) - 1)][1]) if toks else 0

    def define(name, kind, offset):
        # One name, one declaration: an interface's forward declaration and its definition aside.
        if name in defined and (defined[name], kind) != ("interface", "interface"):
            raise IdlError("line %d: %s is declared as %s and as %s: MIDL takes the last for the name, where this "
                           "check reads each" % (line_number(text, offset), name, defined[name], kind))
        defined[name] = kind

    def unread(index, forms, others, level):
        raise IdlError("line %d: %s is not a statement this check reads %s: hMailServer.idl is written there with %s "
                       "alone, and any other MIDL statement (%s) is refused rather than read past, as MIDL might "
                       "read it otherwise - a typedef hyper LONG; makes every member written with LONG another type "
                       "in the type library while the members' text stays the release's; a change that needs "
                       "another changes build/check-com-abi.py with it"
                       % (at(index), render(toks[index:index + 3])[:50], level, forms, others))

    def attributed(index, places, level):
        """The keyword an attribute list at toks[index] is on, or IdlError for one on another."""
        end = closing(toks, index, "[", "]")
        if token(end + 1) == "[":
            raise IdlError("line %d: two attribute lists in a row: MIDL merges them, where this check reads one list "
                           "a place - write them as one" % at(end + 1))
        if token(end + 1) not in places:
            raise IdlError("line %d: an attribute list on %s: %s hMailServer.idl gives one to %s alone, and an "
                           "attribute list on anything else is one this check does not read"
                           % (at(index), render(toks[end + 1:end + 3])[:40] or "nothing", level,
                              " or ".join(places)))
        return end

    def read_interface(index):
        end = closing(toks, index, "[", "]")
        name = token(end + 2)
        attrs, i = attribute_list(toks, index, "interface", text, "interface " + name)
        if not WORD.match(name) or token(i + 2) != ":" or not WORD.match(token(i + 3)) or token(i + 4) != "{":
            raise IdlError("line %d: interface %s is not declared as interface <name> : <base> { ... }; under its "
                           "attribute list, or as interface <name>; - one with no base is a vtable this check does "
                           "not read, and every interface of hMailServer.idl derives from IDispatch" % (at(i), name))
        base, brace = token(i + 3), i + 4
        close = closing(toks, brace, "{", "}")
        if token(close + 1) != ";" or close > brace + 1 and token(close - 1) != ";":
            raise IdlError("line %d: interface %s's body or its } is not ended by a ;, as hMailServer.idl ends each"
                           % (at(close), name))
        members, member_spans = [], []
        for part in split_top(toks[brace + 1:close], ";"):
            if not part:
                raise IdlError("line %d: interface %s has a ; with no member before it" % (at(close), name))
            members.append(member(part, name, text))
            member_spans.append((part[0][1], part[-1][2] + 1))   # with its ;
        if name in interfaces:
            raise IdlError("interface %s is defined twice" % name)
        define(name, "interface", toks[i + 1][1])
        interfaces[name] = (attrs["uuid"].upper(), base, members, [flag for flag in BINDING if flag in attrs])
        spans[name] = member_spans
        return close + 2

    def read_enum(index):
        typedef = ("line %d: a typedef hMailServer.idl does not have: each of its typedefs is typedef [<attributes>] "
                   "enum { <enumerators> } <name>;, and a typedef of anything else is refused - MIDL takes a "
                   "typedef in hMailServer.idl for the type of every member written with its name after it, one the "
                   "Windows SDK defines (LONG, BSTR, VARIANT_BOOL) among them, where this check reads the members' "
                   "text: typedef hyper LONG; made 49 parameters of the release's members I8 for I4" % at(index))
        if token(index + 1) != "[":
            raise IdlError(typedef)
        attrs, i = attribute_list(toks, index + 1, "enum", text, "a typedef")
        if token(i) != "enum":
            raise IdlError(typedef)
        if token(i + 1) != "{":
            raise IdlError("line %d: enum %s: hMailServer.idl's enums have no tag, and a tag is a name C shares with "
                           "the structures' tags of the Windows SDK (tagVARIANT): write typedef [<attributes>] enum "
                           "{ ... } <name>;" % (at(i), token(i + 1)))
        close = closing(toks, i + 1, "{", "}")
        name = token(close + 1)
        if not WORD.match(name) or token(close + 2) != ";":
            raise IdlError("line %d: an enum not named as typedef [<attributes>] enum { ... } <name>;" % at(close))
        entries, names = enumerators(toks[i + 2:close], text, "enum " + name)
        define(name, "enum", toks[close + 1][1])
        for enumerator, offset in names:
            define(enumerator, "enumerator", offset)
        types.extend("enum %s %s" % (name, entry) for entry in entries)
        return close + 3

    def read_coclass(index):
        end = closing(toks, index, "[", "]")
        name = token(end + 2)
        attrs, i = attribute_list(toks, index, "coclass", text, "coclass " + name)
        if not WORD.match(name) or token(i + 2) != "{":
            raise IdlError("line %d: coclass %s is not written coclass <name> { ... };" % (at(i), name))
        close = closing(toks, i + 2, "{", "}")
        if token(close + 1) != ";" or token(close - 1) != ";":
            raise IdlError("line %d: coclass %s's part or its } is not ended by a ;" % (at(close), name))
        # MIDL makes the part marked [default] that is not a source the coclass's default interface. Each coclass
        # of hMailServer.idl has one part, [default] interface <name>;: a source interface or a second part is
        # refused rather than read.
        default = None
        for part in split_top(toks[i + 3:close], ";"):
            if not part:
                raise IdlError("line %d: coclass %s has a ; with no part before it" % (at(close), name))
            if part[0][0] != "[":
                raise IdlError("line %d: coclass %s has a part without its [default]: each coclass of hMailServer.idl "
                               "has one part, [default] interface <name>;" % (line_number(text, part[0][1]), name))
            _, after = attribute_list(part, 0, "coclass part", text, "coclass " + name)
            rest = [value for value, _, _ in part[after:]]
            if len(rest) != 2 or rest[0] != "interface" or not WORD.match(rest[1]):
                raise IdlError("line %d: coclass %s has a part written %s: each coclass of hMailServer.idl has one "
                               "part, [default] interface <name>;" % (line_number(text, part[0][1]), name,
                                                                     render(part)[:50]))
            if default is not None:
                raise IdlError("line %d: coclass %s has a second [default] part: MIDL takes the [default] part that "
                               "is not a source for the default interface a client creates it by, which this check "
                               "reads only where there is one [default] and no source, as in every coclass of "
                               "hMailServer.idl" % (line_number(text, part[0][1]), name))
            default = rest[1]
        if default is None:
            raise IdlError("line %d: coclass %s has no interface" % (at(close), name))
        define(name, "coclass", toks[i + 1][1])
        types.append("coclass %s uuid(%s) default %s" % (name, attrs["uuid"].upper(), default))
        return close + 2

    def read_library(index):
        end = closing(toks, index, "[", "]")
        name = token(end + 2)
        attrs, i = attribute_list(toks, index, "library", text, "library " + name)
        if not WORD.match(name) or token(i + 2) != "{":
            raise IdlError("line %d: library %s is not written library <name> { ... };" % (at(i), name))
        close = closing(toks, i + 2, "{", "}")
        if token(close + 1) != ";":
            raise IdlError("line %d: library %s's } is not followed by its ;" % (at(close), name))
        if libraries:
            raise IdlError("line %d: a second library: hMailServer.idl has one" % at(i))
        libraries.append(name)
        define(name, "library", toks[i + 1][1])
        types.append("library %s uuid(%s) version(%s)" % (name, attrs["uuid"].upper(), attrs.get("version")))
        j = i + 3
        while j < close:
            if token(j) == "importlib":
                if [token(j + k) for k in range(1, 5)] != ["(", IMPORTLIB, ")", ";"]:
                    raise IdlError("line %d: an importlib of other than %s: MIDL reads the type library it names, "
                                   "and this check does not" % (at(j), IMPORTLIB))
                j += 5
            elif token(j) == "[":
                attributed(j, ("coclass",), "in the library")
                j = read_coclass(j)
            else:
                unread(j, LIBRARY_FORMS, LIBRARY_OTHERS, "in the library")
        if j != close:
            raise IdlError("line %d: library %s's body is not read to its }" % (at(j), name))
        return close + 2

    index = 0
    while index < len(toks):
        value = token(index)
        if value == "import":
            names = []
            for k in range(index + 1, len(toks)):
                if toks[k][0] == ";":
                    break
                names.append(toks[k][0])
            if len(names) != 1 or names[0].strip('"') not in IMPORTS or not names[0].startswith('"'):
                raise IdlError("line %d imports %s: MIDL reads an imported file as well, and this check reads "
                               "hMailServer.idl alone, which imports the Windows SDK's %s and nothing else, each by "
                               "an import of its own" % (at(index), " ".join(names) or "nothing", " and ".join(IMPORTS)))
            index += 3
        elif value == "interface":
            if not WORD.match(token(index + 1)) or token(index + 2) != ";":
                raise IdlError("line %d: interface %s is not declared as interface <name> : <base> { ... }; under "
                               "its attribute list, or as interface <name>; - one with no base is a vtable this check "
                               "does not read, and every interface of hMailServer.idl derives from IDispatch"
                               % (at(index), token(index + 1)))
            define(token(index + 1), "interface", toks[index + 1][1])
            index += 3
        elif value == "typedef":
            index = read_enum(index)
        elif value == "[":
            end = attributed(index, ("interface", "library"), "at file level")
            index = read_interface(index) if token(end + 1) == "interface" else read_library(index)
        else:
            unread(index, FILE_FORMS, FILE_OTHERS, "at file level")
    if not libraries:
        raise IdlError("hMailServer.idl has no library: the type library's identity cannot be read")
    counted = sum(len(entry[2]) for entry in interfaces.values())
    if counted != hresults:
        raise IdlError("read %d members in the interfaces' bodies, where the file holds %d HRESULTs: one outside "
                       "every interface body this reads is one it would pass unread" % (counted, hresults))
    return {"interfaces": interfaces, "types": types, "spans": spans, "defined": defined}


def release_names(release):
    """(the words the release's members are written with as types - their return types' and their parameters' -
    with IDispatch and IUnknown, {name: kind} of what the release declared)."""
    words, own = {"IDispatch", "IUnknown"}, {}
    for name, entry in release["interfaces"].items():
        own[name] = "interface"
        for written in entry[2]:
            found = re.match(r"^\S+ id\(\S*\) (.*?) \w+\((.*)\)$", written)
            if found:
                words.update(re.findall(r"[A-Za-z_]\w*", found.group(1) + " "
                                        + re.sub(r"\[[^\]]*\]", " ", found.group(2))))
    for line in release["types"]:
        kind, name = line.split(" ")[:2]
        own[name] = kind
        if kind == "enum":
            own[line.split(" ")[2]] = "enumerator"
    return words, own


def name_problems(release, tree, label):
    """A name the tree declares that the release's members are written with as a type, but not as the release
    declared it: MIDL takes hMailServer.idl's own declaration for the type of every member written with the name
    after it, so a released member would keep its text and change its type (an enum named LONG, an interface
    named BSTR)."""
    words, own = release_names(release)
    return ["%s: the tree declares it as %s %s, where %s's members are written with %s as a type%s: MIDL takes the "
            "file's own declaration for the type of every member written with the name after it, so that a "
            "released parameter keeps its text and changes its type - name it otherwise"
            % (name, "an" if kind[0] in "aeiou" else "a", kind, label, name,
               (" and %s declared it as %s" % (label, own[name])) if name in own else "")
            for name, kind in sorted(tree.get("defined", {}).items()) if name in words and own.get(name) != kind]


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
    found = name_problems(release, tree, label)
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


def shadow_problems(names):
    """The files beside hMailServer.idl that MIDL, which looks in the directory it runs in first, would read in
    place of the Windows SDK's: another .idl, an .acf, a type library, or one of SDK_HEADERS."""
    own = os.path.basename(IDL).lower()
    return ["%s/%s: MIDL, run in hMailServer.idl's directory, looks for the files it imports there before the "
            "Windows SDK's, so it would read this in place of the SDK's file of the name (or as hMailServer.idl's "
            "attributes, an .acf), where this check reads hMailServer.idl alone - keep it elsewhere"
            % (os.path.dirname(IDL_PATH), name) for name in sorted(names)
            if name.lower() != own and (SHADOWING.search(name) or name.lower() in SDK_HEADERS)]


def git(*arguments, timeout=120):
    try:
        # No prompt for a password: the project's tags are read anonymously, and a failure is a problem.
        done = subprocess.run(["git", "-C", ROOT] + list(arguments), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout,
                              env=dict(os.environ, GIT_TERMINAL_PROMPT="0"))
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
    return commit, decode(blob)


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


def local_tag(name):
    """What this clone's refs/tags/<name> points at (a tag object or a commit), or None."""
    found = git("rev-parse", "--verify", "--quiet", "refs/tags/%s" % name)
    return found.decode("ascii").strip() if found else None


def fetch_release_tags(release):
    """In a CI job: the release tags of the project the releases are made in (PROJECT_URL, whatever project the
    job runs in - a fork's has the tags it was made with alone - and whether or not the clone has tags already),
    the baseline's and every newer one, fetched a commit deep each over this clone's own, so that the baseline is
    held to what its tag says and the tree to every release after it. (problems, the newer tags): a tag this clone
    had that pointed elsewhere than the project's is a problem, as a tag made or moved by anything that ran in a
    clone the runner keeps between jobs is not believed."""
    listed = git("ls-remote", "--tags", "--refs", PROJECT_URL, timeout=300)
    if listed is None:
        return ["%s's tags could not be listed (git ls-remote --tags)" % PROJECT_URL], []
    names = {}
    for line in listed.decode("ascii", errors="replace").splitlines():
        found = re.match(r"^([0-9a-f]{40})\trefs/tags/(v\d+\.\d+\.\d+)$", line)
        if found:
            names[found.group(2)] = found.group(1)
    if release not in names:
        return ["%s has no tag %s, the release the baseline names" % (PROJECT_URL, release)], []
    own = tuple(int(part) for part in TAG.match(release).groups())
    newer = sorted((n for n in names if tuple(int(p) for p in TAG.match(n).groups()) > own),
                   key=lambda n: tuple(int(p) for p in TAG.match(n).groups()))
    wanted = [release] + newer
    problems = ["this clone's tag %s pointed at %s, where %s's points at %s: a tag made or moved in the clone is not "
                "believed, and the project's was fetched over it" % (name, local_tag(name), PROJECT_URL, names[name])
                for name in wanted if local_tag(name) not in (None, names[name])]
    if git("fetch", "--quiet", "--no-tags", "--depth=1", PROJECT_URL,
           *["+refs/tags/%s:refs/tags/%s" % (name, name) for name in wanted], timeout=600) is None:
        return problems + ["%s's tag(s) %s could not be fetched" % (PROJECT_URL, ", ".join(wanted))], newer
    problems += ["fetched %s from %s, and this clone's tag points at %s, not %s" % (name, PROJECT_URL,
                                                                                   local_tag(name), names[name])
                 for name in wanted if local_tag(name) != names[name]]
    return problems, newer


def decode(raw):
    """hMailServer.idl's bytes as the text tokens() reads: a carriage return and a line feed become a line feed,
    and a carriage return alone stays one, for tokens() to refuse - universal newlines would make it a line's end,
    which for the preprocessor it is not."""
    return raw.decode("utf-8-sig", errors="replace").replace("\r\n", "\n")


def read_tree():
    with open(IDL, "rb") as f:
        return decode(f.read())


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
    # The same directives spelt with the digraph %:, which the preprocessor obeys as it does #: a macro that
    # declares a member, used in the first member's attributes; the first member inside %:if 0 ... %:endif in
    # the middle of a custom() attribute's argument; and a %:include at the top of a body.
    custom = "custom(D3A9C1F0-0000-4000-8000-000000000001, %s)"
    # (The second definition closes the first one's [ for a reader that takes both for text.)
    macro = ("%%:define SELFTEST_HIDDEN 1)] HRESULT SelfTestMember([out, retval] long *Value); [%s\n"
             "%%:define SELFTEST_UNUSED ]\n" % (custom % "1")[:-1])
    after_imports = text.find("\n", text.find('import "%s";' % IMPORTS[-1])) + 1
    if after_imports <= 0 or after_imports > first[0]:
        raise IdlError("self-test: the tree does not import %s above %s" % (IMPORTS[-1], name))
    controls.append(("a %:define of a member after the imports, used in the first member's attributes",
                     text[:after_imports] + macro + text[after_imports:first[0]]
                     + one.replace("[", "[%s, " % (custom % "SELFTEST_HIDDEN"), 1) + text[first[1]:],
                     ("unread", "other spelling of #"), name))
    opened = "   [%s\n" % (custom % "1")[:-1]
    controls.append(("the first member inside %:if 0 ... %:endif in an attribute's argument", text[:first[0]] + opened
                     + "%:if 0\n   ), " + one.lstrip()[1:].rstrip("\n") + " " + opened.lstrip() + "%:endif\n   ), "
                     + two.lstrip()[1:] + text[second[1]:], ("unread", "other spelling of #"), name))
    controls.append(("a %:include at the top of a body", text[:first[0]] + "   %:include \"self-test.idl\"\n"
                     + text[first[0]:], ("unread", "other spelling of #"), name))
    # A carriage return alone after a // comment, the first member after it on the same line for the preprocessor:
    # the member is comment to MIDL, so that it is removed, where a reader that ended the line there reads it.
    # Read from bytes with CRLF line ends, as a Windows checkout has them, through the reader the tree is read by.
    lone = (text[:first[0]] + "   // note\r" + one + text[first[1]:]).replace("\n", "\r\n").encode("utf-8")
    controls.append(("a // comment and a carriage return before the first member on its line", decode(lone),
                     ("unread", "carriage return that is not followed"), name))
    controls.append(("a no-break space between two of the first member's tokens", text[:first[0]]
                     + one.replace("] HRESULT ", "]" + chr(0xA0) + "HRESULT ", 1) + text[first[1]:],
                     ("unread", "between tokens"), name))
    imported = text.replace('import "%s";' % IMPORTS[-1], 'import "%s";\nimport "self-test.idl";' % IMPORTS[-1], 1)
    if imported == text:
        raise IdlError("self-test: the tree does not import %s" % IMPORTS[-1])
    controls.append(("an import of another file", imported, ("unread", "MIDL reads an imported file"), name))
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
    # A parameter given no name whose type ends in a word a type is written with: MIDL builds each as another
    # type than the release's (I8, R8 and UI4 for a long; a 32-bit pointer in the header for a BSTR *), and a
    # reader that took the last word for the name would read the release's.
    interface, start, end = first_line(r"\[in\] long \w+\)")
    named = re.search(r"\[in\] long \w+\)", text[start:end]).group(0)
    for typed in ("long long", "long double", "long unsigned"):
        controls.append(("an [in] long made an unnamed [in] %s" % typed, text[:start] + text[start:end].replace(
            named, "[in] %s)" % typed, 1) + text[end:], True, interface))
    interface, start, end = first_line(r"\[out, retval\] BSTR \*\w+\)")
    named = re.search(r"\[out, retval\] BSTR \*\w+\)", text[start:end]).group(0)
    controls.append(("an [out, retval] BSTR * made an unnamed BSTR * __ptr32", text[:start] + text[start:end].replace(
        named, "[out, retval] BSTR * __ptr32)", 1) + text[end:], True, interface))
    # A member MIDL fills its slot with otherwise than as declared: call_as() of a [local] method of other
    # parameters appended to the interface, and [local] alone.
    end_of_body = line_of(spans[name][-1])[1]
    controls.append(("the first member made call_as() of a [local] method appended",
                     text[:first[0]] + one.replace("[", "[call_as(SelfTestLocal), ", 1) + text[first[1]:end_of_body]
                     + "   [local] HRESULT SelfTestLocal([in] long first, [in] long second);\n" + text[end_of_body:],
                     ("unread", "MIDL fills a vtable slot by it"), name))
    controls.append(("the first member made [local]", text[:first[0]] + one.replace("[", "[local, ", 1)
                     + text[first[1]:], ("unread", "MIDL fills a vtable slot by it"), name))
    # A coclass's default interface as MIDL takes it: the [default] part that is not a source.
    coclass = re.search(r"\bcoclass (\w+)\s*\{\s*\[default\] interface (\w+);", text)
    other = next((n for n in sorted(tree["interfaces"]) if coclass and n != coclass.group(2)), None)
    if not coclass or not other:
        raise IdlError("self-test: no coclass of the tree is coclass <name> { [default] interface <name>; ... }")
    part = coclass.group(0)[coclass.group(0).index("[default]"):]
    # (The first is the form MIDL makes another interface the default by: the [default] part that is not a source.)
    for what, parts, names in (
            ("a [default, source]", "[default] interface %s;\n      [default, source] interface %s;", (other, None)),
            ("a [source]", "[default] interface %s;\n      [source] interface %s;", (None, other)),
            ("a second [default]", "[default] interface %s;\n      [default] interface %s;", (other, None))):
        given = parts % tuple(n or coclass.group(2) for n in names)
        controls.append(("coclass %s given %s interface" % (coclass.group(1), what), text[:coclass.start()]
                         + coclass.group(0).replace(part, given) + text[coclass.end():],
                         ("unread", "coclass %s has" % coclass.group(1)), "coclass " + coclass.group(1)))
    controls.append(("a new interface derived from IUnknown with a member that returns void", text
                     + "\n[\n   object,\n   uuid(D3A9C1F0-0000-4000-8000-000000000006)\n]\n"
                       "interface SelfTestVoid : IUnknown {\n   [id(1)] void SelfTestMember(void);\n};\n",
                     ("unread", "returns void"), "SelfTestVoid"))
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
    # An interface with no base, which MIDL builds (with a warning), and a member in a dispinterface: HRESULTs
    # outside every interface body read, which must stop the parse rather than pass unread.
    controls.append(("a new interface with no base", text + new_interface(
        "SelfTestNoBase", "IDispatch", "D3A9C1F0-0000-4000-8000-000000000004").replace(" : IDispatch {", " {"),
        ("unread", "is not declared as interface"), "SelfTestNoBase"))
    controls.append(("a dispinterface with a member", text + "\n[\n   uuid(D3A9C1F0-0000-4000-8000-000000000005)\n]\n"
                     "dispinterface SelfTestDispatch {\nproperties:\nmethods:\n"
                     "   [id(1)] HRESULT SelfTestMember([out, retval] long *Value);\n};\n",
                     ("unread", "an attribute list on dispinterface"), "SelfTestDispatch"))
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

    # The statements and attributes the file is not written with, a family each, which the parse must refuse
    # rather than read past: at file level after the imports, in the library after its first importlib, and in
    # the body of the interface the controls above change.
    def uuid(number):
        return "D3A9C1F0-0000-4000-8000-0000000001%02d" % number

    statement = "is not a statement this check reads"
    for what, added, words in (
            ("typedef hyper LONG; between cpp_quote(\"#if 0\") and cpp_quote(\"#endif\")",
             "cpp_quote(\"#if 0\")\ntypedef hyper LONG;\ncpp_quote(\"#endif\")\n", statement),
            ("typedef hyper LONG;", "typedef hyper LONG;\n", "a typedef hMailServer.idl does not have"),
            ("a typedef of a structure", "typedef [uuid(%s)] struct SelfTestStruct { long a; } SelfTestStruct;\n"
             % uuid(1), "a typedef hMailServer.idl does not have"),
            ("midl_pragma", "midl_pragma warning (disable: 2111)\n", statement),
            ("const", "const long SelfTestConst = 1;\n", statement),
            ("struct", "struct SelfTestStruct { long a; };\n", statement),
            ("union", "union SelfTestUnion { long a; };\n", statement),
            ("declare_guid", "declare_guid(SelfTestGuid, %s);\n" % uuid(2), statement),
            ("_Pragma", "_Pragma(\"warning(disable: 2111)\")\n", statement),
            ("__pragma", "__pragma(warning(disable: 2111))\n", statement),
            ("an importlib outside the library", "importlib(\"stdole2.tlb\");\n", statement),
            ("a module", "[dllname(\"self-test.dll\")] module SelfTestModule { [entry(1)] HRESULT SelfTestMember(void); };\n",
             "an attribute list on module"),
            ("a coclass outside the library", "[uuid(%s)] coclass SelfTestClass { [default] interface %s; };\n"
             % (uuid(3), name), "an attribute list on coclass"),
            ("an attribute list on nothing", "[uuid(%s)];\n" % uuid(4), "an attribute list on ;")):
        controls.append(("at file level, %s" % what, text[:after_imports] + added + text[after_imports:],
                         ("unread", words), name))
    importlib = text.find('importlib("stdole2.tlb");')
    in_library = text.find("\n", importlib) + 1
    if importlib < 0 or in_library <= 0:
        raise IdlError("self-test: the tree's library has no importlib(\"stdole2.tlb\");")
    for what, added, words in (
            ("a typedef", "   typedef [uuid(%s)] enum { SelfTestLibrary = 1 } eSelfTestLibrary;\n" % uuid(5),
             "is not a statement this check reads in the library"),
            ("cpp_quote", "   cpp_quote(\"#define SelfTest 1\")\n", "is not a statement this check reads in the library"),
            ("an interface", "   [object, uuid(%s), dual] interface SelfTestInLibrary : IDispatch {\n"
             "      [id(1)] HRESULT SelfTestMember([out, retval] long *Value);\n   };\n" % uuid(6),
             "an attribute list on interface"),
            ("another importlib", "   importlib(\"self-test.tlb\");\n", "an importlib of other than")):
        controls.append(("in the library, %s" % what, text[:in_library] + added + text[in_library:],
                         ("unread", words), name))
    for what, added, words in (
            ("a member without its attribute list", "   HRESULT SelfTestMember([out, retval] long *Value);\n",
             "a member without its attribute list"),
            ("cpp_quote", "   cpp_quote(\"#define SelfTest 1\");\n", "a member without its attribute list"),
            ("a member with a word after its parameters",
             "   [id(9999)] HRESULT SelfTestMember([out, retval] long *Value) SelfTestMore;\n",
             "anything after its parameters"),
            ("a parameter of another form", "   [id(9999)] HRESULT SelfTestMember([in] SAFEARRAY(BSTR) Values);\n",
             "has a parameter written")):
        controls.append(("in %s's body, %s" % (name, what), text[:end_of_body] + added + text[end_of_body:],
                         ("unread", words), name))
    # Attribute lists: a second one, where MIDL merges both (a coclass part's [default] [source] made another
    # interface the default; a [local] list above an interface's made it local); an attribute each place is not
    # given; an argument written as a name; an attribute every such place has, taken off; and an enum's tag.
    two = "two attribute lists in a row"
    controls.append(("coclass %s's part given [default] [source]" % coclass.group(1), text[:coclass.start()]
                     + coclass.group(0).replace("[default]", "[default] [source]", 1) + text[coclass.end():],
                     ("unread", two), "coclass " + coclass.group(1)))
    controls.append(("[local] as a list of its own above %s's" % name, text[:head + 1] + "[local]\n"
                     + text[head + 1:], ("unread", two), name))
    first_enum = re.search(r"\btypedef\s*\[", text)
    first_enumerator = re.search(r"\[(?=helpstring\(\")", text[brace - 400 if brace > 400 else 0:brace])
    if not first_enum or not first_enumerator:
        raise IdlError("self-test: the tree has no typedef [ ... ] enum or no enumerator with a helpstring")
    enumerator_at = (brace - 400 if brace > 400 else 0) + first_enumerator.start()
    library_at = text.rfind("[", 0, text.find("library hMailServer"))
    other = "which this check does not read"
    for what, at, given in (
            ("an interface given [local]", head + 2, "local, "),
            ("an interface given [oleautomation]", head + 2, "oleautomation, "),
            ("the first member given [vararg]", first[0] + one.index("[") + 1, "vararg, "),
            ("an enum given [v1_enum]", first_enum.end(), "v1_enum, "),
            ("an enumerator given [restricted]", enumerator_at + 1, "restricted, "),
            ("the library given [lcid(0)]", library_at + 1, "lcid(0), "),
            ("a coclass given [noncreatable]", text.rfind("[", 0, coclass.start()) + 1, "noncreatable, ")):
        controls.append((what, text[:at] + given + text[at:], ("unread", other), name))
    interface, start, end = first_line(r"\[in\] BSTR ")
    controls.append(("an [in] BSTR given [optional]", text[:start] + text[start:end].replace("[in] BSTR ", "[in, optional] BSTR ", 1)
                     + text[end:], ("unread", other), interface))
    controls.append(("coclass %s's part given [restricted]" % coclass.group(1), text[:coclass.start()]
                     + coclass.group(0).replace("[default]", "[default, restricted]", 1) + text[coclass.end():],
                     ("unread", other), "coclass " + coclass.group(1)))
    form = "is not written as hMailServer.idl writes it"
    controls.append(("the first member's id given as a name", text[:first[0]] + re.sub(
        r"\bid\(\d+\)", "id(NDEBUG)", one, count=1) + text[first[1]:], ("unread", form), name))
    controls.append(("the first member's helpstring given as a wide string", text[:first[0]] + one.replace(
        "helpstring(\"", "helpstring(L\"", 1) + text[first[1]:], ("unread", form), name))
    controls.append(("the first member's id taken off", text[:first[0]] + re.sub(
        r"\bid\(\d+\),\s*", "", one, count=1) + text[first[1]:], ("unread", "have no id"), name))
    controls.append(("an enum given a tag", text[:first_enum.start()] + re.sub(
        r"\benum(\s*)\{", r"enum tagSelfTest\1{", text[first_enum.start():], count=1),
        ("unread", "have no tag"), name))
    # A name the release's members are written with as a type, declared by the tree: MIDL takes it for every
    # member written with it after the declaration, while each member's text stays the release's.
    for what, added, named in (
            ("a new enum named LONG", "typedef [uuid(%s)] enum { SelfTestLong = 1 } LONG;\n" % uuid(7), "LONG"),
            ("a new enum named VARIANT_BOOL", "typedef [uuid(%s)] enum { SelfTestFalse = 0 } VARIANT_BOOL;\n"
             % uuid(8), "VARIANT_BOOL"),
            ("an interface declared as BSTR", "interface BSTR;\n", "BSTR"),
            ("an enumerator named LONG", "typedef [uuid(%s)] enum { LONG = 1 } eSelfTestLong;\n" % uuid(9), "LONG")):
        controls.append((what, text[:after_imports] + added + text[after_imports:], True, named))
    return controls


def self_test(text, release, names=()):
    """The negative controls judged: (failures, how many controls); names are the files beside hMailServer.idl."""
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
    # And the files beside hMailServer.idl: each of these, added to what is there, must be refused.
    names = list(names)
    shadows = ("oaidl.idl", "wtypesbase.idl", "hMailServer.acf", "stdole2.tlb", "basetsd.h")
    for shadow in shadows:
        if not any(shadow in p for p in shadow_problems(names + [shadow])):
            failures.append("self-test: a file named %s beside hMailServer.idl was not refused" % shadow)
    return failures, len(controls) + len(shadows)


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
        beside = os.listdir(os.path.dirname(IDL))
        failures, controls = self_test(text, release, beside)
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
        found += shadow_problems(beside)
        found += failures
        notes = []
        in_ci = os.environ.get("GITLAB_CI") == "true"
        newer, fetched = None, []
        if in_ci:
            # In CI the baseline is held to its tag as the project has it, fetched every time over whatever
            # the clone holds, and the job fails when it cannot be, so that a change of hMailServer.idl and the
            # baseline together cannot pass where nothing reads the tag, nor a tag the clone was given.
            fetched, newer = fetch_release_tags(tag)
            found += ["in CI the baseline is held to the tag it names as %s has it, and %s" % (PROJECT_URL, p)
                      for p in fetched]
            if not fetched:
                notes.append("fetched %s%s from %s" % (tag, (" and " + ", ".join(newer)) if newer else "",
                                                        PROJECT_URL))
        at_tag = tag_idl(tag)
        if at_tag is None:
            if not in_ci:
                notes.append("this clone has no tag %s: the committed baseline is held as it is" % tag)
            elif not fetched:
                found.append("fetched %s from %s, and this clone still has no such tag" % (tag, PROJECT_URL))
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
            # In CI the project's newer tags, fetched; elsewhere the clone's own.
            for later in (newer if newer is not None else newer_tags(tag)):
                later_idl = tag_idl(later)
                if later_idl is None:
                    if in_ci:
                        found.append("fetched %s from %s, and this clone has no such tag" % (later, PROJECT_URL))
                    continue
                parsed = parse(later_idl[1])
                found += ["against %s: %s" % (later, p) for p in problems(
                    {"types": parsed["types"], "interfaces": parsed["interfaces"]}, tree, later)]
                notes.append("%s is newer than the baseline's %s: python build/check-com-abi.py --write-baseline %s"
                             % (later, tag, later))
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
