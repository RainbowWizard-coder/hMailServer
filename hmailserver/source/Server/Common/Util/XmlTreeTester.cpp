// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The backup index's XML tree, written and read. See XmlTreeTester.h.

#include "stdafx.h"

#include "XmlTreeTester.h"
#include "XmlTree.h"

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
            throw std::logic_error("XmlTreeTester: " + message);
      }

      // A text that must not be read, and the problem it must record.
      void ExpectRefused(const String &text, XmlProblem problem, const std::string &name)
      {
         XDoc document;
         Expect(!document.Parse(text), name + ": read");
         Expect(document.GetProblem() == problem, name + ": the problem recorded is not the one expected");
      }

      // The root element of a text that must be read.
      XNode *ExpectRead(XDoc &document, const String &text, const std::string &name)
      {
         Expect(document.Parse(text), name + ": refused");

         XNode *root = document.GetRoot();
         Expect(root != nullptr, name + ": no root element");
         return root;
      }

      // Every character but NUL: each UTF-16 code unit on Windows, each code
      // point where wchar_t is 32 bits.
      String EveryCharacter()
      {
         const unsigned long highest = sizeof(wchar_t) == 2 ? 0xFFFFUL : 0x10FFFFUL;

         String characters;
         characters.reserve(highest);

         for (unsigned long character = 1; character <= highest; character++)
            characters += static_cast<wchar_t>(character);

         return characters;
      }

      String Repeated(const wchar_t *text, int count)
      {
         String repeated;
         for (int index = 0; index < count; index++)
            repeated += text;

         return repeated;
      }
   }

   void
   XmlTreeTester::Test()
   {
      TestWriting_();
      TestRoundTrip_();
      TestReading_();
      TestRefusals_();
      TestEntities_();
      TestDepth_();
      TestEditing_();
   }

   void
   XmlTreeTester::TestWriting_()
   {
      // An index's shape, as every hMailServer since 5.7 has written it.
      {
         XDoc document;
         XNode *backup = document.AppendChild(_T("Backup"));
         XNode *information = backup->AppendChild(_T("BackupInformation"));
         information->AppendAttr(_T("Mode"), _T("11"));
         information->AppendAttr(_T("Version"), _T("6.3.6"));
         XNode *domain = backup->AppendChild(_T("Domains"))->AppendChild(_T("Domain"));
         domain->AppendAttr(_T("Name"), _T("a&b<c>\"d'e"));
         domain->AppendChild(_T("Accounts"));

         Expect(document.GetXML() ==
            _T("\r\n\r\n<Backup>")
            _T("\r\n\t<BackupInformation Mode=\"11\" Version=\"6.3.6\" />")
            _T("\r\n\t<Domains>")
            _T("\r\n\t\t<Domain Name=\"a&amp;b&lt;c&gt;&quot;d&apos;e\" >")
            _T("\r\n\t\t\t<Accounts/>")
            _T("\r\n\t\t</Domain>")
            _T("\r\n\t</Domains>")
            _T("\r\n</Backup>"),
            "an index's shape is not written as it always has been");
      }

      // Text beside children, on a line of its own; control characters and
      // line breaks in an attribute, as they are; a node written on its own.
      {
         XDoc document;
         XNode *element = document.AppendChild(_T("A"), _T("x<y"));
         element->AppendAttr(_T("k"), _T("line\r\nbreak\ttab\x01"));
         element->AppendChild(_T("B"));
         document.AppendChild(_T("C"), _T("t"));

         Expect(document.GetXML() ==
            _T("\r\n\r\n<A k=\"line\r\nbreak\ttab\x01\" >\r\n\t<B/>\r\n\tx&lt;y\r\n</A>\r\n<C>t</C>"),
            "text beside children, or an attribute with control characters, is not written as it always has been");

         Expect(element->GetXML() == _T("\r\n<A k=\"line\r\nbreak\ttab\x01\" >\r\n\t<B/>\r\n\tx&lt;y\r\n</A>"),
            "a node on its own is not written as it always has been");

         XDoc empty;
         Expect(empty.GetXML() == _T("\r\n"), "an empty document is not written as one line break");

         // A value holding a NUL, which only an assignment to the field can put
         // there, is written up to it, as a C string would be.
         XDoc cut;
         XNode *node = cut.AppendChild(_T("N"));
         String withNul = _T("a<");
         withNul += _T('\0');
         withNul += _T("b");
         node->value = withNul;
         node->AppendAttr(_T("k"), _T(""))->value = withNul;
         Expect(cut.GetXML() == _T("\r\n\r\n<N k=\"a&lt;\" >a&lt;</N>"), "a value is not written up to its first NUL");
      }

      // The other kinds of node, which only reading makes.
      {
         XDoc document;
         Expect(document.Parse(_T("<?xml version=\"1.0\"?><!--c--><R><![CDATA[d&]]></R>")), "the other kinds of node: refused");
         Expect(document.GetXML() == _T("\r\n\r\n<?xml version=\"1.0\" ?>\r\n<!--c-->\r\n<R>\r\n\t<![CDATA[d&]]>\r\n</R>"),
            "a processing instruction, a comment or a CDATA section is not written as it always has been");
      }
   }

   void
   XmlTreeTester::TestRoundTrip_()
   {
      // Every character, in an attribute and as an element's text, written and
      // read back as it was.
      const String every = EveryCharacter();

      XDoc written;
      XNode *root = written.AppendChild(_T("R"));
      root->AppendAttr(_T("v"), every.c_str());
      root->AppendChild(_T("T"), every.c_str());

      XDoc read;
      XNode *readRoot = ExpectRead(read, written.GetXML(), "every character");
      Expect(readRoot->GetAttrValue(_T("v")) == every, "every character: the attribute is not read back as it was written");

      const wchar_t *text = readRoot->GetChildValue(_T("T"));
      Expect(text != nullptr && every == text, "every character: the text is not read back as it was written");
   }

   void
   XmlTreeTester::TestReading_()
   {
      // A declaration, a comment and a CDATA section, and the five references.
      {
         XDoc document;
         XNode *root = ExpectRead(document,
            _T("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n")
            _T("<Backup Version=\"6\"><!-- written by the tester --><Domains><Domain Name=\"a&amp;b\"/></Domains>")
            _T("<![CDATA[x]]></Backup>\r\n"),
            "the well-formed document");

         Expect(root->name == _T("Backup") && root->GetAttrValue(_T("Version")) == _T("6"),
            "the well-formed document: its element is not Backup, Version 6");

         XNode *declaration = document.GetChild(0U);
         Expect(declaration != nullptr && declaration->GetType() == XNodeType::ProcessingInstruction &&
                declaration->name == _T("xml") && declaration->GetAttrValue(_T("encoding")) == _T("utf-8"),
            "the well-formed document: the XML declaration is not its first node");

         XAttr *name = root->GetChildAttr(_T("Domains"), _T("Name"));
         Expect(name == nullptr, "the well-formed document: Domains has a Name");

         XNode *domain = root->GetChild(_T("Domains"))->GetChild(_T("Domain"));
         Expect(domain != nullptr && domain->GetAttrValue(_T("Name")) == _T("a&b"),
            "the well-formed document: Domains/Domain Name is not \"a&b\"");

         Expect(root->GetChildCount() == 3 &&
                root->GetChild(0U)->GetType() == XNodeType::Comment && root->GetChild(0U)->value == _T(" written by the tester ") &&
                root->GetChild(2U)->GetType() == XNodeType::CData && root->GetChild(2U)->value == _T("x"),
            "the well-formed document: the comment and the CDATA section are not Backup's first and last nodes");
      }

      // Processing instructions inside an element are the document's, and what
      // follows each is the element's to read.
      {
         XDoc document;
         XNode *element = ExpectRead(document, _T("<A><?pi x=\"1\"?>text<B/><?pi y=\"2\"?></A>"), "processing instructions inside an element");
         Expect(element->name == _T("A") && element->value == _T("text") && element->GetChildCount() == 1 && element->GetChild(_T("B")) != nullptr,
            "processing instructions inside an element: A is not the element, with the text \"text\" and the child B");
         Expect(document.GetChildCount() == 3 && document.GetChild(1U)->GetAttrValue(_T("y")) == _T("2") && document.GetChild(2U) == element,
            "processing instructions inside an element: they are not the document's, before the element");
      }

      // An index cut short, as by a full disk, reads as far as it goes.
      {
         XDoc document;
         XNode *backup = ExpectRead(document, _T("<Backup><BackupInformation Mode=\"1\"/><Domains><Domain Name=\"x\"/>"), "a truncated index");
         Expect(backup->GetChildAttr(_T("BackupInformation"), _T("Mode")) != nullptr &&
                backup->GetChildAttr(_T("BackupInformation"), _T("Mode"))->value == _T("1") &&
                backup->GetChild(_T("Domains"))->GetChildAttr(_T("Domain"), _T("Name"))->value == _T("x"),
            "a truncated index: what it holds is not read");
      }

      // A text is read up to its first NUL, as the C string it used to be handed
      // as: what follows is not there, so here the element is closed at the NUL.
      {
         String text = _T("<A>x");
         text += _T('\0');
         text += _T("</B>");
         XDoc document;
         XNode *root = ExpectRead(document, text, "a text with a NUL in it");
         Expect(root->value == _T("x"), "a text with a NUL in it: what follows the NUL is read");
      }

      // An index read from an indented file: each element with children has as
      // its text the line break and tabs before its close tag, as it always had.
      {
         XDoc written;
         XNode *backup = written.AppendChild(_T("Backup"));
         backup->AppendChild(_T("Domains"))->AppendChild(_T("Domain"))->AppendAttr(_T("Name"), _T("x"));

         XDoc read;
         XNode *root = ExpectRead(read, written.GetXML(), "an indented index");
         Expect(root->value == _T("\r\n") && root->GetChild(_T("Domains"))->value == _T("\r\n\t"),
            "an indented index: the text of an element with children is not the space before its close tag");
      }

      // The lenient forms: text before the first tag, attributes with no value
      // or no quotes, and a close tag with a space before its '>', which leaves
      // the '>' to the text after it.
      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("\xFEFF junk <R b c=1 d='2' e=\"3\"><A>x</A >y</R>"), "the lenient forms");
         Expect(root->GetAttrCount() == 4 && root->GetAttr(0U)->name == _T("b") && root->GetAttr(0U)->value.IsEmpty() &&
                root->GetAttrValue(_T("c")) == _T("1") && root->GetAttrValue(_T("d")) == _T("2") && root->GetAttrValue(_T("e")) == _T("3"),
            "the lenient forms: the attributes are not b, c=1, d=2, e=3");
         Expect(root->value == _T(">y") && root->GetChildValue(_T("A")) != nullptr && String(root->GetChildValue(_T("A"))) == _T("x"),
            "the lenient forms: the texts are not \">y\" and \"x\"");
         Expect(root->GetChildValue(_T("Z")) == nullptr, "the lenient forms: a child that is not there has a value");
      }

      // And the rest of what every earlier version read, each of which a
      // reader written to the XML specification would read otherwise: "<!-->"
      // is an empty comment; "<![cdata[" opens a CDATA section; an unquoted
      // value runs over a '/', so "<B c=1/>" is not empty; an element with no
      // name is dropped with what it holds; vertical tab and form feed are
      // space between attributes (iswspace), and a tab before '=' is part of
      // the name.
      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("<R a=\"1\"\x0B\x0C") _T("b=\"2\" c\t=\"3\"><!--><![cdata[d]]><B c=1/></B><>x<Q/></></R>"), "the other lenient forms");
         Expect(root->GetAttrValue(_T("b")) == _T("2") && root->GetAttr(_T("c")) == nullptr && root->GetAttrValue(_T("c\t")) == _T("3"),
            "the other lenient forms: the attributes are not a, b and \"c\\t\"");
         Expect(root->GetChildCount() == 3 && root->GetChild(0U)->GetType() == XNodeType::Comment && root->GetChild(0U)->value.IsEmpty() &&
                root->GetChild(1U)->GetType() == XNodeType::CData && root->GetChild(1U)->value == _T("d"),
            "the other lenient forms: \"<!-->\" is not an empty comment, or \"<![cdata[\" does not open a CDATA section");
         Expect(root->GetChild(2U)->name == _T("B") && root->GetChild(2U)->GetAttrValue(_T("c")) == _T("1/") && root->GetChild(_T("Q")) == nullptr,
            "the other lenient forms: an unquoted value stops at '/', or an element with no name is kept");
      }
   }

   void
   XmlTreeTester::TestRefusals_()
   {
      // A text that stops inside a tag name, or just after an opener.
      ExpectRefused(_T("<Backup><Dom"), XmlProblem::NotClosed, "a text that stops inside a tag name");
      ExpectRefused(_T("<Backup><?"), XmlProblem::NotClosed, "a text that stops after \"<?\"");
      ExpectRefused(_T("<Backup><!--"), XmlProblem::NotClosed, "a text that stops after \"<!--\"");
      ExpectRefused(_T("<Backup><![CDATA["), XmlProblem::NotClosed, "a text that stops after \"<![CDATA[\"");

      // Openers whose closers never come.
      ExpectRefused(_T("<Backup><?pi a=\"1\""), XmlProblem::NotClosed, "an unterminated processing instruction");
      ExpectRefused(_T("<Backup><!-- a comment -"), XmlProblem::NotClosed, "an unterminated comment");
      ExpectRefused(_T("<Backup><![CDATA[data]]"), XmlProblem::NotClosed, "an unterminated CDATA section");

      // An XML declaration and no element.
      ExpectRefused(_T("<?xml version=\"1.0\"?>"), XmlProblem::NotClosed, "an XML declaration and no element");
      ExpectRefused(_T("<?xml version=\"1.0\"?>\r\n"), XmlProblem::NotClosed, "an XML declaration, a line break and no element");

      // Texts that end inside a tag, and texts with no tag, are refused with
      // nothing to report.
      ExpectRefused(_T("<A b=\"x"), XmlProblem::None, "an unterminated attribute value");
      ExpectRefused(_T("<A b='x"), XmlProblem::None, "an unterminated attribute value in single quotes");
      ExpectRefused(_T(""), XmlProblem::None, "an empty text");
      ExpectRefused(_T("no markup at all"), XmlProblem::None, "a text with no '<'");

      // The other refusals.
      ExpectRefused(_T("<A></B>"), XmlProblem::Nesting, "a close tag that is not the element's");
      ExpectRefused(_T("<A/ >"), XmlProblem::EmptyTagNotClosed, "a / not followed by >");
      ExpectRefused(_T("<A b"), XmlProblem::Attribute, "a text that ends in an attribute name");

      // The first problem is the one kept: an instruction's attribute the text
      // ends in is read past, and then there is no element either.
      ExpectRefused(_T("<?pi abc?>"), XmlProblem::Attribute, "an instruction's unended attribute, and no element");

      // A comment the text ends in after the root element is left, and the
      // element before it kept.
      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("<A/><!--"), "a trailing \"<!--\"");
         Expect(root->name == _T("A") && document.GetChildCount() == 1, "a trailing \"<!--\": the element before it was lost");
         Expect(document.GetProblem() == XmlProblem::NotClosed, "a trailing \"<!--\": the problem is not recorded");
      }
   }

   void
   XmlTreeTester::TestEntities_()
   {
      // A reference without its ";" is not one, and is kept as written.
      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("<A b=\"x&amp\" c=\"&quot\">&ampX&apos</A>"), "references without their \";\"");
         Expect(root->GetAttrValue(_T("b")) == _T("x&amp") && root->GetAttrValue(_T("c")) == _T("&quot") && root->value == _T("&ampX&apos"),
            "references without their \";\" are not kept as written");
      }

      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("<A>x&lt</A>"), "\"x&lt\" as text");
         Expect(root->value == _T("x&lt"), "\"x&lt\" as text is not kept as written");
      }

      // The five, decoded once; any other reference kept.
      {
         XDoc document;
         XNode *root = ExpectRead(document, _T("<A b=\"x&amp;y&amp;lt;\">&lt;&gt;&quot;&apos;&amp;&#65;&nbsp;</A>"), "the five references");
         Expect(root->GetAttrValue(_T("b")) == _T("x&y&lt;"), "the five references: an attribute is not decoded once");
         Expect(root->value == _T("<>\"'&&#65;&nbsp;"), "the five references: the text is not decoded, or another reference is not kept");
      }
   }

   void
   XmlTreeTester::TestDepth_()
   {
      // An empty element at the deepest level is read; an element there with
      // anything in it, even its close tag, is refused.
      {
         XDoc document;
         String deepest = Repeated(_T("<a>"), XDoc::MaximumDepth - 1) + _T("<b/>") + Repeated(_T("</a>"), XDoc::MaximumDepth - 1);
         Expect(document.Parse(deepest), "nesting to the deepest level is refused");
      }

      ExpectRefused(Repeated(_T("<a>"), XDoc::MaximumDepth) + Repeated(_T("</a>"), XDoc::MaximumDepth),
         XmlProblem::Nesting, "nesting one level too deep");

      // Far too deep: refused at the limit, with the stack never near its end.
      ExpectRefused(Repeated(_T("<a>"), 100000), XmlProblem::Nesting, "nesting a hundred thousand levels deep");
   }

   void
   XmlTreeTester::TestEditing_()
   {
      XDoc document;
      XNode *root = document.AppendChild(_T("R"));
      XNode *first = root->AppendChild(_T("A"));
      root->AppendChild(_T("A"), _T("second"));
      XAttr *attribute = root->AppendAttr(_T("k"), _T("v"));
      root->AppendAttr(_T("k"), _T("w"));

      Expect(document.GetRoot() == root && root->parent == &document && first->parent == root && attribute->parent == root,
         "editing: the parents are not the nodes that hold them");
      Expect(root->GetChild(_T("A")) == first && root->GetAttrValue(_T("k")) == _T("v"), "editing: a name does not find the first of its name");
      Expect(root->GetAttrValue(_T("missing")).IsEmpty() && root->GetAttr(_T("missing")) == nullptr && root->GetChildAttr(_T("A"), _T("k")) == nullptr,
         "editing: what is not there is found");

      XDoc other;
      Expect(!root->RemoveChild(other.AppendChild(_T("A"))), "editing: a node that is not a child is removed");
      Expect(root->RemoveChild(first) && root->GetChildCount() == 1 && String(root->GetChildValue(_T("A"))) == _T("second"),
         "editing: removing the first A does not leave the second");
      Expect(root->RemoveAttr(attribute) && root->GetAttrCount() == 1 && root->GetAttrValue(_T("k")) == _T("w"),
         "editing: removing the first k does not leave the second");
      XAttr *foreign = other.AppendChild(_T("B"))->AppendAttr(_T("k"), _T("x"));
      Expect(!root->RemoveAttr(foreign) && foreign->parent != root, "editing: an attribute of another node is removed");

      XNode *unnamed = root->AppendChild(nullptr, nullptr);
      Expect(unnamed->name.IsEmpty() && unnamed->value.IsEmpty(), "editing: a null name or value is not empty");
   }
}
