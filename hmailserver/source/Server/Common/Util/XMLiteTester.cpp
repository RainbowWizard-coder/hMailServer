// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// XMLite on malformed backup indexes. See XMLiteTester.h.

#include "stdafx.h"

#include "XMLiteTester.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Expect(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("XMLiteTester: " + message);
      }

      // A text in an allocation of exactly its characters and its terminator.
      class Text
      {
      public:
         explicit Text(const TCHAR *text) :
            length_(_tcslen(text)),
            characters_(new TCHAR[_tcslen(text) + 1])
         {
            memcpy(characters_.get(), text, (length_ + 1) * sizeof(TCHAR));
         }

         TCHAR *Get() const
         {
            return characters_.get();
         }

         // Where a parse stopped: at the terminator at the latest.
         bool Holds(const TCHAR *position) const
         {
            return position >= characters_.get() && position <= characters_.get() + length_;
         }

      private:
         size_t length_;
         std::unique_ptr<TCHAR[]> characters_;
      };

      // The root element parsed as XDoc::Load parses it, less the report of a
      // failure. NULL when the parse failed; otherwise the element, which the
      // document then owns.
      LPXNode LoadRoot(XDoc &doc, PARSEINFO &info, const Text &text, const std::string &name)
      {
         LPXNode root = new XNode;
         root->parent = &doc;
         root->type = XNODE_ELEMENT;
         root->doc = &doc;

         LPTSTR end = root->Load(text.Get(), &info);

         if (end == NULL)
         {
            delete root;
            return NULL;
         }

         doc.childs.push_back(root);
         Expect(text.Holds(end), name + ": the parse stopped past the end of the text");
         return root;
      }

      // A text that must not parse. When the parser says why, it must say
      // PIE_NOT_CLOSED: the text ends before something it opened is closed.
      void ExpectRefused(const TCHAR *xml, const std::string &name, bool saysWhy, TCHAR escape = 0)
      {
         Text text(xml);
         XDoc doc;
         PARSEINFO info;
         info.escape_value = escape;

         Expect(LoadRoot(doc, info, text, name) == NULL, name + ": parsed");

         if (saysWhy)
            Expect(info.erorr_occur && info.error_code == PIE_NOT_CLOSED, name + ": the parse error is not PIE_NOT_CLOSED");
      }

      // The root element of a text that must parse.
      LPXNode ExpectParsed(XDoc &doc, const Text &text, const std::string &name)
      {
         PARSEINFO info;
         LPXNode root = LoadRoot(doc, info, text, name);
         Expect(root != NULL, name + ": refused");
         return root;
      }
   }

   void
   XMLiteTester::Test()
   {
      TestMalformed_();
      TestEntities_();
      TestWellFormed_();
   }

   void
   XMLiteTester::TestMalformed_()
   {
      // A text that stops inside a tag name: the search for the end of the name
      // found nothing, and the copy of the name was sized from that.
      ExpectRefused(_T("<Backup><Dom"), "a text that stops inside a tag name", true);

      // A text that stops just after an opener. "<?" was recognised only as the
      // last two characters of a text, which then sent the name's search three
      // characters past the end; "<!--" and "<![CDATA[" at the end had their
      // loaders answer a place seven characters past it.
      ExpectRefused(_T("<Backup><?"), "a text that stops after \"<?\"", true);
      ExpectRefused(_T("<Backup><!--"), "a text that stops after \"<!--\"", true);
      ExpectRefused(_T("<Backup><![CDATA["), "a text that stops after \"<![CDATA[\"", true);

      // Openers whose closers never come.
      ExpectRefused(_T("<Backup><?pi a=\"1\""), "an unterminated processing instruction", true);
      ExpectRefused(_T("<Backup><!-- a comment -"), "an unterminated comment", true);
      ExpectRefused(_T("<Backup><![CDATA[data]]"), "an unterminated CDATA section", true);

      // An XML declaration and then nothing: now that the declaration is read as
      // one, there is no element after it to read.
      ExpectRefused(_T("<?xml version=\"1.0\"?>"), "an XML declaration and no element", true);
      ExpectRefused(_T("<?xml version=\"1.0\"?>\r\n"), "an XML declaration, a line break and no element", true);

      // A quoted attribute value that is never closed: the step over its
      // closing quote went over the terminator instead.
      ExpectRefused(_T("<A b=\"x"), "an unterminated attribute value", false);
      ExpectRefused(_T("<A b='x"), "an unterminated attribute value in single quotes", false);

      // An escape character, when the parse is given one, as the last character.
      ExpectRefused(_T("<A b=\"x\\"), "an escape character at the end of the text", false, _T('\\'));

      // Garbage after the element is left alone, and the parse still stops
      // within the text: XDoc::Load reads it for comments after the root, and
      // an unterminated one there answered a place past the end.
      {
         Text text(_T("<A/><!--"));
         XDoc doc;
         LPTSTR end = doc.Load(text.Get());
         Expect(end != NULL && text.Holds(end), "a trailing \"<!--\": the parse stopped past the end of the text");
         Expect(doc.GetRoot() != NULL && doc.GetRoot()->name == _T("A"), "a trailing \"<!--\": the element before it was lost");
      }
   }

   void
   XMLiteTester::TestEntities_()
   {
      // "&amp" with no ";" was taken for "&amp;" and the decoding stepped five
      // characters on - past the terminator at the end of a value, over the
      // next character anywhere else. Without its ";" it is not a reference,
      // and the value keeps it as written.
      {
         Text text(_T("<A b=\"x&amp\"/>"));
         XDoc doc;
         LPXNode root = ExpectParsed(doc, text, "\"x&amp\" in an attribute");
         Expect(root->GetAttrValue(_T("b")) == _T("x&amp"), "\"x&amp\" in an attribute is not kept as written");
      }

      {
         Text text(_T("<A>x&lt</A>"));
         XDoc doc;
         LPXNode root = ExpectParsed(doc, text, "\"x&lt\" as text");
         Expect(root->value == _T("x&lt"), "\"x&lt\" as text is not kept as written");
      }

      {
         Text text(_T("<A>&ampX</A>"));
         XDoc doc;
         LPXNode root = ExpectParsed(doc, text, "\"&ampX\" as text");
         Expect(root->value == _T("&ampX"), "\"&ampX\" as text is not kept as written");
      }

      {
         Text text(_T("<A b=\"&quot\">&apos</A>"));
         XDoc doc;
         LPXNode root = ExpectParsed(doc, text, "\"&quot\" and \"&apos\" at the end");
         Expect(root->GetAttrValue(_T("b")) == _T("&quot") && root->value == _T("&apos"),
            "\"&quot\" and \"&apos\" without their \";\" are not kept as written");
      }

      // Every reference with its ";", decoded.
      {
         Text text(_T("<A b=\"x&amp;y\">&lt;&gt;&quot;&apos;&amp;</A>"));
         XDoc doc;
         LPXNode root = ExpectParsed(doc, text, "the five references");
         Expect(root->GetAttrValue(_T("b")) == _T("x&y"), "\"&amp;\" in an attribute is not decoded");
         Expect(root->value == _T("<>\"'&"), "the five references as text are not decoded");
      }
   }

   void
   XMLiteTester::TestWellFormed_()
   {
      // An XML declaration, a comment and a CDATA section, which the parser
      // did not recognise until 26 September 2026: it compared their openers by
      // their size in bytes, twice their length in characters.
      Text text(_T("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n")
                _T("<Backup Version=\"6\"><!-- written by the tester --><Domains><Domain Name=\"a&amp;b\"/></Domains>")
                _T("<![CDATA[x]]></Backup>\r\n"));
      XDoc doc;
      LPTSTR end = doc.Load(text.Get());
      Expect(end != NULL && text.Holds(end), "the well-formed document: refused, or its parse stopped past the end");

      LPXNode root = doc.GetRoot();
      Expect(root != NULL && root->name == _T("Backup") && root->GetAttrValue(_T("Version")) == _T("6"),
         "the well-formed document: its element is not Backup, Version 6");

      LPXNode declaration = doc.GetChild((unsigned int) 0);
      Expect(declaration != NULL && declaration->type == XNODE_PI && declaration->name == _T("xml") &&
             declaration->GetAttrValue(_T("encoding")) == _T("utf-8"),
         "the well-formed document: the XML declaration is not its first node");

      LPXNode domains = root->GetChild(_T("Domains"));
      LPXNode domain = domains ? domains->GetChild(_T("Domain")) : NULL;
      Expect(domain != NULL && domain->GetAttrValue(_T("Name")) == _T("a&b"),
         "the well-formed document: Domains/Domain Name is not \"a&b\"");

      bool comment = false;
      bool cdata = false;
      for (int i = 0; i < root->GetChildCount(); i++)
      {
         LPXNode child = root->GetChild((unsigned int) i);
         if (child->type == XNODE_COMMENT && child->value == _T(" written by the tester "))
            comment = true;
         if (child->type == XNODE_CDATA && child->value == _T("x"))
            cdata = true;
      }
      Expect(comment, "the well-formed document: the comment is not among Backup's nodes");
      Expect(cdata, "the well-formed document: the CDATA section is not among Backup's nodes");

      // A processing instruction inside an element, before the element's text
      // and before its close tag: what follows each is the element's to read,
      // as after a comment, not a tag of the instruction's own.
      {
         Text inner(_T("<A><?pi x=\"1\"?>text<B/><?pi y=\"2\"?></A>"));
         XDoc innerDoc;
         LPXNode element = ExpectParsed(innerDoc, inner, "processing instructions inside an element");
         Expect(element->name == _T("A") && element->value == _T("text") && element->GetChild(_T("B")) != NULL,
            "processing instructions inside an element: A is not the element, with the text \"text\" and the child B");
      }
   }
}
