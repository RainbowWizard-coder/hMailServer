// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account signature's HTML, reduced to the subset the webmail's cleaner
// admits. See SignatureHtmlSanitizer.h. This is a faithful port of Portal.js
// cleanHtml/cleanNode: the HTML is parsed into a tree, and each node is rebuilt
// from its tag, a link's address, an image's data and the text, so that what the
// page writes passes through unchanged and anything else - a script, a handler,
// a style, a foreign element - is gone.

#include "StdAfx.h"

#include "SignatureHtmlSanitizer.h"

#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef std::wstring Text;

      // The page's fixed table start tag (Portal.js TABLE_OPEN): every table is
      // written with the border, padding and spacing the reader saw, as the
      // attributes a mail client honours, and with nothing else of its own.
      const wchar_t *const TableOpen =
         L"<table border=\"1\" cellpadding=\"6\" cellspacing=\"0\" style=\"border-collapse:collapse\">";

      bool IsSpace(wchar_t c)
      {
         return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f';
      }

      bool IsAsciiAlpha(wchar_t c)
      {
         return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
      }

      wchar_t Lower(wchar_t c)
      {
         return (c >= L'A' && c <= L'Z') ? (wchar_t) (c - L'A' + L'a') : c;
      }

      Text Lowered(const Text &text)
      {
         Text lowered;
         lowered.reserve(text.size());
         for (size_t i = 0; i < text.size(); i++)
            lowered += Lower(text[i]);
         return lowered;
      }

      bool StartsWith(const Text &text, const wchar_t *prefix)
      {
         size_t i = 0;
         for (; prefix[i]; i++)
         {
            if (i >= text.size() || Lower(text[i]) != prefix[i])
               return false;
         }
         return true;
      }

      bool IsAsciiDigit(wchar_t c)
      {
         return c >= L'0' && c <= L'9';
      }

      // A code point appended as the platform's wide string holds it: one unit
      // where wchar_t is 32 bits (Linux), a surrogate pair above U+FFFF where it
      // is 16 (Windows).
      void AppendCodePoint(Text &out, unsigned long codePoint)
      {
#if WCHAR_MAX <= 0xFFFF
         if (codePoint > 0xFFFF)
         {
            const unsigned long offset = codePoint - 0x10000;
            out += static_cast<wchar_t>(0xD800 + (offset >> 10));
            out += static_cast<wchar_t>(0xDC00 + (offset & 0x3FF));
            return;
         }
#endif
         out += static_cast<wchar_t>(codePoint);
      }

      // What a character reference at text[i] (an '&') is. Read as a browser
      // reads one that ends in ';': a numeric one (&#233; &#xE9;) stands for its
      // code point - U+FFFD for none, a surrogate or one past U+10FFFF - and the
      // named ones the page's escapeHtml writes, with &apos; and &nbsp;, for
      // theirs. Any other well-formed named reference (&copy;, &eacute;) is kept
      // exactly as written, for the browser to read: a reference is never
      // markup, in element text or in a double-quoted attribute, so keeping it
      // is safe, and it is what keeps a signature an administrator wrote with
      // one reading as it was written rather than as "&copy;".
      //
      // The scan is bounded - eight digits, thirty-two letters, the longest name
      // HTML has being thirty-one - so a run of ampersands with no ';' after
      // them costs a few characters each, not the rest of the signature each.
      enum ReferenceKind { NotAReference, DecodedReference, KeptReference };

      ReferenceKind ReadReference(const Text &text, size_t i, size_t &length, unsigned long &codePoint)
      {
         const size_t n = text.size();
         size_t j = i + 1;

         if (j < n && text[j] == L'#')
         {
            j++;
            const bool hex = j < n && (text[j] == L'x' || text[j] == L'X');
            if (hex)
               j++;

            const size_t digitsStart = j;
            unsigned long value = 0;
            while (j < n && j - digitsStart < 8)
            {
               const wchar_t c = text[j];
               unsigned long digit = 0;
               if (IsAsciiDigit(c))
                  digit = static_cast<unsigned long>(c - L'0');
               else if (hex && c >= L'a' && c <= L'f')
                  digit = static_cast<unsigned long>(c - L'a') + 10;
               else if (hex && c >= L'A' && c <= L'F')
                  digit = static_cast<unsigned long>(c - L'A') + 10;
               else
                  break;
               value = value * (hex ? 16 : 10) + digit;
               j++;
            }

            if (j == digitsStart || j >= n || text[j] != L';')
               return NotAReference;

            if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
               value = 0xFFFD;

            codePoint = value;
            length = j + 1 - i;
            return DecodedReference;
         }

         const size_t nameStart = j;
         while (j < n && j - nameStart < 32 && (IsAsciiAlpha(text[j]) || IsAsciiDigit(text[j])))
            j++;

         if (j == nameStart || !IsAsciiAlpha(text[nameStart]) || j >= n || text[j] != L';')
            return NotAReference;

         length = j + 1 - i;

         const Text name = text.substr(nameStart, j - nameStart);
         if (name == L"amp" || name == L"AMP") codePoint = L'&';
         else if (name == L"lt" || name == L"LT") codePoint = L'<';
         else if (name == L"gt" || name == L"GT") codePoint = L'>';
         else if (name == L"quot" || name == L"QUOT") codePoint = L'"';
         else if (name == L"apos") codePoint = L'\'';
         else if (name == L"nbsp") codePoint = 0x00A0;
         else
            return KeptReference;

         return DecodedReference;
      }

      // Portal.js escapeHtml: & < > " - and not ', exactly as the page escapes,
      // so the two contexts the page uses (element text, double-quoted
      // attributes) are closed and the result matches the page's own. A NUL,
      // which no page writes, becomes U+FFFD rather than ending the string.
      void AppendEscaped(Text &out, wchar_t c)
      {
         switch (c)
         {
         case L'&': out += L"&amp;"; break;
         case L'<': out += L"&lt;"; break;
         case L'>': out += L"&gt;"; break;
         case L'"': out += L"&quot;"; break;
         case L'\0': out += static_cast<wchar_t>(0xFFFD); break;
         default: out += c; break;
         }
      }

      Text Escape(const Text &text)
      {
         Text out;
         out.reserve(text.size());
         for (size_t i = 0; i < text.size(); i++)
            AppendEscaped(out, text[i]);
         return out;
      }

      // Text or an attribute value with the references read above decoded and a
      // kept one left as written - what a scheme or a data: URL is tested
      // against, so a reference in a scheme ("&colon;") leaves the link out.
      Text Decode(const Text &raw)
      {
         Text out;
         out.reserve(raw.size());
         for (size_t i = 0; i < raw.size();)
         {
            size_t length = 0;
            unsigned long codePoint = 0;
            if (raw[i] == L'&' && ReadReference(raw, i, length, codePoint) == DecodedReference)
            {
               AppendCodePoint(out, codePoint);
               i += length;
               continue;
            }
            out += raw[i];
            i++;
         }
         return out;
      }

      // Text or an attribute value written back for the page: decoded and
      // escaped as escapeHtml escapes, with a kept reference left as written.
      // Writing the page's own output back gives the same text (a fixed point),
      // and writing this function's own output back does too (idempotent).
      Text Reencode(const Text &raw)
      {
         Text out;
         out.reserve(raw.size());
         for (size_t i = 0; i < raw.size();)
         {
            if (raw[i] == L'&')
            {
               size_t length = 0;
               unsigned long codePoint = 0;
               const ReferenceKind kind = ReadReference(raw, i, length, codePoint);
               if (kind == KeptReference)
               {
                  out.append(raw, i, length);
                  i += length;
                  continue;
               }
               if (kind == DecodedReference)
               {
                  Text decoded;
                  AppendCodePoint(decoded, codePoint);
                  for (size_t k = 0; k < decoded.size(); k++)
                     AppendEscaped(out, decoded[k]);
                  i += length;
                  continue;
               }
            }
            AppendEscaped(out, raw[i]);
            i++;
         }
         return out;
      }

      // The element the page rebuilds a tag as, or empty for one it drops
      // (keeping the cleaned contents). The page's ALLOWED map.
      Text Mapped(const Text &nameLower)
      {
         if (nameLower == L"b" || nameLower == L"strong" || nameLower == L"h1" || nameLower == L"h2" || nameLower == L"h3")
            return L"b";
         if (nameLower == L"i" || nameLower == L"em") return L"i";
         if (nameLower == L"u") return L"u";
         if (nameLower == L"br") return L"br";
         if (nameLower == L"p" || nameLower == L"div") return L"p";
         if (nameLower == L"ul") return L"ul";
         if (nameLower == L"ol") return L"ol";
         if (nameLower == L"li") return L"li";
         if (nameLower == L"blockquote") return L"blockquote";
         if (nameLower == L"a") return L"a";
         if (nameLower == L"img") return L"img";
         if (nameLower == L"table") return L"table";
         if (nameLower == L"thead" || nameLower == L"tbody" || nameLower == L"tfoot") return L"tbody";
         if (nameLower == L"tr") return L"tr";
         if (nameLower == L"td") return L"td";
         if (nameLower == L"th") return L"th";
         return Text();
      }

      // An element the parser consumes whole and drops - a raw-text or
      // escapable-raw-text element, whose content is read literally, or a
      // foreign-content or inert container - so that nothing it holds is ever
      // parsed as markup or kept as text. Everything else not on the allowlist
      // is dropped as a tag with its cleaned contents kept, as the page does.
      bool DropsWholeElement(const Text &nameLower)
      {
         return nameLower == L"script" || nameLower == L"style" || nameLower == L"textarea" ||
                nameLower == L"title" || nameLower == L"xmp" || nameLower == L"iframe" ||
                nameLower == L"noscript" || nameLower == L"noframes" || nameLower == L"noembed" ||
                nameLower == L"svg" || nameLower == L"math" || nameLower == L"template" ||
                nameLower == L"plaintext";
      }

      // The void elements, which never hold content and are never put on the
      // open stack, so a tree is built without waiting for an end tag they do
      // not have.
      bool IsVoid(const Text &nameLower)
      {
         return nameLower == L"br" || nameLower == L"img" || nameLower == L"hr" ||
                nameLower == L"area" || nameLower == L"base" || nameLower == L"col" ||
                nameLower == L"embed" || nameLower == L"input" || nameLower == L"link" ||
                nameLower == L"meta" || nameLower == L"param" || nameLower == L"source" ||
                nameLower == L"track" || nameLower == L"wbr";
      }

      // The webmail's isDataImage: the whole value is a data: URL for one of the
      // four picture types, base64, with the base64 alphabet and the white space
      // a wrapped attribute carries, and under the page's length ceiling.
      bool IsDataImage(const Text &src)
      {
         if (src.size() >= 2000000)
            return false;
         const Text lower = Lowered(src);
         const wchar_t *const prefixes[] = {
            L"data:image/png;base64,", L"data:image/jpeg;base64,",
            L"data:image/gif;base64,", L"data:image/webp;base64,"
         };
         size_t start = 0;
         bool matched = false;
         for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++)
         {
            Text prefix = prefixes[p];
            if (lower.compare(0, prefix.size(), prefix) == 0)
            {
               start = prefix.size();
               matched = true;
               break;
            }
         }
         if (!matched)
            return false;
         for (size_t i = start; i < src.size(); i++)
         {
            const wchar_t c = src[i];
            const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                            (c >= L'0' && c <= L'9') || c == L'+' || c == L'/' || c == L'=' ||
                            IsSpace(c);
            if (!ok)
               return false;
         }
         return true;
      }

      // ---- the tree -------------------------------------------------------

      // The deepest the tree is built (see Parse). The webmail's editor writes a
      // signature a few levels deep - a table, a row, a cell, a paragraph, a
      // bold - and nothing it writes comes near this.
      const size_t MaxDepth = 128;

      struct Attribute
      {
         Text name;   // lower case
         Text value;  // raw, as written between the quotes
      };

      struct Node
      {
         bool element = false;
         Text tagLower;                 // the element's name, lower case
         std::vector<Attribute> attributes;
         Text text;                     // raw text, for a text node
         std::vector<std::shared_ptr<Node>> children;
      };

      const Text *AttributeValue(const Node &node, const wchar_t *name)
      {
         for (size_t i = 0; i < node.attributes.size(); i++)
         {
            if (node.attributes[i].name == name)
               return &node.attributes[i].value;
         }
         return nullptr;
      }

      // Parses the attributes of a start tag that begins at i (just past the
      // tag name), to the tag's closing '>'. i is left just past the '>'.
      // selfClosed is set when the tag ends with '/>'. A '>' inside a quoted
      // value does not end the tag.
      void ParseAttributes(const Text &s, size_t &i, std::vector<Attribute> &attributes, bool &selfClosed)
      {
         selfClosed = false;
         const size_t n = s.size();
         while (i < n)
         {
            while (i < n && IsSpace(s[i]))
               i++;
            if (i >= n)
               return;
            if (s[i] == L'>')
            {
               i++;
               return;
            }
            if (s[i] == L'/')
            {
               selfClosed = true;
               i++;
               continue;
            }

            Text name;
            while (i < n && !IsSpace(s[i]) && s[i] != L'=' && s[i] != L'>' && s[i] != L'/')
            {
               name += Lower(s[i]);
               i++;
            }

            Text value;
            while (i < n && IsSpace(s[i]))
               i++;
            if (i < n && s[i] == L'=')
            {
               i++;
               while (i < n && IsSpace(s[i]))
                  i++;
               if (i < n && (s[i] == L'"' || s[i] == L'\''))
               {
                  const wchar_t quote = s[i];
                  i++;
                  while (i < n && s[i] != quote)
                  {
                     value += s[i];
                     i++;
                  }
                  if (i < n)
                     i++; // the closing quote
               }
               else
               {
                  while (i < n && !IsSpace(s[i]) && s[i] != L'>')
                  {
                     value += s[i];
                     i++;
                  }
               }
            }

            if (!name.empty())
            {
               Attribute attribute;
               attribute.name = name;
               attribute.value = value;
               attributes.push_back(attribute);
            }
         }
      }

      // From the '<' of a raw/foreign element's start tag already consumed, skip
      // to just past the matching "</name>" (case-insensitive), or to the end
      // when there is none. Nothing between is kept.
      void SkipToClose(const Text &s, size_t &i, const Text &nameLower)
      {
         const size_t n = s.size();
         const Text needle = L"</" + nameLower;
         while (i < n)
         {
            if (s[i] == L'<' && i + needle.size() <= n && Lowered(s.substr(i, needle.size())) == needle)
            {
               const wchar_t after = (i + needle.size() < n) ? s[i + needle.size()] : L'>';
               if (after == L'>' || after == L'/' || IsSpace(after))
               {
                  i += needle.size();
                  const size_t end = s.find(L'>', i);
                  i = (end == Text::npos) ? n : end + 1;
                  return;
               }
            }
            i++;
         }
      }

      std::shared_ptr<Node> Parse(const Text &s)
      {
         std::shared_ptr<Node> root(new Node());
         std::vector<Node *> open;
         open.push_back(root.get());

         const size_t n = s.size();
         size_t i = 0;
         Text text;

         while (i < n)
         {
            if (s[i] != L'<')
            {
               text += s[i];
               i++;
               continue;
            }

            // A '<' that does not begin a tag is text.
            const wchar_t next = (i + 1 < n) ? s[i + 1] : L'\0';
            const bool startsTag = IsAsciiAlpha(next) || next == L'/' || next == L'!' || next == L'?';
            if (!startsTag)
            {
               text += s[i];
               i++;
               continue;
            }

            if (!text.empty())
            {
               std::shared_ptr<Node> node(new Node());
               node->text = text;
               open.back()->children.push_back(node);
               text.clear();
            }

            if (next == L'!' || next == L'?')
            {
               // A comment, doctype or processing instruction: dropped. A
               // comment ends at "-->", the rest at the next '>'.
               if (next == L'!' && i + 3 < n && s[i + 2] == L'-' && s[i + 3] == L'-')
               {
                  const size_t end = s.find(L"-->", i + 4);
                  i = (end == Text::npos) ? n : end + 3;
               }
               else
               {
                  const size_t end = s.find(L'>', i + 2);
                  i = (end == Text::npos) ? n : end + 1;
               }
               continue;
            }

            if (next == L'/')
            {
               // An end tag: pop to the matching open element, if any.
               size_t j = i + 2;
               Text name;
               while (j < n && !IsSpace(s[j]) && s[j] != L'>')
               {
                  name += Lower(s[j]);
                  j++;
               }
               const size_t end = s.find(L'>', j);
               i = (end == Text::npos) ? n : end + 1;

               for (size_t k = open.size(); k-- > 1;)
               {
                  if (open[k]->tagLower == name)
                  {
                     open.resize(k);
                     break;
                  }
               }
               continue;
            }

            // A start tag.
            size_t j = i + 1;
            Text name;
            while (j < n && !IsSpace(s[j]) && s[j] != L'>' && s[j] != L'/')
            {
               name += Lower(s[j]);
               j++;
            }

            std::vector<Attribute> attributes;
            bool selfClosed = false;
            size_t afterTag = j;
            ParseAttributes(s, afterTag, attributes, selfClosed);
            i = afterTag;

            if (DropsWholeElement(name))
            {
               if (!selfClosed && name != L"plaintext")
                  SkipToClose(s, i, name);
               else if (name == L"plaintext")
                  i = n;
               continue;
            }

            // Past the depth a signature has any use for, a start tag is dropped
            // and what follows it stays in the element already open, so the
            // tree - and the rebuild below, and the tree's own destruction,
            // which both recurse once per level - is never deeper than this
            // whatever the signature holds: tens of thousands of nested
            // elements fit in 64 KB, and the COM, the administrator's route and
            // a database row set no length at all. A browser's own parser caps
            // the depth of what it builds the same way.
            if (open.size() > MaxDepth)
               continue;

            std::shared_ptr<Node> node(new Node());
            node->element = true;
            node->tagLower = name;
            node->attributes = attributes;
            open.back()->children.push_back(node);

            if (!selfClosed && !IsVoid(name))
               open.push_back(node.get());
         }

         if (!text.empty())
         {
            std::shared_ptr<Node> node(new Node());
            node->text = text;
            open.back()->children.push_back(node);
         }

         return root;
      }

      // ---- the rebuild (Portal.js cleanNode) ------------------------------

      Text CleanChildren(const Node &node);

      Text CleanNode(const Node &node)
      {
         if (!node.element)
            return Reencode(node.text);

         const Text tag = Mapped(node.tagLower);
         const Text inner = CleanChildren(node);

         if (tag.empty())
            return inner;

         if (tag == L"br")
            return L"<br>";

         if (tag == L"img")
         {
            const Text *srcRaw = AttributeValue(node, L"src");
            const Text src = srcRaw ? Decode(*srcRaw) : Text();
            if (!IsDataImage(src))
               return Text();
            const Text *altRaw = AttributeValue(node, L"alt");
            const Text alt = altRaw ? *altRaw : Text();
            return L"<img src=\"" + Escape(src) + L"\" alt=\"" + Reencode(alt) + L"\">";
         }

         if (tag == L"a")
         {
            // The scheme is tested on the decoded address, and the literal
            // "http:", "https:" or "mailto:" it has to begin with leaves no
            // reference in front of it for a browser to read differently.
            const Text *hrefRaw = AttributeValue(node, L"href");
            const Text href = hrefRaw ? Decode(*hrefRaw) : Text();
            if (StartsWith(href, L"http:") || StartsWith(href, L"https:") || StartsWith(href, L"mailto:"))
               return L"<a href=\"" + Reencode(*hrefRaw) + L"\">" + inner + L"</a>";
            return inner;
         }

         if (tag == L"p" && inner.empty())
            return L"<br>";

         if (tag == L"table")
            return Text(TableOpen) + inner + L"</table>";

         return L"<" + tag + L">" + inner + L"</" + tag + L">";
      }

      Text CleanChildren(const Node &node)
      {
         Text out;
         for (size_t i = 0; i < node.children.size(); i++)
            out += CleanNode(*node.children[i]);
         return out;
      }
   }

   String
   SignatureHtmlSanitizer::Sanitize(const String &html)
   {
      // A signature with no tag at all - the plain one every account that has
      // not made a formatted one carries - is returned untouched, so the common
      // case costs nothing and a plain-text signature is never altered.
      if (html.Find(_T("<")) < 0)
         return html;

      const Text source(html.c_str(), html.GetLength());
      std::shared_ptr<Node> root = Parse(source);
      const Text cleaned = CleanChildren(*root);
      return String(cleaned.c_str());
   }

   // ---- the tester --------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("SignatureHtmlSanitizerTester: " + message);
      }

      std::string Narrow(const String &text)
      {
         std::string out;
         const wchar_t *s = text.c_str();
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = s[i];
            out += (c >= 0x20 && c < 0x7F) ? (char) c : '.';
         }
         return out;
      }

      void ExpectSanitizes(const String &input, const String &expected)
      {
         const String got = SignatureHtmlSanitizer::Sanitize(input);
         Expect(got == expected, "\"" + Narrow(input) + "\" -> \"" + Narrow(got) + "\", not \"" + Narrow(expected) + "\"");
         // Idempotent: the result sanitises to itself.
         const String again = SignatureHtmlSanitizer::Sanitize(got);
         Expect(again == got, "not idempotent on \"" + Narrow(input) + "\": \"" + Narrow(got) + "\" -> \"" + Narrow(again) + "\"");
      }

      void ExpectStripsScript(const String &input)
      {
         const String got = SignatureHtmlSanitizer::Sanitize(input);
         Expect(got.FindNoCase(_T("<script")) < 0, "a <script> survived in \"" + Narrow(got) + "\"");
         Expect(got.FindNoCase(_T("javascript:")) < 0, "a javascript: survived in \"" + Narrow(got) + "\"");
         Expect(got.FindNoCase(_T("onerror")) < 0 && got.FindNoCase(_T("onclick")) < 0 && got.FindNoCase(_T("onload")) < 0,
            "an on* handler survived in \"" + Narrow(got) + "\"");
         Expect(got.FindNoCase(_T("<style")) < 0 && got.FindNoCase(_T("<iframe")) < 0 && got.FindNoCase(_T("<svg")) < 0,
            "a dangerous element survived in \"" + Narrow(got) + "\"");
      }
   }

   void
   SignatureHtmlSanitizerTester::Test()
   {
      // A plain signature, and empty, pass through.
      ExpectSanitizes(_T(""), _T(""));
      ExpectSanitizes(_T("Regards, Ada"), _T("Regards, Ada"));

      // The elements the page keeps, kept; its mapping applied.
      ExpectSanitizes(_T("<b>Regards</b>"), _T("<b>Regards</b>"));
      ExpectSanitizes(_T("<strong>Regards</strong>"), _T("<b>Regards</b>"));
      ExpectSanitizes(_T("<em>x</em>"), _T("<i>x</i>"));
      ExpectSanitizes(_T("<h1>Big</h1>"), _T("<b>Big</b>"));
      ExpectSanitizes(_T("<div>line</div>"), _T("<p>line</p>"));
      ExpectSanitizes(_T("<ul><li>one</li></ul>"), _T("<ul><li>one</li></ul>"));
      ExpectSanitizes(_T("<blockquote>q</blockquote>"), _T("<blockquote>q</blockquote>"));

      // An empty paragraph is a line break; a break is a break.
      ExpectSanitizes(_T("<p></p>"), _T("<br>"));
      ExpectSanitizes(_T("a<br>b"), _T("a<br>b"));
      ExpectSanitizes(_T("a<br/>b"), _T("a<br>b"));

      // A disallowed element is dropped, its cleaned contents kept.
      ExpectSanitizes(_T("<span style=\"color:red\">hi</span>"), _T("hi"));
      ExpectSanitizes(_T("<font color=\"red\"><b>hi</b></font>"), _T("<b>hi</b>"));
      ExpectSanitizes(_T("<center>x</center>"), _T("x"));

      // No attribute of an allowed element survives but the three named.
      ExpectSanitizes(_T("<p style=\"x\" onclick=\"y\" class=\"z\">t</p>"), _T("<p>t</p>"));
      ExpectSanitizes(_T("<b onmouseover=\"evil()\">t</b>"), _T("<b>t</b>"));

      // A link: http(s)/mailto kept (href only); anything else drops the tag,
      // keeps the text.
      ExpectSanitizes(_T("<a href=\"https://example.test/\">site</a>"), _T("<a href=\"https://example.test/\">site</a>"));
      ExpectSanitizes(_T("<a href=\"http://example.test/\">site</a>"), _T("<a href=\"http://example.test/\">site</a>"));
      ExpectSanitizes(_T("<a href=\"mailto:ada@example.test\">mail</a>"), _T("<a href=\"mailto:ada@example.test\">mail</a>"));
      ExpectSanitizes(_T("<a href=\"javascript:alert(1)\">x</a>"), _T("x"));
      ExpectSanitizes(_T("<a href=\"https://x/\" onclick=\"e()\" target=\"_blank\">x</a>"), _T("<a href=\"https://x/\">x</a>"));

      // An image: a data: image of the four types kept (src and alt only);
      // anything else dropped.
      {
         const String png = _T("data:image/png;base64,iVBORw0KGgo=");
         ExpectSanitizes(String(_T("<img src=\"")) + png + _T("\" alt=\"logo\">"),
                         String(_T("<img src=\"")) + png + _T("\" alt=\"logo\">"));
         ExpectSanitizes(String(_T("<img src=\"")) + png + _T("\" alt=\"logo\" onerror=\"e()\" width=\"10\">"),
                         String(_T("<img src=\"")) + png + _T("\" alt=\"logo\">"));
      }
      ExpectSanitizes(_T("<img src=\"https://evil.test/x.png\" alt=\"a\">"), _T(""));
      ExpectSanitizes(_T("<img src=\"data:image/svg+xml;base64,AAA\" alt=\"a\">"), _T(""));
      ExpectSanitizes(_T("<img src=\"javascript:alert(1)\">"), _T(""));

      // A simple table, rebuilt with the page's fixed attributes.
      ExpectSanitizes(_T("<table class=\"x\"><tr><td>a</td><th>b</th></tr></table>"),
                      String(_T("<table border=\"1\" cellpadding=\"6\" cellspacing=\"0\" style=\"border-collapse:collapse\">")) +
                      _T("<tr><td>a</td><th>b</th></tr></table>"));
      ExpectSanitizes(_T("<table><thead><tr><th>h</th></tr></thead></table>"),
                      String(_T("<table border=\"1\" cellpadding=\"6\" cellspacing=\"0\" style=\"border-collapse:collapse\">")) +
                      _T("<tbody><tr><th>h</th></tr></tbody></table>"));

      // Text is escaped as the page escapes it, and an entity the page writes is
      // decoded and written back as itself (a fixed point).
      ExpectSanitizes(_T("a < b & c > d"), _T("a &lt; b &amp; c &gt; d"));
      ExpectSanitizes(_T("a &lt; b &amp; c &gt; d"), _T("a &lt; b &amp; c &gt; d"));
      ExpectSanitizes(_T("<b>A &amp; B</b>"), _T("<b>A &amp; B</b>"));

      // Dangerous elements dropped whole - the content never kept as text or
      // parsed as markup.
      ExpectSanitizes(_T("before<script>alert(1)</script>after"), _T("beforeafter"));
      ExpectSanitizes(_T("x<style>body{}</style>y"), _T("xy"));
      ExpectSanitizes(_T("x<iframe src=\"javascript:alert(1)\">no</iframe>y"), _T("xy"));
      ExpectSanitizes(_T("x<svg><script>alert(1)</script></svg>y"), _T("xy"));
      ExpectSanitizes(_T("x<math><mtext>t</mtext></math>y"), _T("xy"));
      ExpectSanitizes(_T("x<textarea></textarea>y"), _T("xy"));
      ExpectSanitizes(_T("x<title>t</title>y"), _T("xy"));
      ExpectSanitizes(_T("<b>keep</b><plaintext>&lt;script&gt;everything after is gone"), _T("<b>keep</b>"));

      // A comment is dropped.
      ExpectSanitizes(_T("a<!-- <script>x</script> -->b"), _T("ab"));

      // A mutation attempt - script content, a handler, a scheme, a comment -
      // leaves nothing executable, however it is shaped.
      ExpectStripsScript(_T("<SCRIPT>alert(1)</SCRIPT>"));
      ExpectStripsScript(_T("<img src=x onerror=alert(1)>"));
      ExpectStripsScript(_T("<a href=\"JAVASCRIPT:alert(1)\">x</a>"));
      ExpectStripsScript(_T("<div onclick=\"alert(1)\">x</div>"));
      ExpectStripsScript(_T("<svg/onload=alert(1)>"));
      ExpectStripsScript(_T("<iframe srcdoc=\"<script>alert(1)</script>\"></iframe>"));
      ExpectStripsScript(_T("<a href=\"&#106;avascript:alert(1)\">x</a>"));
      ExpectStripsScript(_T("<p style=\"background:url(javascript:alert(1))\">x</p>"));

      // The exact signature the image fixture stores (a paragraph with a name
      // and a data: image) comes back byte for byte: a fixed point, which is
      // what keeps the webmail's round trip true.
      {
         const String canonical =
            _T("<p><b>Ada Lovelace</b> \u00E9 here</p>")
            _T("<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUg==\" alt=\"\">");
         ExpectSanitizes(canonical, canonical);
      }

      // A quoted '>' inside an attribute does not end the tag early.
      ExpectSanitizes(_T("<a href=\"https://x/?a=1\" title=\"a>b\">t</a>"), _T("<a href=\"https://x/?a=1\">t</a>"));

      // Character references. A numeric one is read as a browser reads it, and
      // escaped again where it has to be; the page's own named ones are read;
      // any other named one is kept as written, so a signature written with
      // &copy; still shows the sign and not the word. A bare ampersand is an
      // ampersand. Each comes back from a second pass as it went in. (A value
      // with no '<' at all is returned as it is - no tag can come of it - so
      // these carry one.)
      //
      // Every character beyond ASCII in these literals is written as an escape.
      // The file has no byte-order mark and the Windows build does not pass
      // /utf-8, so MSVC reads raw UTF-8 bytes in the ANSI code page: a raw
      // copyright sign was two characters there, and the expected text was
      // wrong on Windows alone.
      ExpectSanitizes(_T("<b>&copy; 2026 Example Ltd</b>"), _T("<b>&copy; 2026 Example Ltd</b>"));
      ExpectSanitizes(_T("<p>Fran&ccedil;ois &mdash; Paris</p>"), _T("<p>Fran&ccedil;ois &mdash; Paris</p>"));
      ExpectSanitizes(_T("<b>&#169; &#xA9; &#XA9;</b>"), _T("<b>\u00A9 \u00A9 \u00A9</b>"));
      ExpectSanitizes(_T("<b>&#60;script&#62;</b>"), _T("<b>&lt;script&gt;</b>"));
      ExpectSanitizes(_T("<b>&LT;b&GT; &QUOT;</b>"), _T("<b>&lt;b&gt; &quot;</b>"));
      ExpectSanitizes(_T("<b>&#0; &#xD800; &#x110000;</b>"), _T("<b>\uFFFD \uFFFD \uFFFD</b>"));
      ExpectSanitizes(_T("<b>a&b &unknown &#x; &;</b>"), _T("<b>a&amp;b &amp;unknown &amp;#x; &amp;;</b>"));
      ExpectSanitizes(_T("<b>&amp;copy;</b>"), _T("<b>&amp;copy;</b>"));
      ExpectSanitizes(_T("<img src=\"data:image/png;base64,iVBO\" alt=\"&copy; Ada &amp; Co\">"),
                      _T("<img src=\"data:image/png;base64,iVBO\" alt=\"&copy; Ada &amp; Co\">"));
      // A code point above U+FFFF: a surrogate pair on Windows, one unit on
      // Linux - either way the same character back, as the compiler writes it.
      ExpectSanitizes(_T("<b>&#x1F600; &#128512;</b>"), _T("<b>\U0001F600 \U0001F600</b>"));

      // A link's scheme is tested on the decoded address: a numeric reference
      // that spells http is read as http, and a kept named one in front of the
      // colon leaves the link out, as a browser would read some other scheme.
      ExpectSanitizes(_T("<a href=\"&#104;ttps://x/\">t</a>"), _T("<a href=\"https://x/\">t</a>"));
      ExpectSanitizes(_T("<a href=\"javascript&colon;alert(1)\">t</a>"), _T("t"));
      ExpectSanitizes(_T("<a href=\"https://x/?a=1&amp;b=2&copy;\">t</a>"), _T("<a href=\"https://x/?a=1&amp;b=2&copy;\">t</a>"));
      ExpectSanitizes(_T("<a href=\"https://x/?q=&quot;&gt;&lt;script&gt;\">t</a>"),
                      _T("<a href=\"https://x/?q=&quot;&gt;&lt;script&gt;\">t</a>"));

      // A hostile depth. Twenty thousand nested elements are some 60 KB - a
      // signature the webmail's own limit admits, and the administrator's route,
      // COM and a database row set none. Built to any depth, the rebuild and the
      // tree's destruction, which recurse once per level, would run out of
      // stack and take the service with them on every sign-in that read it. The
      // tree stops at 128 levels and what is deeper stays in the element that
      // is open; the text survives.
      {
         String deep;
         for (int i = 0; i < 20000; i++)
            deep += _T("<b>");
         deep += _T("bottom");
         for (int i = 0; i < 20000; i++)
            deep += _T("</b>");
         const String got = SignatureHtmlSanitizer::Sanitize(deep);
         Expect(got.Find(_T("bottom")) >= 0, "the text at the bottom of a deep signature was lost");
         int opened = 0;
         for (int at = got.Find(_T("<b>")); at >= 0; at = got.Find(_T("<b>"), at + 1))
            opened++;
         Expect(opened > 0 && opened <= 128, "a deep signature was rebuilt " + std::to_string(opened) + " levels deep");
         Expect(SignatureHtmlSanitizer::Sanitize(got) == got, "a deep signature's result is not a fixed point");

         // And a deep signature of an element dropped as a tag, whose contents
         // are kept: the same bound.
         String spans;
         for (int i = 0; i < 20000; i++)
            spans += _T("<span><div>");
         spans += _T("x");
         Expect(SignatureHtmlSanitizer::Sanitize(spans).Find(_T("x")) >= 0, "the text inside deep dropped elements was lost");
      }

      // A hostile time. Ampersands with no ';' after them, end tags that close
      // nothing, and unterminated comments, each 200,000 characters long: each
      // character is looked at a bounded number of times, so this is
      // milliseconds; a scan that ran to the end of the signature from every
      // one of them would be minutes, on every sign-in. Five seconds is the
      // bound, far over the one and far under the other on any machine that
      // builds this (HM_TEST_TIME_BOUND: ten times that where the aarch64
      // package build runs this under qemu-user).
      {
         const ULONGLONG started = GetTickCount64();

         std::wstring ampersands(L"<b>x</b>");
         ampersands.append(200000, L'&');
         const String cleanedAmpersands = SignatureHtmlSanitizer::Sanitize(String(ampersands.c_str()));
         Expect(cleanedAmpersands.GetLength() == 8 + 200000 * 5, "a run of bare ampersands was not each escaped");

         std::wstring closes;
         for (int i = 0; i < 20000; i++)
            closes += L"<b>";
         for (int i = 0; i < 20000; i++)
            closes += L"</x>";
         SignatureHtmlSanitizer::Sanitize(String(closes.c_str()));

         std::wstring comments;
         for (int i = 0; i < 50000; i++)
            comments += L"<!x ";
         SignatureHtmlSanitizer::Sanitize(String(comments.c_str()));

         const ULONGLONG elapsed = GetTickCount64() - started;
         const ULONGLONG bound = (ULONGLONG) HM_TEST_TIME_BOUND(5000);
         Expect(elapsed < bound, "three hostile signatures of 200,000 characters took " + std::to_string(elapsed) +
                " ms, and the bound is " + std::to_string(bound) + " ms");
      }
   }
}
