// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The script-src hash of a page's one inline script. See InlineScriptHash.h.

#include "StdAfx.h"

#include "InlineScriptHash.h"
#include "Encoding/Base64.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <openssl/sha.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const size_t None = std::string::npos;

      // White space as the tokenizer has it, once CR is gone: tab, line feed,
      // form feed and space.
      bool IsSpace(char c)
      {
         return c == ' ' || c == '\t' || c == '\n' || c == '\f';
      }

      // ASCII case folding only: a tag name is ASCII, and a byte of a UTF-8
      // sequence must stay what it is whatever the locale says.
      char Folded(char c)
      {
         return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
      }

      // The page as the HTML parser reads it: CR LF and a lone CR made LF
      // (input stream preprocessing, before the tokenizer sees a character).
      std::string Preprocessed(const AnsiString &page)
      {
         const char *bytes = page.c_str();
         const size_t size = page.size();

         std::string text;
         text.reserve(size);
         for (size_t i = 0; i < size; i++)
         {
            if (bytes[i] == '\r')
            {
               text += '\n';
               if (i + 1 < size && bytes[i + 1] == '\n')
                  i++;
            }
            else
               text += bytes[i];
         }
         return text;
      }

      // Where a tag that starts with word ("<script" or "</script", lower
      // case) begins at or after from: the word in any case, then white space,
      // "/" or ">" - so "<scripts>" and "</scripts>" are not it. None when
      // there is no such tag, or when the word is the page's last characters
      // and nothing says yet whether it is one.
      size_t FindTag(const std::string &text, size_t from, const char *word)
      {
         const size_t length = strlen(word);
         for (size_t at = text.find('<', from); at != None; at = text.find('<', at + 1))
         {
            if (at + length >= text.size())
               return None;

            bool matches = true;
            for (size_t i = 0; matches && i < length; i++)
               matches = Folded(text[at + i]) == word[i];
            if (!matches)
               continue;

            const char next = text[at + length];
            if (IsSpace(next) || next == '/' || next == '>')
               return at;
         }
         return None;
      }

      // A start tag's attributes, read from just after its name the way the
      // tokenizer reads them: names up to white space, "/", ">" or "=", values
      // in double quotes, single quotes or none, a ">" inside quotes part of
      // the value. Returns where the text after the tag's ">" begins, or None
      // when the tag never ends; src is set when an attribute is named src.
      size_t ReadAttributes(const std::string &text, size_t at, bool &src)
      {
         src = false;
         const size_t size = text.size();
         while (at < size)
         {
            const char c = text[at];
            if (c == '>')
               return at + 1;
            if (IsSpace(c) || c == '/')
            {
               at++;
               continue;
            }

            // The name. Its first character is taken whatever it is - a "="
            // there begins a name rather than a value.
            std::string name(1, Folded(c));
            at++;
            while (at < size && !IsSpace(text[at]) && text[at] != '/' && text[at] != '>' && text[at] != '=')
               name += Folded(text[at++]);
            if (name == "src")
               src = true;

            while (at < size && IsSpace(text[at]))
               at++;
            if (at >= size || text[at] != '=')
               continue;

            at++;
            while (at < size && IsSpace(text[at]))
               at++;
            if (at >= size)
               return None;

            if (text[at] == '"' || text[at] == '\'')
            {
               const size_t close = text.find(text[at], at + 1);
               if (close == None)
                  return None;
               at = close + 1;
            }
            else
            {
               while (at < size && !IsSpace(text[at]) && text[at] != '>')
                  at++;
            }
         }
         return None;
      }
   }

   bool
   InlineScriptHash::ScriptText(const AnsiString &page, std::string &script)
   {
      script.clear();
      const std::string text = Preprocessed(page);

      const size_t open = FindTag(text, 0, "<script");
      if (open == None)
         return false;

      bool src = false;
      const size_t start = ReadAttributes(text, open + strlen("<script"), src);
      if (start == None)
         return false;

      const size_t end = FindTag(text, start, "</script");
      if (end == None)
         return false;

      // Exactly one: a second script element is one the hash would not allow,
      // and a page that has one is not the page this policy was written for.
      if (FindTag(text, end + strlen("</script"), "<script") != None)
         return false;

      // A script file is not an inline script, and a hash of the text between
      // its tags would allow nothing the page runs.
      if (src || end == start)
         return false;

      script.assign(text, start, end - start);
      return true;
   }

   AnsiString
   InlineScriptHash::ScriptSource(const AnsiString &page)
   {
      std::string script;
      if (!ScriptText(page, script))
         return "'none'";

      unsigned char digest[SHA256_DIGEST_LENGTH] = {};
      SHA256(reinterpret_cast<const unsigned char *>(script.data()), script.size(), digest);
      return "'sha256-" + Base64::Encode(reinterpret_cast<const char *>(digest), SHA256_DIGEST_LENGTH) + "'";
   }

   AnsiString
   InlineScriptHashCache::ScriptSource(const AnsiString &page)
   {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!held_ || page_.size() != page.size() || page_.compare(0, None, page.c_str(), page.size()) != 0)
      {
         page_.assign(page.c_str(), page.size());
         source_ = InlineScriptHash::ScriptSource(page);
         held_ = true;
         computed_++;
      }
      return source_;
   }

   unsigned long
   InlineScriptHashCache::Computed()
   {
      std::lock_guard<std::mutex> guard(mutex_);
      return computed_;
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests and inline-script-hash-tests.
   // ------------------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("InlineScriptHashTester: " + message);
      }

      void ExpectSource(const AnsiString &page, const char *expected, const std::string &name)
      {
         const AnsiString source = InlineScriptHash::ScriptSource(page);
         Expect(source == expected, name + ": " + std::string(source.c_str()) + ", not " + expected);
      }
   }

   void
   InlineScriptHashTester::Test()
   {
      // The values are SHA-256 as Python's hashlib computes it over the UTF-8
      // text a browser hashes, base64 - another implementation, not this one.
      ExpectSource("<!doctype html><html><body><script>alert(1)</script></body></html>",
                   "'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI='", "a page with one script");

      // The trap that kills the Deck: the browser hashes the text after CR LF
      // and a lone CR are made LF, so the bytes on disk are the wrong input.
      ExpectSource("<script>\r\na\r\nb\r\n</script>",
                   "'sha256-VFrdBa/c7HUah7N3AIuKqvlrWqf3bYrZRL7PWDFBxNI='", "CR LF made LF");
      Expect(InlineScriptHash::ScriptSource("<script>a\r\nb</script>") == "'sha256-fhj3NzEbLcOy8mndeDlrA1HxT7Zu+oefdoyyMYGIPHg='",
             "a\\r\\nb is hashed as a\\nb");
      Expect(InlineScriptHash::ScriptSource("<script>a\r\nb</script>") != "'sha256-GHRfNqBeKQcnCQQtYGLOVPGwj/NsJ7qAw5+B+wEMjOI='",
             "a\\r\\nb is hashed as its raw bytes, which every browser refuses");
      ExpectSource("<script>a\rb</script>", "'sha256-fhj3NzEbLcOy8mndeDlrA1HxT7Zu+oefdoyyMYGIPHg='", "a lone CR made LF");
      ExpectSource("<script>a\r\r\nb</script>", "'sha256-OAIv0rjbxcs9LO504IPtv1nj1OE9Bn68tdtjPUz/TYw='", "CR then CR LF: two line feeds");

      // The text a browser hashes is UTF-8, byte for byte as the page has it.
      ExpectSource("<script>var s=\"\xC3\xA9\";</script>", "'sha256-qaEN+0jwPYj3e3dzodl8YzRdP5K33NNLev+dNh08L4Q='", "UTF-8 text");
      ExpectSource("<script>if(a>b)c()</script>", "'sha256-NaN/OPjVy/u/btARJhnbYCdtFl6070rciTSktstqY/Q='", "a > in the script");

      // The tokenizer's tags: any case, attributes with quotes, the end tag
      // followed by white space, "/" or ">", and names that only begin alike.
      const char *x = "'sha256-LXEWQrcmsEQBYnyp+6wy9chTD7GQPMTbAiWHF5IaSIE='";
      ExpectSource("<SCRIPT>x</SCRIPT >", x, "upper case");
      ExpectSource("<Script\ttype=\"text/javascript\">x</sCript>", x, "mixed case and an attribute");
      ExpectSource("<script data-x='a>b' nonce=\"q\">x</script>", x, "a > inside a quoted attribute value");
      ExpectSource("<script data-src=\"/deck.js\" async>x</script>", x, "an attribute whose name only contains src");
      ExpectSource("<script>x</script\n>", x, "an end tag with white space before its >");
      ExpectSource("<script>x</script/>", x, "an end tag with a /");
      ExpectSource("<script>a</scripts>b</script>", "'sha256-fzpocqRtdymNmlIlOYPGwEe+1qx3bff24ee9Zo9f/CQ='", "</scripts> does not end the script");
      ExpectSource("<scripts>x</scripts><script>y</script>", "'sha256-ofzkNjhU/4iM/0uOeHXWAMJoI5BBKoz3mzfQsRFIsPo='", "<scripts> is not a script");
      ExpectSource("<script>\nvar t=\"<scripts>\";\n</script>", "'sha256-NQUDvUzE+VfyHWbzM/ahAaY76tiln4+qvVghOsDKt9o='", "a tag-like string inside the script");

      // Nothing to allow: 'none', and the page runs nothing.
      ExpectSource("<!doctype html><html><body style=\"font-family:sans-serif\"><h1>hMailServer</h1>"
                   "<p>Web administration page not installed.</p></body></html>", "'none'", "the page that stands in when the Deck is not installed");
      ExpectSource("", "'none'", "an empty page");
      ExpectSource("<script>a</script><script>b</script>", "'none'", "two scripts");
      ExpectSource("<script>a</script>\r\n<!-- <script> -->", "'none'", "a second script tag, even in a comment");
      ExpectSource("<script src=\"/deck.js\"></script>", "'none'", "a script file");
      ExpectSource("<script src=/deck.js>y</script>", "'none'", "a script file with text between its tags");
      ExpectSource("<script/src=x>y</script>", "'none'", "a script file named after a /");
      ExpectSource("<script SRC='x'>y</script>", "'none'", "a script file in upper case");
      ExpectSource("<script></script>", "'none'", "a script with no text");
      ExpectSource("<script>abc", "'none'", "a script that never ends");
      ExpectSource("<script>abc</script", "'none'", "an end tag the page stops inside");
      ExpectSource("<script type=\"a>abc</script>", "'none'", "a start tag that never ends");
      ExpectSource("<script", "'none'", "a page that stops after <script");

      // Once per distinct page: the same bytes are not hashed again, and other
      // bytes - an upgrade's page - are never answered with the old hash.
      InlineScriptHashCache cache;
      const AnsiString first = "<script>x</script>";
      const AnsiString second = "<script>y</script>";
      Expect(cache.ScriptSource(first) == x, "the cache's first answer");
      Expect(cache.ScriptSource(first) == x && cache.Computed() == 1, "the same page hashed again");
      Expect(cache.ScriptSource(second) == "'sha256-ofzkNjhU/4iM/0uOeHXWAMJoI5BBKoz3mzfQsRFIsPo='", "a changed page answered with the old hash");
      Expect(cache.ScriptSource(first) == x && cache.Computed() == 3, "the first page again after the second");
      Expect(cache.ScriptSource("<script>x</script >") == x && cache.Computed() == 4, "a page of other bytes with the same script");
   }
}
