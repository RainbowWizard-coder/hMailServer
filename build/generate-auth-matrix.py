#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The web authorisation matrix: every route the server's web listeners serve, by
every kind of credential, read from the code.

Writes hmailserver/docs/WebAuthorisationMatrix.md. Nothing in that file is typed
by hand: every row comes from the C++ that decides it, so that the matrix says
what the server does rather than what somebody once meant it to do, and --check
fails the moment the two part: when the routes, the lists or the roles it is
read from have changed and the file has not (exit 1), and when a step of one
of the functions it holds has changed, an enumerator it reads has a value that
is not its place, something it reads stands inside a conditional of the
preprocessor, which it does not evaluate, or holds a directive of it - a
conditional, an #include - whose effect it does not read, or a file it reads
holds a form it does not read as the compilers do (exit 2; STEPS, lex,
conditionals, tokenize and enumeration, below).

What is read, all of it from hmailserver/source/Server/Common:

  Util/RestApiServer.cpp, ProcessRequest_
      the routes answered before any credential is looked at (the chain of
      `if (...) return Handle..._(...)` above `Caller caller = Authenticate_`),
      and the dispatch switch that hands each RouteKind to its handler.
  Util/RestApiServer.cpp, ParseRoute_ (and ParseScimRoute_, ParseDiscoveryRoute_,
  ParseAccountResourceRoute_ in their own files)
      the method and path of every RouteKind. These functions are imperative C++,
      so they are not matched with patterns: they are INTERPRETED, by a small
      abstract interpreter below that knows the handful of string operations the
      parser is written with (==, StartsWith, EndsWith, Find, Mid, GetLength,
      IsEmpty, ParseQueueId, SplitString) and keeps the path as a template of
      literal text and named holes. A condition it cannot model narrows nothing,
      so a template is never narrower than what the code accepts.
  Util/RestApiServer.cpp, Authorize_ and the functions whose lists it reads
      IsSessionRoute_, IsSelfServiceRoute_, IsScimRoute_, IsApiKeyRoute_,
      IsMutatingRoute_, ChangesAccountByAddress_, PermissionFor_,
      SecondPermissionFor_, DomainNamedByRoute_, the refusals a credential
      confined to named domains meets, and the role-holding account's
      exceptions to the API-key routes. The decision below follows Authorize_'s
      order, step for step; build/check-role-choke-point.py already holds every
      RouteKind to a classification, and this script holds the decision to the
      same reading.
  Util/RestApiServer.h, enum RouteKind
      the route kinds, in order, each one's value, as the compiler gives it,
      its place in the enumeration.
  Util/RestApiReports.cpp, SECTIONS
      the report's sections and which of them are counted from server-wide
      metrics: those IsServerWideReportSection_ answers for, which Authorize_
      refuses a credential confined to named domains.
  Application/RoleManager.h, enum RolePermission
      the permissions, in order, and Count after them. Each one's value, as
      the compiler gives it, must be its place in the enumeration: that is the
      bit Bit sets it at and RoleGrant::Holds asks, and the place of its token
      in RoleManager.cpp's PermissionTokens, which is read beside it. A value
      written that is not its place - one permission given another's bit -
      or one that is not an integer literal stops the script.
  Application/RoleManager.cpp, the four built-in roles' functions
      the permissions DomainAdminPermissions, HelpdeskPermissions and
      AuditorPermissions each set, one a line (set |= Bit(RolePermission::X););
      serveradmin is every permission, as ServerAdminPermissions sets every bit
      below RolePermission::Count. RoleManager::BuiltInPermissions_ picks a
      role's function by the role's name.
  Util/WebServicesServer.cpp, ProcessRequest_
      the second listener: MTA-STS, autoconfiguration, the Web Key Directory, the
      S/MIME authority, ACME, security.txt, and the CalDAV and CardDAV trees.

The steps between those lists are the script's own writing, so they are held
to the code: STEPS carries the steps of Authorize_; of the functions whose
lists it reads; of the four built-in roles' functions, ServerAdminPermissions
whole and each other one but for its permission lines, of the Bit they set a
permission with, and of RoleManager::BuiltInPermissions_, which picks one by a
role's name; of the
functions Authorize_ hands its decision to - RoleManager::Allows (the overload
with a domain) with the RoleGrant::Holds and RoleManager::IsDomainReached_ it
asks, IsDomainAllowed_, and IsServerWideReportSection_ with the FindSection it
asks; and of ProcessRequest_ from the end of the anonymous routes to the
handler - each with what the readers take out of it masked, and its prose
(skeleton, below). A step added, taken out, moved or given a condition, or a
name a step compares or is kept under changed, stops the script until decide()
has been read against it and STEPS updated. Every file it reads is read as
the compiler's first three translation phases read it (lex, below): its line
splices joined, its comments taken out, a raw string literal and a number
with digit separators read whole, a directive of the preprocessor wherever it
begins a logical line - after spaces or a comment, over a splice, with a
comment in it - and nothing inside a comment or a literal taken for code. A
form it does not read as every compiler does stops it: a backslash with only
spaces or tabs after it before a line's end, which compilers read differently,
a carriage return that ends no line, a line splice inside a raw string
literal, and a raw string literal over more than one line inside a function it
reads. Every definition it reads - a function, the report's table, an
enumeration, a constant - is read only where it stands outside the
preprocessor's conditionals and holds none, and only when it is the only
definition of its name in its file: one inside a conditional, or holding one,
stops it, since another copy of it, or another branch, may be what is
compiled, and so does a second definition, an overload or a copy in another
namespace, since the compiler may use that one. A directive of any other kind
inside what it reads stops it too, since an #include there brings in code it
never sees and it does not read what another directive does: one inside a
function it reads - a route parser, a predicate, either listener's
ProcessRequest_, a function whose steps it holds - from the line that names
the function to its closing brace, its parameter list among it, as a
conditional there does (function_span); one inside an enumeration's body,
PermissionTokens or a constant's initialiser, which its parser reads
(tokenize); and one between the report's SectionInfo and the end of
FindSection. Not held: how Authenticate_ makes a request one of the
nine credentials - the grant RoleManager gives it, of which role and for
which domains (GrantForAccount, GrantForApiKey and ServerAdministrator, and
the GrantForRole and GrantByAdminLevel they ask) - which the matrix's columns
take as given, the string helpers the functions above use, a macro defined
elsewhere over a name they use, an overload of one of them defined in another
file, and what the functions ProcessRequest_'s other checks call decide (the
matrix's "Not shown" names them).

Usage:
  python build/generate-auth-matrix.py            # write the matrix
  python build/generate-auth-matrix.py --check    # fail if the committed one is stale
  python build/generate-auth-matrix.py --print    # to stdout
  python build/generate-auth-matrix.py --steps    # the steps as the code has them, as STEPS holds them
  python build/generate-auth-matrix.py --self-test
"""

import bisect
import contextlib
import copy
import difflib
import functools
import io
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
DOC = os.path.join(ROOT, "hmailserver", "docs", "WebAuthorisationMatrix.md")

REST_H = "Common/Util/RestApiServer.h"
REST_CPP = "Common/Util/RestApiServer.cpp"
REPORTS_CPP = "Common/Util/RestApiReports.cpp"
ROLE_CPP = "Common/Application/RoleManager.cpp"
WEB_CPP = "Common/Util/WebServicesServer.cpp"
SUB_PARSERS = {
    "ParseScimRoute_": "Common/Util/RestApiScim.cpp",
    "ParseDiscoveryRoute_": "Common/Util/RestApiDiscovery.cpp",
    "ParseAccountResourceRoute_": "Common/Util/RestApiAccountResources.cpp",
}
# Static predicates the parsers call, inlined when their body is one return.
PREDICATES = {
    "IsScimPath_": "Common/Util/RestApiScim.cpp",
    "CalDavServer::IsCalendarTarget": "Common/Util/CalDavServer.cpp",
    "CardDavServer::IsDavTarget": "Common/Util/CardDavServer.cpp",
}
CONSTANTS = {
    # static const char * members the predicates name, each defined as a string
    # literal
    "CalendarsPath": ("Common/Util/CalDavServer.cpp", "CalDavServer::CalendarsPath"),
    "ContextPath": ("Common/Util/CardDavServer.cpp", "CardDavServer::ContextPath"),
}

METHODS = ("GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "PROPFIND", "PROPPATCH", "REPORT", "MKCOL",
           "MKCALENDAR", "COPY", "MOVE", "LOCK", "UNLOCK")


class GenerationError(Exception):
    pass


def read(rel, sources=None):
    if sources is not None and rel in sources:
        return sources[rel]
    with open(os.path.join(SERVER, rel), encoding="utf-8", errors="replace") as handle:
        text = handle.read().replace("\r\n", "\n")
    # A byte-order mark is not part of the text the compiler reads.
    return text[1:] if text.startswith(chr(0xFEFF)) else text


# ---------------------------------------------------------------------------
# Reading C++ as the compiler's first three translation phases read it
# ---------------------------------------------------------------------------
#
# Every reader below reads what lex() makes of a file, so that what is a
# comment, a literal or a directive of the preprocessor to the compiler is one
# to this script too.
#
# Phase 1: read() makes every CR LF a line feed and leaves out a byte-order
# mark, and a carriage return left after that, which ends no line, is refused.
# No build of the server replaces a trigraph - the POSIX build compiles C++20,
# which has none, and MSVC replaces them only under /Zc:trigraphs, which the
# Windows build does not set - so none is read as one.
# Phase 2 (logical_text): a backslash that ends a line joins the line to the
# next. A backslash with only spaces or tabs after it on its line is refused,
# outside a raw string literal, and so is a backslash that ends a line inside
# one, whose splice the compiler takes back: this script reads neither.
# Phase 3 (scan): the comments, each one space; the literals - a character or
# string literal with any of its prefixes, and a raw string literal whole, to
# the ")" and delimiter that end it; a number with its digit separators, as one
# preprocessing number; and the directives - a # or %: that is the first thing
# on a logical line but for whitespace and comments, to the end of that line,
# a header name after #include and __has_include read whole, a comment in a
# directive one space however many lines it runs over.

LEX = re.compile(r"""
    (?P<nl>\n)
  | (?P<ws>[ \t\f\v]+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<raw>(?:u8|[uUL])?R"(?P<delim>[^ ()\\\t\f\v\n]{0,16})\(.*?\)(?P=delim)")
  | (?P<str>(?:u8|[uUL])?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>(?:u8|[uUL])?'(?:[^'\\\n]|\\.)*')
  | (?P<num>\.?[0-9](?:[eEpP][-+]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>::|->|\+\+|--|<<=|>>=|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|\#\#|%:%:
        |[-+*/%<>=!&|^~?:;,.(){}\[\]\#])
""", re.S | re.X)
# What begins a directive at the head of a logical line: # or %:, but not ## or
# %:%:, which are one token of their own.
DIRECTIVE_START = re.compile(r"\#(?!\#)|%:(?!%:)")
HEADER_NAME = re.compile(r'<[^>\n]*>|"[^"\n]*"')
# The directives whose header name follows the name, and the operators in a
# directive whose header name follows a "(".
INCLUDES = frozenset(["include", "include_next", "import", "embed"])
HAS_INCLUDES = frozenset(["__has_include", "__has_include_next", "__has_embed"])
SPACED_SPLICE = re.compile(r"\\[ \t\f\v]+\n")


def logical_text(text):
    """text as translation phase 2 leaves it: (the text with every backslash
    that ends a line taken out with the line feed after it, the offsets in that
    text at which one was taken out). A carriage return that ends no line is
    refused: this script does not read it as the compilers do."""
    if "\r" in text:
        raise GenerationError("a carriage return that ends no line, at line %d, which this script does not read"
                              % (text.count("\n", 0, text.index("\r")) + 1))
    if "\\\n" not in text:
        return text, ()
    out = []
    offsets = []
    pos = 0
    length = 0
    at = text.find("\\\n")
    while at >= 0:
        out.append(text[pos:at])
        length += at - pos
        offsets.append(length)
        pos = at + 2
        at = text.find("\\\n", pos)
    out.append(text[pos:])
    return "".join(out), tuple(offsets)


def scan(text, pos=0, bol=True):
    """Translation phase 3's pieces of text, a text phase 2 has joined, from the
    offset pos: (kind, start, end, whether the piece is part of a directive),
    the kinds LEX's groups and "pp" for the # or %: that begins a directive and
    "header" for a header name. bol: whether pos is at the start of a logical
    line but for whitespace and comments."""
    end_of_text = len(text)
    directive = False
    named = False
    expect = None
    match = LEX.match
    while pos < end_of_text:
        if expect == "header" and text[pos] in '<"':
            m = HEADER_NAME.match(text, pos)
            if m:
                expect = None
                yield "header", pos, m.end(), True
                pos = m.end()
                continue
        if bol and not directive:
            m = DIRECTIVE_START.match(text, pos)
            if m:
                directive, named, expect, bol = True, False, None, False
                yield "pp", pos, m.end(), True
                pos = m.end()
                continue
        m = match(text, pos)
        if not m:
            raise GenerationError("cannot tokenize near line %d: %r" % (text.count("\n", 0, pos) + 1, text[pos:pos + 40]))
        kind = m.lastgroup
        end = m.end()
        if kind == "nl":
            bol, directive, expect = True, False, None
        elif kind != "ws" and kind != "comment":
            bol = False
            if directive:
                word = m.group()
                if not named:
                    named = True
                    expect = "header" if kind == "id" and word in INCLUDES else None
                elif kind == "id" and word in HAS_INCLUDES:
                    expect = "paren"
                elif expect == "paren" and word == "(":
                    expect = "header"
                else:
                    expect = None
        yield kind, pos, end, directive
        pos = end


def blank(text):
    return re.sub(r"[^\n]", " ", text)


def literal_parts(kind, text):
    """(what opens a literal, what it holds, what closes it): of a raw string
    literal its prefix, delimiter and parentheses, of any other its prefix and
    quotes."""
    if kind == "raw":
        quote = text.index('"')
        paren = text.index("(", quote)
        close = len(text) - (paren - quote) - 1
        return text[:paren + 1], text[paren + 1:close], text[close:]
    quote = text.index('"' if kind == "str" else "'")
    return text[:quote + 1], text[quote + 1:-1], text[-1:]


class Lexed(object):
    """What lex() makes of a text: code, blind, the directives and the
    literals."""
    __slots__ = ("code", "blind", "directives", "literals", "starts")

    def __init__(self, code, blind, directives, literals):
        self.code = code
        self.blind = blind
        self.directives = directives
        self.literals = literals
        self.starts = [begin for begin, _, _ in literals]

    def literal_at(self, at):
        """The literal of the code that the offset at stands inside, its
        quotes or prefix included: (start, end, kind), or None."""
        k = bisect.bisect_right(self.starts, at) - 1
        if k >= 0 and self.literals[k][0] <= at < self.literals[k][1]:
            return self.literals[k]
        return None


@functools.lru_cache(maxsize=None)
def lex(text):
    """text as translation phases 1 to 3 read it (logical_text, scan).

    code: the text with each comment taken out - one space, or as many line
    feeds as it holds - and each line splice joined, each line after a logical
    line's end where it was: a line feed the compiler does not see as one, in a
    splice or in a comment inside a directive, is written at the end of the
    logical line it stands in, so that a directive is one line of the code.
    blind: the code with what each literal holds made spaces, its line feeds
    kept, so that a search of it finds nothing inside a literal and its offsets
    are the code's. directives: (the line of the code it stands on, its name or
    "", its text) of each directive. literals: (start, end, kind) in the code
    of each literal, "raw", "str" or "chr"."""
    logical, splices = logical_text(text or "")
    try:
        pieces = list(scan(logical))
    except GenerationError:
        spaced = SPACED_SPLICE.search(logical)
        if spaced:
            raise GenerationError("a backslash with only spaces or tabs after it on its line, at line %d, which this "
                                  "script does not read" % (logical.count("\n", 0, spaced.start()) + 1))
        raise
    raws = [(start, end) for kind, start, end, _ in pieces if kind == "raw"]
    for spaced in SPACED_SPLICE.finditer(logical):
        if not any(start <= spaced.start() < end for start, end in raws):
            raise GenerationError("a backslash with only spaces or tabs after it on its line, at line %d, which this "
                                  "script does not read" % (logical.count("\n", 0, spaced.start()) + 1))
    for start, end in raws:
        k = bisect.bisect_right(splices, start)
        if k < len(splices) and splices[k] < end:
            raise GenerationError("a raw string literal at line %d holds a backslash that ends a line, a splice the "
                                  "compiler takes back there and this script does not"
                                  % (logical.count("\n", 0, start) + 1))
    code = []
    blind = []
    directives = []
    literals = []
    length = 0
    line = 0
    deferred = 0
    joined = 0
    directive = None
    for kind, start, end, inside in pieces:
        piece = logical[start:end]
        if kind == "nl":
            if directive is not None:
                directives.append((directive[0], directive[1], "".join(directive[2]).strip()))
                directive = None
            passed = bisect.bisect_right(splices, start)
            deferred += passed - joined
            joined = passed
            piece = "\n" * (1 + deferred)
            deferred = 0
            line += len(piece)
            code.append(piece)
            blind.append(piece)
            length += len(piece)
            continue
        if kind == "comment":
            feeds = piece.count("\n")
            if inside:
                deferred += feeds
                piece = " "
            else:
                piece = "\n" * feeds or " "
                line += feeds
            covered = piece
        elif kind in ("raw", "str", "chr"):
            opening, held, closing = literal_parts(kind, piece)
            covered = opening + blank(held) + closing
            literals.append((length, length + len(piece), kind))
            line += piece.count("\n")
        else:
            covered = piece
        if kind == "pp":
            directive = [line, None, []]
        elif directive is not None and directive[1] is None and kind not in ("ws", "comment"):
            directive[1] = piece if kind == "id" else ""
        if directive is not None:
            directive[2].append(piece)
        code.append(piece)
        blind.append(covered)
        length += len(piece)
    if directive is not None:
        directives.append((directive[0], directive[1] or "", "".join(directive[2]).strip()))
    rest = "\n" * (deferred + len(splices) - joined)
    code.append(rest)
    blind.append(rest)
    return Lexed("".join(code), "".join(blind), tuple(directives), tuple(literals))


def code_of(text):
    """The text with its comments taken out, as the compiler reads it (lex): a
    case label, a definition or a directive in a comment is none, and a //
    inside a string is no comment."""
    return lex(text or "").code


def blind_of(text):
    """The code of text with what each literal holds made spaces (lex), for a
    search that must find code and nothing inside a literal."""
    return lex(text or "").blind


def unescape(body):
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            out.append({"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'", "0": "\0"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literal_value(kind, text):
    """What a string or character literal holds: a raw string literal's text as
    it is, another's with its escapes read."""
    held = literal_parts(kind, text)[1]
    return held if kind == "raw" else unescape(held)


# An integer literal as C++ writes it: hexadecimal, binary, octal or decimal,
# with digit separators and a suffix.
INTEGER_LITERAL = re.compile(r"(0[xX][0-9a-fA-F](?:'?[0-9a-fA-F])*|0[bB][01](?:'?[01])*|0(?:'?[0-7])*|[1-9](?:'?[0-9])*)"
                             r"(?:[uU](?:ll|LL|[lLzZ])?|(?:ll|LL|[lLzZ])[uU]?)?\Z")


def integer_value(text):
    """The value of an integer literal as the compiler reads it, or None when
    text is not one."""
    m = INTEGER_LITERAL.match(text)
    if not m:
        return None
    digits = m.group(1).replace("'", "")
    if digits[:2] in ("0x", "0X"):
        return int(digits[2:], 16)
    if digits[:2] in ("0b", "0B"):
        return int(digits[2:], 2)
    if digits[0] == "0":
        return int(digits, 8)
    return int(digits)


class Tok(object):
    __slots__ = ("kind", "text", "line")

    def __init__(self, kind, text, line):
        self.kind = kind
        self.text = text
        self.line = line

    def __repr__(self):
        return "%s:%r@%d" % (self.kind, self.text, self.line)


def tokenize(text, first_line, what):
    """The tokens of text, code as lex() gives it whose first line is the
    line first_line of its file, for the parser: adjacent string literals one,
    as in C++. A directive of the preprocessor in text stops the script, as a
    conditional does: what an #include there brings in is code it never reads,
    and it does not read what another directive does, so the tokens it would
    give need not be those the compiler reads. what: what text is, for the
    message."""
    tokens = []
    line = first_line
    for kind, start, end, inside in scan(text):
        value = text[start:end]
        if inside:
            finish = text.find("\n", start)
            raise GenerationError("%s: a directive of the preprocessor at line %d, %s, inside what this script reads "
                                  "as code, which it does not read: an #include there brings in code it never sees, "
                                  "and it does not read what another directive does"
                                  % (what, line, text[start:finish if finish >= 0 else len(text)].strip()))
        if kind in ("str", "raw"):
            literal = literal_value(kind, value)
            # adjacent literals are one literal, as in C++
            if tokens and tokens[-1].kind == "str":
                tokens[-1].text += literal
            else:
                tokens.append(Tok("str", literal, line))
        elif kind == "chr":
            tokens.append(Tok("chr", literal_value(kind, value), line))
        elif kind in ("num", "id", "op"):
            tokens.append(Tok(kind, value, line))
        line += value.count("\n")
    tokens.append(Tok("eof", "", line))
    return tokens


# ---------------------------------------------------------------------------
# A parser for the subset of C++ the route functions are written in
# ---------------------------------------------------------------------------

# AST nodes are tuples: (tag, line, ...)
BINARY = {
    "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6,
    "<": 7, ">": 7, "<=": 7, ">=": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10,
}
ASSIGN = ("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=")
TYPE_WORDS = {"const", "static", "unsigned", "signed", "long", "short", "volatile", "auto", "constexpr"}


class Parser(object):
    def __init__(self, tokens):
        self.t = tokens
        self.i = 0

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def at(self, text, k=0):
        tok = self.peek(k)
        return tok.kind in ("op", "id") and tok.text == text

    def take(self, text=None):
        tok = self.t[self.i]
        if text is not None and not (tok.kind in ("op", "id") and tok.text == text):
            raise GenerationError("expected %r at line %d, found %r" % (text, tok.line, tok.text))
        self.i += 1
        return tok

    # statements -------------------------------------------------------------
    def block(self):
        start = self.take("{").line
        body = []
        while not self.at("}"):
            if self.peek().kind == "eof":
                raise GenerationError("unterminated block from line %d" % start)
            body.append(self.statement())
        self.take("}")
        return ("block", start, body)

    def statement(self):
        tok = self.peek()
        if tok.kind == "op" and tok.text == "{":
            return self.block()
        if tok.kind == "op" and tok.text == ";":
            self.take()
            return ("empty", tok.line)
        if tok.kind == "id":
            word = tok.text
            if word == "if":
                self.take()
                self.take("(")
                cond = self.expression()
                self.take(")")
                then = self.statement()
                other = None
                if self.at("else"):
                    self.take()
                    other = self.statement()
                return ("if", tok.line, cond, then, other)
            if word == "for" and self.at("(", 1):
                self.take()
                header_start = self.i
                self.skip_balanced("(", ")")
                header = self.t[header_start + 1:self.i - 1]
                colon = [k for k, h in enumerate(header) if h.kind == "op" and h.text == ":"]
                body = self.statement()
                ranged = colon and header[colon[0] - 1].kind == "id" and len(header) == colon[0] + 2
                if ranged and header[colon[0] + 1].kind == "id":
                    return ("rangefor", tok.line, header[colon[0] - 1].text, header[colon[0] + 1].text, body)
                return ("loop", tok.line, body)
            if word == "while":
                self.take()
                self.skip_balanced("(", ")")
                return ("loop", tok.line, self.statement())
            if word == "struct" and self.peek(1).kind == "id" and self.at("{", 2):
                self.take()
                name = self.take().text
                self.take("{")
                fields = []
                while not self.at("}"):
                    member = []
                    while not self.at(";"):
                        member.append(self.take())
                    self.take(";")
                    ids = [m.text for m in member if m.kind == "id"]
                    if ids:
                        fields.append(ids[-1])
                self.take("}")
                self.take(";")
                return ("struct", tok.line, name, fields)
            if word == "do":
                self.take()
                body = self.statement()
                self.take("while")
                self.skip_balanced("(", ")")
                self.take(";")
                return ("loop", tok.line, body)
            if word == "switch":
                self.take()
                self.take("(")
                subject = self.expression()
                self.take(")")
                return ("switch", tok.line, subject, self.block())
            if word in ("case",):
                self.take()
                label = self.expression()
                self.take(":")
                return ("case", tok.line, label)
            if word == "default" and self.at(":", 1):
                self.take()
                self.take(":")
                return ("default", tok.line)
            if word == "return":
                self.take()
                value = None
                if not self.at(";"):
                    value = self.expression()
                self.take(";")
                return ("return", tok.line, value)
            if word in ("break", "continue"):
                self.take()
                self.take(";")
                return (word, tok.line)
            if word == "try":
                self.take()
                body = self.block()
                handlers = []
                while self.at("catch"):
                    self.take()
                    self.skip_balanced("(", ")")
                    handlers.append(self.block())
                return ("try", tok.line, body, handlers)
            decl = self.try_declaration()
            if decl is not None:
                return decl
        start = self.i
        try:
            expr = self.expression()
            self.take(";")
            return ("expr", tok.line, expr)
        except GenerationError:
            self.i = start
            self.skip_statement()
            return ("opaque", tok.line)

    def skip_balanced(self, open_text, close_text):
        self.take(open_text)
        depth = 1
        while depth:
            tok = self.take()
            if tok.kind == "eof":
                raise GenerationError("unbalanced %s" % open_text)
            if tok.kind == "op" and tok.text == open_text:
                depth += 1
            elif tok.kind == "op" and tok.text == close_text:
                depth -= 1

    def skip_statement(self):
        depth = 0
        while True:
            tok = self.take()
            if tok.kind == "eof":
                return
            if tok.kind == "op" and tok.text in "({[":
                depth += 1
            elif tok.kind == "op" and tok.text in ")}]":
                depth -= 1
            elif tok.kind == "op" and tok.text == ";" and depth <= 0:
                return

    def try_declaration(self):
        """[const] Type[::Type][<...>] [&*] name (= expr | ;)"""
        j = self.i
        saw_type = False
        while True:
            tok = self.t[j]
            if tok.kind == "id" and tok.text in TYPE_WORDS:
                j += 1
                continue
            break
        tok = self.t[j]
        if tok.kind != "id":
            return None
        type_name = tok.text
        j += 1
        saw_type = True
        while self.t[j].kind == "op" and self.t[j].text == "::" and self.t[j + 1].kind == "id":
            j += 2
        if self.t[j].kind == "op" and self.t[j].text == "<":
            depth = 0
            while True:
                tok = self.t[j]
                if tok.kind == "eof":
                    return None
                if tok.kind == "op" and tok.text == "<":
                    depth += 1
                elif tok.kind == "op" and tok.text == ">":
                    depth -= 1
                elif tok.kind == "op" and tok.text == ">>":
                    depth -= 2
                elif tok.kind == "op" and tok.text in (";", "{", "}"):
                    return None
                j += 1
                if depth <= 0:
                    break
        while self.t[j].kind == "op" and self.t[j].text in ("&", "*"):
            j += 1
        name = self.t[j]
        if not saw_type or name.kind != "id" or name.text in TYPE_WORDS:
            return None
        after = self.t[j + 1]
        opens_table = after.kind == "op" and after.text == "[" and self.t[j + 2].kind == "op" and self.t[j + 2].text == "]"
        if opens_table and self.t[j + 3].kind == "op" and self.t[j + 3].text == "=" and self.t[j + 4].text == "{":
            # a table: T name[] = { { a, b, ... }, ... };
            self.i = j + 4
            self.take("{")
            rows = []
            while not self.at("}"):
                self.take("{")
                row = []
                while not self.at("}"):
                    row.append(self.assignment())
                    if self.at(","):
                        self.take()
                self.take("}")
                rows.append(row)
                if self.at(","):
                    self.take()
            self.take("}")
            self.take(";")
            return ("table", name.line, name.text, rows)
        if not (after.kind == "op" and after.text in ("=", ";", "(", "{")):
            return None
        if after.text in ("(", "{"):
            # a constructor-style declaration: `std::string s(x);` - treated as opaque
            self.i = j + 1
            self.skip_statement()
            return ("decl", name.line, name.text, None)
        self.i = j + 1
        if after.text == ";":
            self.take(";")
            if type_name in ("AnsiString", "String") or (type_name == "std" and self.t[j - 1].text == "string"):
                return ("decl", name.line, name.text, ("str", name.line, ""))
            return ("decl", name.line, name.text, None)
        self.take("=")
        start = self.i
        try:
            value = self.expression()
            self.take(";")
        except GenerationError:
            self.i = start
            self.skip_statement()
            value = None
        return ("decl", name.line, name.text, value)

    # expressions --------------------------------------------------------------
    def expression(self):
        return self.assignment()

    def assignment(self):
        left = self.ternary()
        tok = self.peek()
        if tok.kind == "op" and tok.text in ASSIGN:
            self.take()
            right = self.assignment()
            return ("assign", tok.line, tok.text, left, right)
        return left

    def ternary(self):
        cond = self.binary(1)
        if self.at("?"):
            line = self.take().line
            a = self.assignment()
            self.take(":")
            b = self.assignment()
            return ("ternary", line, cond, a, b)
        return cond

    def binary(self, level):
        left = self.unary()
        while True:
            tok = self.peek()
            if tok.kind != "op" or tok.text not in BINARY or BINARY[tok.text] < level:
                return left
            prec = BINARY[tok.text]
            self.take()
            right = self.binary(prec + 1)
            left = ("bin", tok.line, tok.text, left, right)

    def unary(self):
        tok = self.peek()
        if tok.kind == "op" and tok.text in ("!", "-", "+", "~", "*", "&", "++", "--"):
            self.take()
            return ("un", tok.line, tok.text, self.unary())
        # C-style cast of a simple type: (int) x
        if tok.kind == "op" and tok.text == "(" and self.peek(1).kind == "id" and self.at(")", 2) \
                and self.peek(1).text in ("int", "size_t", "long", "unsigned", "char", "bool", "double", "__int64"):
            self.take()
            self.take()
            self.take(")")
            return self.unary()
        return self.postfix(self.primary())

    def primary(self):
        tok = self.take()
        if tok.kind == "str":
            return ("str", tok.line, tok.text)
        if tok.kind == "chr":
            return ("chr", tok.line, tok.text)
        if tok.kind == "num":
            # An integer literal's value; another number, which the parsers do
            # not compare, is 0.
            value = integer_value(tok.text)
            return ("num", tok.line, 0 if value is None else value)
        if tok.kind == "id":
            name = tok.text
            while self.at("::") and self.peek(1).kind == "id":
                self.take()
                name += "::" + self.take().text
            if name in ("_T",) and self.at("("):
                self.take("(")
                inner = self.expression()
                self.take(")")
                return inner
            # template arguments on a call: std::make_shared<T>(...)
            return ("id", tok.line, name)
        if tok.kind == "op" and tok.text == "(":
            inner = self.expression()
            self.take(")")
            return inner
        if tok.kind == "op" and tok.text == "[":
            # a lambda: [captures](params) { body }
            self.i -= 1
            self.skip_balanced("[", "]")
            if self.at("("):
                self.skip_balanced("(", ")")
            while self.peek().kind == "id" or self.at("->") or self.at("::"):
                self.take()
            if self.at("{"):
                self.skip_balanced("{", "}")
            return ("lambda", tok.line)
        if tok.kind == "op" and tok.text == "{":
            self.i -= 1
            self.skip_balanced("{", "}")
            return ("init", tok.line)
        raise GenerationError("unexpected %r at line %d" % (tok.text, tok.line))

    def postfix(self, node):
        while True:
            tok = self.peek()
            if tok.kind != "op":
                return node
            if tok.text == "(":
                self.take()
                args = []
                while not self.at(")"):
                    args.append(self.assignment())
                    if self.at(","):
                        self.take()
                self.take(")")
                node = ("call", tok.line, node, args)
            elif tok.text in (".", "->"):
                self.take()
                name = self.take()
                node = ("member", tok.line, node, name.text)
            elif tok.text == "[":
                self.take()
                index = self.expression()
                self.take("]")
                node = ("index", tok.line, node, index)
            elif tok.text in ("++", "--"):
                self.take()
                node = ("post", tok.line, tok.text, node)
            else:
                return node


# The directives that open, divide and close a conditional of the
# preprocessor.
CONDITIONAL_OPENS = frozenset(["if", "ifdef", "ifndef"])
CONDITIONAL_DIVIDES = frozenset(["elif", "elifdef", "elifndef", "else"])
CONDITIONAL_CLOSES = frozenset(["endif"])


@functools.lru_cache(maxsize=None)
def conditionals(text):
    """The preprocessor's conditionals in text, code as lex() gives it, read
    from its directives (lex), which the compiler does not evaluate here: for
    each line, how many are open at its start - those an #if, an #ifdef or an
    #ifndef above it opened and no #endif has closed - and the numbers (from 0)
    of the lines that open, divide or close one."""
    lexed = lex(text)
    change = {}
    lines = []
    for line, name, _ in lexed.directives:
        if name in CONDITIONAL_OPENS:
            change[line] = 1
        elif name in CONDITIONAL_DIVIDES:
            change[line] = 0
        elif name in CONDITIONAL_CLOSES:
            change[line] = -1
        else:
            continue
        lines.append(line)
    depths = []
    depth = 0
    for number in range(lexed.code.count("\n") + 1):
        depths.append(depth)
        delta = change.get(number)
        if delta == 1:
            depth += 1
        elif delta == -1:
            depth = max(0, depth - 1)
    return depths, lines


def in_conditional(text, at):
    """Whether the line the offset at of text, code as lex() gives it, is on
    stands inside a conditional of the preprocessor. This script does not
    evaluate a condition, so what it reads there may be a copy the compiler
    leaves out, and the copy it compiles another."""
    return conditionals(text)[0][text.count("\n", 0, at)] > 0


def holds_conditional(text, begin, end):
    """Whether a line of text, code as lex() gives it, from the offset begin to
    the offset end opens, divides or closes a conditional of the preprocessor,
    whose branches this script would read alike."""
    lines = conditionals(text)[1]
    k = bisect.bisect_left(lines, text.count("\n", 0, begin))
    return k < len(lines) and lines[k] <= text.count("\n", 0, end)


def defined_once(blind, head, what):
    """Refused unless the pattern head, a definition's head - the name and the
    = (not the first of ==), { or ( that follows it in a definition, so that a
    comparison is not one - matches once in blind, the code of a file outside
    its literals (lex): a second definition of the name, wherever it stands -
    in another namespace or class, or under a conditional - may be the one the
    compiler uses, which this script does not work out."""
    if len(re.findall(head, blind)) > 1:
        raise GenerationError("%s is defined more than once in its file - in another namespace or class, or under a "
                              "conditional - and this script does not work out which the compiler uses" % what)


def outside_conditionals(text, m, what):
    """m, a match in text (code as lex() gives it, or its blind) of what a
    reader takes, refused when it stands inside a conditional of the
    preprocessor (in_conditional) or holds one."""
    if m and (in_conditional(text, m.start()) or holds_conditional(text, m.start(), m.end())):
        raise GenerationError("%s stands inside a conditional of the preprocessor (#if to #endif), or holds one, which "
                              "this script does not evaluate: what it would read may not be what is compiled" % what)
    return m


def function_span(text, qualified, params=None):
    """(the start of the definition's first line, its '{', the end of its '}',
    the text of its parameter list) of a member function's first definition in
    the code of text as the compiler reads it (lex) - so that a definition in a
    comment is none, and one in a literal too - and its closing '}' at the
    definition's indent, or None; the offsets are into code_of(text). With
    params, of the overload whose parameter list holds that text. A definition
    that stands inside a conditional of the preprocessor, or holds one, is
    refused, since another copy of it, or another branch, may be what is
    compiled; so is one that holds a directive of any other kind from the line
    that names it to its closing '}', whose effect this script does not read;
    so is one that is not the only definition of the function (with
    params, of that overload) in the file, and one that holds a raw string
    literal over more than one line, since the readers read a definition a line
    at a time."""
    lexed = lex(text)
    code = lexed.code
    # Searched in the blind code, so that nothing inside a literal is taken for
    # a definition or the '}' that ends one. Tried at the start of each line
    # that names the function, in order: the first definition, as a search of
    # the whole text would find it, without trying the pattern at every line of
    # a file of ten thousand. Before the name on its line stand only the words
    # of a type - a return type, a qualifier, *, &, <, > - so that a call in a
    # condition with its block below is not taken for a definition.
    blind = lexed.blind
    head = re.compile(r"([ \t]*)[\w:*&<>,~ \t]*?\b" + re.escape(qualified) + r"\s*\(([^;{]*?)\)\s*(?:const\s*)?\n\1\{",
                      re.S)
    at = blind.find(qualified)
    while at >= 0:
        m = head.match(blind, blind.rfind("\n", 0, at) + 1)
        if m and (params is None or params in m.group(2)):
            break
        at = blind.find(qualified, m.end() if m else at + 1)
    else:
        return None
    if in_conditional(code, m.start()):
        raise GenerationError("%s is defined inside a conditional of the preprocessor (#if to #endif), which this "
                              "script does not evaluate: the copy it would read may not be the one compiled"
                              % qualified)
    indent = m.group(1)
    open_at = m.end() - 1
    close = blind.find("\n" + indent + "}\n", open_at)
    if close < 0:
        return None
    end = close + len(indent) + 2
    if holds_conditional(code, m.start(), end):
        raise GenerationError("%s holds a conditional of the preprocessor (#if to #endif), which this script does not "
                              "evaluate: it would read every branch as compiled" % qualified)
    # A directive of any other kind, from the line that names it to its '}' -
    # its parameter list among it - is refused as a conditional is: what an
    # #include there brings in is code this script never reads.
    first, last = code.count("\n", 0, m.start()), code.count("\n", 0, end)
    for line, _, directive in lexed.directives:
        if first <= line <= last:
            raise GenerationError("%s holds a directive of the preprocessor at line %d, %s, which this script does not "
                                  "read: an #include there brings in code it never sees, and it does not read what "
                                  "another directive does" % (qualified, line + 1, directive))
    # The only definition of it in the file, wherever another would stand: an
    # overload, or a copy in another namespace or under a conditional, may be
    # the one the compiler calls, which this script does not work out.
    again = blind.find(qualified, end)
    while again >= 0:
        other = head.match(blind, blind.rfind("\n", 0, again) + 1)
        if other and (params is None or params in other.group(2)):
            raise GenerationError("%s is defined more than once in its file - an overload, or a copy in another "
                                  "namespace - and this script does not work out which the compiler calls"
                                  % qualified)
        again = blind.find(qualified, other.end() if other else again + 1)
    k = bisect.bisect_left(lexed.starts, m.start())
    while k < len(lexed.literals) and lexed.literals[k][0] < end:
        begin, finish, kind = lexed.literals[k]
        if kind == "raw" and "\n" in code[begin:finish]:
            raise GenerationError("%s holds a raw string literal over more than one line, which this script, reading a "
                                  "definition a line at a time, does not read" % qualified)
        k += 1
    return m.start(), open_at, end, m.group(2)


def function_text(text, qualified, params=None):
    """The body of a member function, '{' to its closing '}' at the definition's indent,
    its comments taken out, and the line its '{' is on (function_span)."""
    span = function_span(text, qualified, params)
    if span is None:
        return None, 0
    code = code_of(text)
    return code[span[1]:span[2]], code.count("\n", 0, span[1]) + 1


def parse_function(text, qualified, rel):
    """The statements of a member function's body (function_text) as the
    parser reads them, and the line its '{' is on; rel: its file."""
    body, line = function_text(text, qualified)
    if body is None:
        return None, 0
    parser = Parser(tokenize(body, line, "%s: %s" % (rel, qualified)))
    return parser.block(), line


def function_params(text, qualified):
    """The names of the parameters of the definition function_span reads."""
    span = function_span(text, qualified)
    if span is None:
        return []
    params = []
    for part in span[3].split(","):
        words = re.findall(r"[A-Za-z_]\w*", part)
        if words:
            params.append(words[-1])
    return params


# ---------------------------------------------------------------------------
# The abstract interpreter: a path as a template of literal text and holes
# ---------------------------------------------------------------------------

class Hole(object):
    __slots__ = ("name", "numeric", "noslash", "role")

    def __init__(self, name=None):
        self.name = name
        self.numeric = None
        self.noslash = False
        self.role = None


class Unknown(object):
    pass


UNKNOWN = Unknown()


class State(object):
    counter = [0]

    def __init__(self):
        self.methods = None            # None: any method
        self.env = {}
        self.subst = {}                 # hole id -> tuple of parts
        self.holes = {}                 # hole id -> Hole
        self.finds = {}                 # key -> dict
        self.notes = ()
        self.handler_notes = ()

    def clone(self):
        other = State.__new__(State)
        other.methods = self.methods
        other.env = dict(self.env)
        other.subst = dict(self.subst)
        other.holes = dict(self.holes)   # shared; Interpreter.hole copies one before changing it
        other.finds = {k: {kk: (list(vv) if isinstance(vv, list) else vv) for kk, vv in v.items()}
                       for k, v in self.finds.items()}
        other.notes = self.notes
        other.handler_notes = self.handler_notes
        return other

    def new_hole(self, name=None):
        State.counter[0] += 1
        hid = State.counter[0]
        self.holes[hid] = Hole(name)
        return hid

    def resolve(self, template):
        out = []
        for part in template:
            if isinstance(part, int) and part in self.subst:
                out.extend(self.resolve(self.subst[part]))
            else:
                out.append(part)
        merged = []
        for part in out:
            if isinstance(part, str):
                if not part:
                    continue
                if merged and isinstance(merged[-1], str):
                    merged[-1] += part
                    continue
            merged.append(part)
        return tuple(merged)

    def key(self):
        return (self.methods, repr(sorted((k, repr(v)) for k, v in self.env.items())),
                repr(sorted(self.subst.items())))


def lit(text):
    return (text,)


def is_literal(template):
    return all(isinstance(p, str) for p in template)


def literal_text(template):
    return "".join(template)


class Interpreter(object):
    def __init__(self, sources, record_kind, record_return, functions):
        self.sources = sources
        self.record_kind = record_kind
        self.record_return = record_return
        self.functions = functions      # name -> (ast, params)
        self.depth = 0
        self.structs = {}
        self.snapshots = []
        self.snapshot_ids = {}
        self.final = {}

    # values -------------------------------------------------------------
    def eval(self, s, node):
        tag = node[0]
        if tag == "str":
            return ("str", lit(node[2]))
        if tag == "chr":
            return ("str", lit(node[2]))
        if tag == "num":
            return ("int", ("const", node[2]))
        if tag == "id":
            name = node[2]
            if name in ("true", "false"):
                return ("boolconst", name == "true")
            if name in s.env:
                return s.env[name]
            if name in self.functions.get("__constants__", {}):
                return ("str", lit(self.functions["__constants__"][name]))
            return UNKNOWN
        if tag == "call":
            return self.eval_call(s, node)
        if tag == "bin":
            op = node[2]
            a = self.eval(s, node[3])
            b = self.eval(s, node[4])
            if op == "+":
                if a is not UNKNOWN and b is not UNKNOWN and a[0] == "str" and b[0] == "str":
                    return ("str", tuple(a[1]) + tuple(b[1]))
                if a is not UNKNOWN and b is not UNKNOWN and a[0] == "int" and b[0] == "int":
                    return ("int", ("add", a[1], b[1]))
            if op == "-":
                if a is not UNKNOWN and b is not UNKNOWN and a[0] == "int" and b[0] == "int":
                    return ("int", ("sub", a[1], b[1]))
            if op in ("==", "!=", "<", ">", "<=", ">=", "&&", "||"):
                return ("bool", node, self.snapshot(s))
            return UNKNOWN
        if tag == "un" and node[2] == "!":
            return ("bool", node, self.snapshot(s))
        if tag == "ternary":
            which = self.definite(s, node[2])
            if which is True:
                return self.eval(s, node[3])
            if which is False:
                return self.eval(s, node[4])
            return UNKNOWN
        if tag == "member":
            field = self.record_field(s, node)
            if field is not None:
                return self.eval(s, field)
            return UNKNOWN
        if tag == "index":
            base = self.eval(s, node[2])
            index = self.eval(s, node[3])
            if base is not UNKNOWN and base[0] == "segs" and index is not UNKNOWN and index[0] == "int" \
                    and index[1][0] == "const":
                hid = self.segment(s, base[1], index[1][1])
                return ("str", (hid,)) if hid is not None else UNKNOWN
            return UNKNOWN
        return UNKNOWN

    def snapshot(self, s):
        """The scope a boolean was made in, kept aside and named by a number, so
        that a state's key does not grow with every boolean it holds."""
        key = repr(sorted((k, repr(v)) for k, v in s.env.items()))
        if key not in self.snapshot_ids:
            self.snapshot_ids[key] = len(self.snapshots)
            self.snapshots.append(dict(s.env))
        return self.snapshot_ids[key]

    def record_field(self, s, node):
        if node[0] == "member" and node[2][0] == "id":
            rec = s.env.get(node[2][2])
            if rec is not None and rec is not UNKNOWN and rec[0] == "record":
                return rec[1].get(node[3])
        return None

    def int_const(self, s, iv):
        kind = iv[0]
        if kind == "const":
            return iv[1]
        if kind == "len":
            t = s.resolve(iv[1])
            return len(literal_text(t)) if is_literal(t) else None
        if kind in ("add", "sub"):
            a = self.int_const(s, iv[1])
            b = self.int_const(s, iv[2])
            if a is None or b is None:
                return None
            return a + b if kind == "add" else a - b
        return None

    def eval_call(self, s, node):
        fn = node[2]
        args = node[3]
        if fn[0] == "id":
            name = fn[2]
            if name in ("AnsiString", "String", "std::string", "DecodePathSegment_", "PercentDecode",
                        "RestApiServer::DecodePathSegment_"):
                if not args:
                    return ("str", ())
                value = self.eval(s, args[0])
                return value if value is not UNKNOWN and value[0] == "str" else UNKNOWN
            if name in ("StringParser::SplitString",) and len(args) == 2:
                base = self.eval(s, args[0])
                sep = self.eval(s, args[1])
                if base is not UNKNOWN and base[0] == "str" and sep is not UNKNOWN and sep[0] == "str" \
                        and literal_text(sep[1]) == "/":
                    return ("segs", self.new_segs(s, base[1]))
                return UNKNOWN
            if name in self.functions and self.functions[name][2]:
                # a route parser: run it for the kinds it assigns
                self.call_parser(s, name, args)
                return UNKNOWN
            return UNKNOWN
        if fn[0] == "member" and fn[3] in ("StartsWith", "EndsWith", "IsEmpty"):
            return ("bool", node, self.snapshot(s))
        if fn[0] == "id" and fn[2] in ("ParseQueueId", "isNumber", "IsNumber"):
            return ("bool", node, self.snapshot(s))
        if fn[0] == "member":
            obj = self.eval(s, fn[2])
            method = fn[3]
            if obj is UNKNOWN:
                return UNKNOWN
            if obj[0] == "str":
                t = obj[1]
                if method == "GetLength":
                    return ("int", ("len", t))
                if method in ("c_str", "Trim", "MakeLower", "MakeUpper"):
                    return obj
                if method == "Mid":
                    start = self.eval(s, args[0]) if args else UNKNOWN
                    length = self.eval(s, args[1]) if len(args) > 1 else None
                    return self.mid(s, t, start, length)
                if method == "Left" and args:
                    n = self.eval(s, args[0])
                    if n is not UNKNOWN and n[0] == "int":
                        return self.mid(s, t, ("int", ("const", 0)), n)
                    return UNKNOWN
                if method == "Find":
                    needle = self.eval(s, args[0]) if args else UNKNOWN
                    start = 0
                    if len(args) > 1:
                        sv = self.eval(s, args[1])
                        start = self.int_const(s, sv[1]) if sv is not UNKNOWN and sv[0] == "int" else None
                    if needle is UNKNOWN or needle[0] != "str" or not is_literal(needle[1]) or start is None:
                        return UNKNOWN
                    return ("int", ("find", self.find_key(s, t, literal_text(needle[1]), start)))
            if obj[0] == "segs":
                if method == "size":
                    return ("int", ("segcount", obj[1]))
            return UNKNOWN
        return UNKNOWN

    # templates ----------------------------------------------------------
    def find_key(self, s, template, needle, start):
        key = (repr(tuple(template)), needle, start)
        if key not in s.finds:
            s.finds[key] = {"of": tuple(template), "needle": needle, "start": start, "status": None,
                            "before": None, "after": None}
        return key

    def split_at(self, s, template, needle, start):
        """Refine template so that needle is found at or after start: (before, after).
        A literal that holds the needle is preferred to a hole that might: the
        parsers search for a separator they have just seen in a literal."""
        parts = list(s.resolve(template))
        offset = 0
        known = True
        first_hole = None
        for index, part in enumerate(parts):
            if isinstance(part, str):
                begin = max(0, start - offset) if known else 0
                found = part.find(needle, begin) if begin <= len(part) else -1
                if found >= 0:
                    before = tuple(parts[:index]) + (part[:found],)
                    after = (part[found + len(needle):],) + tuple(parts[index + 1:])
                    return before, after
                offset += len(part)
            else:
                placeable = not known or offset >= start
                if first_hole is None and placeable and not (needle == "/" and self.hole(s, part).noslash):
                    first_hole = index
                known = False
        if first_hole is None:
            return False if known else None
        part = parts[first_hole]
        a = s.new_hole()
        b = s.new_hole()
        self.hole(s, a).name = self.hole(s, part).name
        self.hole(s, a).noslash = needle == "/" or self.hole(s, part).noslash
        self.hole(s, b).noslash = self.hole(s, part).noslash
        s.subst[part] = (a, needle, b)
        return tuple(parts[:first_hole]) + (a,), (b,) + tuple(parts[first_hole + 1:])

    def mid(self, s, template, start, length):
        if start is UNKNOWN or start[0] != "int":
            return UNKNOWN
        sv = start[1]
        t = s.resolve(template)
        # Mid(find [+ k])
        base, extra = self.find_offset(sv)
        if base is not None:
            f = self.literal_find(s, base)
            if not f or f["status"] != "found":
                return UNKNOWN
            k = self.int_const(s, extra) if extra is not None else 0
            if k is None:
                return UNKNOWN
            after = (f["needle"],) + tuple(f["after"])
            rest = self.drop_prefix(s, after, k)
            if rest is None:
                return UNKNOWN
            if length is not None:
                return UNKNOWN
            return ("str", rest)
        n = self.int_const(s, sv)
        if n is None:
            return UNKNOWN
        rest = self.drop_prefix(s, t, n)
        if rest is None:
            return UNKNOWN
        if length is None:
            return ("str", rest)
        if length is UNKNOWN or length[0] != "int":
            return UNKNOWN
        lv = length[1]
        # Mid(n, find - n): what is before the needle, less the first n
        lbase, lextra = self.find_offset(lv)
        if lbase is not None:
            f = self.literal_find(s, lbase)
            if not f or f["status"] != "found":
                return UNKNOWN
            k = self.int_const(s, lextra) if lextra is not None else 0
            if k is None or k != -n:
                return UNKNOWN
            before = self.drop_prefix(s, tuple(f["before"]), n)
            return ("str", before) if before is not None else UNKNOWN
        # Mid(n, len - c): all but the first n and the last c - n
        form = self.linear(s, lv)
        if form is not None and form[0] is not None and repr(s.resolve(form[0])) == repr(t):
            k = -form[1] - n
            if k < 0:
                return UNKNOWN
            return self.drop_suffix(s, rest, k)
        fixed = self.int_const(s, lv)
        if fixed is not None and is_literal(rest):
            return ("str", lit(literal_text(rest)[:fixed]))
        return UNKNOWN

    def literal_find(self, s, key):
        """The find, settled as found when its needle is a literal of the template
        already: a parser that tested for the separator before searching for it."""
        f = s.finds.get(key)
        if f is None or f["status"] is not None:
            return f
        whole = s.resolve(f["of"])
        if any(isinstance(p, str) and f["needle"] in p for p in whole):
            split = self.split_at(s, f["of"], f["needle"], f["start"])
            if split:
                f["status"] = "found"
                f["before"], f["after"] = split
        return f

    def linear(self, s, iv):
        """(template or None, constant) for iv = len(template) + constant."""
        kind = iv[0]
        if kind == "const":
            return None, iv[1]
        if kind == "len":
            whole = s.resolve(iv[1])
            if is_literal(whole):
                return None, len(literal_text(whole))
            return tuple(iv[1]), 0
        if kind in ("add", "sub"):
            a = self.linear(s, iv[1])
            b = self.linear(s, iv[2])
            if a is None or b is None:
                return None
            sign = 1 if kind == "add" else -1
            if a[0] is not None and b[0] is not None:
                return None
            if b[0] is not None and sign < 0:
                return None
            return (a[0] if a[0] is not None else b[0], a[1] + sign * b[1])
        return None

    def find_offset(self, iv):
        if iv[0] == "find":
            return iv[1], None
        if iv[0] == "add" and iv[1][0] == "find":
            return iv[1][1], iv[2]
        if iv[0] == "add" and iv[2][0] == "find":
            return iv[2][1], iv[1]
        if iv[0] == "sub" and iv[1][0] == "find":
            return iv[1][1], ("sub", ("const", 0), iv[2])
        return None, None

    def drop_prefix(self, s, template, n):
        parts = list(s.resolve(template))
        while n > 0 and parts:
            head = parts[0]
            if isinstance(head, str):
                if len(head) <= n:
                    n -= len(head)
                    parts.pop(0)
                else:
                    parts[0] = head[n:]
                    n = 0
            else:
                # n characters out of a hole: a fresh hole for what remains
                fresh = s.new_hole(self.hole(s, head).name)
                s.holes[fresh].noslash = self.hole(s, head).noslash
                parts[0] = fresh
                n = 0
        return tuple(parts)

    def drop_suffix(self, s, template, k):
        parts = list(s.resolve(template))
        while k > 0 and parts:
            tail = parts[-1]
            if isinstance(tail, str):
                if len(tail) <= k:
                    k -= len(tail)
                    parts.pop()
                else:
                    parts[-1] = tail[:-k]
                    k = 0
            else:
                fresh = s.new_hole(self.hole(s, tail).name)
                s.holes[fresh].noslash = self.hole(s, tail).noslash
                s.subst[tail] = (fresh, s.new_hole())
                parts[-1] = fresh
                k = 0
        return ("str", tuple(parts))

    def new_segs(self, s, template):
        return {"base": tuple(template), "segs": None, "fixed": None, "id": repr(s.resolve(template))}

    def segment(self, s, segs, index):
        key = ("segs", segs["id"])
        info = s.finds.get(key)
        if info is None:
            info = {"segs": [], "more": None}
            s.finds[key] = info
            whole = s.resolve(segs["base"])
            if len(whole) == 1 and isinstance(whole[0], int):
                info["root"] = whole[0]
            else:
                return None
        while len(info["segs"]) <= index:
            if info["segs"] and info["more"] is None:
                return None
            if not info["segs"]:
                seg = s.new_hole()
                more = s.new_hole()
                s.holes[seg].noslash = True
                s.subst[info["root"]] = (seg, more)
                info["segs"].append(seg)
                info["more"] = more
            else:
                seg = s.new_hole()
                more = s.new_hole()
                s.holes[seg].noslash = True
                s.subst[info["more"]] = ("/", seg, more)
                info["segs"].append(seg)
                info["more"] = more
        return info["segs"][index]

    def segment_count(self, s, segs, count, truth):
        key = ("segs", segs["id"])
        info = s.finds.get(key)
        if info is not None and info.get("segs") and info.get("more") is None:
            # the count is settled already
            return [s] if (len(info["segs"]) == count) == truth else []
        if info is not None and len(info.get("segs") or []) > count and truth:
            return []
        if not truth:
            return [s]
        n = s.clone()
        if count <= 0:
            return [n]
        if self.segment(n, segs, count - 1) is None:
            return []
        info = n.finds.get(key)
        if info is None or info.get("more") is None:
            return [n]
        n.subst[info["more"]] = ()
        info["more"] = None
        return [n]

    def unify(self, s, template, text):
        """Refine template to equal text; False when it cannot."""
        parts = list(s.resolve(template))
        holes = [p for p in parts if isinstance(p, int)]
        if not holes:
            return literal_text(parts) == text
        if len(holes) > 1:
            # Ambiguous: put the text's prefix and suffix against the literals
            # and give the middle to one hole, the rest empty.
            pass
        prefix = ""
        for p in parts:
            if isinstance(p, str):
                prefix += p
            else:
                break
        suffix = ""
        for p in reversed(parts):
            if isinstance(p, str):
                suffix = p + suffix
            else:
                break
        if not text.startswith(prefix) or not text.endswith(suffix) or len(prefix) + len(suffix) > len(text):
            return False
        middle = text[len(prefix):len(text) - len(suffix)]
        inner = parts[len([p for p in parts[:parts.index(holes[0])]]):]
        first = holes[0]
        if len(holes) == 1:
            if self.hole(s, first).noslash and "/" in middle:
                return False
            s.subst[first] = (middle,)
            return True
        # several holes: the first takes the middle, the others are empty, and the
        # literals between them must then be empty too - otherwise give up
        between = [p for p in parts[parts.index(first) + 1:parts.index(holes[-1])] if isinstance(p, str)]
        if any(between):
            return None
        s.subst[first] = (middle,)
        for h in holes[1:]:
            s.subst[h] = ()
        return True

    def starts_with(self, s, template, needle):
        parts = list(s.resolve(template))
        rest = needle
        for index, part in enumerate(parts):
            if not rest:
                return True
            if isinstance(part, str):
                common = min(len(part), len(rest))
                if part[:common].lower() != rest[:common].lower():
                    return False
                rest = rest[common:]
            else:
                if self.hole(s, part).noslash and "/" in rest:
                    return False
                fresh = s.new_hole(self.hole(s, part).name)
                s.holes[fresh].noslash = self.hole(s, part).noslash
                s.subst[part] = (rest, fresh)
                return True
        return not rest

    def ends_with(self, s, template, needle):
        parts = list(s.resolve(template))
        rest = needle
        for index in range(len(parts) - 1, -1, -1):
            part = parts[index]
            if not rest:
                return True
            if isinstance(part, str):
                common = min(len(part), len(rest))
                if part[len(part) - common:].lower() != rest[len(rest) - common:].lower():
                    return False
                rest = rest[:len(rest) - common]
            else:
                if self.hole(s, part).noslash and "/" in rest:
                    return False
                fresh = s.new_hole(self.hole(s, part).name)
                s.holes[fresh].noslash = self.hole(s, part).noslash
                s.subst[part] = (fresh, rest)
                return True
        return not rest

    def mark(self, s, template, **attrs):
        t = s.resolve(template)
        holes = [p for p in t if isinstance(p, int)]
        if len(holes) != 1:
            return
        hole = self.hole(s, holes[0])
        for k, v in attrs.items():
            if k == "name":
                if hole.name is None or hole.name in ("rest", "tail", "after", "idText", "idPart", "path", "target"):
                    hole.name = v
            else:
                setattr(hole, k, v)

    def hole(self, s, hid):
        """The hole's annotations in s, as a copy s owns: states share the rest."""
        if hid not in s.holes:
            s.holes[hid] = Hole()
            return s.holes[hid]
        own = copy.copy(s.holes[hid])
        s.holes[hid] = own
        return own

    # conditions ---------------------------------------------------------
    def definite(self, s, node):
        """True or False when the condition is decided in s without refining it."""
        trial_t = self.assume(s.clone(), node, True)
        trial_f = self.assume(s.clone(), node, False)
        if trial_t and not trial_f:
            return True
        if trial_f and not trial_t:
            return False
        return None

    def assume(self, s, node, truth):
        """The states in which node evaluates to truth (empty when it cannot)."""
        tag = node[0]
        if tag == "un" and node[2] == "!":
            return self.assume(s, node[3], not truth)
        if tag == "bin" and node[2] in ("&&", "||"):
            conj = (node[2] == "&&") == truth
            left, right = node[3], node[4]
            if conj:
                out = []
                for a in self.assume(s, left, truth):
                    out.extend(self.assume(a, right, truth))
                return self.dedupe(out)
            # a disjunction of the two: left, or (not left and right)
            first = self.assume(s.clone(), left, truth)
            second = []
            for a in self.assume(s.clone(), left, not truth):
                second.extend(self.assume(a, right, truth))
            out = first + second
            if any(self.same(x, s) for x in out):
                return [s]
            return self.dedupe(out)
        if tag == "id":
            value = s.env.get(node[2])
            if value is not None and value is not UNKNOWN and value[0] == "bool":
                # asked in the scope it was made in: a later assignment to a
                # variable it names does not change what it said
                saved = s.env
                trial = s.clone()
                trial.env = dict(self.snapshots[value[2]])
                out = self.assume(trial, value[1], truth)
                for st in out:
                    st.env = dict(saved)
                if len(out) == 1 and out[0].key() == s.key() and out[0].subst == s.subst:
                    return [s]
                return out
            if value is not None and value is not UNKNOWN and value[0] == "boolconst":
                return [s] if value[1] == truth else []
            return [s]
        if tag == "num":
            return [s] if bool(node[2]) == truth else []
        if tag == "bin" and node[2] in ("==", "!="):
            positive = (node[2] == "==") == truth
            return self.assume_equal(s, node[3], node[4], positive, node)
        if tag == "bin" and node[2] in ("<", ">", "<=", ">="):
            return self.assume_order(s, node, truth)
        if tag == "call":
            return self.assume_call(s, node, truth)
        return [s]

    def same(self, a, b):
        return a is b

    def dedupe(self, states):
        groups = {}
        order = []
        for st in states:
            key = (repr(sorted((k, repr(v)) for k, v in st.env.items())), repr(sorted(st.subst.items())), st.notes)
            if key not in groups:
                groups[key] = st
                order.append(key)
                continue
            kept = groups[key]
            if kept.methods is None or st.methods is None:
                merged = None
            else:
                merged = frozenset(kept.methods) | frozenset(st.methods)
                if merged >= frozenset(METHODS):
                    merged = None
            if merged != kept.methods:
                kept = kept.clone()
                kept.methods = merged
                groups[key] = kept
        return [groups[k] for k in order]

    def assume_equal(self, s, a, b, positive, node):
        # route.kind == RouteX: the kind this path has been given so far
        for x, y in ((a, b), (b, a)):
            is_kind = x[0] == "member" and x[3] == "kind" and x[2][0] == "id" and s.env.get(x[2][2]) == ("route",)
            if is_kind and y[0] == "id" and y[2].startswith("Route"):
                current = s.env.get("__kind__")
                name = current[1] if current is not None and current is not UNKNOWN and current[0] == "kind" else None
                is_it = (name or "RouteUnknown") == y[2]
                return [s] if is_it == positive else []
        # method == "X"
        for x, y in ((a, b), (b, a)):
            if x[0] == "id" and s.env.get(x[2]) == ("method",):
                if y[0] != "str":
                    return [s]
                word = y[2]
                current = set(s.methods) if s.methods is not None else None
                if positive:
                    if current is not None and word not in current:
                        return []
                    n = s.clone()
                    n.methods = frozenset([word])
                    return [n]
                if current is not None:
                    current.discard(word)
                    if not current:
                        return []
                    n = s.clone()
                    n.methods = frozenset(current)
                    return [n]
                n = s.clone()
                n.methods = frozenset(m for m in METHODS if m != word)
                return [n]
        # x.GetAt(x.GetLength() - 1) == 'c', x.GetAt(0) == 'c': EndsWith, StartsWith
        for x, y in ((a, b), (b, a)):
            if x[0] == "call" and x[2][0] == "member" and x[2][3] == "GetAt" and y[0] == "chr" and x[3]:
                index = x[3][0]
                which = None
                if index[0] == "num" and index[2] == 0:
                    which = "StartsWith"
                elif (index[0] == "bin" and index[2] == "-" and index[4][0] == "num" and index[4][2] == 1
                      and index[3][0] == "call" and index[3][2][0] == "member" and index[3][2][3] == "GetLength"):
                    which = "EndsWith"
                if which:
                    call = ("call", x[1], ("member", x[1], x[2][2], which), [("str", y[1], y[2])])
                    return self.assume_call(s, call, positive)
        # x.CompareNoCase("lit") == 0
        for x, y in ((a, b), (b, a)):
            if x[0] == "call" and x[2][0] == "member" and x[2][3] == "CompareNoCase" and y[0] == "num" and y[2] == 0:
                return self.assume_equal(s, x[2][2], x[3][0], positive, node)
        # int comparisons: a Find result against -1, a segment count against N
        va = self.eval(s, a)
        vb = self.eval(s, b)
        if va is not UNKNOWN and va[0] == "int" and va[1][0] == "segcount" and vb is not UNKNOWN and vb[0] == "int":
            count = self.int_const(s, vb[1])
            if count is not None:
                return self.segment_count(s, va[1][1], count, positive)
            return [s]
        if va is not UNKNOWN and vb is not UNKNOWN and va[0] == "str" and vb[0] == "str":
            if is_literal(s.resolve(vb[1])):
                target, text = va[1], literal_text(s.resolve(vb[1]))
            elif is_literal(s.resolve(va[1])):
                target, text = vb[1], literal_text(s.resolve(va[1]))
            else:
                return [s]
            if positive:
                n = s.clone()
                result = self.unify(n, target, text)
                if result is False:
                    return []
                return [n] if result else [s]
            # not equal: impossible only when it is that text already
            t = s.resolve(target)
            if is_literal(t) and literal_text(t) == text:
                return []
            return [s]
        return [s]

    def assume_order(self, s, node, truth):
        op = node[2]
        a = self.eval(s, node[3])
        b = self.eval(s, node[4])
        if a is UNKNOWN or b is UNKNOWN or a[0] != "int" or b[0] != "int":
            return [s]
        if a[1][0] != "find":
            # GetLength() comparisons and the like narrow nothing
            if a[1][0] == "len" and b[1][0] == "const" and truth and op in (">", ">="):
                return [s]
            return [s]
        bound = self.int_const(s, b[1])
        if bound is None:
            return [s]
        # Find(...) compared with a number: found or not
        found_when = {
            ("<", 0): False, ("<=", -1): False, (">=", 0): True, (">", -1): True,
        }.get((op, bound))
        if found_when is None:
            if op in (">", ">=") and bound >= 0:
                found_when = True
            elif op in ("<", "<=") and bound <= 0:
                found_when = False
            else:
                return [s]
        found = found_when == truth
        key = a[1][1]
        return self.set_found(s, key, found)

    def set_found(self, s, key, found):
        f = s.finds.get(key)
        if f is None:
            return [s]
        if f["status"] is not None:
            return [s] if (f["status"] == "found") == found else []
        n = s.clone()
        g = n.finds[key]
        if found:
            split = self.split_at(n, g["of"], g["needle"], g["start"])
            if split is False:
                return []
            if split is None:
                g["status"] = "found"
                g["before"] = (n.new_hole(),)
                g["after"] = (n.new_hole(),)
                return [n]
            g["status"] = "found"
            g["before"], g["after"] = split
            return [n]
        g["status"] = "notfound"
        t = n.resolve(g["of"])
        offset = 0
        for part in t:
            if isinstance(part, str):
                if part.find(g["needle"], max(0, g["start"] - offset)) >= 0 and offset + len(part) > g["start"]:
                    return []
                offset += len(part)
            elif g["needle"] == "/" and offset >= g["start"]:
                self.hole(n, part).noslash = True
        return [n]

    def assume_call(self, s, node, truth):
        fn = node[2]
        args = node[3]
        if fn[0] == "member":
            obj = self.eval(s, fn[2])
            method = fn[3]
            if obj is UNKNOWN and method.startswith(("Get", "Is", "Has")):
                n = s.clone()
                n.notes = n.notes + ((method if truth else "not " + method),)
                return [n]
            if obj is UNKNOWN or obj[0] not in ("str", "segs"):
                return [s]
            if obj[0] == "segs":
                if method == "empty":
                    return [s]
                return [s]
            t = obj[1]
            if method in ("StartsWith", "EndsWith") and args:
                needle = self.eval(s, args[0])
                if needle is UNKNOWN or needle[0] != "str" or not is_literal(s.resolve(needle[1])):
                    return [s]
                text = literal_text(s.resolve(needle[1]))
                if not truth:
                    whole = s.resolve(t)
                    prefix = ""
                    for p in whole:
                        if not isinstance(p, str):
                            break
                        prefix += p
                    suffix = ""
                    for p in reversed(whole):
                        if not isinstance(p, str):
                            break
                        suffix = p + suffix
                    if method == "StartsWith":
                        holds = prefix.lower().startswith(text.lower())
                    else:
                        holds = suffix.lower().endswith(text.lower())
                    return [] if holds else [s]
                n = s.clone()
                ok = self.starts_with(n, t, text) if method == "StartsWith" else self.ends_with(n, t, text)
                return [n] if ok else []
            if method == "IsEmpty":
                if truth:
                    n = s.clone()
                    result = self.unify(n, t, "")
                    if result is False:
                        return []
                    return [n] if result else [s]
                whole = s.resolve(t)
                if is_literal(whole) and not literal_text(whole):
                    return []
                return [s]
            return [s]
        if fn[0] == "id":
            name = fn[2]
            if name in ("ParseQueueId", "isNumber", "IsNumber") and args:
                value = self.eval(s, args[0])
                if value is UNKNOWN or value[0] != "str":
                    return [s]
                if not truth:
                    whole = s.resolve(value[1])
                    if is_literal(whole) and literal_text(whole).isdigit():
                        return []
                    return [s]
                whole = s.resolve(value[1])
                if is_literal(whole):
                    return [s] if literal_text(whole).isdigit() else []
                n = s.clone()
                field = "id"
                if len(args) > 1 and args[1][0] == "member":
                    field = args[1][3]
                self.mark(n, value[1], numeric=field, noslash=True)
                return [n]
            inlined = self.functions.get("__predicates__", {}).get(name)
            if inlined is not None:
                params, expr = inlined
                n = s.clone()
                saved = dict(n.env)
                for p, a in zip(params, args):
                    n.env[p] = self.eval(s, a)
                out = self.assume(n, expr, truth)
                for st in out:
                    for p in params:
                        if p in saved:
                            st.env[p] = saved[p]
                        else:
                            st.env.pop(p, None)
                return out
            if name in self.functions and self.functions[name][2]:
                self.call_parser(s, name, args)
                return [s]
        # A condition about something other than the path - a setting, the
        # transport: kept as a note for the anonymous chains' "When" column.
        callee = self.callee_name(fn)
        if callee:
            n = s.clone()
            n.notes = n.notes + ((callee if truth else "not " + callee),)
            return [n]
        return [s]

    def callee_name(self, fn):
        if fn[0] == "id":
            return fn[2].split("::")[-1].rstrip("_")
        if fn[0] == "member":
            return fn[3].rstrip("_")
        return None

    # statements ---------------------------------------------------------
    def run(self, node, states):
        """Executes node in each state; returns the states that fall through."""
        tag = node[0]
        if not states:
            return []
        if tag == "block":
            live = states
            entry = [set(st.env) for st in states][0] if states else set()
            for stmt in node[2]:
                live = self.dedupe(self.run(stmt, live))
                if len(live) > 256:
                    raise GenerationError("line %d: %d paths at once - the interpreter cannot follow this function"
                                          % (stmt[1], len(live)))
                if not live:
                    break
            for st in live:
                for name in [k for k in st.env if k not in entry]:
                    del st.env[name]
            return self.dedupe(live)
        if tag == "if":
            out = []
            for st in states:
                yes = self.assume(st.clone(), node[2], True)
                no = self.assume(st.clone(), node[2], False)
                # What falls out of the branch is widened back to the state
                # before the condition, keeping only what the branch assigned
                # to variables that were there before it: a sound
                # over-approximation, and what keeps a parser written as a long
                # run of ifs from doubling its paths at every one.
                for done in self.run(node[3], yes):
                    out.append(self.widen(st, done))
                if node[4] is not None:
                    for done in self.run(node[4], no):
                        out.append(self.widen(st, done))
                else:
                    out.extend(no)
            return self.dedupe(out)
        if tag == "return":
            for st in states:
                if node[2] is not None:
                    self.on_return(st, node[2])
                self.settle(st)
            return []
        if tag in ("decl", "expr"):
            out = []
            for st in states:
                out.extend(self.run_assignment(st, node))
            return out
        if tag == "loop":
            # Recorded once for what it assigns, then passed over: no loop in a
            # route parser decides a route.
            self.run(node[2], [st.clone() for st in states])
            return states
        if tag == "try":
            return self.run(node[2], states)
        if tag == "struct":
            self.structs[node[2]] = node[3]
            return states
        if tag == "table":
            for st in states:
                st.env[node[2]] = ("table", node[3])
            return states
        if tag == "rangefor":
            var, table, body = node[2], node[3], node[4]
            live = states
            rows = None
            for st in states:
                value = st.env.get(table)
                if value is not None and value is not UNKNOWN and value[0] == "table":
                    rows = value[1]
            if rows is None:
                self.run(body, [st.clone() for st in states])
                return states
            fields = None
            for candidate in self.structs.values():
                if all(len(row) == len(candidate) for row in rows):
                    fields = candidate
            for row in rows:
                bound = []
                for st in live:
                    n = st.clone()
                    n.env[var] = ("record", dict(zip(fields or [], row)))
                    bound.append(n)
                live = self.run(body, bound)
                for st in live:
                    st.env.pop(var, None)
                live = self.dedupe(live)
            return live
        if tag == "switch":
            self.run(node[3], [st.clone() for st in states])
            return states
        return states

    def widen(self, before, after):
        """The state a branch falls out with, widened back to the state before
        it - unless the branch assigned a variable that was there before, whose
        new value only means something with the refinements made beside it."""
        for name, value in after.env.items():
            if name == "__kind__":
                continue
            if name in before.env and repr(before.env[name]) != repr(value):
                return after
        n = before.clone()
        if "__kind__" in after.env:
            n.env["__kind__"] = after.env["__kind__"]
        # The methods are kept: narrowing them costs no paths, since states
        # that differ only in their methods are merged.
        n.methods = after.methods
        for hid, hole in after.holes.items():
            if hid not in n.holes:
                n.holes[hid] = hole
        return n

    def on_return(self, st, value):
        if value[0] == "call" and self.record_return is not None:
            fn = value[2]
            name = fn[2] if fn[0] == "id" else None
            if name is None and fn[0] == "member":
                return
            if name and (name.startswith("Handle") or "::Handle" in name or name.startswith("Refuse")
                         or name.startswith("WebKeyDirectory")):
                self.record_return(st, name, value[1])
        if value[0] == "call" and value[2][0] == "id" and value[2][2] in self.functions \
                and self.functions[value[2][2]][2]:
            self.call_parser(st, value[2][2], value[3])

    def run_assignment(self, st, node):
        if node[0] == "decl":
            name, value = node[2], node[3]
            if value is None:
                st = st.clone()
                st.env[name] = UNKNOWN
                return [st]
            return self.assign(st, ("id", node[1], name), value)
        expr = node[2]
        if expr[0] == "assign" and expr[2] == "=":
            return self.assign(st, expr[3], expr[4])
        if expr[0] == "call":
            fn = expr[2]
            if fn[0] == "id" and fn[2] in self.functions and self.functions[fn[2]][2]:
                self.call_parser(st, fn[2], expr[3])
        return [st]

    def assign(self, st, target, value):
        if value[0] == "ternary":
            out = []
            for branch, truth in ((value[3], True), (value[4], False)):
                for n in self.assume(st.clone(), value[2], truth):
                    out.extend(self.assign(n, target, branch))
            return out
        n = st.clone()
        if target[0] == "member" and target[2][0] == "id" and n.env.get(target[2][2]) == ("route",):
            field = target[3]
            if field == "kind":
                # Recorded where the parser lets go of the request - a return,
                # or the end of the function - and not here: a later assignment
                # on the same path (the API keys' "unsupported" first, then the
                # verb) is the one that decides.
                resolved = self.record_field(n, value) or value
                if resolved[0] == "id" and resolved[2].startswith("Route"):
                    index = self.record_kind(n, resolved[2], value[1])
                    n.env["__kind__"] = ("kind", resolved[2], index)
                return [n]
            v = self.eval(n, value)
            if v is not UNKNOWN and v[0] == "str":
                if field == "identifier":
                    self.mark(n, v[1], role="identifier")
                elif field == "name":
                    self.mark(n, v[1], role="name")
            elif value[0] == "call" and value[3]:
                # route.record_id = _atoi64(segments[0].c_str())
                inner = value[3][0]
                v2 = self.eval(n, inner)
                if v2 is not UNKNOWN and v2[0] == "str":
                    self.mark(n, v2[1], numeric=field, noslash=True)
            return [n]
        if target[0] != "id":
            return [n]
        name = target[2]
        if self.pinned and name in ("path", "method", "query", "queryPosition"):
            # The anonymous chains are read from the path the request line
            # gives with its query string already taken off: the statements
            # that take it off are passed over, not followed.
            return [n]
        v = self.eval(n, value)
        if v is not UNKNOWN and v[0] == "str":
            self.mark(n, v[1], name=name)
        if v is not UNKNOWN and v[0] == "bool":
            # keep the condition, to be asked where the variable is
            pass
        n.env[name] = v
        return [n]

    pinned = False

    def settle(self, st):
        """The route kind a state leaves the parser with: the assignment that made
        it is final, for the methods still possible here."""
        kind = st.env.get("__kind__")
        if kind is not None and kind is not UNKNOWN and kind[0] == "kind" and kind[1] and kind[2] is not None:
            self.final.setdefault(kind[2], []).append(st.methods)

    def call_parser(self, st, name, args):
        if self.depth > 4:
            return
        ast, params, is_parser = self.functions[name]
        n = st.clone()
        values = [self.eval(st, a) for a in args]
        n.env = {"__kind__": st.env.get("__kind__", ("kind", None, 0))}
        for p, v in zip(params, values):
            n.env[p] = v
        self.depth += 1
        try:
            for done in self.run(ast, [n]):
                self.settle(done)
        finally:
            self.depth -= 1


# ---------------------------------------------------------------------------
# Reading the route table
# ---------------------------------------------------------------------------

def hole_display(state, hid):
    hole = state.holes.get(hid)
    if hole is None:
        return "{...}"
    if hole.numeric:
        name = {"message_id": "id", "record_id": "id", "folder_id": "folder_id", "range_id": "id",
                "archive_id": "id", "member_account_id": "account_id", "attachment_index": "index",
                "id": "id"}.get(hole.numeric, hole.numeric)
        return "{%s}" % name
    name = hole.name if hole.name not in (None, "path", "target") else None
    name = name or ("identifier" if hole.role == "identifier" else ("name" if hole.role == "name" else None))
    if name is None:
        return "{...}" if not hole.noslash else "{segment}"
    simple = {
        "idText": "id", "idPart": "id", "rest": "...", "tail": "...", "after": "...", "remainder": "...",
        "listAddress": "list", "accountAddress": "address", "domainName": "domain", "addressText": "address",
        "collection": "collection",
    }.get(name, name)
    if simple == "..." and hole.noslash:
        simple = "segment"
    if hole.role == "identifier" and simple == "...":
        simple = "identifier"
    return "{%s}" % simple


def render_template(state, template):
    out = []
    for part in state.resolve(template):
        if isinstance(part, str):
            out.append(part)
        else:
            out.append(hole_display(state, part))
    text = "".join(out)
    text = re.sub(r"(\{\.\.\.\})+", "{...}", text)
    return text


def method_text(methods):
    if methods is None:
        return "any"
    if set(methods) >= set(METHODS) - {"HEAD"} and len(methods) >= len(METHODS) - 1:
        return "any"
    if len(methods) > 6:
        return "any but " + ", ".join(m for m in METHODS if m not in methods)
    order = {m: i for i, m in enumerate(METHODS)}
    return ", ".join(sorted(methods, key=lambda m: order.get(m, 99)))


def load_functions(sources):
    functions = {}
    rest = read(REST_CPP, sources)
    for name, rel in SUB_PARSERS.items():
        text = read(rel, sources)
        qualified = "RestApiServer::" + name
        ast, _ = parse_function(text, qualified, rel)
        if ast is None:
            raise GenerationError("%s: %s could not be read" % (rel, qualified))
        functions[name] = (ast, function_params(text, qualified), True)
    predicates = {}
    for name, rel in PREDICATES.items():
        text = read(rel, sources)
        qualified = name if "::" in name else "RestApiServer::" + name
        body, line = function_text(text, qualified)
        if body is None:
            raise GenerationError("%s: %s could not be read" % (rel, qualified))
        ast = Parser(tokenize(body, line, "%s: %s" % (rel, qualified))).block()
        # the predicate is its last return; earlier statements strip a query string
        returns = [stmt for stmt in ast[2] if stmt[0] == "return"]
        if not returns:
            raise GenerationError("%s: %s has no return to inline" % (rel, qualified))
        params = function_params(text, qualified)
        expr = returns[-1][2]
        # CalDav's and CardDav's copy the target into `path` first
        if params and any(stmt[0] == "decl" and stmt[2] == "path" for stmt in ast[2]):
            params = ["path"]
        predicates[name] = (params, expr)
        short = name.split("::")[-1]
        predicates.setdefault(short, (params, expr))
    functions["__predicates__"] = predicates
    constants = {}
    for name, (rel, qualified) in CONSTANTS.items():
        # Its definition, outside every comment, literal and conditional of the
        # preprocessor: the one string literal it is given.
        what = "%s: the constant %s" % (rel, name)
        lexed = lex(read(rel, sources))
        m = outside_conditionals(lexed.code, re.search(r"\b" + re.escape(qualified) + r"\s*=(?!=)([^;]*);",
                                                       lexed.blind), what)
        value = string_list(lexed.code[m.start(1):m.end(1)], what, lexed.code.count("\n", 0, m.start(1)) + 1) \
            if m else []
        if len(value) != 1:
            raise GenerationError("%s could not be read: it is not defined as one string literal" % what)
        defined_once(lexed.blind, r"\b" + re.escape(qualified) + r"\s*=(?!=)", what)
        constants[name] = value[0]
    functions["__constants__"] = constants
    return functions


def read_rest_routes(sources):
    """[(kind, methods, template text, line)] from ParseRoute_ and its sub-parsers."""
    text = read(REST_CPP, sources)
    functions = load_functions(sources)
    ast, _ = parse_function(text, "RestApiServer::ParseRoute_", REST_CPP)
    if ast is None:
        raise GenerationError("%s: ParseRoute_ could not be read" % REST_CPP)
    records = []

    def record_kind(st, kind, line):
        records.append((kind, st.methods, render_template(st, (0,)), line, st.resolve((0,)), st))
        return len(records) - 1

    interp = Interpreter(sources, record_kind, None, functions)
    start = State()
    start.holes[0] = Hole("path")
    start.env = {"method": ("method",), "path": ("str", (0,)), "route": ("route",), "query": UNKNOWN,
                 "__kind__": ("kind", None, 0)}
    for done in interp.run(ast, [start]):
        interp.settle(done)
    # Only the assignments a path leaves the parser with, each for the methods
    # both its own conditions and the path's later ones allow.
    final = []
    for index, record in enumerate(records):
        if index not in interp.final:
            continue
        for exit_methods in interp.final[index]:
            methods = record[1]
            if methods is None:
                methods = exit_methods
            elif exit_methods is not None:
                methods = frozenset(methods) & frozenset(exit_methods)
            if methods is not None and not methods:
                continue
            final.append((record[0], methods) + tuple(record[2:]))
    return final


def read_anonymous_routes(sources, rel, qualified, stop_at_try):
    """[(handler, methods, template text, conditions, line)] answered before any credential."""
    text = read(rel, sources)
    functions = load_functions(sources)
    ast, _ = parse_function(text, qualified, rel)
    if ast is None:
        raise GenerationError("%s: %s could not be read" % (rel, qualified))
    body = list(ast[2])
    if stop_at_try:
        cut = [i for i, stmt in enumerate(body) if stmt[0] == "try"]
        if not cut:
            raise GenerationError("%s: %s has no try block; the anonymous chain cannot be found" % (rel, qualified))
        body = body[:cut[0]]
    else:
        tries = [stmt for stmt in body if stmt[0] == "try"]
        if tries:
            index = body.index(tries[0])
            body = body[:index] + list(tries[0][2][2])
    records = []

    def record_return(st, name, line):
        records.append((name, st.methods, render_template(st, (0,)), line, st))

    interp = Interpreter(sources, lambda *a: None, record_return, functions)
    interp.pinned = True
    start = State()
    start.holes[0] = Hole("path")
    start.env = {"method": ("method",), "path": ("str", (0,)), "route": ("route",), "query": UNKNOWN,
                 "queryPosition": UNKNOWN}
    interp.run(("block", ast[1], body), [start])
    # One return reached with the method narrowed differently on the way there -
    # a page whose POST is refused before its handler is, say - is one route: its
    # methods are those that reach it, together. The web services listener's
    # table shows the conditions each route is answered under, so there a return
    # reached under other conditions stays a row of its own.
    merged = []
    where = {}
    for name, methods, template, line, st in records:
        key = (name, template, line, tuple(sorted(set(st.notes))) if not stop_at_try else ())
        if key not in where:
            where[key] = len(merged)
            merged.append((name, methods, template, line, st))
            continue
        first = merged[where[key]]
        union = None if first[1] is None or methods is None else frozenset(first[1]) | frozenset(methods)
        merged[where[key]] = (first[0], union) + first[2:]
    return merged


# ---------------------------------------------------------------------------
# Reading the decision
# ---------------------------------------------------------------------------

def body_of(text, qualified, params=None):
    body, _ = function_text(text, qualified, params)
    return body


# What the readers below take out of the code. steps_of masks exactly what these
# take, with the same patterns, before it compares a function's steps with STEPS:
# so a list may change - a route into or out of a refusal, a permission, a
# handler - and nothing else may without the script stopping.
LABEL = r"case (Route\w+):"
LABEL_LINE = r"\s*case (Route\w+):\s*$"
PERMISSION_LINE = r"\s*return RolePermission::(\w+);\s*$"
SECOND_PERMISSION_LINE = r"\s*permission = RolePermission::(\w+);\s*$"
DOMAIN_LINE = r"\s*return (.+);\s*$"
HANDLER_LINE = r"\s*return (\w+)\((.*)\);\s*$"
KEY_ROUTES = "if (IsApiKeyRoute_(route.kind))"
CONFINEMENT = "if (caller.domains.empty())"
DISPATCH = "switch (route.kind)"
CONFINED_REFUSAL = re.compile(r"if \(((?:route\.kind == Route\w+\s*(?:\|\|)?\s*)+)\)\s*\{\s*"
                              r"refusalReason = \"this credential is restricted", re.S)
SERVER_WIDE = re.compile(r"switch \(route\.kind\)\s*\{(.*?)refusalReason = \"this credential is restricted to named "
                         r"domains, and that resource is server-wide\"", re.S)
# A built-in role's permission, as RoleManager.cpp sets each on a line of its
# own: what builtin_permissions reads and steps_of masks.
ROLE_PERMISSION_LINE = r"\s*set \|= Bit\(RolePermission::(\w+)\);\s*$"
# The built-in roles, by the name RoleManager::BuiltInPermissions_ picks each
# one's function by, and whether the function's permission lines are read:
# serveradmin's are not, it is every permission there is, so the whole of
# ServerAdminPermissions is a step.
BUILTIN_ROLES = [
    ("serveradmin", "ServerAdminPermissions", False),
    ("domainadmin", "DomainAdminPermissions", True),
    ("helpdesk", "HelpdeskPermissions", True),
    ("auditor", "AuditorPermissions", True),
]
# The domain DomainNamedByRoute_ answers with, by the expression it is written as.
DOMAIN_EXPRESSIONS = {
    "StringParser::ExtractDomain(String(route.identifier))": "the address's domain",
    "InboundWebhooks::DomainOf(String(route.identifier))": "the address's or domain's",
    "String(route.identifier)": "the domain",
}


class Piece(object):
    """A piece of a text as tokens_of gives it, asked as a match object is:
    lastgroup, group(), start(), end()."""
    __slots__ = ("lastgroup", "text", "begin", "finish")

    def __init__(self, kind, text, begin, finish):
        self.lastgroup = kind
        self.text = text
        self.begin = begin
        self.finish = finish

    def group(self, index=0):
        return self.text

    def start(self):
        return self.begin

    def end(self):
        return self.finish


def tokens_of(text, start=0):
    """Every piece of text from the offset start in order, as translation
    phase 3 makes them (scan), whitespace and comments included, so that joining
    them gives the text back: a run of whitespace, line feeds among it, one
    piece "ws", a directive to the end of its line one piece "pp", a raw string
    literal a "str". text is code, as lex() gives it."""
    line_start = text.rfind("\n", 0, start) + 1
    pending = None
    for kind, begin, end, inside in scan(text, start, not text[line_start:start].strip(" \t\f\v")):
        if inside and kind != "pp":
            pending[2] = end
            continue
        if kind in ("ws", "nl") and pending is not None and pending[0] == "ws":
            pending[2] = end
            continue
        if pending is not None:
            yield Piece(pending[0], text[pending[1]:pending[2]], pending[1], pending[2])
            pending = None
        if kind in ("ws", "nl", "pp"):
            pending = ["pp" if kind == "pp" else "ws", begin, end]
            continue
        yield Piece("str" if kind == "raw" else kind, text[begin:end], begin, end)
    if pending is not None:
        yield Piece(pending[0], text[pending[1]:pending[2]], pending[1], pending[2])


def one_statement_a_line(code):
    """The code with every line break inside parentheses made a space, so that a
    call written over several lines - a dispatch's return among them - is read
    as the one line it is."""
    out = []
    depth = 0
    for m in tokens_of(code):
        piece = m.group(0)
        if m.lastgroup == "op" and piece == "(":
            depth += 1
        elif m.lastgroup == "op" and piece == ")":
            depth = max(0, depth - 1)
        elif m.lastgroup == "ws" and depth and "\n" in piece:
            piece = " "
        out.append(piece)
    return "".join(out)


def braced(code, start):
    """(begin, end) of the first { ... } at or after the offset start, braces
    matched by token, or None."""
    depth = 0
    begin = None
    for m in tokens_of(code, start):
        if m.lastgroup != "op":
            continue
        if m.group(0) == "{":
            if begin is None:
                begin = m.start()
            depth += 1
        elif m.group(0) == "}" and begin is not None:
            depth -= 1
            if depth == 0:
                return begin, m.end()
    return None


def code_find(code, what, start=0):
    """The offset of the first what in code, outside every literal (blind_of),
    or -1."""
    return blind_of(code).find(what, start)


def code_matches(pattern, code):
    """The matches of pattern in code that begin outside every literal: of a
    pattern that holds no quote before the literal it ends in, if any, each one
    is code to its literal."""
    lexed = lex(code)
    return [m for m in pattern.finditer(code) if lexed.literal_at(m.start()) is None]


def key_route_block(authorize):
    """Authorize_'s block for IsApiKeyRoute_, which holds the role-holding
    account's exceptions: (begin, end) in its code, or None."""
    at = code_find(authorize, KEY_ROUTES)
    return braced(authorize, at + len(KEY_ROUTES)) if at >= 0 else None


def authenticated_part(process):
    """ProcessRequest_'s try block - where the anonymous routes end, and from
    which every request has a credential - and the (begin, end) inside it of the
    dispatch switch's braces: (part, span), or (None, None)."""
    depth = 0
    for m in tokens_of(process):
        if m.lastgroup == "op" and m.group(0) == "{":
            depth += 1
        elif m.lastgroup == "op" and m.group(0) == "}":
            depth -= 1
        elif m.lastgroup == "id" and m.group(0) == "try" and depth == 1:
            block = braced(process, m.start())
            if block is None:
                return None, None
            part = process[m.start():block[1]]
            at = code_find(part, DISPATCH)
            return part, (braced(part, at) if at >= 0 else None)
    return None, None


def enumerators(body, what, first_line):
    """[(name, value)] of the enumerators of an enumeration's body, code as
    lex() gives it whose first line is the line first_line of its file, with
    each one's value as the compiler gives it: the integer literal written for
    it, or one more than the one before, the first 0. An enumerator written any
    other way - its value an expression, an attribute after its name - stops
    the script, which evaluates nothing else, and so does a directive of the
    preprocessor in the body (tokenize)."""
    tokens = tokenize(body, first_line, what)
    items = []
    value = -1
    i = 0
    while tokens[i].kind != "eof":
        name = tokens[i]
        if name.kind != "id":
            raise GenerationError("%s: %r is not an enumerator this script reads, a name or a name = an integer "
                                  "literal" % (what, name.text))
        i += 1
        if tokens[i].kind == "op" and tokens[i].text == "=":
            written = tokens[i + 1]
            value = integer_value(written.text) if written.kind == "num" else None
            following = tokens[i + 2] if written.kind != "eof" else written
            if value is None or not (following.kind == "eof" or (following.kind == "op" and following.text == ",")):
                raise GenerationError("%s: %s's value is not written as an integer literal, which is all this script "
                                      "evaluates" % (what, name.text))
            i += 2
        else:
            value += 1
        items.append((name.text, value))
        if tokens[i].kind == "op" and tokens[i].text == ",":
            i += 1
        elif tokens[i].kind != "eof":
            raise GenerationError("%s: %s is followed by %r, where this script reads a comma or the end"
                                  % (what, name.text, tokens[i].text))
    return items


def held_to_places(items, what):
    """items, enumerators(), refused unless each one's value is its place in the
    enumeration, from 0, which the script reads it as."""
    for place, (name, value) in enumerate(items):
        if value != place:
            raise GenerationError("%s: %s is %d, and its place in the enumeration %d: this script reads each "
                                  "enumerator as its place" % (what, name, value, place))
    return items


def enumeration(text, name, pattern, what):
    """The enumerators of the enumeration name whose body is pattern's group 1
    in the code of text (lex), outside every literal and every conditional of
    the preprocessor and the only enumeration of that name in it
    (defined_once), each one's value its place (held_to_places)."""
    lexed = lex(text)
    m = outside_conditionals(lexed.code, re.search(pattern, lexed.blind, re.S), what)
    if not m:
        raise GenerationError("%s could not be read" % what)
    defined_once(lexed.blind, r"\benum\s+(?:class\s+|struct\s+)?" + name + r"\b[^;{]*\{", what)
    return held_to_places(enumerators(lexed.code[m.start(1):m.end(1)], what,
                                      lexed.code.count("\n", 0, m.start(1)) + 1), what)


def route_kinds(header):
    """The route kinds of RestApiServer.h's enum RouteKind, in order, each one's
    value its place."""
    return [name for name, _ in enumeration(header, "RouteKind", r"enum RouteKind\s*\{(.*?)\n      \};",
                                            "%s's enum RouteKind" % REST_H)]


def case_set(body):
    """The route kinds of the case labels in body, outside every comment and
    literal."""
    return set(re.findall(LABEL, blind_of(body)))


def case_matches(body, answer):
    """{kind: the match of answer} for a switch whose groups end in a line
    matching answer."""
    mapping = {}
    pending = []
    for line in one_statement_a_line(code_of(body)).split("\n"):
        label = re.match(LABEL_LINE, line)
        if label:
            pending.append(label.group(1))
            continue
        m = re.match(answer, line)
        if m and pending:
            for kind in pending:
                mapping[kind] = m
            pending = []
    return mapping


def case_groups(body, answer):
    """{kind: answer-group} for a switch whose groups end in a line matching answer."""
    return {kind: m.group(1) for kind, m in case_matches(body, answer).items()}


def builtin_permissions(role_cpp, function):
    """The permissions a built-in role's function sets, each on a line of its
    own in the shape ROLE_PERMISSION_LINE has, read from the code with its
    comments taken out; None when the function is not there. Every other step
    of the function is held in STEPS (steps_of)."""
    body = body_of(role_cpp, function)
    if body is None:
        return None
    lines = one_statement_a_line(code_of(body)).split("\n")
    return {m.group(1) for m in (re.match(ROLE_PERMISSION_LINE, line) for line in lines) if m}


def string_list(body, what, first_line):
    """The strings of a brace list's body, code as lex() gives it whose first
    line is the line first_line of its file: each element a string literal
    (adjacent ones one, as in C++), a comma between them. Any other element
    stops the script, and so does a directive of the preprocessor (tokenize)."""
    tokens = tokenize(body, first_line, what)
    strings = []
    i = 0
    while tokens[i].kind != "eof":
        if tokens[i].kind != "str":
            raise GenerationError("%s: %r is not a string literal, which is all this script reads there"
                                  % (what, tokens[i].text))
        strings.append(tokens[i].text)
        i += 1
        if tokens[i].kind == "op" and tokens[i].text == ",":
            i += 1
        elif tokens[i].kind != "eof":
            raise GenerationError("%s: %r follows a string, where this script reads a comma or the end"
                                  % (what, tokens[i].text))
    return strings


def permission_tokens(role_cpp):
    """The tokens of RoleManager.cpp's PermissionTokens, in order: the token of
    each permission at its place."""
    what = "%s's PermissionTokens" % ROLE_CPP
    lexed = lex(role_cpp)
    m = outside_conditionals(lexed.code, re.search(r"PermissionTokens\[\]\s*=\s*\{(.*?)\};", lexed.blind, re.S), what)
    if not m:
        raise GenerationError("%s could not be read" % what)
    defined_once(lexed.blind, r"\bPermissionTokens\s*\[[^\]]*\]\s*(?:=(?!=)|\{)", what)
    return string_list(lexed.code[m.start(1):m.end(1)], what, lexed.code.count("\n", 0, m.start(1)) + 1)


def permission_enum(role_h):
    """The permissions of RoleManager.h's enum RolePermission, in order, Count
    left out. Each one's value, as the compiler gives it, must be its place in
    the enumeration (held_to_places) - the bit Bit sets it at and RoleGrant::Holds
    asks, and the place of its token in PermissionTokens - so that a value
    written for one, which would give it another's bit, stops the script; and
    Count, the last, is the number of permissions."""
    what = "Common/Application/RoleManager.h's enum RolePermission"
    items = enumeration(role_h, "RolePermission", r"enum class RolePermission\s*\{(.*?)\};", what)
    if not items or items[-1][0] != "Count":
        raise GenerationError("%s does not end in Count, which this script reads as the number of permissions" % what)
    return [name for name, _ in items[:-1]]


# The report's sections, RestApiReports.cpp's table: FindSection finds a section
# by its name in it, and IsServerWideReportSection_ answers with its second field.
REPORT_STRUCT = re.compile(r"struct SectionInfo\s*\{\s*const char \*name;\s*bool server_wide;")
REPORT_TABLE = "const SectionInfo SECTIONS[] ="
REPORT_ROW = re.compile(r'\{\s*"(\w+)"\s*,\s*(true|false)\s*,')
REPORT_COUNT = re.compile(r"const int SectionCount = \(int\) \(sizeof\(SECTIONS\) / sizeof\(SECTIONS\[0\]\)\);")


def report_sections(reports):
    """[(section, whether it is counted from server-wide metrics)], in the
    table's order. A row is read as SectionInfo's first two members, which must
    be the name and server_wide; FindSection searches SectionCount rows of it,
    which must be the whole table. What is read is the first SectionInfo, the
    table and SectionCount after it and the FindSection after those, and only
    there: that stretch must stand outside every conditional of the
    preprocessor and hold no directive of it, since this script does not
    evaluate them as the compiler does. Each is searched for outside every
    literal (lex)."""
    lexed = lex(reports)
    code, blind = lexed.code, lexed.blind
    struct = REPORT_STRUCT.search(blind)
    if not struct:
        raise GenerationError("%s: the report's SectionInfo does not begin with the name and server_wide, which a "
                              "row of its table is read as" % REPORTS_CPP)
    if in_conditional(code, struct.start()):
        raise GenerationError("%s: the report's SectionInfo stands inside a conditional of the preprocessor (#if to "
                              "#endif), which this script does not evaluate: the table it would read may not be the "
                              "one compiled" % REPORTS_CPP)
    find = function_span(code, "FindSection")
    if find is None or find[0] < struct.end():
        raise GenerationError("%s: FindSection could not be found after the report's SectionInfo" % REPORTS_CPP)
    head, _, end, _ = find
    first, last = code.count("\n", 0, struct.start()), code.count("\n", 0, end)
    for line, _, directive in lexed.directives:
        if first <= line <= last:
            raise GenerationError("%s: a preprocessor line between the report's SectionInfo and the end of "
                                  "FindSection, which this script does not read as the compiler does: %s"
                                  % (REPORTS_CPP, directive))
    for once, what in ((r"\bstruct\s+SectionInfo\b[^;{]*\{", "the report's SectionInfo"),
                       (r"\bSECTIONS\s*\[[^\]]*\]\s*(?:=(?!=)|\{)", "the report's table of sections"),
                       (r"\bSectionCount\s*(?:=(?!=)|[{(])", "the report's SectionCount")):
        defined_once(blind, once, "%s: %s" % (REPORTS_CPP, what))
    at = blind.find(REPORT_TABLE, struct.end(), head)
    table = braced(code, at + len(REPORT_TABLE)) if at >= 0 else None
    if table is None or table[1] > head:
        raise GenerationError("%s: the report's table of sections could not be found between SectionInfo and "
                              "FindSection" % REPORTS_CPP)
    inside = code[:table[1] - 1]
    sections = []
    row = braced(inside, table[0] + 1)
    while row is not None:
        m = REPORT_ROW.match(inside, row[0], row[1])
        if not m:
            raise GenerationError("%s: a row of the report's table of sections could not be read: %s"
                                  % (REPORTS_CPP, " ".join(inside[row[0]:row[1]].split())[:80]))
        sections.append((m.group(1), m.group(2) == "true"))
        row = braced(inside, row[1])
    if not sections:
        raise GenerationError("%s: the report's table of sections has no row" % REPORTS_CPP)
    if not REPORT_COUNT.search(blind, table[1], head):
        raise GenerationError("%s: FindSection searches SectionCount rows of the report's table, and SectionCount "
                              "is not written as the table's length between the table and FindSection"
                              % REPORTS_CPP)
    return sections


class Decision(object):
    """What Authorize_ decides, read out of RestApiServer.cpp."""

    def __init__(self, sources):
        cpp = read(REST_CPP, sources)
        scim = read(SUB_PARSERS["ParseScimRoute_"], sources)
        role_cpp = read(ROLE_CPP, sources)
        role_h = read("Common/Application/RoleManager.h", sources)
        self.kinds = route_kinds(read(REST_H, sources))
        if not self.kinds:
            raise GenerationError("%s: the RouteKind enumeration could not be read" % REST_H)
        self.session = case_set(body_of(cpp, "RestApiServer::IsSessionRoute_"))
        self.self_service = case_set(body_of(cpp, "RestApiServer::IsSelfServiceRoute_"))
        self.scim = case_set(body_of(scim, "RestApiServer::IsScimRoute_"))
        self.api_key = case_set(body_of(cpp, "RestApiServer::IsApiKeyRoute_"))
        self.mutating = case_set(body_of(cpp, "RestApiServer::IsMutatingRoute_"))
        self.takes_in = case_set(body_of(cpp, "RestApiServer::ChangesAccountByAddress_"))
        self.permission = case_groups(body_of(cpp, "RestApiServer::PermissionFor_"), PERMISSION_LINE)
        self.second = case_groups(body_of(cpp, "RestApiServer::SecondPermissionFor_"), SECOND_PERMISSION_LINE)
        default = re.search(r"default:\s*break;\s*\}\s*return RolePermission::(\w+);",
                            blind_of(body_of(cpp, "RestApiServer::PermissionFor_")))
        self.default_permission = default.group(1) if default else None
        named = case_groups(body_of(cpp, "RestApiServer::DomainNamedByRoute_"), DOMAIN_LINE)
        self.domain_named = {}
        for kind, expr in named.items():
            self.domain_named[kind] = DOMAIN_EXPRESSIONS.get(re.sub(r"\s+", "", expr), expr)
        authorize = code_of(body_of(cpp, "RestApiServer::Authorize_"))
        self.authorize_text = authorize
        # The role-holding account's exceptions inside the API-key block
        span = key_route_block(authorize)
        self.role_key_routes = case_set(authorize[span[0]:span[1]]) if span else set()
        # The refusals a credential confined to named domains meets, each read
        # where it is code, outside every literal
        at = code_find(authorize, CONFINEMENT)
        if at < 0:
            raise GenerationError("%s: Authorize_'s confinement section could not be found" % REST_CPP)
        confined = authorize[at + len(CONFINEMENT):]
        self.confined_refused = set()
        for block in code_matches(CONFINED_REFUSAL, confined):
            self.confined_refused |= set(re.findall(r"Route\w+", block.group(1)))
        sw = code_matches(SERVER_WIDE, confined)
        if not sw:
            raise GenerationError("%s: Authorize_'s list of server-wide routes could not be found" % REST_CPP)
        self.confined_refused |= case_set(sw[0].group(1))
        self.report_kind = "RouteReport" if code_find(confined, "route.kind == RouteReport") >= 0 else None
        # The sections of it that block refuses: IsServerWideReportSection_'s,
        # from the table FindSection searches.
        self.server_wide_sections = [name for name, wide in report_sections(read(REPORTS_CPP, sources)) if wide]
        if self.report_kind in self.confined_refused:
            # Its own block stands between the refusals, so which of the two
            # answers depends on where the refusal is - an order decide() does
            # not read.
            raise GenerationError("%s: RouteReport is in a refusal of a confined credential as well as in its own "
                                  "block in Authorize_; read decide() against the two" % REST_CPP)
        # Dispatch: kind -> handler call text, from the switch in ProcessRequest_'s
        # try block, where every request has a credential
        part, span = authenticated_part(code_of(body_of(cpp, "RestApiServer::ProcessRequest_")))
        if span is None:
            raise GenerationError("%s: ProcessRequest_'s dispatch could not be found" % REST_CPP)
        self.handler = {}
        self.handler_args = {}
        for kind, m in case_matches(part[span[0]:span[1]], HANDLER_LINE).items():
            self.handler[kind] = m.group(1)
            self.handler_args[kind] = m.group(2)
        # The built-in roles' permissions: the lines of each one's function,
        # and every permission there is for serveradmin, whose function sets
        # every bit below RolePermission::Count.
        enum = permission_enum(role_h)
        roles = {}
        for name, function, listed in BUILTIN_ROLES:
            held = builtin_permissions(role_cpp, function)
            if held is None or (listed and not held):
                raise GenerationError("%s: the built-in role %s could not be read" % (ROLE_CPP, name))
            roles[name] = held if listed else set(enum)
        self.roles = roles
        tokens = permission_tokens(role_cpp)
        if len(tokens) != len(enum):
            raise GenerationError("%s: PermissionTokens holds %d tokens and RolePermission %d permissions"
                                  % (ROLE_CPP, len(tokens), len(enum)))
        self.tokens = dict(zip(enum, tokens))
        for label, value in (("IsSessionRoute_", self.session), ("IsSelfServiceRoute_", self.self_service),
                             ("IsScimRoute_", self.scim), ("IsApiKeyRoute_", self.api_key),
                             ("PermissionFor_", self.permission), ("DomainNamedByRoute_", self.domain_named),
                             ("the dispatch", self.handler), ("the role-holder's key-route exceptions", self.role_key_routes)):
            if not value:
                raise GenerationError("%s: %s could not be read" % (REST_CPP, label))
        # What decide() takes as given - the order of Authorize_'s steps and their
        # conditions, what lies between the credential and the handler - held to
        # the code, so that a change of a step stops the script rather than
        # leaving the matrix as it was.
        check_steps(sources)

    def token(self, permission):
        return self.tokens.get(permission, permission)

    def decide(self, kind, who):
        """The cell for one principal. who: dict(result, role, domains)."""
        result = who["result"]
        if kind in self.session:
            if kind == "RouteSessionGet":
                return "yes"
            if kind in ("RouteSendList", "RouteSendGet"):
                return "yes" if result == "key" else "no"
            return "yes" if result in ("account", "admin") else "no"
        if kind == "RouteSendCreate" and result != "key":
            return "no"
        if kind in self.scim and result != "key":
            return "401"
        if kind in self.self_service:
            return "yes" if result == "account" else "no"
        role = who["role"]
        if result == "account" and role is None:
            return "no"
        if result == "admin":
            return "yes"
        if kind in self.api_key:
            if result != "account":
                return "401"
            if kind not in self.role_key_routes:
                return "no"
        held = self.roles[role]
        # RoleManager::Allows, asked of each permission the route needs.
        # AdministerServer is every permission only for a grant with no list of
        # domains. Held with a list - a Scope=full key issued for named domains,
        # serveradmin assigned with one - it is judged as every confined grant
        # is: it must hold the permission, it is refused AdministerServer
        # itself, and where the route names a domain its list must reach that
        # domain, which is the "own" of the cells below. Which sentence that
        # last refusal gives - it is not the same for a grant holding
        # AdministerServer - is in no cell.
        full = "AdministerServer" in held and who["domains"] == "all"
        first = self.permission.get(kind, self.default_permission)
        needed = [first] + ([self.second[kind]] if kind in self.second else [])
        if not full and (not all(p in held for p in needed) or "AdministerServer" in needed):
            return "no"
        cell = "yes"
        if kind == "RouteSendCreate":
            cell = "sender"
        if who["domains"] == "all":
            return cell
        if kind in self.confined_refused:
            return "no"
        if kind == self.report_kind:
            return "own+"
        if kind in self.domain_named:
            return "own"
        return "own*" if cell == "yes" else cell


# ---------------------------------------------------------------------------
# The steps decide() takes as given
# ---------------------------------------------------------------------------
#
# decide() reads its lists out of the code but writes the steps between them
# itself: which test comes first, what each one asks, what follows a refusal.
# So the functions it models are held to STEPS, the steps as they were when
# decide() was last read against them: Authorize_, the functions whose lists it
# reads, the built-in roles' functions, the Bit they set a permission with and
# the one that picks them by a role's name, the functions it hands its decision
# to and those they ask (steps_of
# names each), and ProcessRequest_ from the end of the anonymous routes to the
# handler. A list may change unseen - the readers above take it - and so may
# prose, and nothing else of those functions may: a step added, taken out,
# moved or given a condition, or a name a step compares or is kept under
# changed, stops the script with the difference, until decide() has been read
# against the new steps and STEPS updated with them. Not held: Authenticate_,
# which makes a request one of PRINCIPALS - the grant RoleManager gives it, of
# a role and for its domains - the string helpers, and the functions that
# ProcessRequest_'s other checks call.

# The functions read with case_set and case_groups, and what their answers are
# masked to.
STEP_CASE_SETS = [
    ("RestApiServer::IsSessionRoute_", REST_CPP),
    ("RestApiServer::IsSelfServiceRoute_", REST_CPP),
    ("RestApiServer::IsScimRoute_", SUB_PARSERS["ParseScimRoute_"]),
    ("RestApiServer::IsApiKeyRoute_", REST_CPP),
    ("RestApiServer::IsMutatingRoute_", REST_CPP),
    ("RestApiServer::ChangesAccountByAddress_", REST_CPP),
]
STEP_CASE_GROUPS = [
    ("RestApiServer::PermissionFor_", PERMISSION_LINE, lambda m: "return RolePermission::PERMISSION;"),
    ("RestApiServer::SecondPermissionFor_", SECOND_PERMISSION_LINE, lambda m: "permission = RolePermission::PERMISSION;"),
    # One of the expressions the reader knows is a domain; any other is a step.
    ("RestApiServer::DomainNamedByRoute_", DOMAIN_LINE,
     lambda m: "return DOMAIN;" if re.sub(r"\s+", "", m.group(1)) in DOMAIN_EXPRESSIONS else None),
]


def mask_labels(code):
    """code with each case label case_set reads, outside every literal, made
    "case ROUTE:"."""
    out = []
    pos = 0
    for m in re.finditer(LABEL, blind_of(code)):
        out.append(code[pos:m.start()])
        out.append("case ROUTE:")
        pos = m.end()
    out.append(code[pos:])
    return "".join(out)


def mask_lines(code, answer, masked):
    """code, one statement a line, with each line case_matches would take as a
    label or as an answer masked."""
    out = []
    for line in one_statement_a_line(code).split("\n"):
        if re.match(LABEL_LINE, line):
            line = "case ROUTE:"
        else:
            m = re.match(answer, line)
            if m and masked(m) is not None:
                line = masked(m)
        out.append(line)
    return "\n".join(out)


def last_statement(tokens):
    """The first token of the last statement of tokens, or None when they do not
    end in one."""
    if not tokens or tokens[-1] != ";":
        return None
    k = len(tokens) - 2
    depth = 0
    while k >= 0:
        t = tokens[k]
        if t in (")", "]", "}"):
            depth += 1
        elif t in ("(", "[", "{"):
            if depth == 0:
                break
            depth -= 1
        elif depth == 0 and t in (";", ":"):
            break
        k -= 1
    return tokens[k + 1]


# A string literal that is one word - a name, a key, a header, a method, a role -
# rather than prose.
NAME_LITERAL = re.compile(r'(?:u8|[uUL])?"[A-Za-z0-9][A-Za-z0-9_.-]*"\Z')
# What wraps a literal that is compared as itself.
LITERAL_WRAPPERS = ("_T", "String", "AnsiString")


def kept_literals(significant):
    """The indexes in significant - (kind, text) of every token but whitespace
    and comments - of the string and character literals that are part of a step:
    those a condition holds, of an if, a while, a for or a switch; those compared
    wherever they stand, an operand of == or != as they are or as the one argument
    of _T, String or AnsiString; and every string literal that is one word."""
    texts = [text for _, text in significant]
    kept = set()
    conditions = []
    for k, (kind, text) in enumerate(significant):
        if kind in ("str", "chr"):
            before, after = k - 1, k + 1
            if k > 1 and texts[k - 1] == "(" and texts[k - 2] in LITERAL_WRAPPERS and after < len(texts) \
                    and texts[after] == ")":
                before, after = k - 3, k + 2
            compared = (before >= 0 and texts[before] in ("==", "!=")) \
                or (after < len(texts) and texts[after] in ("==", "!="))
            if (conditions and conditions[-1]) or compared or (kind == "str" and NAME_LITERAL.match(text)):
                kept.add(k)
        elif text == "(":
            conditions.append(bool(k and texts[k - 1] in ("if", "while", "for", "switch"))
                              or bool(conditions and conditions[-1]))
        elif text == ")" and conditions:
            conditions.pop()
    return kept


def skeleton(code):
    """A function's code as its steps, one statement a line: its tokens with the
    whitespace and the comments gone, and every string literal "" (adjacent ones
    one, as in C++) but those kept_literals keeps, which are part of the step:
    what a condition holds, what is compared, and a name - the header a check
    compares, the role a choice compares, the key a throttle is kept under. Prose
    is masked, so that a refusal's reason, a message or a response's body may be
    reworded. A run of masked labels is one label, a run of a built-in role's
    masked permission lines one line, and two masked switch groups or two masked
    refusals that are alike, side by side and end in a jump are one, so that a
    route given a group or a refusal of its own, and a permission given to a
    role or taken from it, is a list edit too."""
    tokens = []
    significant = [(m.lastgroup, m.group(0)) for m in tokens_of(code) if m.lastgroup not in ("ws", "comment")]
    kept = kept_literals(significant)
    for k, (kind, text) in enumerate(significant):
        if kind in ("str", "chr") and k in kept:
            tokens.append(text)
        elif kind == "str":
            if not (tokens and tokens[-1] == '""'):
                tokens.append('""')
        elif kind == "chr":
            tokens.append("''")
        else:
            tokens.append(text)
    label = ["case", "ROUTE", ":"]
    permission = ["set", "|=", "Bit", "(", "RolePermission", "::", "PERMISSION", ")", ";"]
    refusal = ["if", "(", "route", ".", "kind", "==", "ROUTES", ")", "{"]
    runs = []
    i = 0
    while i < len(tokens):
        for unit in (label, permission):
            if tokens[i:i + len(unit)] == unit and runs[-len(unit):] == unit:
                i += len(unit)
                break
        else:
            runs.append(tokens[i])
            i += 1
    tokens = runs

    def unit_end(i):
        if tokens[i:i + 3] == label:
            depth = 0
            j = i + 3
            while j < len(tokens):
                t = tokens[j]
                if t in ("(", "[", "{"):
                    depth += 1
                elif t in (")", "]", "}"):
                    if depth == 0:
                        break
                    depth -= 1
                elif depth == 0 and t in ("case", "default"):
                    break
                j += 1
            return j if last_statement(tokens[i + 3:j]) in ("return", "break") else None
        if tokens[i:i + len(refusal)] == refusal:
            depth = 0
            j = i + len(refusal) - 1
            while j < len(tokens):
                if tokens[j] == "{":
                    depth += 1
                elif tokens[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if j >= len(tokens) or tokens[j + 1:j + 2] == ["else"]:
                return None
            return j + 1 if last_statement(tokens[i + len(refusal):j]) == "return" else None
        return None

    kept = []
    previous = None
    i = 0
    while i < len(tokens):
        end = unit_end(i)
        if end is None:
            kept.append(tokens[i])
            previous = None
            i += 1
            continue
        unit = tokens[i:end]
        if unit != previous:
            kept.extend(unit)
        previous = unit
        i = end
    lines = []
    line = []
    state = {"depth": 0, "paren": 0, "label": False}

    def end_line():
        if line:
            text = " ".join(line)
            if text == ";" and lines:
                lines[-1] += " ;"
            else:
                lines.append("  " * state["depth"] + text)
            del line[:]
    for t in kept:
        if t == "}":
            end_line()
            state["depth"] -= 1
            line.append(t)
            end_line()
            continue
        if t in ("case", "default") and state["paren"] == 0:
            end_line()
            state["label"] = True
        line.append(t)
        if t == "(":
            state["paren"] += 1
        elif t == ")":
            state["paren"] -= 1
        elif t == "{":
            end_line()
            state["depth"] += 1
        elif t == ";" and state["paren"] == 0:
            end_line()
        elif t == ":" and state["label"] and state["paren"] == 0:
            state["label"] = False
            end_line()
    end_line()
    return "\n".join(lines)


def steps_of(sources=None):
    """[(function, its steps)] for every function decide() models, each masked
    where the readers above take something out of it: Authorize_ (the
    role-holder's exceptions, the routes of each refusal of a confined
    credential and the server-wide switch's labels); the functions read with
    case_set and case_groups; the decisions Authorize_ hands a permission, a
    domain or a report section to and the functions those ask -
    RoleManager::Allows, with the domain, and the RoleGrant::Holds and
    RoleManager::IsDomainReached_ it asks, IsDomainAllowed_, and
    IsServerWideReportSection_ and the FindSection it asks, whose table
    report_sections reads; RoleManager::BuiltInPermissions_, which picks a
    built-in role's function by the role's name, the four functions (their
    permission lines masked, but for ServerAdminPermissions, whose every step is
    one) and the Bit they set a permission with; and ProcessRequest_'s try
    block, everything between the end of the anonymous routes and the handler,
    the dispatch's labels and handlers masked."""

    def code(rel, qualified, params=None):
        body = body_of(read(rel, sources), qualified, params)
        if body is None:
            raise GenerationError("%s: %s could not be read" % (rel, qualified))
        return code_of(body)

    steps = []
    authorize = code(REST_CPP, "RestApiServer::Authorize_")
    # What the readers take, masked where they take it: outside every literal.
    spans = []
    blind = blind_of(authorize)
    key = key_route_block(authorize)
    if key:
        spans += [(m.start(), m.end(), "case ROUTE:") for m in re.compile(LABEL).finditer(blind, key[0], key[1])]
    at = code_find(authorize, CONFINEMENT)
    if at >= 0:
        base = at + len(CONFINEMENT)
        confined = authorize[base:]
        spans += [(base + m.start(1), base + m.end(1), "route.kind == ROUTES")
                  for m in code_matches(CONFINED_REFUSAL, confined)]
        sw = code_matches(SERVER_WIDE, confined)
        if sw:
            spans += [(m.start(), m.end(), "case ROUTE:")
                      for m in re.compile(LABEL).finditer(blind, base + sw[0].start(1), base + sw[0].end(1))]
    for begin, end, new in sorted(spans, reverse=True):
        authorize = authorize[:begin] + new + authorize[end:]
    steps.append(("RestApiServer::Authorize_", skeleton(authorize)))
    for qualified, rel in STEP_CASE_SETS:
        steps.append((qualified, skeleton(mask_labels(code(rel, qualified)))))
    for qualified, answer, masked in STEP_CASE_GROUPS:
        steps.append((qualified, skeleton(mask_lines(code(REST_CPP, qualified), answer, masked))))
    steps.append(("RoleManager::Allows", skeleton(code(ROLE_CPP, "RoleManager::Allows", "const String &domainName"))))
    steps.append(("RoleGrant::Holds", skeleton(code(ROLE_CPP, "RoleGrant::Holds"))))
    steps.append(("RoleManager::IsDomainReached_", skeleton(code(ROLE_CPP, "RoleManager::IsDomainReached_"))))
    steps.append(("RoleManager::BuiltInPermissions_", skeleton(code(ROLE_CPP, "RoleManager::BuiltInPermissions_"))))
    for _, function, listed in BUILTIN_ROLES:
        body = code(ROLE_CPP, function)
        if listed:
            body = mask_lines(body, ROLE_PERMISSION_LINE, lambda m: "set |= Bit(RolePermission::PERMISSION);")
        steps.append((function, skeleton(body)))
    steps.append(("Bit", skeleton(code(ROLE_CPP, "Bit"))))
    steps.append(("RestApiServer::IsDomainAllowed_", skeleton(code(REST_CPP, "RestApiServer::IsDomainAllowed_"))))
    steps.append(("RestApiServer::IsServerWideReportSection_",
                  skeleton(code(REPORTS_CPP, "RestApiServer::IsServerWideReportSection_"))))
    steps.append(("FindSection", skeleton(code(REPORTS_CPP, "FindSection"))))
    part, span = authenticated_part(code(REST_CPP, "RestApiServer::ProcessRequest_"))
    if span is None:
        raise GenerationError("%s: ProcessRequest_'s dispatch could not be found" % REST_CPP)
    part = part[:span[0]] + mask_lines(part[span[0]:span[1]], HANDLER_LINE, lambda m: "return HANDLER();") + part[span[1]:]
    steps.append(("RestApiServer::ProcessRequest_", skeleton(part)))
    return steps


def steps_literal(steps):
    """STEPS as this script carries it, for steps."""
    out = ["STEPS = {"]
    for name, text in steps:
        out.append('    "%s": r"""' % name)
        out.append(text)
        out.append('""",')
    out.append("}")
    return "\n".join(out) + "\n"


def check_steps(sources=None):
    now = steps_of(sources)
    if [name for name, _ in now] != list(STEPS):
        raise GenerationError("STEPS names %s, and the steps read are of %s"
                              % (", ".join(STEPS), ", ".join(name for name, _ in now)))
    for name, text in now:
        carried = STEPS[name].strip("\n")
        if text != carried:
            diff = list(difflib.unified_diff(carried.split("\n"), text.split("\n"), "STEPS", "the code", n=1, lineterm=""))
            raise GenerationError("%s's steps have changed: read decide() against them, then update its entry in STEPS "
                                  "(python build/generate-auth-matrix.py --steps prints them as the code has them "
                                  "now):\n%s" % (name, "\n".join(diff[:60])))


# The steps, as decide() was last read against them: what steps_of reads,
# printed by --steps.
STEPS = {
    "RestApiServer::Authorize_": r"""
{
  refusalReason = "" ;
  if ( IsSessionRoute_ ( route . kind ) ) {
    if ( route . kind == RouteSessionGet ) {
      return AuthorizationAllowed ;
    }
    if ( route . kind == RouteSendList || route . kind == RouteSendGet ) {
      if ( caller . result == AuthenticatedWithApiKey ) return AuthorizationAllowed ;
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
    if ( caller . result == AuthenticatedAsAccount || caller . result == AuthenticatedAsAdministrator ) return AuthorizationAllowed ;
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  if ( route . kind == RouteSendCreate && caller . result != AuthenticatedWithApiKey ) {
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  if ( IsScimRoute_ ( route . kind ) && caller . result != AuthenticatedWithApiKey ) return AuthorizationUnauthenticated ;
  if ( IsSelfServiceRoute_ ( route . kind ) ) {
    if ( caller . result == AuthenticatedAsAccount ) return AuthorizationAllowed ;
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  if ( caller . result == AuthenticatedAsAccount && ! caller . grant . administrative ) {
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  if ( caller . result == AuthenticatedAsAdministrator ) return AuthorizationAllowed ;
  if ( IsApiKeyRoute_ ( route . kind ) ) {
    if ( caller . result != AuthenticatedAsAccount ) return AuthorizationUnauthenticated ;
    switch ( route . kind ) {
      case ROUTE :
      break ;
      default :
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
  }
  {
    String roleRefusal ;
    const String routeDomain = DomainNamedByRoute_ ( route ) ;
    RolePermission secondPermission = RolePermission :: Count ;
    const bool permitted = RoleManager :: Allows ( caller . grant , PermissionFor_ ( route . kind ) , routeDomain , roleRefusal ) && ( ! SecondPermissionFor_ ( route . kind , secondPermission ) || RoleManager :: Allows ( caller . grant , secondPermission , routeDomain , roleRefusal ) ) ;
    if ( ! permitted ) {
      refusalReason = ( caller . result == AuthenticatedWithApiKey && caller . read_only && caller . grant . name == _T ( "auditor" ) && IsMutatingRoute_ ( route . kind ) ) ? AnsiString ( "" ) : AnsiString ( roleRefusal ) ;
      return AuthorizationForbidden ;
    }
  }
  if ( route . kind == RouteSendCreate ) {
    if ( caller . send_from . IsEmpty ( ) ) {
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
    if ( ! IsDomainAllowed_ ( caller . domains , StringParser :: ExtractDomain ( caller . send_from ) ) ) {
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
  }
  if ( caller . domains . empty ( ) ) return AuthorizationAllowed ;
  if ( route . kind == ROUTES ) {
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  if ( route . kind == RouteReport ) {
    if ( IsServerWideReportSection_ ( route . name ) ) {
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
    if ( ! route . identifier . IsEmpty ( ) && ! IsDomainAllowed_ ( caller . domains , String ( route . identifier ) ) ) {
      refusalReason = "" ;
      return AuthorizationForbidden ;
    }
    return AuthorizationAllowed ;
  }
  if ( route . kind == ROUTES ) {
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  switch ( route . kind ) {
    case ROUTE :
    refusalReason = "" ;
    return AuthorizationForbidden ;
    default :
    break ;
  }
  const String targetDomain = DomainNamedByRoute_ ( route ) ;
  if ( targetDomain . IsEmpty ( ) ) return AuthorizationAllowed ;
  if ( ! IsDomainAllowed_ ( caller . domains , targetDomain ) ) {
    refusalReason = "" ;
    return AuthorizationForbidden ;
  }
  return AuthorizationAllowed ;
}
""",
    "RestApiServer::IsSessionRoute_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::IsSelfServiceRoute_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::IsScimRoute_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::IsApiKeyRoute_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::IsMutatingRoute_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::ChangesAccountByAddress_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return true ;
    default :
    return false ;
  }
}
""",
    "RestApiServer::PermissionFor_": r"""
{
  switch ( kind ) {
    case ROUTE :
    return RolePermission :: PERMISSION ;
    default :
    break ;
  }
  return RolePermission :: PERMISSION ;
}
""",
    "RestApiServer::SecondPermissionFor_": r"""
{
  switch ( kind ) {
    case ROUTE :
    permission = RolePermission :: PERMISSION ;
    return true ;
    case ROUTE :
    default :
    break ;
  }
  return false ;
}
""",
    "RestApiServer::DomainNamedByRoute_": r"""
{
  switch ( route . kind ) {
    case ROUTE :
    return DOMAIN ;
    default :
    break ;
  }
  return String ( ) ;
}
""",
    "RoleManager::Allows": r"""
{
  reason = _T ( "" ) ;
  if ( ! grant . administrative ) {
    reason = _T ( "" ) ;
    return false ;
  }
  const bool full = grant . Holds ( RolePermission :: AdministerServer ) && grant . domains . empty ( ) ;
  if ( full ) return true ;
  if ( ! grant . Holds ( permission ) ) {
    reason . Format ( _T ( "" ) , grant . name . c_str ( ) , String ( PermissionName ( permission ) ) . c_str ( ) ) ;
    return false ;
  }
  if ( permission == RolePermission :: AdministerServer ) {
    reason . Format ( _T ( "" ) , grant . name . c_str ( ) ) ;
    return false ;
  }
  if ( domainName . IsEmpty ( ) ) return true ;
  if ( ! IsDomainReached_ ( grant , Lower ( domainName ) ) ) {
    if ( grant . Holds ( RolePermission :: AdministerServer ) ) reason = _T ( "" ) ;
    else reason . Format ( _T ( "" ) , grant . name . c_str ( ) ) ;
    return false ;
  }
  return true ;
}
""",
    "RoleGrant::Holds": r"""
{
  return ( permissions & ( 1ULL << static_cast < int > ( permission ) ) ) != 0 ;
}
""",
    "RoleManager::IsDomainReached_": r"""
{
  if ( grant . domains . empty ( ) ) return true ;
  if ( domainName . IsEmpty ( ) ) return false ;
  for ( const String & allowed : grant . domains ) {
    if ( allowed . CompareNoCase ( domainName . c_str ( ) ) == 0 ) return true ;
  }
  return false ;
}
""",
    "RoleManager::BuiltInPermissions_": r"""
{
  found = true ;
  const String name = Lower ( roleName ) ;
  if ( name . Compare ( _T ( "serveradmin" ) ) == 0 ) return ServerAdminPermissions ( ) ;
  if ( name . Compare ( _T ( "domainadmin" ) ) == 0 ) return DomainAdminPermissions ( ) ;
  if ( name . Compare ( _T ( "helpdesk" ) ) == 0 ) return HelpdeskPermissions ( ) ;
  if ( name . Compare ( _T ( "auditor" ) ) == 0 ) return AuditorPermissions ( ) ;
  found = false ;
  return 0 ;
}
""",
    "ServerAdminPermissions": r"""
{
  unsigned __int64 set = 0 ;
  for ( int index = 0 ; index < static_cast < int > ( RolePermission :: Count ) ; index ++ ) set |= 1ULL << index ;
  return set ;
}
""",
    "DomainAdminPermissions": r"""
{
  unsigned __int64 set = 0 ;
  set |= Bit ( RolePermission :: PERMISSION ) ;
  return set ;
}
""",
    "HelpdeskPermissions": r"""
{
  unsigned __int64 set = 0 ;
  set |= Bit ( RolePermission :: PERMISSION ) ;
  return set ;
}
""",
    "AuditorPermissions": r"""
{
  unsigned __int64 set = 0 ;
  set |= Bit ( RolePermission :: PERMISSION ) ;
  return set ;
}
""",
    "Bit": r"""
{
  return 1ULL << static_cast < int > ( permission ) ;
}
""",
    "RestApiServer::IsDomainAllowed_": r"""
{
  if ( domains . empty ( ) ) return true ;
  if ( domainName . IsEmpty ( ) ) return false ;
  for ( const String & allowed : domains ) {
    if ( allowed . CompareNoCase ( domainName . c_str ( ) ) == 0 ) return true ;
  }
  return false ;
}
""",
    "RestApiServer::IsServerWideReportSection_": r"""
{
  const SectionInfo * info = FindSection ( section ) ;
  return info != 0 && info -> server_wide ;
}
""",
    "FindSection": r"""
{
  for ( int i = 0 ; i < SectionCount ; i ++ ) {
    if ( name . CompareNoCase ( SECTIONS [ i ] . name ) == 0 ) return & SECTIONS [ i ] ;
  }
  return 0 ;
}
""",
    "RestApiServer::ProcessRequest_": r"""
try {
  const bool adminBasic = BasicUserName_ ( request ) == "administrator" ;
  if ( adminBasic ) {
    const int waitMs = SessionThrottleRetryMs_ ( "administrator" , peer_address ) ;
    if ( waitMs > 0 ) {
      const int retryAfter = ( waitMs + 999 ) / 1000 ;
      AnsiString headers ;
      headers . Format ( "" , retryAfter < 1 ? 1 : retryAfter ) ;
      return BuildResponse_ ( 429 , "" , headers ) ;
    }
  }
  Caller caller = Authenticate_ ( request , peer_address ) ;
  if ( adminBasic ) {
    if ( caller . result == AuthenticatedAsAdministrator ) ClearSessionFailure_ ( "administrator" , peer_address ) ;
    else if ( caller . result == AuthenticationFailed && ! caller . second_factor_required ) RegisterSessionFailure_ ( "administrator" , peer_address ) ;
  }
  if ( caller . via_session && method != "GET" && method != "HEAD" && GetHeader_ ( request , "x-requested-with" ) != "hMailServer" ) {
    if ( scimPath ) return BuildScimError_ ( 403 , "" , _T ( "" ) ) ;
    return BuildResponse_ ( 403 , "" ) ;
  }
  if ( caller . result == AuthenticationFailed && caller . mailbox_key_unknown ) return BuildResponse_ ( 503 , "" ) ;
  if ( caller . result == AuthenticationFailed && scimPath ) return BuildScimError_ ( 401 , "" , _T ( "" ) , "" ) ;
  if ( caller . result == AuthenticationFailed && caller . second_factor_locked_for > 0 ) return TooManyWrongCodes_ ( caller . second_factor_locked_for ) ;
  if ( caller . result == AuthenticationFailed ) return BuildUnauthorizedResponse_ ( caller . second_factor_required , IsPageScriptRequest_ ( request ) ) ;
  if ( RequestIsCrossSite_ ( request , true ) ) {
    if ( scimPath ) return BuildScimError_ ( 403 , "" , _T ( "" ) ) ;
    return BuildResponse_ ( 403 , "" ) ;
  }
  RoleActor actor ;
  actor . identity = caller . identity ;
  actor . account_id = caller . account ? caller . account -> GetID ( ) : 0 ;
  actor . domain_id = caller . account ? caller . account -> GetDomainID ( ) : 0 ;
  actor . grant = caller . grant ;
  RoleManager :: ActorScope actorScope ( actor ) ;
  bool firstRefusal = false ;
  if ( ! IsWithinRequestRate_ ( caller . identity , firstRefusal ) ) {
    if ( firstRefusal ) {
      String message ;
      message . Format ( _T ( "" ) , String ( caller . identity ) . c_str ( ) , MaxRequestsPerWindowPerCredential , ( int ) ( RateWindowMilliseconds / 1000 ) ) ;
      LOG_APPLICATION ( message ) ;
    }
    if ( scimPath ) {
      AnsiString retryAfter ;
      retryAfter . Format ( "" , ( int ) ( RateWindowMilliseconds / 1000 ) ) ;
      return BuildScimError_ ( 429 , "" , _T ( "" ) , retryAfter ) ;
    }
    return BuildTooManyRequestsResponse_ ( ) ;
  }
  Route route ;
  route . query = query ;
  ParseRoute_ ( method , path , route ) ;
  route . identifier = DecodePathSegment_ ( route . identifier ) ;
  route . name = DecodePathSegment_ ( route . name ) ;
  AnsiString refusalReason ;
  AuthorizationResult authorization = Authorize_ ( caller , route , refusalReason ) ;
  if ( authorization == AuthorizationUnauthenticated && scimPath ) return BuildScimError_ ( 401 , "" , _T ( "" ) , "" ) ;
  if ( authorization == AuthorizationUnauthenticated ) return BuildUnauthorizedResponse_ ( false , IsPageScriptRequest_ ( request ) ) ;
  if ( authorization == AuthorizationForbidden ) {
    String message ;
    message . Format ( _T ( "" ) , String ( caller . identity ) . c_str ( ) , String ( method ) . c_str ( ) , String ( path ) . c_str ( ) , String ( refusalReason ) . c_str ( ) ) ;
    LOG_APPLICATION ( message ) ;
    if ( scimPath ) return BuildScimError_ ( 403 , "" , String ( refusalReason ) ) ;
    return BuildForbiddenResponse_ ( refusalReason ) ;
  }
  if ( ChangesAccountByAddress_ ( route . kind ) ) {
    String wideName ;
    Unicode :: MultiByteToWide ( route . identifier , wideName ) ;
    std :: shared_ptr < const Account > candidates [ 2 ] = {
      CacheContainer :: Instance ( ) -> GetAccount ( wideName ) , CacheContainer :: Instance ( ) -> GetAccount ( String ( route . identifier ) )
    } ;
    for ( const std :: shared_ptr < const Account > & named : candidates ) {
      if ( named && ! CallerTakesInAccount_ ( caller , named ) ) {
        LOG_APPLICATION ( "" + String ( method ) + "" + String ( path ) + "" + String ( caller . identity ) + "" + named -> GetAddress ( ) + "" ) ;
        return AccountNotTakenIn_ ( ) ;
      }
    }
  }
  if ( caller . result == AuthenticatedAsAccount && caller . via_app_password && caller . account && IsSelfServiceRoute_ ( route . kind ) ) {
    const String keyRefusal = MailboxKeys :: SessionRefusal ( caller . account , SignInStore :: CredentialAppPassword ) ;
    if ( ! keyRefusal . IsEmpty ( ) ) return BuildResponse_ ( MailboxKeys :: RefusalStatus ( keyRefusal , 403 ) , "" + JsonEscape_ ( Utf8_ ( keyRefusal ) ) + "" ) ;
  }
  AuditScope auditScope ( AuditActorFor_ ( caller ) ) ;
  switch ( route . kind ) {
    case ROUTE :
    return HANDLER ( ) ;
    default :
    break ;
  }
  return BuildResponse_ ( 404 , "" ) ;
}
""",
}


PRINCIPALS = [
    ("Account", dict(result="account", role=None, domains="own")),
    ("Domain admin", dict(result="account", role="domainadmin", domains="own")),
    ("Help desk", dict(result="account", role="helpdesk", domains="own")),
    ("Auditor", dict(result="account", role="auditor", domains="all")),
    ("Server admin account", dict(result="account", role="serveradmin", domains="all")),
    ("Admin password", dict(result="admin", role="serveradmin", domains="all")),
    ("Key, full", dict(result="key", role="serveradmin", domains="all")),
    ("Key, read-only", dict(result="key", role="auditor", domains="all")),
    ("Key, one domain", dict(result="key", role="serveradmin", domains="own")),
]


# ---------------------------------------------------------------------------
# The document
# ---------------------------------------------------------------------------

def group_of(path):
    """The section a path is listed under, by whole segments: /api/v1/message-trace
    is not under /api/v1/me."""
    def under(prefix):
        return path == prefix or path.startswith(prefix + "/")
    if under("/api/v1/me"):
        return "The account's own: /api/v1/me"
    if under("/api/v1/accounts"):
        return "Accounts by address: /api/v1/accounts"
    if under("/api/v1/domains"):
        return "Domains: /api/v1/domains"
    if under("/api/v1/settings") or under("/api/v1/ini"):
        return "Settings: /api/v1/settings and the ini file"
    if under("/scim"):
        return "SCIM: /scim"
    if path.startswith("/api/v1/"):
        return "The rest of /api/v1"
    return "Other paths"


GROUP_ORDER = [
    "The account's own: /api/v1/me", "Accounts by address: /api/v1/accounts", "Domains: /api/v1/domains",
    "Settings: /api/v1/settings and the ini file", "The rest of /api/v1", "SCIM: /scim", "Other paths",
]


def md_escape(text):
    return text.replace("|", "\\|")


def build(sources=None):
    decision = Decision(sources)
    records = read_rest_routes(sources)
    anon = read_anonymous_routes(sources, REST_CPP, "RestApiServer::ProcessRequest_", True)
    web = read_anonymous_routes(sources, WEB_CPP, "WebServicesServer::ProcessRequest_", False)
    return decision, records, anon, web


def route_rows(decision, records):
    """One row per route kind, method and path. RouteUnknown - every path no
    other kind takes - is described in the text rather than listed. A path the
    parser also takes with a trailing slash (or two) is one row, marked."""
    union = {}
    order = []
    for kind, methods, template, line, parts, st in records:
        if kind == "RouteUnknown":
            continue
        key = (kind, template)
        if key not in union:
            union[key] = frozenset(methods) if methods is not None else None
            order.append(key)
        elif union[key] is not None:
            union[key] = None if methods is None else union[key] | frozenset(methods)
            if union[key] is not None and union[key] >= frozenset(METHODS):
                union[key] = None
    rows = {}
    for kind, template in order:
        text = method_text(union[(kind, template)])
        rows.setdefault((kind, text, template), [kind, text, template, False])
    for key in list(rows):
        kind, method, template = key
        shorter = template.rstrip("/")
        if shorter != template and (kind, method, shorter) in rows:
            rows[(kind, method, shorter)][3] = True
            del rows[key]
    out = [tuple(r) for r in rows.values()]
    return sorted(out, key=lambda r: (GROUP_ORDER.index(group_of(r[2])), r[2], r[1], r[0]))


def findings(decision, rows, anon=()):
    """The routes whose check looks wrong or rests on a handler: computed, then listed."""
    out = []
    produced = {r[0] for r in rows}
    dispatched = set(decision.handler)
    for kind in decision.kinds:
        if kind in ("RouteUnknown", "RouteApiKeyUnsupported"):
            continue
        if kind not in produced:
            out.append(("F-unparsed", kind, "a RouteKind ParseRoute_ never produces (the interpreter found no path to it)"))
        elif kind not in dispatched:
            out.append(("F-undispatched", kind, "produced by ParseRoute_ and authorised, but ProcessRequest_'s switch has no case for it: answered 404 after Authorize_"))
    by_kind = {}
    for kind, method, template, _ in rows:
        by_kind.setdefault(kind, []).append((method, template))
    for kind, entries in sorted(by_kind.items()):
        if kind in decision.mutating:
            for method, template in entries:
                if method == "any" or "GET" in method.split(", "):
                    out.append(("F-mutating-get", kind, "IsMutatingRoute_ calls it a change, and it is reached with %s %s" % (method, template)))
    for kind in sorted(by_kind):
        cells = [decision.decide(kind, who) for _, who in PRINCIPALS]
        if "own*" in cells:
            args = decision.handler_args.get(kind, "")
            if "caller" in args:
                out.append(("F-handler-confines", kind, "a role confined to its domain reaches it, the route names no domain, and the handler (%s) is given the caller to narrow it" % decision.handler.get(kind, "?")))
            else:
                out.append(("F-handler-blind", kind, "a role confined to its domain reaches it, the route names no domain, and the handler (%s) is not given the caller, so cannot narrow it" % decision.handler.get(kind, "?")))
        elif kind in decision.mutating and ("own" in cells or "own+" in cells) \
                and "caller" not in decision.handler_args.get(kind, ""):
            out.append(("F-write-blind", kind, "a role confined to its domain makes this change on its own domain's path, and the handler (%s) is not given the caller, so whatever the body names beyond that domain - another domain's name, an address, a route, one of the server's addresses - is held to nothing of the credential's" % decision.handler.get(kind, "?")))
    seen = set()
    for name, methods, template, line, st in anon:
        if methods is not None and not ("GET" in methods and "POST" in methods):
            continue
        key = "%s %s" % (method_text(methods), template)
        if key in seen:
            continue
        seen.add(key)
        out.append(("F-anonymous-get", key, "answered before any credential by %s for GET and POST alike: its GET must change nothing, since mail scanners and link previews open the links a message carries" % name))
    return out


def render(decision, records, anon, web):
    rows = route_rows(decision, records)
    lines = []
    w = lines.append
    w("<!-- GENERATED by build/generate-auth-matrix.py from the server's C++. Do not edit: run the script. -->")
    w("")
    w("# The web authorisation matrix")
    w("")
    w("Every route the server's two web listeners answer, and what each kind of credential gets there. "
      "It is written by `build/generate-auth-matrix.py` from the code that decides it, not by hand, and "
      "`python build/generate-auth-matrix.py --check` fails in CI (the `checkers` job) the moment the code and this "
      "file disagree: when this file is not what the script reads from the code now, and when a step of a function "
      "the script holds (\"The answer\", below) is not the one it follows, until the script has been read against the "
      "new step. The threat model "
      "that reads it is [WebThreatModel.md](WebThreatModel.md); the routes this file flags are triaged there.")
    w("")
    w("## How it is read from the code")
    w("")
    w("- **Paths** come from `RestApiServer::ParseRoute_` and the three parsers it hands a path to "
      "(`ParseScimRoute_`, `ParseDiscoveryRoute_`, `ParseAccountResourceRoute_`), interpreted rather than "
      "pattern-matched: the script follows the parser's string operations and keeps the path as literal text "
      "and named holes. `{id}` is a part the parser holds to digits (`ParseQueueId`); `{segment}` is one path "
      "segment; `{...}` is a part the parser does not narrow, which may hold a `/`. A condition the "
      "interpreter cannot model narrows nothing, so a template is never narrower than what the code accepts.")
    w("- **The answer** follows `RestApiServer::Authorize_` step by step: `IsSessionRoute_`, the send API's "
      "key-only rule, `IsScimRoute_`, `IsSelfServiceRoute_`, the ordinary account's refusal, the administrator "
      "password, `IsApiKeyRoute_` (with the role-holding account's exceptions), then `RoleManager::Allows` on "
      "`PermissionFor_` and `SecondPermissionFor_` - in which `administer_server` is every permission only for a "
      "grant with no list of domains: one that holds it with a list (a `Scope=full` key issued for named domains, "
      "`serveradmin` assigned with one) is judged as any confined grant is, by the permissions it holds and, where the "
      "route names a domain, by its list, and is refused `administer_server` itself - then the refusals a credential "
      "confined to named domains meets, the report's sections among them (`IsServerWideReportSection_`, from "
      "`RestApiReports.cpp`'s table), "
      "then `DomainNamedByRoute_`. Then the built-in roles' permissions from `RoleManager.cpp`: for each role's "
      "name, those of the function `RoleManager::BuiltInPermissions_` picks - every permission for `serveradmin`, "
      "whose `ServerAdminPermissions` sets every bit below `RolePermission::Count`, and for each other role the "
      "permissions its function sets, one a line, `set |= Bit(RolePermission::X);`. Those steps are the script's, "
      "so it holds them to the code: `Authorize_` and each function whose list it reads, "
      "`RoleManager::BuiltInPermissions_`, the four roles' functions and the `Bit` they set a permission with, "
      "`RoleManager::Allows` and the `RoleGrant::Holds` and `RoleManager::IsDomainReached_` it asks, "
      "`IsDomainAllowed_`, `IsServerWideReportSection_` and the `FindSection` it asks, and `ProcessRequest_` from "
      "the end of the anonymous routes to the handler are compared with the steps the script was last read "
      "against, their lists left out, and a step added, taken out, moved or given a condition, or a name a step "
      "compares or is kept under changed, stops it; prose - a refusal's reason, a message - may be reworded. The "
      "enumerations of route kinds and of permissions are read with the value the compiler gives each enumerator, "
      "which must be its place - a permission's place is the bit it is set at and the place of its token - so a value "
      "written that is not its place stops it. It reads the code as the compiler's first three translation phases "
      "do - line splices joined, comments taken out, raw string literals and numbers with digit separators read "
      "whole, a directive wherever it begins a line, after spaces or a comment too - so that nothing in a comment or "
      "a literal is read as code, and a form it does not read as the compilers do - a backslash with spaces after it "
      "at a line's end, which they read differently, among them - stops it. What it reads must stand outside the "
      "preprocessor's conditionals (`#if` to `#endif`), which it does not evaluate, and be the only definition of "
      "its name in its file: a definition inside one, or holding one, stops it, since another copy, or another "
      "branch, may be what is compiled, and so does a second definition - an overload, a copy in another namespace "
      "- since the compiler may use that one. A directive of the preprocessor of any other kind inside what it reads "
      "- an `#include`, which brings in code it never sees, a `#define` - stops it too, as a conditional does: "
      "inside a function it reads, from the line that names it to its closing brace, its parameter list among it, "
      "inside the enumerations, `PermissionTokens` or a constant's initialiser, and from the report's `SectionInfo` "
      "to the end of `FindSection`. Not held: how `Authenticate_` makes a request one "
      "of the columns below - the grant `RoleManager` gives it, of which role and for which domains "
      "(`GrantForAccount`, `GrantForApiKey` and `ServerAdministrator`, and the `GrantForRole` and `GrantByAdminLevel` "
      "they ask) - which they take as given, the string helpers those functions use, a macro defined elsewhere over a "
      "name they use and an overload of one of them defined in another file.")
    w("- **Not shown**, because they narrow every route alike: the Host check (421), the request budget per "
      "credential (429), the administrator password's throttle, the `X-Requested-With: hMailServer` header a "
      "session must send on anything but GET and HEAD (403), the refusal of a request that carries a credential "
      "and changes something when the browser reports it as sent from another site (403), the mailbox-key "
      "refusal of an app password on an encrypted mailbox, and `ChangesAccountByAddress_`'s refusal of an "
      "account the credential does not take in. Those are in WebThreatModel.md. All but the Host check, which "
      "comes before the anonymous routes, are called in the part of `ProcessRequest_` the script holds, with the "
      "conditions they are asked under and the names they compare or are kept under (the administrator's name and "
      "the throttle's key, the methods, the header and its value), so a check taken out, moved, put under another "
      "condition or given another name there stops it. What the functions they call decide is not held: "
      "`RequestIsCrossSite_`, `IsWithinRequestRate_`, `GetHeader_`, `CallerTakesInAccount_` (and the "
      "`RoleManager::Encompasses` it asks), "
      "`MailboxKeys::SessionRefusal` and `MailboxKeys::RefusalStatus`, and the throttle's `BasicUserName_`, "
      "`SessionThrottleRetryMs_`, `RegisterSessionFailure_` and `ClearSessionFailure_`.")
    w("")
    w("## The columns")
    w("")
    w("| Column | Who |")
    w("|---|---|")
    w("| Anonymous | No credential. Every route below the first table answers 401. |")
    w("| Account | An ordinary account's password, app password, session cookie or session bearer token: the webmail and the portal. |")
    w("| Domain admin | An account whose level is domain administrator, or which is assigned `domainadmin` with no list: confined to its own domain. On its password or its session cookie; a bearer session of the same account is never administrative, and counts as Account. The same holds for the next three columns. |")
    w("| Help desk | An account assigned `helpdesk` with no list: confined to its own domain. |")
    w("| Auditor | An account assigned `auditor`: every domain. |")
    w("| Server admin account | An account whose level is server administrator, or which is assigned `serveradmin` for "
      "every domain (no list of domains, or `*`): every domain, but an account. An account assigned `serveradmin` with a "
      "list of domains is confined to them and is refused `administer_server` (`RoleManager::Allows`), though its role "
      "holds it, as a key issued for named domains is. |")
    w("| Admin password | The `administrator` password from hMailServer.ini (HTTP Basic, or the Control Deck's session made from it). |")
    w("| Key, full | An API key with `Scope=full` and no domain list (`serveradmin`). |")
    w("| Key, read-only | An API key with `Scope=readonly` (`auditor`). |")
    w("| Key, one domain | A `Scope=full` key issued for named domains: `serveradmin` confined to them, which is refused "
      "`administer_server` (`RoleManager::Allows`), though its role holds it. |")
    w("")
    w("| Cell | Meaning |")
    w("|---|---|")
    w("| yes | Allowed. |")
    w("| own | Allowed where the domain the route names (in its path) is one the credential reaches; another domain's answers 403. Only the path is held here: what the body names beyond that domain is the handler's to hold, and a change whose handler is not given the caller is listed under Flagged. |")
    wide = ["`%s`" % name for name in decision.server_wide_sections]
    if wide:
        w("| own+ | The report route: allowed for a named domain the credential reaches; the sections `RestApiReports.cpp`'s "
          "table counts from server-wide metrics (%s) are refused. |"
          % (wide[0] if len(wide) == 1 else ", ".join(wide[:-1]) + " and " + wide[-1]))
    else:
        w("| own+ | The report route: allowed for a named domain the credential reaches; `RestApiReports.cpp`'s table "
          "counts none of its sections from server-wide metrics, so none is refused. |")
    w("| own* | Allowed, and the route names no domain: whatever it shows or changes beyond the credential's domains is narrowed by the handler, or not at all. Each is listed under Flagged. |")
    w("| sender | Allowed when the key names a sender (`SendFrom`) in a domain it reaches. |")
    w("| no | 403. |")
    w("| 401 | 401, as if no credential were given. |")
    w("")
    w("## Before any credential")
    w("")
    w("Answered by `RestApiServer::ProcessRequest_` above `Authenticate_`, in this order. The first match wins. "
      "A capability token in the path or the query is the whole of the authority on several of these; "
      "WebThreatModel.md says which and how each is held.")
    w("")
    w("| Method | Path | Handler |")
    w("|---|---|---|")
    seen = set()
    for name, methods, template, line, st in anon:
        key = (method_text(methods), template, name)
        if key in seen:
            continue
        seen.add(key)
        w("| %s | `%s` | `%s` |" % (method_text(methods), md_escape(template), name))
    w("")
    w("## The REST API, decided by Authorize_")
    w("")
    w("%d rows (method, path and route kind) of %d route kinds. Permission is `PermissionFor_`'s, with `SecondPermissionFor_`'s after a `+`; "
      "a route classified in `IsSelfServiceRoute_` or `IsSessionRoute_` needs no permission and says so. "
      "Anonymous is 401 for every row." % (len(rows), len({r[0] for r in rows})))
    w("")
    heads = [p[0] for p in PRINCIPALS]
    w("What each credential reaches, counted over the rows below:")
    w("")
    w("| Credential | yes | own | own+ | own* | sender | no | 401 |")
    w("|---|---|---|---|---|---|---|---|")
    for label, who in PRINCIPALS:
        counts = {}
        for kind, method, template, slash in rows:
            cell = decision.decide(kind, who)
            counts[cell] = counts.get(cell, 0) + 1
        w("| %s | %s |" % (label, " | ".join(str(counts.get(c, 0)) for c in ("yes", "own", "own+", "own*", "sender", "no", "401"))))
    w("")
    unknown = [label for label, who in PRINCIPALS if decision.decide("RouteUnknown", who) in ("no", "401")]
    w("Any other path is `RouteUnknown`. It needs `%s`; a credential that holds it is answered 404, and %s %s "
      "refused first (403, or 401 for a key's credential problem)."
      % (decision.token(decision.permission.get("RouteUnknown", decision.default_permission)),
         ", ".join(unknown) if unknown else "nobody",
         "is" if len(unknown) == 1 else "are"))
    w("")
    current = None
    for kind, method, template, slash in rows:
        group = group_of(template)
        if group != current:
            if current is not None:
                w("")
            current = group
            w("### %s" % group)
            w("")
            w("| Method | Path | Route | Permission | %s |" % " | ".join(heads))
            w("|" + "---|" * (4 + len(heads)))
        if kind in decision.session:
            perm = "(session)"
        elif kind in decision.self_service:
            perm = "(own account)"
        else:
            first = decision.permission.get(kind, decision.default_permission)
            perm = decision.token(first)
            if kind in decision.second:
                perm += " + " + decision.token(decision.second[kind])
        cells = [decision.decide(kind, who) for _, who in PRINCIPALS]
        shown = "`%s`" % md_escape(template) + (" (and with `/` after)" if slash else "")
        w("| %s | %s | %s | %s | %s |" % (method, shown, kind[5:], perm, " | ".join(cells)))
    w("")
    w("## The web services listener")
    w("")
    w("`WebServicesServer::ProcessRequest_`, on its own ports (`WebServicesPort` and `WebServicesHttpsPort`, both 0 "
      "by default). Nothing here takes the REST API's credentials. CalDAV, CardDAV and autodiscover take an "
      "account's password over HTTPS (`Dav::Authenticate`); the rest is public by design.")
    w("")
    w("| Method | Path | Handler | When |")
    w("|---|---|---|---|")
    seen = set()
    for name, methods, template, line, st in web:
        when = ", ".join(sorted(set(st.notes))) or "always"
        key = (method_text(methods), template, name, when)
        if key in seen:
            continue
        seen.add(key)
        w("| %s | `%s` | `%s` | %s |" % (method_text(methods), md_escape(template), name, when))
    w("")
    w("## Flagged")
    w("")
    w("Computed from the tables above, so that a new route of these shapes is flagged the day it is written. "
      "Each is judged in [WebThreatModel.md](WebThreatModel.md), under \"The routes the matrix flags, judged\".")
    w("")
    flagged = findings(decision, rows, anon)
    labels = {
        "F-handler-blind": "A confined role reaches it, it names no domain, and the handler cannot narrow it",
        "F-write-blind": "A confined role makes this change on its own domain's path, and the handler is not given the caller",
        "F-handler-confines": "A confined role reaches it, it names no domain, and the handler is given the caller",
        "F-mutating-get": "A change reached by GET",
        "F-anonymous-get": "A page answered before any credential for GET and POST alike",
        "F-undispatched": "Authorised but never dispatched",
        "F-unparsed": "A route kind no path reaches",
    }
    for code in ("F-handler-blind", "F-write-blind", "F-mutating-get", "F-anonymous-get", "F-undispatched", "F-unparsed",
                 "F-handler-confines"):
        items = [f for f in flagged if f[0] == code]
        w("### %s (%d)" % (labels[code], len(items)))
        w("")
        if not items:
            w("None.")
            w("")
            continue
        for _, kind, why in items:
            w("- `%s`: %s." % (kind, why))
        w("")
    return "\n".join(lines) + "\n"


USAGE = "usage: python build/generate-auth-matrix.py [--check | --print | --steps | --self-test]"


def main(argv):
    if argv in (["-h"], ["--help"]):
        print(USAGE)
        print("With no argument it writes hmailserver/docs/WebAuthorisationMatrix.md; --check compares, --print writes "
              "to standard output, --steps prints the steps of the functions it holds to the code (steps_of) as STEPS "
              "would hold them, and --self-test breaks a copy of the source and proves each rule is still read. A step that "
              "is not the one STEPS holds stops every mode but --steps, and a definition it reads that stands inside a "
              "conditional of the preprocessor, or holds one, stops every mode.")
        return 0
    if len(argv) > 1 or any(a not in ("--check", "--print", "--steps", "--self-test") for a in argv):
        # A misspelt --check in CI must fail, not write the matrix and pass.
        print(USAGE, file=sys.stderr)
        print("generate-auth-matrix: not an argument it takes: %s" % " ".join(argv), file=sys.stderr)
        return 2
    mode = "write"
    if "--check" in argv:
        mode = "check"
    elif "--print" in argv:
        mode = "print"
    elif "--steps" in argv:
        try:
            sys.stdout.write(steps_literal(steps_of()))
        except GenerationError as error:
            print("generate-auth-matrix: %s" % error, file=sys.stderr)
            return 2
        return 0
    elif "--self-test" in argv:
        return self_test()
    try:
        decision, records, anon, web = build()
        text = render(decision, records, anon, web)
    except GenerationError as error:
        print("generate-auth-matrix: %s" % error, file=sys.stderr)
        return 2
    if mode == "print":
        sys.stdout.write(text)
        return 0
    if mode == "check":
        try:
            with open(DOC, encoding="utf-8", newline="") as handle:
                committed = handle.read().replace("\r\n", "\n")
        except OSError:
            committed = None
        if committed != text:
            print("hmailserver/docs/WebAuthorisationMatrix.md is not what the code decides now; run "
                  "python build/generate-auth-matrix.py and commit what it writes.", file=sys.stderr)
            return 1
        print("WebAuthorisationMatrix.md is current.")
        return 0
    with open(DOC, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("Wrote %s" % os.path.relpath(DOC, ROOT))
    return 0


def self_test():
    """Each rule this script reads, broken in a copy of the source, must change
    what it writes or stop it - so that a script that has stopped seeing the
    source fails - and the documents' index must say what it holds as the
    matrix does."""
    failures = []
    base = {}
    for rel in [REST_H, REST_CPP, REPORTS_CPP, ROLE_CPP, WEB_CPP, "Common/Application/RoleManager.h"] \
            + list(SUB_PARSERS.values()) + list(PREDICATES.values()) + [c[0] for c in CONSTANTS.values()]:
        base[rel] = read(rel)

    def run(sources):
        decision, records, anon, web = build(sources)
        return decision, route_rows(decision, records), render(decision, records, anon, web)

    def broken_copy(rel, old, new):
        copy_ = dict(base)
        copy_[rel] = base[rel].replace(old, new, 1)
        if copy_[rel] == base[rel]:
            failures.append("the negative control could not be applied: %r is no longer in %s" % (old[:60], rel))
        return copy_

    decision, rows, text = run(base)

    def expect(label, condition):
        if not condition:
            failures.append(label)

    # 0. The reading itself: known routes come out as the code has them.
    known = {
        ("RouteAccountDelete", "DELETE", "/api/v1/accounts/{address}"),
        ("RouteMeMessageFlags", "PUT", "/api/v1/me/messages/{id}/flags"),
        ("RouteDiscoveryExportCreate", "POST", "/api/v1/discovery/cases/{id}/searches/{id}/export"),
        ("RouteDnsBlackListUpdate", "PUT", "/api/v1/dns-blacklists/{id}"),
        ("RouteSettingsProfileApply", "POST", "/api/v1/settings/profiles/{segment}/apply"),
    }
    have = {(r[0], r[1], r[2]) for r in rows}
    for item in sorted(known):
        expect("the route %s %s %s is read from ParseRoute_" % (item[1], item[2], item[0]), item in have)
    expect("a domain administrator's account delete is 'own'",
           decision.decide("RouteAccountDelete", dict(result="account", role="domainadmin", domains="own")) == "own")
    expect("an API key is refused the API keys with 401",
           decision.decide("RouteApiKeyList", dict(result="key", role="serveradmin", domains="all")) == "401")
    expect("the administrator password is refused the account's own routes",
           decision.decide("RouteMe", dict(result="admin", role="serveradmin", domains="all")) == "no")
    expect("an anonymous route is read", "| GET | `/portal` | `HandlePortalPage_` |" in text)

    nl = chr(10)

    # 1. A route left out of DomainNamedByRoute_ is flagged.
    d1, r1, t1 = run(broken_copy(
        REST_CPP,
        "      case RouteAccountDelete:" + nl + "         // The reason the whole mechanism exists.",
        "      case RouteAccountDeleteX:" + nl + "         // The reason the whole mechanism exists."))
    expect("a route DomainNamedByRoute_ forgets is flagged", "RouteAccountDelete" in {f[1] for f in findings(d1, r1)})

    # 2. A change reached by GET is flagged.
    d2, r2, t2 = run(broken_copy(
        REST_CPP, "      route.kind = RouteUnknown;" + nl + '      route.identifier = "";',
        '      route.kind = RouteUnknown;' + nl + '      if (method == "GET" && path == "/api/v1/self-test") { route.kind = RouteQueueRetry; return; }'
        + nl + '      route.identifier = "";'))
    expect("a mutating route reached by GET is flagged",
           any(f[0] == "F-mutating-get" and f[1] == "RouteQueueRetry" for f in findings(d2, r2)))

    # 3. A route with no case in the dispatch is flagged.
    d3, r3, t3 = run(broken_copy(
        REST_CPP,
        "         case RouteTlsa:" + nl + "            return HandleTlsa_();",
        "         case RouteTlsaX:" + nl + "            return HandleTlsa_();"))
    expect("an authorised route that is never dispatched is flagged",
           any(f[0] == "F-undispatched" and f[1] == "RouteTlsa" for f in findings(d3, r3)))

    # 4. A permission taken from a built-in role changes a cell.
    d4, r4, t4 = run(broken_copy(
        ROLE_CPP,
        "         set |= Bit(RolePermission::ResetPassword);" + nl + "         set |= Bit(RolePermission::UnlockAccount);" + nl
        + "         set |= Bit(RolePermission::ViewMessageTrace);",
        "         set |= Bit(RolePermission::UnlockAccount);" + nl + "         set |= Bit(RolePermission::ViewMessageTrace);"))
    expect("a permission taken from the help desk changes the matrix", t4 != text)

    # 5. A route added to the server-wide refusals changes its cell: one the
    # matrix flags now, so that the control shows the flag go.
    expect("the bounce classifier, which names no domain, is flagged",
           "RouteBounceClassify" in {f[1] for f in findings(decision, rows) if f[0] == "F-handler-blind"})
    d5, r5, t5 = run(broken_copy(
        REST_CPP, "      case RouteScheduledRun:" + nl + "         // IP ranges, certificates",
        "      case RouteScheduledRun:" + nl + "      case RouteBounceClassify:" + nl + "         // IP ranges, certificates"))
    expect("a route added to the server-wide refusals is no longer flagged",
           "RouteBounceClassify" not in {f[1] for f in findings(d5, r5) if f[0] == "F-handler-blind"})

    # 6. A shape the script cannot read fails loudly rather than writing less.
    try:
        run(broken_copy(REST_CPP, "      if (caller.domains.empty())" + nl + "         return AuthorizationAllowed;",
                        "      if (caller.domains.size() == 0)" + nl + "         return AuthorizationAllowed;"))
        failures.append("an Authorize_ whose confinement section cannot be found is refused")
    except GenerationError:
        pass
    try:
        run(broken_copy(REST_CPP, "AuditScope auditScope(AuditActorFor_(caller));" + nl + nl + "         switch (route.kind)",
                        "AuditScope auditScope(AuditActorFor_(caller));" + nl + nl + "         switch (route.kind + 0)"))
        failures.append("a ProcessRequest_ whose dispatch switch cannot be found is refused")
    except GenerationError:
        pass

    # 7. A page whose POST is refused before its handler is reached is still one
    # route for GET and POST, listed once with both.
    reset = ('         return HandlePasswordResetPage_(method, query, method == "POST" ? GetRequestBody_(request) : '
             'AnsiString(), peer_address);')
    d7, r7, t7 = run(broken_copy(
        REST_CPP, 'path == "/portal/reset")' + nl + reset,
        'path == "/portal/reset")' + nl + "      {" + nl
        + '         if (method == "POST" && RequestIsCrossSite_(request, true))' + nl
        + '            return BuildResponse_(403, "{}");' + nl + nl + reset + nl + "      }"))
    expect("a page whose POST may be refused before its handler is one route for GET and POST",
           "| GET, POST | `/portal/reset` | `HandlePasswordResetPage_` |" in t7
           and "| POST | `/portal/reset` |" not in t7)

    # 8. A confined change whose handler is not given the caller is flagged, and
    # giving it the caller takes the flag away.
    blind = {f[1] for f in findings(decision, rows) if f[0] == "F-write-blind"}
    expect("an alias made by a confined credential, its handler given no caller, is flagged",
           "RouteAliasCreate" in blind)
    expect("a route that names no domain is not flagged as a confined change on its own domain's path",
           not (blind & {f[1] for f in findings(decision, rows) if f[0] in ("F-handler-blind", "F-handler-confines")}))
    d8, r8, t8 = run(broken_copy(
        REST_CPP, "return HandleCreateAlias_(String(route.identifier), GetRequestBody_(request));",
        "return HandleCreateAlias_(String(route.identifier), GetRequestBody_(request), caller);"))
    expect("a confined change whose handler is given the caller is no longer flagged",
           "RouteAliasCreate" not in {f[1] for f in findings(d8, r8) if f[0] == "F-write-blind"})

    # 9. An anonymous page answered for GET and POST alike is flagged.
    d9, r9, t9 = run(broken_copy(
        REST_CPP, 'if (method == "POST" && path == "/api/v1/password-reset")',
        'if ((method == "GET" || method == "POST") && path == "/api/v1/password-reset")'))
    expect("an anonymous route answered for GET and POST alike is flagged",
           "- `GET, POST /api/v1/password-reset`: " in t9 and "- `GET, POST /api/v1/password-reset`: " not in text)

    # 10. Paths are grouped by whole segments.
    expect("/api/v1/message-trace is not listed with the account's own routes",
           group_of("/api/v1/message-trace") != group_of("/api/v1/me") and group_of("/api/v1/me/rules") == group_of("/api/v1/me"))

    # 11. An argument it does not know is refused, and nothing is written.
    before = open(DOC, "rb").read() if os.path.exists(DOC) else None
    with contextlib.redirect_stderr(io.StringIO()):
        refused = main(["--chek"])
    after = open(DOC, "rb").read() if os.path.exists(DOC) else None
    expect("a misspelt --check is refused with 2 and writes nothing", refused == 2 and before == after)

    # 12. A step changed that no reader takes - a test taken out or made always
    # true, an allow or a condition put in, a label the reader cannot see -
    # stops the script and names the function, where it left the matrix as it
    # was.
    def stops(label, rel, old, new, function):
        try:
            Decision(broken_copy(rel, old, new))
        except GenerationError as error:
            if "%s's steps have changed" % function not in str(error):
                failures.append("%s: it stopped, but not on %s's steps: %s" % (label, function, str(error).split(nl)[0]))
            return
        failures.append(label)

    authorize = "RestApiServer::Authorize_"
    stops("the domain test at Authorize_'s end made false stops the script", REST_CPP,
          "      if (!IsDomainAllowed_(caller.domains, targetDomain))", "      if (false)", authorize)
    stops("an allow put at the head of the confinement section stops the script", REST_CPP,
          "      if (caller.domains.empty())" + nl + "         return AuthorizationAllowed;" + nl,
          "      if (caller.domains.empty())" + nl + "         return AuthorizationAllowed;" + nl + nl
          + "      if (caller.result == AuthenticatedAsAccount)" + nl + "         return AuthorizationAllowed;" + nl, authorize)
    stops("the permission check made always true stops the script", REST_CPP,
          "         const bool permitted =" + nl, "         const bool permitted = true ||" + nl, authorize)
    stops("the administrator password's allow taken out stops the script", REST_CPP,
          "      if (caller.result == AuthenticatedAsAdministrator)" + nl + "         return AuthorizationAllowed;" + nl, "",
          authorize)
    stops("an allow of one route before the queue's refusal stops the script", REST_CPP,
          "      // A key restricted to named domains. The delivery queue is server-wide -",
          "      if (route.kind == RouteQueueList)" + nl + "         return AuthorizationAllowed;" + nl + nl
          + "      // A key restricted to named domains. The delivery queue is server-wide -", authorize)
    stops("a refusal put under a condition stops the script", REST_CPP,
          "      if (route.kind == RouteQueueList || route.kind == RouteQueueRetry",
          "      if (caller.result != AuthenticatedAsAccount)" + nl
          + "      if (route.kind == RouteQueueList || route.kind == RouteQueueRetry", authorize)
    stops("an allow inside the switch of server-wide routes stops the script", REST_CPP,
          "      case RouteAdminSecondFactorGet:" + nl + "      case RouteLogList:" + nl + "      case RouteLogTail:",
          "      case RouteAdminSecondFactorGet:" + nl + "      case RouteLogList:" + nl
          + "         if (caller.result == AuthenticatedAsAccount)" + nl + "            return AuthorizationAllowed;" + nl
          + "      case RouteLogTail:", authorize)
    stops("a refusal given a second condition stops the script", REST_CPP,
          "      if (route.kind == RouteMetricsHistory)" + nl,
          "      if (route.kind == RouteMetricsHistory && caller.domains.size() > 1)" + nl, authorize)
    stops("a label written in a form the reader does not take stops the script", REST_CPP,
          "      case RouteScheduledRun:" + nl + "         // IP ranges, certificates",
          "      case RouteScheduledRun :" + nl + "         // IP ranges, certificates", authorize)
    stops("IsDomainAllowed_ made to allow every domain stops the script", REST_CPP,
          "      if (domains.empty())" + nl + "         return true;",
          "      if (true || domains.empty())" + nl + "         return true;", "RestApiServer::IsDomainAllowed_")
    allows_full = "      const bool full = grant.Holds(RolePermission::AdministerServer) && grant.domains.empty();"
    stops("RoleManager::Allows made to grant every permission stops the script", ROLE_CPP,
          allows_full, "      const bool full = true;", "RoleManager::Allows")
    # A grant holding AdministerServer is the whole server only with no list of
    # domains: the list taken out of that test, the refusal of AdministerServer
    # itself to a grant with a list taken out, or the refusal of a domain its
    # list does not reach made an allow, each stops the script.
    stops("RoleManager::Allows made to read AdministerServer as the whole server whatever the grant's list stops the "
          "script", ROLE_CPP, allows_full, "      const bool full = grant.Holds(RolePermission::AdministerServer);",
          "RoleManager::Allows")
    stops("RoleManager::Allows's refusal of AdministerServer to a grant with a list taken out stops the script",
          ROLE_CPP, "      if (permission == RolePermission::AdministerServer)" + nl + "      {" + nl, "      if (false)"
          + nl + "      {" + nl, "RoleManager::Allows")
    stops("RoleManager::Allows made to allow a domain the list of a grant holding AdministerServer does not reach stops "
          "the script", ROLE_CPP, "            reason = _T(\"this credential is not permitted for that domain\");",
          "            return true;", "RoleManager::Allows")
    stops("a route of DomainNamedByRoute_ answered with no domain stops the script", REST_CPP,
          "         return StringParser::ExtractDomain(String(route.identifier));" + nl + nl
          + "      // An inbound webhook is named by its address",
          "         return String();" + nl + nl + "      // An inbound webhook is named by its address",
          "RestApiServer::DomainNamedByRoute_")
    stops("a group of IsApiKeyRoute_ that answers false stops the script", REST_CPP,
          "      switch (kind)" + nl + "      {" + nl + "      case RouteApiKeyUnsupported:",
          "      switch (kind)" + nl + "      {" + nl + "      case RouteTlsa:" + nl + "         return false;" + nl
          + "      case RouteApiKeyUnsupported:", "RestApiServer::IsApiKeyRoute_")
    stops("a permission chosen by a condition stops the script", REST_CPP,
          "      case RouteStoreStatus:" + nl + "         return RolePermission::ViewConfiguration;",
          "      case RouteStoreStatus:" + nl + "         if (kind == RouteStoreStatus)" + nl
          + "            return RolePermission::ViewLogs;" + nl + "         return RolePermission::ViewConfiguration;",
          "RestApiServer::PermissionFor_")
    try:
        Decision(broken_copy(REST_CPP, "      if (route.kind == RouteDomainAliasCreate)" + nl,
                             "      if (route.kind == RouteDomainAliasCreate || route.kind == RouteReport)" + nl))
        failures.append("the report route put in a refusal after its own block stops the script")
    except GenerationError as error:
        expect("the report route put in a refusal after its own block stops the script, naming it",
               "RouteReport is in a refusal" in str(error))
    stops("a string a condition compares, changed, stops the script", REST_CPP,
          "GetHeader_(request, \"x-requested-with\") != \"hMailServer\"",
          "GetHeader_(request, \"x-requested-with\") != \"hMailServerX\"", "RestApiServer::ProcessRequest_")
    # A name is part of its step wherever it stands: compared outside a
    # condition, compared as the argument of _T, or the key a call is given.
    stops("the administrator's name the throttle compares, changed, stops the script", REST_CPP,
          "const bool adminBasic = BasicUserName_(request) == \"administrator\";",
          "const bool adminBasic = BasicUserName_(request) == \"Administrator\";", "RestApiServer::ProcessRequest_")
    stops("the key the throttle is read under, made another, stops the script", REST_CPP,
          "SessionThrottleRetryMs_(\"administrator\", peer_address)",
          "SessionThrottleRetryMs_(\"administrator \", peer_address)", "RestApiServer::ProcessRequest_")
    stops("the key a failure is registered under, made another, stops the script", REST_CPP,
          "RegisterSessionFailure_(\"administrator\", peer_address);",
          "RegisterSessionFailure_(\"admin\", peer_address);", "RestApiServer::ProcessRequest_")
    stops("a role's name compared as the argument of _T, changed, stops the script", REST_CPP,
          "caller.grant.name == _T(\"auditor\")", "caller.grant.name == _T(\"Auditor\")", authorize)
    stops("a handler called before Authorize_ stops the script", REST_CPP,
          "         AnsiString refusalReason;" + nl + "         AuthorizationResult authorization = Authorize_(",
          "         if (route.kind == RouteQueueList)" + nl + "            return HandleQueueList_();" + nl + nl
          + "         AnsiString refusalReason;" + nl + "         AuthorizationResult authorization = Authorize_(",
          "RestApiServer::ProcessRequest_")
    # The functions Authorize_ hands a permission, a domain or a report section
    # to ask others, and those are held too.
    stops("RoleGrant::Holds made to hold every permission stops the script", ROLE_CPP,
          "      return (permissions & (1ULL << static_cast<int>(permission))) != 0;", "      return true;",
          "RoleGrant::Holds")
    stops("RoleManager::IsDomainReached_ made to reach every domain stops the script", ROLE_CPP,
          "      // an unrestricted key have." + nl + "      if (grant.domains.empty())",
          "      // an unrestricted key have." + nl + "      if (true || grant.domains.empty())",
          "RoleManager::IsDomainReached_")
    stops("IsServerWideReportSection_ made to answer no for every section stops the script", REPORTS_CPP,
          "      return info != 0 && info->server_wide;", "      return false;",
          "RestApiServer::IsServerWideReportSection_")
    stops("FindSection made to find another section than the one named stops the script", REPORTS_CPP,
          "            if (name.CompareNoCase(SECTIONS[i].name) == 0)",
          "            if (name.CompareNoCase(SECTIONS[i].name) != 0)", "FindSection")
    # The report's table is read as the compiler reads it, or not at all.
    for label, old, new, said in (
            ("a SectionCount that is not the report table's length", "const int SectionCount = (int) "
             "(sizeof(SECTIONS) / sizeof(SECTIONS[0]));", "const int SectionCount = 8;", "SectionCount"),
            ("a row of the report's table put under the preprocessor", '         { "countries", true,',
             "#if 0" + nl + '         { "countries", true,', "preprocessor"),
            ("SectionInfo's members put in another order", "         const char *name;" + nl
             + "         bool server_wide;", "         bool server_wide;" + nl + "         const char *name;",
             "SectionInfo")):
        try:
            Decision(broken_copy(REPORTS_CPP, old, new))
            failures.append("%s stops the script" % label)
        except GenerationError as error:
            expect("%s stops the script, saying so" % label, said in str(error))

    # The built-in roles' functions are held but for the lines that set a
    # permission, which are read, and the function that picks one by a role's
    # name is held whole.
    helpdesk_end = ("         set |= Bit(RolePermission::ViewMessageTrace);" + nl + "         return set;" + nl + "      }"
                    + nl + nl + "      unsigned __int64 AuditorPermissions()")
    stops("a built-in role made to take in another's permissions stops the script", ROLE_CPP, helpdesk_end,
          helpdesk_end.replace("         return set;", "         set |= DomainAdminPermissions();" + nl
                               + "         return set;"), "HelpdeskPermissions")
    stops("the server administrator's role made to leave a permission out stops the script", ROLE_CPP,
          "            set |= 1ULL << index;",
          "            if (index != static_cast<int>(RolePermission::AdministerServer))" + nl
          + "               set |= 1ULL << index;", "ServerAdminPermissions")
    stops("a permission's line of a built-in role given a mask stops the script", ROLE_CPP,
          "         set |= Bit(RolePermission::ManageAccounts);", "         set |= Bit(RolePermission::ManageAccounts) & 0;",
          "DomainAdminPermissions")
    stops("the bit a built-in role's permission is set at, moved, stops the script", ROLE_CPP,
          "         return 1ULL << static_cast<int>(permission);",
          "         return 1ULL << (static_cast<int>(permission) + 1);", "Bit")
    stops("a built-in role's name given another role's function stops the script", ROLE_CPP,
          "      if (name.Compare(_T(\"helpdesk\")) == 0)" + nl + "         return HelpdeskPermissions();",
          "      if (name.Compare(_T(\"helpdesk\")) == 0)" + nl + "         return DomainAdminPermissions();",
          "RoleManager::BuiltInPermissions_")

    # What is read is read as the compiler would read it: a definition inside a
    # conditional of the preprocessor, beside which another copy may be the one
    # compiled, stops the script, from the report's table to a route parser's
    # predicate and the enumeration of route kinds.
    def refused(label, sources, said):
        try:
            run(sources)
        except GenerationError as error:
            expect("%s stops the script, saying so" % label, said in str(error))
            return
        failures.append("%s stops the script" % label)

    def stopped(label, sources, said):
        """refused(), of a change to a file Decision reads, which stops it before
        the routes are interpreted."""
        try:
            Decision(sources)
        except GenerationError as error:
            expect("%s stops the script, saying so" % label, said in str(error))
            return
        failures.append("%s stops the script" % label)

    def under_conditional(rel, begin, end, change, opening="#if 0"):
        """A copy of the source with the text of rel from begin to the end of end
        put under a conditional, and a copy of it changed by change under its
        #else."""
        text = base[rel]
        at = text.find(begin)
        to = text.find(end, at) if at >= 0 else -1
        if to < 0:
            failures.append("the negative control could not be applied: %r is no longer in %s" % (begin[:60], rel))
            return dict(base)
        region = text[at:to + len(end)]
        live = change(region)
        if live == region:
            failures.append("the negative control could not be applied: its change is no longer in %s" % rel)
        copy_ = dict(base)
        copy_[rel] = text[:at] + opening + nl + region + "#else" + nl + live + "#endif" + nl + text[to + len(end):]
        return copy_

    find_end = "               return &SECTIONS[i];" + nl + "         }" + nl + nl + "         return 0;" + nl + "      }" + nl
    for opening in ("#if 0", "#ifndef HM_REPORTS_V2"):
        refused("the report's table, its count and FindSection under %s, a copy under its #else" % opening,
                under_conditional(REPORTS_CPP, "      struct SectionInfo" + nl, find_end,
                                  lambda region: region.replace('{ "countries", true,', '{ "countries", false,'), opening),
                "conditional of the preprocessor")
    refused("IsDomainAllowed_ under #if 0, a copy that reaches every domain under its #else",
            under_conditional(REST_CPP, "   bool" + nl + "   RestApiServer::IsDomainAllowed_(", nl + "   }" + nl,
                              lambda region: region.replace("      if (domains.empty())", "      if (true)")),
            "conditional of the preprocessor")
    refused("a route parser's predicate under #if 0, a copy under its #else",
            under_conditional("Common/Util/CalDavServer.cpp", "   bool" + nl + "   CalDavServer::IsCalendarTarget(",
                              nl + "   }" + nl, lambda region: region.replace('"/dav/calendars"', '"/dav/calendar"')),
            "conditional of the preprocessor")
    refused("the enumeration of route kinds under #if 0, a copy under its #else",
            under_conditional(REST_H, "      enum RouteKind" + nl, nl + "      };" + nl,
                              lambda region: region.replace("         RouteUnknown = 0,", "         RouteUnknown = 0," + nl
                                                            + "         RouteNothing,")),
            "conditional of the preprocessor")
    count = "      const int SectionCount = (int) (sizeof(SECTIONS) / sizeof(SECTIONS[0]));"
    counted = broken_copy(REPORTS_CPP, count, "      const int SectionCount = 8;")
    counted[REPORTS_CPP] = counted[REPORTS_CPP].replace(find_end, find_end + "#if 0" + nl + count + nl + "#endif" + nl, 1)
    refused("a SectionCount of another length, its right form under #if 0 after FindSection", counted, "SectionCount")
    # A definition that holds a conditional is refused too, since the script
    # would read each of its branches as compiled; and a definition in a comment
    # is none.
    refused("a route parser that holds a conditional",
            broken_copy(REST_CPP, "      route.kind = RouteUnknown;" + nl + '      route.identifier = "";',
                        "      route.kind = RouteUnknown;" + nl + "#if 0" + nl
                        + '      if (path == "/api/v1/self-test") { route.kind = RouteQueueList; return; }' + nl + "#endif"
                        + nl + '      route.identifier = "";'),
            "holds a conditional of the preprocessor")
    refused("the enumeration of route kinds holding a conditional",
            broken_copy(REST_H, "         RouteUnknown = 0," + nl,
                        "         RouteUnknown = 0," + nl + "#ifdef HM_ROUTES_V2" + nl + "         RouteNothing," + nl
                        + "#endif" + nl),
            "or holds one")
    allowed = base[REST_CPP].find("   bool" + nl + "   RestApiServer::IsDomainAllowed_(")
    allowed = base[REST_CPP][allowed:base[REST_CPP].find(nl + "   }" + nl, allowed) + len(nl + "   }" + nl)]
    stops("a copy of IsDomainAllowed_ in a comment before it, the one compiled reaching every domain, stops the "
          "script", REST_CPP, allowed, "/*" + nl + allowed + "*/" + nl
          + allowed.replace("      if (domains.empty())", "      if (true)"), "RestApiServer::IsDomainAllowed_")

    # 13. A list changed is read, not stopped on: a route into a refusal, a
    # refusal of the same shape beside the others, a group split, a reason
    # reworded; and what is not code is not read.
    def reads(label, rel, old, new, holds):
        try:
            changed = Decision(broken_copy(rel, old, new))
        except GenerationError as error:
            failures.append("%s: it stopped: %s" % (label, str(error).split(nl)[0]))
            return
        expect(label, holds(changed))

    one_domain_key = dict(result="key", role="serveradmin", domains="own")
    expect("a key confined to one domain reaches /api/v1/tlsa, which names no domain (own*)",
           decision.decide("RouteTlsa", one_domain_key) == "own*")
    # A route that needs AdministerServer - here /api/v1/tlsa taken out of
    # PermissionFor_'s list, so that it falls to the permission after the
    # switch - is refused a grant that holds it with a list of domains, a key
    # issued for named domains or an account assigned serveradmin with a list,
    # and allowed one with no list (RoleManager::Allows): such a grant is not
    # the whole server.
    full_key = dict(result="key", role="serveradmin", domains="all")
    server_admin_account = dict(result="account", role="serveradmin", domains="all")
    confined_server_admin_account = dict(result="account", role="serveradmin", domains="own")
    reads("a route that needs administer_server is refused serveradmin with a list of domains and allowed it with none",
          REST_CPP, "      case RouteStatus:" + nl + "      case RouteTlsa:" + nl + "      case RouteSrv:" + nl,
          "      case RouteStatus:" + nl + "      case RouteSrv:" + nl,
          lambda d: d.permission.get("RouteTlsa") is None and d.default_permission == "AdministerServer"
          and decision.decide("RouteTlsa", one_domain_key) == "own*"
          and decision.decide("RouteTlsa", confined_server_admin_account) == "own*"
          and d.decide("RouteTlsa", one_domain_key) == "no" and d.decide("RouteTlsa", confined_server_admin_account) == "no"
          and d.decide("RouteTlsa", full_key) == "yes" and d.decide("RouteTlsa", server_admin_account) == "yes")
    reads("a route added to a refusal's list is read as refused", REST_CPP,
          "      if (route.kind == RouteStoreStatus || route.kind == RouteStoreMeasure)",
          "      if (route.kind == RouteStoreStatus || route.kind == RouteStoreMeasure || route.kind == RouteTlsa)",
          lambda d: d.decide("RouteTlsa", one_domain_key) == "no")
    reads("a refusal of the same shape put beside another is read as refused", REST_CPP,
          "      if (route.kind == RouteMetricsHistory)" + nl,
          "      if (route.kind == RouteTlsa)" + nl + "      {" + nl
          + "         refusalReason = \"this credential is restricted to named domains, and the records are the server's\";"
          + nl + "         return AuthorizationForbidden;" + nl + "      }" + nl + nl
          + "      if (route.kind == RouteMetricsHistory)" + nl,
          lambda d: d.decide("RouteTlsa", one_domain_key) == "no")
    reads("a refusal's reason reworded, with a comment beside it, changes nothing", REST_CPP,
          "\"this credential is restricted to named domains, and the delivery queue is server-wide\"",
          "\"this credential is restricted to named domains, and the queue is every domain's\" /* reworded */",
          lambda d: d.decide("RouteQueueList", one_domain_key) == "no" and d.decide("RouteTlsa", one_domain_key) == "own*")
    reads("a response's body reworded between the credential and the handler changes nothing", REST_CPP,
          "could not be read just now; try again in a moment",
          "cannot be read at the moment; try again shortly",
          lambda d: d.handler == decision.handler)
    reads("a permission's group split in two is read the same", REST_CPP,
          "      case RouteScimUnknown:" + nl + "         return RolePermission::Provision;",
          "         return RolePermission::Provision;" + nl + "      case RouteScimUnknown:" + nl
          + "         return RolePermission::Provision;",
          lambda d: d.permission.get("RouteScimUnknown") == "Provision" and d.permission == decision.permission)
    reads("a case label inside a block comment is not read", REST_CPP,
          "      case RouteScheduledRun:" + nl + "         // IP ranges, certificates",
          "      case RouteScheduledRun:" + nl + "      /* case RouteTlsa: */" + nl + "         // IP ranges, certificates",
          lambda d: d.decide("RouteTlsa", one_domain_key) == "own*")
    expect("a dispatch's return written over several lines is read as its own handler",
           decision.handler.get("RouteMePushSubscribe") == "HandleMePushSubscribe_"
           and decision.handler.get("RouteMePushUpdate") == "HandleMePushUpdate_")
    # The report's table is a list: a section counted from server-wide metrics
    # is named in the own+ cell, and one made so is read there.
    expect("the report's server-wide sections are read from its table and named in the own+ cell",
           "server-wide metrics (`countries`) are refused" in text)
    d14, r14, t14 = run(broken_copy(REPORTS_CPP, '{ "storage", false,', '{ "storage", true,'))
    expect("a report section made one counted from server-wide metrics is named in the own+ cell",
           "server-wide metrics (`storage` and `countries`) are refused" in t14)
    # A second definition of what is read, wherever it stands - under a
    # conditional after FindSection, in another namespace - stops the script,
    # which reads one definition of each name and does not work out which the
    # compiler uses.
    table_copy = ("      const SectionInfo SECTIONS[] =" + nl + "      {" + nl
                  + '         { "traffic", true, "hm_messagetrace", "" }' + nl + "      };" + nl)
    for label, extra in (("under #if 0 after FindSection", "#if 0" + nl + table_copy + "#endif" + nl),
                         ("in another namespace after FindSection",
                          "      namespace Old_" + nl + "      {" + nl + table_copy + "      }" + nl)):
        stopped("a copy of the report's table %s" % label, broken_copy(REPORTS_CPP, find_end, find_end + extra),
                "defined more than once")
    helpdesk_whole = base[ROLE_CPP][base[ROLE_CPP].find("      unsigned __int64 HelpdeskPermissions()"):]
    helpdesk_whole = helpdesk_whole[:helpdesk_whole.find(nl + "      }" + nl) + len(nl + "      }" + nl)]
    stopped("a copy of HelpdeskPermissions in another namespace before it, the one compiled taking in another role's "
            "permissions",
            broken_copy(ROLE_CPP, helpdesk_whole, "      namespace Old_" + nl + "      {" + nl + helpdesk_whole + "      }"
                        + nl + nl + helpdesk_whole.replace("         return set;", "         set |= "
                                                           "DomainAdminPermissions();" + nl + "         return set;")),
            "defined more than once")
    stopped("an overload of IsDomainAllowed_ before it",
            broken_copy(REST_CPP, allowed, allowed.replace("const String &domainName)", "const char *domainName)")
                        + nl + allowed), "defined more than once")
    stopped("a second definition of RolePermission, in another namespace",
            broken_copy("Common/Application/RoleManager.h", "   enum class RolePermission" + nl,
                        "   namespace Old_ { enum class RolePermission { AdministerServer, Count }; }" + nl + nl
                        + "   enum class RolePermission" + nl),
            "defined more than once")
    # What a comment holds is not read: a constant a route parser names, or a
    # route kind.
    dc, rc, tc = run(broken_copy("Common/Util/CalDavServer.cpp", "   const char *CalDavServer::CalendarsPath = ",
                                 '   // const char *CalDavServer::CalendarsPath = "/dav/old/";' + nl
                                 + "   const char *CalDavServer::CalendarsPath = "))
    expect("a constant a route parser names, in a comment before it, is not read", tc == text)
    reads("a route kind in a comment of the enumeration is not read", REST_H, "         RouteUnknown = 0," + nl,
          "         RouteUnknown = 0," + nl + "         /*" + nl + "         RouteNothing," + nl + "         */" + nl,
          lambda d: d.kinds == decision.kinds)
    # A built-in role's permission lines are a list: one given to a role is read
    # as its own, and one named in a comment is not read.
    helpdesk = dict(result="account", role="helpdesk", domains="own")
    reads("a permission given to a built-in role is read as its own", ROLE_CPP, helpdesk_end,
          helpdesk_end.replace("         return set;", "         set |= Bit(RolePermission::ManageAccounts);" + nl
                               + "         return set;"),
          lambda d: d.roles["helpdesk"] == decision.roles["helpdesk"] | {"ManageAccounts"}
          and decision.decide("RouteAccountCreate", helpdesk) == "no" and d.decide("RouteAccountCreate", helpdesk) == "own")
    reads("a permission named in a comment of a built-in role's function is not read", ROLE_CPP,
          "         set |= Bit(RolePermission::ManageAccounts);",
          "         set |= Bit(RolePermission::ManageAccounts);" + nl + "         // set |= Bit(RolePermission::ManageServer);",
          lambda d: d.roles == decision.roles)

    # 15. The code is read as the compiler's first three translation phases
    # read it: a directive written over a line splice, with a comment in it or
    # after whitespace is one, a comment a digit separator or a raw string
    # literal holds is none, and what a literal holds is not code - so that a
    # definition held under a conditional so written, or a copy the compiler
    # reads as a comment or a literal, stops the script as the plain forms do;
    # and the forms the server's source is written in are read through.
    def current(label, sources):
        try:
            same = run(sources)[2] == text
        except GenerationError as error:
            failures.append("%s: it stopped: %s" % (label, str(error).split(nl)[0]))
            return
        expect(label, same)

    allowed_begin = "   bool" + nl + "   RestApiServer::IsDomainAllowed_("
    allowed_end = nl + "   }" + nl
    every_domain = lambda region: region.replace("      if (domains.empty())", "      if (true)")
    for label, opening in (
            ("a line splice between # and if", "#\\" + nl + "if 0"),
            ("a comment between # and if", "#/**/if 0"),
            ("a comment over a line's end between # and if", "#/*" + nl + "*/if 0"),
            ("spaces before the #", "   #if 0"),
            ("a comment before the #", "/* a note */ #if 0"),
            ("the digraph %: for the #", "%:if 0"),
            ("a raw string literal holding /* before it", '   static const char *const QuotedNote_ = R"(" /* )";' + nl
             + "#if 0" + nl + "   // */")):
        stopped("IsDomainAllowed_ under an #if 0 written with %s, a copy that reaches every domain under its #else"
                % label, under_conditional(REST_CPP, allowed_begin, allowed_end, every_domain, opening),
                "conditional of the preprocessor")
    stopped("HelpdeskPermissions under an #if 0 written over a line splice, a copy that takes in another role's "
            "permissions under its #else",
            under_conditional(ROLE_CPP, "      unsigned __int64 HelpdeskPermissions()", nl + "      }" + nl,
                              lambda region: region.replace("         return set;", "         set |= "
                                                            "DomainAdminPermissions();" + nl + "         return set;"),
                              "#\\" + nl + "if 0"),
            "conditional of the preprocessor")
    stopped("the report's table, its count and FindSection under an #if 0 written over a line splice, a copy under its "
            "#else",
            under_conditional(REPORTS_CPP, "      struct SectionInfo" + nl, find_end,
                              lambda region: region.replace('{ "countries", true,', '{ "countries", false,'),
                              "#\\" + nl + "if 0"),
            "conditional of the preprocessor")
    stops("a copy of IsDomainAllowed_ in a comment a digit separator does not open, the one compiled reaching every "
          "domain, stops the script", REST_CPP, allowed,
          "   static const int CommentWidth_ = 1'0; /* '" + nl + allowed + "   // */" + nl + every_domain(allowed),
          "RestApiServer::IsDomainAllowed_")
    stops("a copy of HelpdeskPermissions in a comment a digit separator does not open, the one compiled taking in "
          "another role's permissions, stops the script", ROLE_CPP, helpdesk_whole,
          "      static const int CommentWidth_ = 1'0; /* '" + nl + helpdesk_whole + "      // */" + nl
          + helpdesk_whole.replace("         return set;", "         set |= DomainAdminPermissions();" + nl
                                + "         return set;"), "HelpdeskPermissions")
    stops("a copy of IsDomainAllowed_ in a comment begun on a directive's line, the one compiled reaching every "
          "domain, stops the script", REST_CPP, allowed,
          "#define HM_ALLOWED_NOTE_ /*" + nl + allowed + "*/" + nl + every_domain(allowed),
          "RestApiServer::IsDomainAllowed_")
    stops("a copy of IsDomainAllowed_ in a raw string literal before it, the one compiled reaching every domain, "
          "stops the script", REST_CPP, allowed,
          '   static const char *const AllowedCopy_ = R"copy(' + nl + allowed + ')copy";' + nl + every_domain(allowed),
          "RestApiServer::IsDomainAllowed_")
    dr, rr, tr = run(broken_copy("Common/Util/CalDavServer.cpp", "   const char *CalDavServer::CalendarsPath = ",
                                 '   static const char *const OldPath_ = R"(CalDavServer::CalendarsPath = "/dav/old/";)";'
                                 + nl + "   const char *CalDavServer::CalendarsPath = "))
    expect("a constant a route parser names, its definition in a raw string literal before it, is not read", tr == text)
    stopped("a SectionCount of another length, its right form in a string literal before it",
            broken_copy(REPORTS_CPP, count, '      static const char *const CountNote_ = "const int SectionCount = '
                        '(int) (sizeof(SECTIONS) / sizeof(SECTIONS[0]));";' + nl + "      const int SectionCount = 8;"),
            "SectionCount")
    key_reason = '"the api keys and the [Settings] section of hMailServer.ini, including'
    reads("a case label in a string literal of Authorize_'s block of API-key routes is not read", REST_CPP, key_reason,
          '"the api keys and the [Settings] section (case RouteApiKeyList: among them) of hMailServer.ini, including',
          lambda d: d.role_key_routes == decision.role_key_routes)
    reads("the dispatch's switch named in a string literal before it is not read there", REST_CPP,
          "could not be read just now; try again in a moment\\\",",
          "could not be read just now (switch (route.kind) { case it: }); try again in a moment\\\",",
          lambda d: d.handler == decision.handler and d.handler_args == decision.handler_args)
    # The forms this script does not read stop it, rather than being read one way
    # by it and another by the compiler.
    stopped("a backslash with a space after it at a line's end",
            broken_copy(REST_CPP, allowed, "   // a note that ends in a backslash and a space \\ " + nl + allowed),
            "only spaces or tabs after it")
    stopped("a raw string literal holding a line splice",
            broken_copy(REST_CPP, allowed, '   static const char *const SplicedNote_ = R"(a \\' + nl + 'b)";' + nl
                        + allowed), "holds a backslash that ends a line")
    stopped("a raw string literal over more than one line in Authorize_",
            broken_copy(REST_CPP, '"the api keys and the [Settings] section of hMailServer.ini, including the list of '
                        'what this server has changed from stock, answer to the administrator password only"',
                        'R"(the api keys and the [Settings] section of hMailServer.ini, including the list of what this'
                        + nl + 'server has changed from stock, answer to the administrator password only)"'),
            "raw string literal over more than one line")
    stopped("a carriage return that ends no line",
            broken_copy(REST_CPP, allowed, "   // a note with a carriage return\rin it" + nl + allowed),
            "carriage return that ends no line")
    # The forms the server's source is written in elsewhere are read through,
    # at the file scope of RestApiServer.cpp, and leave the matrix as it is.
    scope = "   bool" + nl + "   RestApiServer::HoldsServerAuthority_("
    for label, line in (
            ("an #include after spaces", "   #include <algorithm>"),
            ("an #ifdef and its #endif after spaces", "   #ifdef HM_TRACE_AUTHORISATION" + nl + "   #endif"),
            ("a number with digit separators", "   static const int LargestBatch_ = 10'000;"),
            ("a raw string literal holding an odd number of quotes, /* and #if 0",
             '   static const char *const QuotedExample_ = R"x(a "quoted /* example' + nl + "#if 0" + nl + ')x";'),
            ("a raw string literal holding IsDomainAllowed_ as it is compiled",
             '   static const char *const AllowedExample_ = R"copy(' + nl + allowed + ')copy";'),
            ("a macro defined over line splices, after spaces",
             "   #define HM_ROUTE_TRACE_(x) \\" + nl + "      do { (void) (x); } \\" + nl + "      while (0)"),
            ("a string literal with a prefix and a character literal with one",
             '   static const char16_t *const Wide_ = u"a \\" /* b"; static const char32_t Quote_ = U\'"\';')):
        current("%s is read through" % label, broken_copy(REST_CPP, scope, line + nl + nl + scope))
    for label, path in (("over a line splice", '"/api/v1/sta\\' + nl + 'tus"'),
                        ("as a raw string literal", 'R"(/api/v1/status)"'), ("as a u8 literal", 'u8"/api/v1/status"')):
        current("a route's path written %s is read as the path it is" % label,
                broken_copy(REST_CPP, 'path == "/api/v1/status"', "path == " + path))
    reads("two permissions on one line of the enumeration are read as two", "Common/Application/RoleManager.h",
          "      ManageAccounts,        // create, change and delete accounts, aliases, lists, groups" + nl
          + "      ResetPassword,", "      ManageAccounts, ResetPassword,",
          lambda d: d.tokens == decision.tokens and d.roles == decision.roles)
    reads("two route kinds on one line of the enumeration are read as two", REST_H,
          "         RouteApiKeyList," + nl + "         RouteApiKeyCreate,",
          "         RouteApiKeyList, RouteApiKeyCreate,", lambda d: d.kinds == decision.kinds)

    # 16. An enumerator is read as its place: RolePermission's is the bit Bit
    # sets and the place of its token, so a value written that is not its place
    # - one permission given another's bit - stops the script, as does a value
    # it does not evaluate; and so for the route kinds.
    role_h = "Common/Application/RoleManager.h"
    aliased = broken_copy(role_h, "      ManageAccounts,", "      ManageAccounts = 9,")
    aliased[role_h] = aliased[role_h].replace("      ResetPassword,", "      ResetPassword = 12,", 1)
    stopped("a permission given another's value, the next given back its own", aliased, "its place in the enumeration")
    for written in ("0x0B", "1'1", "013"):
        reads("a permission's value written %s, its place, is read as its place" % written, role_h,
              "      ManageAccounts,", "      ManageAccounts = %s," % written,
              lambda d: d.tokens == decision.tokens and d.roles == decision.roles)
    stopped("a permission's value written as an expression",
            broken_copy(role_h, "      ManageAccounts,", "      ManageAccounts = 11 + 0,"), "integer literal")
    stopped("a route kind given another's value",
            broken_copy(REST_H, "         RouteApiKeyCreate,", "         RouteApiKeyCreate = 2,"),
            "its place in the enumeration")
    expect("integer literals are read as the compiler reads them",
           [integer_value(t) for t in ("0", "017", "0x1F", "0b101", "10'000", "42ULL", "0xFFu", "1.5", "08")]
           == [0, 15, 31, 5, 10000, 42, 255, None, None])

    # 17. A directive of the preprocessor inside what the script reads - an
    # enumeration, PermissionTokens, a constant's initialiser, which its parser
    # reads, and a function it reads from the line that names it to its '}',
    # its parameter list among it: a route parser, a predicate, either
    # listener's ProcessRequest_, a dispatch's return its steps mask among it,
    # or a function whose steps it holds - stops the script, as a conditional
    # does, naming its line: an #include there brings in code it never reads,
    # and it does not read what another directive does. The file named need
    # not exist, since the script does not open it.
    def directive_stops(label, rel, old, new, directive, whole):
        sources = broken_copy(rel, old, new)
        line = sources[rel][:sources[rel].find(directive)].count(nl) + 1
        shown = directive[directive.find("%:") if directive.startswith("%:") else directive.find("#"):]
        said = "a directive of the preprocessor at line %d, %s," % (line, shown)
        (refused if whole else stopped)("a directive inside %s" % label, sources, said)

    include = '#include "Extra.inc"'
    for label, directive in (("an #include", include),
                             ("an #include after spaces and a comment", '   /* the next ones */ #include "Extra.inc"'),
                             ("an #include written with the digraph %:", '%:include "Extra.inc"'),
                             ("a #define", "#define HM_PERMISSION_NOTE_ 1")):
        directive_stops("RolePermission's body, %s" % label, role_h, "      ManageAccounts,",
                        directive + nl + "      ManageAccounts,", directive, False)
    directive_stops("RouteKind's body, an #include", REST_H, "         RouteApiKeyList,",
                    include + nl + "         RouteApiKeyList,", include, False)
    directive_stops("PermissionTokens, an #include", ROLE_CPP, '         "view_accounts",',
                    include + nl + '         "view_accounts",', include, False)
    directive_stops("a constant a route parser names, an #include between its = and its literal",
                    "Common/Util/CalDavServer.cpp", '   const char *CalDavServer::CalendarsPath = "',
                    "   const char *CalDavServer::CalendarsPath =" + nl + include + nl + '      "', include, True)
    for label, rel, old in (
            ("ParseRoute_", REST_CPP, '      const AnsiString apiKeysPath = "/api/v1/apikeys";' + nl),
            ("ParseScimRoute_", SUB_PARSERS["ParseScimRoute_"], '      const AnsiString prefix = "/scim/v2/";' + nl),
            ("a route parser's predicate", "Common/Util/CalDavServer.cpp",
             '      return path == "/dav/calendars" || path.StartsWith(CalendarsPath);' + nl),
            ("ProcessRequest_'s anonymous chain", REST_CPP, '      if (method == "GET" && path == "/api/v1/help")' + nl),
            ("a return of ProcessRequest_'s dispatch written over several lines", REST_CPP,
             "                                          HttpsClient::IsLoopbackHost(caller.peer.ToString()) &&" + nl),
            ("the web services listener's ProcessRequest_", WEB_CPP,
             "      AnsiString host = GetRequestHost_(request);" + nl)):
        directive_stops("%s, an #include" % label, rel, old, include + nl + old, include, True)
    directive_stops("a route parser's parameter list, an #include", SUB_PARSERS["ParseScimRoute_"],
                    "RestApiServer::ParseScimRoute_(const AnsiString &method, const AnsiString &path, Route &route)",
                    "RestApiServer::ParseScimRoute_(const AnsiString &method," + nl + include + nl
                    + "      const AnsiString &path, Route &route)", include, True)
    directive_stops("a built-in role's function, an #include", ROLE_CPP, helpdesk_end,
                    helpdesk_end.replace("         return set;", include + nl + "         return set;"), include, False)
    directive_stops("the parameter list of a function whose steps are held, an #include", REST_CPP,
                    "RestApiServer::IsDomainAllowed_(const std::vector<String> &domains, const String &domainName)",
                    "RestApiServer::IsDomainAllowed_(const std::vector<String> &domains," + nl + include + nl
                    + "      const String &domainName)", include, False)

    # 18. A comparison is not a definition: PermissionTokens, the report's table
    # and SectionCount compared with == in a function the script does not hold
    # leave the matrix as it is, where the test for a second definition must not
    # take the first = of == for one.
    civil = "      long long DaysFromCivil(int year, int month, int day)"
    for label, rel, old, new in (
            ("an element of PermissionTokens", ROLE_CPP,
             '         return "";' + nl + nl + "      return PermissionTokens[index];",
             '         return "";' + nl + nl + "      if (PermissionTokens[index] == nullptr)" + nl + '         return "";'
             + nl + nl + "      return PermissionTokens[index];"),
            ("SectionCount", REPORTS_CPP, civil,
             "      bool NoSectionFound(const SectionInfo *info)" + nl + "      {" + nl
             + "         return info != 0 && SectionCount == 0;" + nl + "      }" + nl + nl + civil),
            ("an element of the report's table", REPORTS_CPP, civil,
             "      bool IsFirstSection(const SectionInfo *info)" + nl + "      {" + nl
             + "         return &SECTIONS[0] == info;" + nl + "      }" + nl + nl + civil)):
        current("%s compared with == in a function the script does not hold is read through" % label,
                broken_copy(rel, old, new))

    # 14. The documents' index says what is held as the matrix does: where its
    # row for the matrix names ProcessRequest_, it names the part of it held.
    with open(os.path.join(ROOT, "hmailserver", "docs", "README.md"), encoding="utf-8") as handle:
        row = [line for line in handle if line.startswith("| [WebAuthorisationMatrix.md](WebAuthorisationMatrix.md) |")]
    held = "`ProcessRequest_` from the end of the anonymous routes to the handler"
    expect("the documents' index has one row for the matrix", len(row) == 1)
    expect("the documents' index names the part of ProcessRequest_ the matrix holds wherever it names ProcessRequest_",
           all(r.count("`ProcessRequest_`") == r.count(held) for r in row))

    if failures:
        for failure in failures:
            print("generate-auth-matrix self-test: FAILED: %s" % failure, file=sys.stderr)
        return 1
    print("generate-auth-matrix self-test: %d rows read; every negative control fired." % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
