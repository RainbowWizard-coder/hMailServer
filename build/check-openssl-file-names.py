#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A file's name handed to OpenSSL is the bytes FileUtilities::OpenSslPath gave.

OpenSSL's fopen on Windows (crypto/o_fopen.c, openssl_fopen) reads a name as UTF-8
first and opens the file at that reading when one is there, falling back to the
code page's reading only when none is. The server handed its own files to OpenSSL
in the ANSI code page's bytes (FileUtilities::NarrowPath, AnsiString(String)), so a
name whose bytes outside ASCII also read as UTF-8 - U+00C3 U+00A9 on code page
1252 are C3 A9, which is U+00E9 - was read from another file whenever one was at
that other name: a listener's certificate and key, a port's client-certificate CA
file, the object store's CA file, OAuth2PublicKeyFile, the ACME client's files.
The same shape was found twice, in the database clients' CA files and then in the
server's own files. FileUtilities::OpenSslPath is the one conversion now: on
Windows it refuses such a name (HM7536), on Linux it is NarrowPath's UTF-8.

This fails, in the server's C and C++ (hmailserver/source/Server), comments and
strings skipped, on a call of one of OpenSSL's functions that open a file by its
name - BIO_new_file, BIO_read_filename and the other BIO_*_filename,
SSL_load_client_CA_file, SSL_add_file_cert_subjects_to_stack and
SSL_add_dir_cert_subjects_to_stack, SSL_CTX_* and SSL_use_* *_file,
SSL_CTX_load_verify_locations, SSL_CTX_load_verify_file and _dir,
X509_LOOKUP_load_file, X509_load_*_file, X509_STORE_load_* - or of Boost.Asio's
ssl::context members that reach them (use_certificate_file,
use_certificate_chain_file, use_private_key_file, use_rsa_private_key_file,
use_tmp_dh_file, load_verify_file, add_verify_path), whose name argument is not a
variable that FileUtilities::OpenSslPath (or AcmeClient's OpenSslPathOf_, which
calls it) filled: the variable alone, its .c_str(), or std::string of either. The
fill has to be in the block of the call or in one around it, before it, with no
assignment to the variable between. A NarrowPath, an AnsiString(...), a literal or
any other expression in that place is refused, as is a variable filled in another
function or in a block the call is not inside. nullptr or NULL, which names no
file, is allowed. A call that must hand over another name is allowed when its line
or one of the three above it carries "openssl-file-name-exempt:" and the reason.

Run from the repository root: python build/check-openssl-file-names.py
Pass --self-test to run it against made-up files and against the tree with each
call's name put back to NarrowPath, and prove it can fail.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREES = [os.path.join(ROOT, "hmailserver", "source", "Server")]
EXTENSIONS = (".cpp", ".h", ".hpp", ".c", ".cc", ".inl")
EXEMPT = "openssl-file-name-exempt:"

# Free functions, by the indexes (from 0) of their arguments that are file names.
FREE = {
    "BIO_new_file": (0,),
    "BIO_read_filename": (1,),
    "BIO_write_filename": (1,),
    "BIO_append_filename": (1,),
    "BIO_rw_filename": (1,),
    "SSL_load_client_CA_file": (0,),
    "SSL_add_file_cert_subjects_to_stack": (1,),
    "SSL_add_dir_cert_subjects_to_stack": (1,),
    "SSL_CTX_use_certificate_file": (1,),
    "SSL_CTX_use_certificate_chain_file": (1,),
    "SSL_CTX_use_PrivateKey_file": (1,),
    "SSL_CTX_use_RSAPrivateKey_file": (1,),
    "SSL_CTX_use_serverinfo_file": (1,),
    "SSL_use_certificate_file": (1,),
    "SSL_use_certificate_chain_file": (1,),
    "SSL_use_PrivateKey_file": (1,),
    "SSL_use_RSAPrivateKey_file": (1,),
    "SSL_CTX_load_verify_locations": (1, 2),
    "SSL_CTX_load_verify_file": (1,),
    "SSL_CTX_load_verify_dir": (1,),
    "X509_LOOKUP_load_file": (1,),
    "X509_LOOKUP_add_dir": (1,),
    "X509_load_cert_file": (1,),
    "X509_load_crl_file": (1,),
    "X509_load_cert_crl_file": (1,),
    "X509_STORE_load_locations": (1, 2),
    "X509_STORE_load_file": (1,),
    "X509_STORE_load_path": (1,),
}

# Boost.Asio's ssl::context members, called with . or ->, whose first argument is
# a file's name.
MEMBERS = ("use_certificate_file", "use_certificate_chain_file", "use_private_key_file",
           "use_rsa_private_key_file", "use_tmp_dh_file", "load_verify_file", "add_verify_path")

FREE_CALL = re.compile(r"(?<![\w.>])(?:::\s*)?(" + "|".join(sorted(FREE)) + r")\s*\(")
MEMBER_CALL = re.compile(r"(?:\.|->)\s*(" + "|".join(MEMBERS) + r")\s*\(")
FILL_CALL = re.compile(r"\b(OpenSslPath|OpenSslPathOf_)\s*\(")
IDENTIFIER = r"[A-Za-z_]\w*"
ACCEPTED = [
    re.compile(r"^(" + IDENTIFIER + r")$"),
    re.compile(r"^(" + IDENTIFIER + r")\s*\.\s*c_str\s*\(\s*\)$"),
    re.compile(r"^std\s*::\s*string\s*\(\s*(" + IDENTIFIER + r")\s*\)$"),
    re.compile(r"^std\s*::\s*string\s*\(\s*(" + IDENTIFIER + r")\s*\.\s*c_str\s*\(\s*\)\s*\)$"),
]
NO_NAME = {"nullptr", "NULL"}
KEYWORDS = {"true", "false", "this", "nullptr", "NULL", "return", "const", "auto"}


def without_comments(text):
    """The text with comments blanked and string and character literals emptied, line breaks kept."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
            continue
        c = text[i]
        if c == '"' or (c == "'" and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"))):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def arguments(code, open_paren):
    """The top-level arguments of the call whose ( is at open_paren, stripped, and the offset after its )."""
    depth = 0
    args = []
    start = open_paren + 1
    i = open_paren
    while i < len(code):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(code[start:i].strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append(code[start:i].strip())
            start = i + 1
        i += 1
    args.append(code[start:].strip())
    return args, len(code)


def brace_depths(code):
    """The depth of braces at each offset of the code."""
    depths = []
    depth = 0
    for c in code:
        if c == "{":
            depth += 1
        depths.append(depth)
        if c == "}":
            depth -= 1
    return depths


def exempt(raw_lines, number):
    """Whether line number (1-based) or one of the three above it carries the marker and a reason."""
    for index in range(max(0, number - 4), min(number, len(raw_lines))):
        line = raw_lines[index]
        at = line.find(EXEMPT)
        if at >= 0 and line[at + len(EXEMPT):].strip():
            return True
    return False


def calls_of(code):
    """(function, offset of the call, the name arguments) for every call of a function that opens a file by name."""
    found = []
    for match in FREE_CALL.finditer(code):
        args, _ = arguments(code, match.end() - 1)
        names = [args[i] for i in FREE[match.group(1)] if i < len(args)]
        found.append((match.group(1), match.start(1), names))
    for match in MEMBER_CALL.finditer(code):
        args, _ = arguments(code, match.end() - 1)
        found.append((match.group(1), match.start(1), args[:1]))
    return sorted(found, key=lambda call: call[1])


def fills_of(code):
    """(offset of the fill, offset after it, the variable) for every call of OpenSslPath or OpenSslPathOf_."""
    fills = []
    for match in FILL_CALL.finditer(code):
        args, end = arguments(code, match.end() - 1)
        if len(args) >= 2 and re.match(r"^" + IDENTIFIER + r"$", args[1]) and args[1] not in KEYWORDS:
            fills.append((match.start(1), end, args[1]))
    return fills


def assigned_between(code, variable, start, end):
    """Whether the variable is assigned, appended to or emptied between the offsets."""
    span = code[start:end]
    pattern = re.compile(r"(?<![\w.>])" + re.escape(variable) +
                         r"\s*(?:(?:\+|-|\|)?=(?!=)|\.\s*(?:assign|append|Append|Format|Empty|clear|swap|resize|insert|erase|replace)\s*\()")
    return pattern.search(span) is not None or re.search(r"\bswap\s*\(\s*" + re.escape(variable) + r"\b", span) is not None


def line_of(code, offset):
    return code.count("\n", 0, offset) + 1


def problems_of(path, text):
    """The problems of one file, and the number of calls it makes."""
    problems = []
    code = without_comments(text)
    raw_lines = text.split("\n")
    depths = brace_depths(code)
    fills = fills_of(code)
    calls = calls_of(code)
    for function, offset, names in calls:
        number = line_of(code, offset)
        if exempt(raw_lines, number):
            continue
        for name in names:
            if name in NO_NAME:
                continue
            variable = None
            for pattern in ACCEPTED:
                match = pattern.match(name)
                if match and match.group(1) not in KEYWORDS:
                    variable = match.group(1)
                    break
            if variable is None:
                problems.append("%s:%d: %s is handed %s, not a name FileUtilities::OpenSslPath gave: on Windows OpenSSL may "
                                "read those bytes as another file's name" % (path, number, function, " ".join(name.split())))
                continue
            fill = None
            for start, end, filled in fills:
                if filled != variable or end > offset:
                    continue
                # The fill's block holds the call: the depth never falls below the
                # fill's between the two.
                if min(depths[start:offset + 1]) >= depths[start]:
                    fill = (start, end)
            if fill is None:
                problems.append("%s:%d: %s is handed %s, which FileUtilities::OpenSslPath did not fill in this block or one "
                                "around it" % (path, number, function, variable))
                continue
            if assigned_between(code, variable, fill[1], offset):
                problems.append("%s:%d: %s is handed %s, which is assigned again after FileUtilities::OpenSslPath filled it "
                                "at line %d" % (path, number, function, variable, line_of(code, fill[0])))
    return problems, len(calls)


def files():
    for tree in TREES:
        for directory, _, names in os.walk(tree):
            for name in sorted(names):
                if name.lower().endswith(EXTENSIONS):
                    path = os.path.join(directory, name)
                    yield os.path.relpath(path, ROOT).replace(os.sep, "/"), path


def read(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        return f.read().replace("\r\n", "\n")


def check():
    problems = []
    count = 0
    calls = 0
    for relative, path in files():
        count += 1
        found, made = problems_of(relative, read(path))
        problems += found
        calls += made
    return problems, count, calls


def self_test():
    failures = []

    def expect(label, text, count):
        found, _ = problems_of("sample.cpp", text)
        if len(found) != count:
            failures.append("%s: %d problem(s) reported, not %d: %s" % (label, len(found), count, found))

    problems, count, calls = check()
    if problems:
        failures.append("the tree passes: %s" % problems[:3])
    if count < 1000:
        failures.append("only %d files were read: this check has stopped seeing the source" % count)
    if calls < 18:
        failures.append("only %d calls were seen, where the tree made 18 when this check was written" % calls)

    # Every call in the tree, its name put back to what it was handed before
    # OpenSslPath (NarrowPath), and to AnsiString(String), one call at a time: each
    # must be refused, at its own line.
    put_back = 0
    for relative, path in files():
        text = read(path)
        code = without_comments(text)
        for function, offset, names in calls_of(code):
            for name in names:
                if name in NO_NAME:
                    continue
                for replacement in ("FileUtilities::NarrowPath(file)", "AnsiString(file)"):
                    # The same call in the raw text: found by its line and the name.
                    number = line_of(code, offset)
                    lines = text.split("\n")
                    window = "\n".join(lines[number - 1:number + 3])
                    at = window.find(name)
                    if at < 0:
                        failures.append("%s:%d: the name %s of %s was not found in the text to put it back" % (relative, number, name, function))
                        continue
                    changed_window = window[:at] + replacement + window[at + len(name):]
                    changed = "\n".join(lines[:number - 1] + [changed_window] + lines[number + 3:])
                    found, _ = problems_of(relative, changed)
                    if len(found) != 1 or (":%d:" % number) not in found[0]:
                        failures.append("%s:%d: %s handed %s was not refused there alone: %s" % (relative, number, function, replacement, found))
                    put_back += 1
    if put_back < 36:
        failures.append("only %d names were put back to NarrowPath and AnsiString, where the tree's 18 calls give 36" % put_back)

    fill = "AnsiString bytes;\nif (!FileUtilities::OpenSslPath(path, bytes))\n   return false;\n"

    # What the tree does, accepted.
    expect("a filled variable", "bool F(const String &path)\n{\n" + fill + "BIO *bio = BIO_new_file(bytes.c_str(), \"r\");\n}\n", 0)
    expect("a filled variable, as it is", "bool F(const String &path)\n{\n" + fill + "BIO *bio = BIO_new_file(bytes, \"r\");\n}\n", 0)
    expect("std::string of it", "bool F()\n{\n" + fill + "ctx.load_verify_file(std::string(bytes.c_str()), ec);\n}\n", 0)
    expect("a member through ->", "bool F()\n{\n" + fill + "ctx->use_private_key_file(bytes, boost::asio::ssl::context::pem);\n}\n", 0)
    expect("OpenSslPathOf_", "bool F()\n{\nAnsiString k;\nif (!OpenSslPathOf_(file, k, _T(\"x\")))\n   return false;\nBIO *b = BIO_new_file(k.c_str(), \"w\");\n}\n", 0)
    expect("filled in the condition, used in the else",
           "void F()\n{\n   AnsiString d;\n   if (!FileUtilities::OpenSslPath(f, d))\n   {\n      Report();\n   }\n   else if (Exists(f))\n   {\n      context.use_tmp_dh_file(d);\n   }\n}\n", 0)
    expect("in a conditional", "void F()\n{\nAnsiString n;\nconst bool ok = OpenSslPathOf_(f, n, w);\nBIO *b = ok ? BIO_new_file(n.c_str(), \"w\") : nullptr;\n}\n", 0)
    expect("the second argument of SSL_CTX_use_PrivateKey_file",
           "void F()\n{\n" + fill + "SSL_CTX_use_PrivateKey_file(ctx, bytes.c_str(), SSL_FILETYPE_PEM);\n}\n", 0)
    expect("load_verify_locations with no directory",
           "void F()\n{\n" + fill + "SSL_CTX_load_verify_locations(ctx, bytes.c_str(), nullptr);\n}\n", 0)
    expect("a call split over lines",
           "void F()\n{\n" + fill + "BIO *b = BIO_new_file(\n   bytes.c_str(),\n   \"r\");\n}\n", 0)

    # Not a call: a comment, a string, a name that only ends in one.
    expect("a line comment", "// BIO_new_file(FileUtilities::NarrowPath(f).c_str(), \"r\")\nx = 1;\n", 0)
    expect("a block comment", "/* context.use_certificate_file(AnsiString(f), pem); */ x = 1;\n", 0)
    expect("a string", "Log(\"BIO_new_file(NarrowPath(f)) failed\");\n", 0)
    expect("a name that ends in one", "x = my_BIO_new_file(NarrowPath(f));\n", 0)

    # The shapes that were the defect, and the next call that bypasses the rule.
    expect("NarrowPath", "void F()\n{\nBIO *b = BIO_new_file(FileUtilities::NarrowPath(f).c_str(), \"r\");\n}\n", 1)
    expect("AnsiString(...)", "void F()\n{\ncontext.use_certificate_file(AnsiString(certificate->GetCertificateFile()), pem);\n}\n", 1)
    expect("a variable NarrowPath filled",
           "void F()\n{\nAnsiString n = FileUtilities::NarrowPath(f);\nBIO *b = BIO_new_file(n.c_str(), \"r\");\n}\n", 1)
    expect("Unicode::WideToMultiByte", "void F()\n{\nBIO *b = BIO_new_file(Unicode::WideToMultiByte(f), \"r\");\n}\n", 1)
    expect("a literal", "void F()\n{\nBIO *b = BIO_new_file(\"dh2048.pem\", \"r\");\n}\n", 1)
    expect("a new call with nothing filled", "void F(const AnsiString &name)\n{\nSSL_load_client_CA_file(name.c_str());\n}\n", 1)
    expect("the second argument of SSL_CTX_use_certificate_file",
           "void F()\n{\nSSL_CTX_use_certificate_file(ctx, NarrowPath(f).c_str(), SSL_FILETYPE_PEM);\n}\n", 1)
    expect("the directory of load_verify_locations",
           "void F()\n{\n" + fill + "SSL_CTX_load_verify_locations(ctx, bytes.c_str(), NarrowPath(d).c_str());\n}\n", 1)
    expect("X509_LOOKUP_load_file", "void F()\n{\nX509_LOOKUP_load_file(lookup, AnsiString(f), X509_FILETYPE_PEM);\n}\n", 1)
    expect("add_verify_path", "void F()\n{\nctx.add_verify_path(NarrowPath(dir));\n}\n", 1)
    expect("a call split over lines, NarrowPath",
           "void F()\n{\nBIO *b = BIO_new_file(\n   FileUtilities::NarrowPath(f).c_str(),\n   \"r\");\n}\n", 1)
    expect("filled, then assigned again",
           "void F()\n{\n" + fill + "bytes = FileUtilities::NarrowPath(path);\nBIO *b = BIO_new_file(bytes.c_str(), \"r\");\n}\n", 1)
    expect("filled in another function",
           "void A()\n{\n" + fill + "}\nvoid B()\n{\nAnsiString bytes = NarrowPath(p);\nBIO *b = BIO_new_file(bytes.c_str(), \"r\");\n}\n", 1)
    expect("filled in a block the call is not inside",
           "void F()\n{\nAnsiString bytes = NarrowPath(p);\nif (x)\n{\n   OpenSslPath(path, bytes);\n}\nBIO *b = BIO_new_file(bytes.c_str(), \"r\");\n}\n", 1)
    expect("filled after the call",
           "void F()\n{\nAnsiString bytes = NarrowPath(p);\nBIO *b = BIO_new_file(bytes.c_str(), \"r\");\nOpenSslPath(path, bytes);\n}\n", 1)
    expect("two calls, both NarrowPath",
           "void F()\n{\ncontext.use_certificate_file(NarrowPath(c), pem);\ncontext.use_private_key_file(NarrowPath(k), pem);\n}\n", 2)

    # The marker, with a reason, on the line or within the three above it - and not
    # without a reason, nor four lines away.
    expect("exempt above", "// openssl-file-name-exempt: a name in ASCII the program makes\nBIO_new_file(\"x.pem\", \"r\");\n", 0)
    expect("exempt with no reason", "// openssl-file-name-exempt:\nBIO_new_file(\"x.pem\", \"r\");\n", 1)
    expect("exempt too far above", "// openssl-file-name-exempt: a reason\n\n\n\nBIO_new_file(\"x.pem\", \"r\");\n", 1)

    for failure in failures:
        print("SELF-TEST FAILED: " + failure)
    print("self-test: %d failure(s), %d names in the tree put back and refused" % (len(failures), put_back))
    return 1 if failures else 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    problems, count, calls = check()
    if problems:
        print("A file's name is handed to OpenSSL in bytes FileUtilities::OpenSslPath did not give:")
        for problem in problems:
            print("  " + problem)
        return 1

    print("check-openssl-file-names: %d C and C++ files, %d calls that open a file by its name, each handed the "
          "bytes FileUtilities::OpenSslPath gave" % (count, calls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
