#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The web authorisation matrix: every route the server's web listeners serve, by
every kind of credential, read from the code.

Writes hmailserver/docs/WebAuthorisationMatrix.md. Nothing in that file is typed
by hand: every row comes from the C++ that decides it, so that the matrix says
what the server does rather than what somebody once meant it to do, and --check
fails the moment the two part.

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
  Util/RestApiServer.cpp, Authorize_ and the functions it asks
      IsSessionRoute_, IsSelfServiceRoute_, IsScimRoute_, IsApiKeyRoute_,
      PermissionFor_, SecondPermissionFor_, DomainNamedByRoute_, the refusals a
      credential confined to named domains meets, and the role-holding account's
      exceptions to the API-key routes. The decision below follows Authorize_'s
      order, step for step; build/check-role-choke-point.py already holds every
      RouteKind to a classification, and this script holds the decision to the
      same reading.
  Application/RoleManager.cpp
      the four built-in roles' permissions.
  Util/WebServicesServer.cpp, ProcessRequest_
      the second listener: MTA-STS, autoconfiguration, the Web Key Directory, the
      S/MIME authority, ACME, security.txt, and the CalDAV and CardDAV trees.

Usage:
  python build/generate-auth-matrix.py            # write the matrix
  python build/generate-auth-matrix.py --check    # fail if the committed one is stale
  python build/generate-auth-matrix.py --print    # to stdout
  python build/generate-auth-matrix.py --self-test
"""

import copy
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SERVER = os.path.join(ROOT, "hmailserver", "source", "Server")
DOC = os.path.join(ROOT, "hmailserver", "docs", "WebAuthorisationMatrix.md")

REST_H = "Common/Util/RestApiServer.h"
REST_CPP = "Common/Util/RestApiServer.cpp"
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
    # static const char * members the predicates name
    "CalendarsPath": ("Common/Util/CalDavServer.cpp", r'CalDavServer::CalendarsPath\s*=\s*"([^"]*)"'),
    "ContextPath": ("Common/Util/CardDavServer.cpp", r'CardDavServer::ContextPath\s*=\s*"([^"]*)"'),
}

METHODS = ("GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "PROPFIND", "PROPPATCH", "REPORT", "MKCOL",
           "MKCALENDAR", "COPY", "MOVE", "LOCK", "UNLOCK")


class GenerationError(Exception):
    pass


def read(rel, sources=None):
    if sources is not None and rel in sources:
        return sources[rel]
    with open(os.path.join(SERVER, rel), encoding="utf-8", errors="replace") as handle:
        return handle.read().replace("\r\n", "\n")


# ---------------------------------------------------------------------------
# A tokenizer and parser for the subset of C++ the route functions are written in
# ---------------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<pp>^\#[^\n]*)
  | (?P<str>(?:L|u8)?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>'(?:[^'\\\n]|\\.)*')
  | (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+(?:\.\d+)?[uUlLfF]*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>::|->|\+\+|--|<<=|>>=|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|[-+*/%<>=!&|^~?:;,.(){}\[\]])
""", re.S | re.X | re.M)


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


class Tok(object):
    __slots__ = ("kind", "text", "line")

    def __init__(self, kind, text, line):
        self.kind = kind
        self.text = text
        self.line = line

    def __repr__(self):
        return "%s:%r@%d" % (self.kind, self.text, self.line)


def tokenize(text, first_line=1):
    tokens = []
    line = first_line
    pos = 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise GenerationError("cannot tokenize near line %d: %r" % (line, text[pos:pos + 40]))
        kind = m.lastgroup
        value = m.group(kind)
        if kind == "str":
            literal = value[value.index('"') + 1:-1]
            # adjacent literals are one literal, as in C++
            if tokens and tokens[-1].kind == "str":
                tokens[-1].text += unescape(literal)
            else:
                tokens.append(Tok("str", unescape(literal), line))
        elif kind == "chr":
            tokens.append(Tok("chr", unescape(value[1:-1]), line))
        elif kind in ("num", "id", "op"):
            tokens.append(Tok(kind, value, line))
        line += value.count("\n")
        pos = m.end()
    tokens.append(Tok("eof", "", line))
    return tokens


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
            text = tok.text.rstrip("uUlLfF")
            try:
                value = int(text, 0)
            except ValueError:
                value = 0
            return ("num", tok.line, value)
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


def function_text(text, qualified):
    """The body of a member function, '{' to its closing '}' at the definition's indent,
    and the line its '{' is on."""
    m = re.search(r"^([ \t]*)[^\n]*\b" + re.escape(qualified) + r"\s*\([^;{]*?\)\s*(?:const\s*)?\n\1\{", text, re.M | re.S)
    if not m:
        return None, 0
    indent = m.group(1)
    open_at = m.end() - 1
    close = text.find("\n" + indent + "}\n", open_at)
    if close < 0:
        return None, 0
    return text[open_at:close + len(indent) + 2], text.count("\n", 0, open_at) + 1


def parse_function(text, qualified):
    body, line = function_text(text, qualified)
    if body is None:
        return None, 0
    parser = Parser(tokenize(body, line))
    return parser.block(), line


def function_params(text, qualified):
    m = re.search(re.escape(qualified) + r"\s*\(([^)]*)\)", text)
    if not m:
        return []
    params = []
    for part in m.group(1).split(","):
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
        ast, _ = parse_function(text, qualified)
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
        ast = Parser(tokenize(body, line)).block()
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
    for name, (rel, pattern) in CONSTANTS.items():
        m = re.search(pattern, read(rel, sources))
        if not m:
            raise GenerationError("%s: the constant %s could not be read" % (rel, name))
        constants[name] = m.group(1)
    functions["__constants__"] = constants
    return functions


def read_rest_routes(sources):
    """[(kind, methods, template text, line)] from ParseRoute_ and its sub-parsers."""
    text = read(REST_CPP, sources)
    functions = load_functions(sources)
    ast, _ = parse_function(text, "RestApiServer::ParseRoute_")
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
    ast, _ = parse_function(text, qualified)
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

def body_of(text, qualified):
    body, _ = function_text(text, qualified)
    return body


def route_kinds(header):
    m = re.search(r"enum RouteKind\s*\{(.*?)\n      \};", header, re.S)
    if not m:
        return []
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return re.findall(r"^\s*(Route\w+)\s*(?:=\s*\d+)?\s*,?\s*$", body, re.M)


def case_set(body):
    return set(re.findall(r"case (Route\w+):", re.sub(r"//[^\n]*", "", body or "")))


def case_groups(body, answer):
    """{kind: answer-group} for a switch whose groups end in a line matching answer."""
    mapping = {}
    pending = []
    for line in re.sub(r"//[^\n]*", "", body or "").split("\n"):
        label = re.match(r"\s*case (Route\w+):\s*$", line)
        if label:
            pending.append(label.group(1))
            continue
        m = re.match(answer, line)
        if m and pending:
            for kind in pending:
                mapping[kind] = m.group(1)
            pending = []
    return mapping


def builtin_permissions(role_cpp, function):
    index = role_cpp.find("unsigned __int64 " + function + "()")
    if index < 0:
        return None
    end = role_cpp.find("\n      }\n", index)
    return set(re.findall(r"Bit\(RolePermission::(\w+)\)", role_cpp[index:end]))


def permission_tokens(role_cpp):
    m = re.search(r"PermissionTokens\[\]\s*=\s*\{(.*?)\};", role_cpp, re.S)
    return re.findall(r'"(\w+)"', m.group(1)) if m else []


def permission_enum(role_h):
    m = re.search(r"enum class RolePermission\s*\{(.*?)\};", role_h, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1)) if m else ""
    names = re.findall(r"^\s*(\w+)\s*(?:=\s*\d+)?\s*,?\s*$", body, re.M)
    return [n for n in names if n != "Count"]


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
        self.permission = case_groups(body_of(cpp, "RestApiServer::PermissionFor_"),
                                      r"\s*return RolePermission::(\w+);\s*$")
        self.second = case_groups(body_of(cpp, "RestApiServer::SecondPermissionFor_"),
                                  r"\s*permission = RolePermission::(\w+);\s*$")
        default = re.search(r"default:\s*break;\s*\}\s*return RolePermission::(\w+);",
                            re.sub(r"//[^\n]*", "", body_of(cpp, "RestApiServer::PermissionFor_") or ""))
        self.default_permission = default.group(1) if default else None
        named = case_groups(body_of(cpp, "RestApiServer::DomainNamedByRoute_"), r"\s*return (.+);\s*$")
        self.domain_named = {}
        for kind, expr in named.items():
            if "ExtractDomain" in expr:
                self.domain_named[kind] = "the address's domain"
            elif "DomainOf" in expr:
                self.domain_named[kind] = "the address's or domain's"
            elif "identifier" in expr:
                self.domain_named[kind] = "the domain"
            else:
                self.domain_named[kind] = expr
        authorize = body_of(cpp, "RestApiServer::Authorize_") or ""
        self.authorize_text = authorize
        # The role-holding account's exceptions inside the API-key block
        m = re.search(r"if \(IsApiKeyRoute_\(route\.kind\)\)(.*?)// THE DECISION", authorize, re.S)
        self.role_key_routes = case_set(m.group(1)) if m else set()
        # The refusals a credential confined to named domains meets
        after = authorize.split("if (caller.domains.empty())", 1)
        if len(after) != 2:
            raise GenerationError("%s: Authorize_'s confinement section could not be found" % REST_CPP)
        confined = after[1]
        self.confined_refused = set()
        for block in re.finditer(r"if \(((?:route\.kind == Route\w+\s*(?:\|\|)?\s*)+)\)\s*\{\s*refusalReason = \"this credential is restricted",
                                 confined, re.S):
            self.confined_refused |= set(re.findall(r"Route\w+", block.group(1)))
        sw = re.search(r"switch \(route\.kind\)\s*\{(.*?)refusalReason = \"this credential is restricted to named domains, and that resource is server-wide\"",
                       confined, re.S)
        if not sw:
            raise GenerationError("%s: Authorize_'s list of server-wide routes could not be found" % REST_CPP)
        self.confined_refused |= case_set(sw.group(1))
        self.report_kind = "RouteReport" if "route.kind == RouteReport" in confined else None
        # Dispatch: kind -> handler call text
        process = body_of(cpp, "RestApiServer::ProcessRequest_") or ""
        dispatch = process.split("switch (route.kind)", 1)
        if len(dispatch) != 2:
            raise GenerationError("%s: ProcessRequest_'s dispatch could not be found" % REST_CPP)
        self.handler = case_groups(dispatch[1], r"\s*return (\w+)\((.*)\);\s*$")
        self.handler_args = {}
        pending = []
        for line in re.sub(r"//[^\n]*", "", dispatch[1]).split("\n"):
            label = re.match(r"\s*case (Route\w+):\s*$", line)
            if label:
                pending.append(label.group(1))
                continue
            m2 = re.match(r"\s*return (\w+)\((.*)\);\s*$", line)
            if m2 and pending:
                for kind in pending:
                    self.handler[kind] = m2.group(1)
                    self.handler_args[kind] = m2.group(2)
                pending = []
        roles = {
            "serveradmin": builtin_permissions(role_cpp, "ServerAdminPermissions"),
            "domainadmin": builtin_permissions(role_cpp, "DomainAdminPermissions"),
            "helpdesk": builtin_permissions(role_cpp, "HelpdeskPermissions"),
            "auditor": builtin_permissions(role_cpp, "AuditorPermissions"),
        }
        enum = permission_enum(role_h)
        roles["serveradmin"] = set(enum)
        self.roles = roles
        self.tokens = dict(zip(enum, permission_tokens(role_cpp)))
        for name, held in roles.items():
            if held is None or (name != "serveradmin" and not held):
                raise GenerationError("%s: the built-in role %s could not be read" % (ROLE_CPP, name))
        for label, value in (("IsSessionRoute_", self.session), ("IsSelfServiceRoute_", self.self_service),
                             ("IsScimRoute_", self.scim), ("IsApiKeyRoute_", self.api_key),
                             ("PermissionFor_", self.permission), ("DomainNamedByRoute_", self.domain_named),
                             ("the dispatch", self.handler), ("the role-holder's key-route exceptions", self.role_key_routes)):
            if not value:
                raise GenerationError("%s: %s could not be read" % (REST_CPP, label))

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
        full = "AdministerServer" in held
        first = self.permission.get(kind, self.default_permission)
        needed = [first] + ([self.second[kind]] if kind in self.second else [])
        if not full and not all(p in held for p in needed):
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
    if path.startswith("/api/v1/me"):
        return "The account's own: /api/v1/me"
    if path.startswith("/api/v1/accounts"):
        return "Accounts by address: /api/v1/accounts"
    if path.startswith("/api/v1/domains"):
        return "Domains: /api/v1/domains"
    if path.startswith("/api/v1/settings") or path.startswith("/api/v1/ini"):
        return "Settings: /api/v1/settings and the ini file"
    if path.startswith("/scim"):
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


def findings(decision, rows):
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
      "file disagree. The threat model that reads it is [WebThreatModel.md](WebThreatModel.md); the routes this "
      "file flags are triaged there.")
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
      "`PermissionFor_` and `SecondPermissionFor_`, then the refusals a credential confined to named domains "
      "meets, then `DomainNamedByRoute_`. Then the built-in roles' permissions from `RoleManager.cpp`.")
    w("- **Not shown**, because they narrow every route alike: the Host check (421), the request budget per "
      "credential (429), the administrator password's throttle, the `X-Requested-With: hMailServer` header a "
      "session must send on anything but GET and HEAD (403), the refusal of a request that carries a credential "
      "and changes something when the browser reports it as sent from another site (403), the mailbox-key "
      "refusal of an app password on an encrypted mailbox, and `ChangesAccountByAddress_`'s refusal of an "
      "account the credential does not take in. Those are in WebThreatModel.md.")
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
    w("| Server admin account | An account whose level is server administrator (or assigned `serveradmin`): every domain, but an account. |")
    w("| Admin password | The `administrator` password from hMailServer.ini (HTTP Basic, or the Control Deck's session made from it). |")
    w("| Key, full | An API key with `Scope=full` and no domain list (`serveradmin`). |")
    w("| Key, read-only | An API key with `Scope=readonly` (`auditor`). |")
    w("| Key, one domain | A `Scope=full` key issued for named domains. |")
    w("")
    w("| Cell | Meaning |")
    w("|---|---|")
    w("| yes | Allowed. |")
    w("| own | Allowed where the domain the route names (in its path) is one the credential reaches; another domain's answers 403. |")
    w("| own+ | The report route: allowed for a named domain the credential reaches; the sections counted from server-wide metrics are refused. |")
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
    flagged = findings(decision, rows)
    labels = {
        "F-handler-blind": "A confined role reaches it, it names no domain, and the handler cannot narrow it",
        "F-handler-confines": "A confined role reaches it, it names no domain, and the handler is given the caller",
        "F-mutating-get": "A change reached by GET",
        "F-undispatched": "Authorised but never dispatched",
        "F-unparsed": "A route kind no path reaches",
    }
    for code in ("F-handler-blind", "F-mutating-get", "F-undispatched", "F-unparsed", "F-handler-confines"):
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


def main(argv):
    mode = "write"
    if "--check" in argv:
        mode = "check"
    elif "--print" in argv:
        mode = "print"
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
    what it writes - so that a script that has stopped seeing the source fails."""
    failures = []
    base = {}
    for rel in [REST_H, REST_CPP, ROLE_CPP, WEB_CPP, "Common/Application/RoleManager.h"] + list(SUB_PARSERS.values()) \
            + list(PREDICATES.values()) + [c[0] for c in CONSTANTS.values()]:
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
        run(broken_copy(REST_CPP, "Caller caller = Authenticate_(request, peer_address);",
                        "Caller caller = Authenticate_(request, peer_address);" + nl + "try { }"))
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

    if failures:
        for failure in failures:
            print("generate-auth-matrix self-test: FAILED: %s" % failure, file=sys.stderr)
        return 1
    print("generate-auth-matrix self-test: %d rows read; every negative control fired." % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
