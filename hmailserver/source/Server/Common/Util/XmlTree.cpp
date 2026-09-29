// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The backup index's XML tree: writing it, and reading it back. XmlTree.h says
// what is written and what is read, and why the reader is as lenient as it is.

#include "stdafx.h"

#include "XmlTree.h"

#include <algorithm>
#include <cwctype>
#include <locale>
#include <memory>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   const size_t NotFound = static_cast<size_t>(-1);

   // The five references, both ways.
   struct Reference
   {
      wchar_t character;
      const wchar_t *text;
      size_t length;
   };

   const Reference References[] =
   {
      { L'&', L"&amp;", 5 },
      { L'"', L"&quot;", 6 },
      { L'\'', L"&apos;", 6 },
      { L'<', L"&lt;", 4 },
      { L'>', L"&gt;", 4 },
   };

   const Reference *ReferenceFor(wchar_t character)
   {
      for (const Reference &reference : References)
      {
         if (reference.character == character)
            return &reference;
      }

      return nullptr;
   }

   // characters, with & " ' < > as references, up to the first NUL.
   void AppendEscaped(HM::String &text, const HM::String &characters)
   {
      for (wchar_t character : characters)
      {
         if (character == 0)
            break;

         const Reference *reference = ReferenceFor(character);
         if (reference)
            text.append(reference->text, reference->length);
         else
            text += character;
      }
   }

   void AppendLine(HM::String &text, size_t depth)
   {
      text += L"\r\n";
      text.append(depth, L'\t');
   }

   // True when every character is space as the global locale sees it - the
   // question the reader asks of an element's text before it takes another run.
   bool IsBlank(const HM::String &text)
   {
      const std::locale locale;

      for (wchar_t character : text)
      {
         if (!std::isspace(character, locale))
            return false;
      }

      return true;
   }
}

// ---------------------------------------------------------------- the reader

// One document read from one text. The text is looked at by position, never
// past its length, and each Read* answers false when the reading must stop -
// having recorded why, when there is a why to record.
class XmlTreeReader
{
public:
   // The text up to its first NUL, as a C string would be read.
   XmlTreeReader(XDoc &document, const HM::String &text) :
      document_(document),
      text_(text.c_str()),
      length_(std::min(text.find(L'\0'), text.size()))
   {
   }

   bool Read()
   {
      size_t start = Find(L'<', 0);
      if (start == NotFound)
         return false;

      std::unique_ptr<XNode> root(new XNode(XNodeType::Element));

      // A close tag where the root should be: an element with no name, and
      // nothing more read into it.
      if (At(start + 1) == L'/')
      {
         document_.Adopt_(std::move(root));
         ReadAfterRoot(start);
         return true;
      }

      size_t position = start;
      if (!ReadBeforeRoot(position))
         return false;

      if (At(position) != L'<')
      {
         Note(XmlProblem::NotClosed, L"An element was expected where the text has none.");
         return false;
      }

      size_t end = 0;
      if (!ReadElement(*root, position, 1, end))
         return false;

      document_.Adopt_(std::move(root));
      ReadAfterRoot(end);
      return true;
   }

private:
   // ------------------------------------------------------------ the text

   wchar_t At(size_t position) const
   {
      return position < length_ ? text_[position] : L'\0';
   }

   size_t Find(wchar_t character, size_t from) const
   {
      for (size_t position = from; position < length_; position++)
      {
         if (text_[position] == character)
            return position;
      }

      return NotFound;
   }

   // The first of any of the characters in set, a literal.
   size_t FindAny(const wchar_t *set, size_t from) const
   {
      for (size_t position = from; position < length_; position++)
      {
         for (const wchar_t *member = set; *member; member++)
         {
            if (text_[position] == *member)
               return position;
         }
      }

      return NotFound;
   }

   // Whether the length characters of needle are the text's at position.
   bool Matches(size_t position, const wchar_t *needle, size_t length) const
   {
      if (position > length_ || length > length_ - position)
         return false;

      for (size_t index = 0; index < length; index++)
      {
         if (text_[position + index] != needle[index])
            return false;
      }

      return true;
   }

   size_t FindText(const wchar_t *needle, size_t length, size_t from) const
   {
      for (size_t position = from; position < length_; position++)
      {
         if (Matches(position, needle, length))
            return position;
      }

      return NotFound;
   }

   size_t SkipSpace(size_t from) const
   {
      size_t position = from;
      while (position < length_ && iswspace(static_cast<wint_t>(text_[position])))
         position++;

      return position;
   }

   // Whether the text at position begins with opener, A to Z matching a to z.
   bool Opens(size_t position, const wchar_t *opener) const
   {
      for (size_t index = 0; opener[index]; index++)
      {
         wchar_t character = At(position + index);
         wchar_t expected = opener[index];

         if (character >= L'A' && character <= L'Z')
            character = static_cast<wchar_t>(character - L'A' + L'a');
         if (expected >= L'A' && expected <= L'Z')
            expected = static_cast<wchar_t>(expected - L'A' + L'a');

         if (character != expected)
            return false;
      }

      return true;
   }

   // The characters from begin up to end; empty when end is not after begin.
   HM::String Span(size_t begin, size_t end) const
   {
      if (end == NotFound || end <= begin)
         return HM::String();

      HM::String span;
      span.assign(text_ + begin, end - begin);
      return span;
   }

   // The characters from begin up to end, the five references read as their
   // characters and anything else as it is.
   HM::String Decoded(size_t begin, size_t end) const
   {
      HM::String decoded;
      if (end == NotFound || end <= begin)
         return decoded;

      decoded.reserve(end - begin);

      size_t position = begin;
      while (position < end)
      {
         const Reference *found = nullptr;

         if (text_[position] == L'&')
         {
            for (const Reference &reference : References)
            {
               if (reference.length <= end - position && Matches(position, reference.text, reference.length))
               {
                  found = &reference;
                  break;
               }
            }
         }

         if (found)
         {
            decoded += found->character;
            position += found->length;
         }
         else
         {
            decoded += text_[position];
            position++;
         }
      }

      return decoded;
   }

   // The first problem found is the one kept, but for a nesting too deep, which
   // is kept over whatever was found before it.
   void Note(XmlProblem problem, const HM::String &text, bool overrides = false)
   {
      if (document_.problem_ != XmlProblem::None && !overrides)
         return;

      document_.problem_ = problem;
      document_.problem_text_ = text;
   }

   // ------------------------------------------------------------ the markup

   bool IsOtherNode(size_t position) const
   {
      return Opens(position, L"<?") || Opens(position, L"<!--") || Opens(position, L"<![CDATA[");
   }

   // A processing instruction, comment or CDATA section at position, which
   // IsOtherNode has said is there. A comment or CDATA section goes into
   // container; a processing instruction always into the document. end is
   // where what follows it starts.
   bool ReadOtherNode(size_t position, XNode &container, size_t &end)
   {
      if (Opens(position, L"<?"))
      {
         const size_t close = FindText(L"?>", 2, position);
         if (close == NotFound)
         {
            Note(XmlProblem::NotClosed, L"A processing instruction (<?) is not closed with ?>.");
            return false;
         }

         const size_t nameStart = position + 2;
         const size_t nameEnd = FindAny(L" ?>", nameStart);
         if (nameEnd == NotFound)
         {
            Note(XmlProblem::NotClosed, L"A processing instruction's name is not ended.");
            return false;
         }

         std::unique_ptr<XNode> instruction(new XNode(XNodeType::ProcessingInstruction));
         instruction->name = Span(nameStart, nameEnd);

         // Its attributes end where the instruction does; what is wrong with
         // them is recorded, and the instruction is kept with those read.
         size_t unused = 0;
         (void) ReadAttributes(*instruction, nameEnd, close, unused);

         document_.Adopt_(std::move(instruction));
         end = close + 2;
         return true;
      }

      const bool comment = Opens(position, L"<!--");
      const wchar_t *closer = comment ? L"-->" : L"]]>";
      const size_t openerLength = comment ? 4 : 9;

      const size_t close = FindText(closer, 3, position);
      if (close == NotFound)
      {
         Note(XmlProblem::NotClosed, comment ? L"A comment (<!--) is not closed with -->." : L"A CDATA section is not closed with ]]>.");
         return false;
      }

      std::unique_ptr<XNode> node(new XNode(comment ? XNodeType::Comment : XNodeType::CData));
      node->name = comment ? L"#COMMENT" : L"#CDATA";
      node->value = Span(position + openerLength, close);

      container.Adopt_(std::move(node));
      end = close + 3;
      return true;
   }

   // Before the root element: whitespace and the nodes that are not elements,
   // until something else. position is left at that something.
   bool ReadBeforeRoot(size_t &position)
   {
      for (;;)
      {
         position = SkipSpace(position);
         if (!IsOtherNode(position))
            return true;

         if (!ReadOtherNode(position, document_, position))
            return false;
      }
   }

   // After it the same, as far as they go; nothing there fails the document.
   void ReadAfterRoot(size_t position)
   {
      for (;;)
      {
         position = SkipSpace(position);
         if (!IsOtherNode(position) || !ReadOtherNode(position, document_, position))
            return;
      }
   }

   // An element's attributes, from just after its name. They end at a '>' or
   // '/' where a name would start or, when limit is not NotFound, at limit or
   // beyond (a processing instruction's, whose ?> is at limit). stop is where
   // they ended.
   bool ReadAttributes(XNode &node, size_t from, size_t limit, size_t &stop)
   {
      size_t position = from;

      while (position < length_)
      {
         position = SkipSpace(position);

         const bool ended = limit != NotFound ? position >= limit : (At(position) == L'>' || At(position) == L'/');
         if (ended)
         {
            stop = position;
            return true;
         }

         const size_t nameEnd = FindAny(L" =", position);
         if (nameEnd == NotFound)
         {
            Note(XmlProblem::Attribute, L"An attribute of <" + node.name + L"> is not ended.");
            return false;
         }

         XAttr *attribute = node.AppendAttr(Span(position, nameEnd).c_str(), nullptr);

         position = SkipSpace(nameEnd);
         if (At(position) != L'=')
            continue;

         position = SkipSpace(position + 1);

         const wchar_t quote = At(position);
         const bool quoted = quote == L'"' || quote == L'\'';

         size_t valueStart = position;
         size_t valueEnd = 0;

         if (quoted)
         {
            valueStart = position + 1;
            valueEnd = Find(quote, valueStart);
         }
         else
            valueEnd = FindAny(L" >", valueStart);

         if (valueEnd == NotFound)
            valueEnd = length_;

         attribute->value = Decoded(valueStart, valueEnd);

         position = valueEnd;
         if (quoted && position < length_)
            position++;
      }

      // The text ended inside the tag.
      return false;
   }

   // An element whose start tag begins at position (a '<' that opens neither a
   // close tag nor another node), at depth. end is just after it.
   bool ReadElement(XNode &element, size_t position, int depth, size_t &end)
   {
      const size_t nameStart = position + 1;
      const size_t nameEnd = FindAny(L" />\t\r\n", nameStart);
      if (nameEnd == NotFound)
      {
         Note(XmlProblem::NotClosed, L"An element's name is not ended.");
         return false;
      }

      element.name = Span(nameStart, nameEnd);

      size_t tagEnd = 0;
      if (!ReadAttributes(element, nameEnd, NotFound, tagEnd))
         return false;

      if (At(tagEnd) == L'/')
      {
         if (At(tagEnd + 1) != L'>')
         {
            Note(XmlProblem::EmptyTagNotClosed, L"<" + element.name + L"> has a / that is not followed by >.");
            return false;
         }

         end = tagEnd + 2;
         return true;
      }

      return ReadContent(element, tagEnd + 1, depth, end);
   }

   // A run of text from position to the next '<', or to the end.
   size_t RunEnd(size_t position) const
   {
      const size_t next = Find(L'<', position);
      return next == NotFound ? length_ : next;
   }

   // What is inside an element, from just after its start tag, and its close
   // tag. end is just after that, or the end of the text.
   bool ReadContent(XNode &element, size_t position, int depth, size_t &end)
   {
      size_t runEnd = RunEnd(position);
      if (runEnd > position)
         element.value = Decoded(position, runEnd);

      position = runEnd;

      while (position < length_)
      {
         if (!ReadInside(element, position, depth + 1, position))
            return false;

         if (At(position) == L'<' && At(position + 1) == L'/')
            return ReadCloseTag(element, position, end);

         if (At(position) != L'<' && IsBlank(element.value))
         {
            runEnd = RunEnd(position);
            if (runEnd > position)
               element.value = Decoded(position, runEnd);

            position = runEnd;
         }
      }

      end = position;
      return true;
   }

   // One step inside an element, taken at depth: to the next '<' and what it
   // opens - a close tag, which is left for the element to read, another node,
   // or a child element. next is where what follows it starts.
   bool ReadInside(XNode &element, size_t position, int depth, size_t &next)
   {
      if (depth > XDoc::MaximumDepth)
      {
         const int maximum = XDoc::MaximumDepth;
         Note(XmlProblem::Nesting, L"The document nests elements deeper than " + HM::String(std::to_wstring(maximum)) +
            L" levels, and is refused rather than read: the reader recurses once for each level.", true);
         return false;
      }

      const size_t open = Find(L'<', position);
      if (open == NotFound)
         return false;

      if (At(open + 1) == L'/')
      {
         next = open;
         return true;
      }

      if (IsOtherNode(open))
         return ReadOtherNode(open, element, next);

      std::unique_ptr<XNode> child(new XNode(XNodeType::Element));
      if (!ReadElement(*child, open, depth, next))
         return false;

      // An element with no name is read and then dropped, with all it held.
      if (!child->name.IsEmpty())
         element.Adopt_(std::move(child));

      return true;
   }

   // The close tag at position, "</", which must name element.
   bool ReadCloseTag(const XNode &element, size_t position, size_t &end)
   {
      const size_t nameStart = SkipSpace(position + 2);
      const size_t nameEnd = FindAny(L" >", nameStart);
      if (nameEnd == NotFound)
      {
         Note(XmlProblem::NotClosed, L"<" + element.name + L"> is not closed: its close tag is not ended.");
         return false;
      }

      const HM::String closeName = Span(nameStart, nameEnd);
      if (closeName != element.name)
      {
         Note(XmlProblem::Nesting, L"<" + element.name + L"> is closed by </" + closeName + L">.");
         return false;
      }

      end = nameEnd + 1;
      return true;
   }

   XDoc &document_;
   const wchar_t *text_;
   size_t length_;
};

// ---------------------------------------------------------------- XNode

XNode::XNode(XNodeType type) :
   parent(nullptr),
   type_(type)
{
}

XNode::~XNode()
{
}

void
XNode::Clear_()
{
   children_.clear();
   attributes_.clear();
}

int
XNode::GetChildCount() const
{
   return static_cast<int>(children_.size());
}

XNode *
XNode::GetChild(unsigned int index) const
{
   return index < children_.size() ? children_[index].get() : nullptr;
}

XNode *
XNode::GetChild(const wchar_t *childName) const
{
   if (!childName)
      return nullptr;

   for (const std::unique_ptr<XNode> &child : children_)
   {
      if (child->name == childName)
         return child.get();
   }

   return nullptr;
}

const wchar_t *
XNode::GetChildValue(const wchar_t *childName) const
{
   XNode *child = GetChild(childName);
   return child ? child->value.c_str() : nullptr;
}

XAttr *
XNode::GetChildAttr(const wchar_t *childName, const wchar_t *attrName) const
{
   XNode *child = GetChild(childName);
   return child ? child->GetAttr(attrName) : nullptr;
}

XNode *
XNode::AppendChild(const wchar_t *childName, const wchar_t *childValue)
{
   std::unique_ptr<XNode> child(new XNode(XNodeType::Element));
   child->name = childName ? childName : L"";
   child->value = childValue ? childValue : L"";

   return Adopt_(std::move(child));
}

XNode *
XNode::Adopt_(std::unique_ptr<XNode> child)
{
   child->parent = this;
   children_.push_back(std::move(child));
   return children_.back().get();
}

bool
XNode::RemoveChild(XNode *child)
{
   for (auto iterator = children_.begin(); iterator != children_.end(); ++iterator)
   {
      if (iterator->get() == child)
      {
         children_.erase(iterator);
         return true;
      }
   }

   return false;
}

int
XNode::GetAttrCount() const
{
   return static_cast<int>(attributes_.size());
}

XAttr *
XNode::GetAttr(unsigned int index) const
{
   return index < attributes_.size() ? attributes_[index].get() : nullptr;
}

XAttr *
XNode::GetAttr(const wchar_t *attrName) const
{
   if (!attrName)
      return nullptr;

   for (const std::unique_ptr<XAttr> &attribute : attributes_)
   {
      if (attribute->name == attrName)
         return attribute.get();
   }

   return nullptr;
}

HM::String
XNode::GetAttrValue(const wchar_t *attrName) const
{
   XAttr *attribute = GetAttr(attrName);
   return attribute ? attribute->value : HM::String();
}

XAttr *
XNode::AppendAttr(const wchar_t *attrName, const wchar_t *attrValue)
{
   std::unique_ptr<XAttr> attribute(new XAttr());
   attribute->name = attrName ? attrName : L"";
   attribute->value = attrValue ? attrValue : L"";
   attribute->parent = this;

   attributes_.push_back(std::move(attribute));
   return attributes_.back().get();
}

bool
XNode::RemoveAttr(XAttr *attr)
{
   for (auto iterator = attributes_.begin(); iterator != attributes_.end(); ++iterator)
   {
      if (iterator->get() == attr)
      {
         attributes_.erase(iterator);
         return true;
      }
   }

   return false;
}

HM::String
XNode::GetXML() const
{
   HM::String text;
   WriteTo_(text, 0);
   return text;
}

void
XNode::WriteTo_(HM::String &text, size_t depth) const
{
   AppendLine(text, depth);

   switch (type_)
   {
   case XNodeType::Document:
      for (const std::unique_ptr<XNode> &child : children_)
         child->WriteTo_(text, depth);
      return;

   case XNodeType::Comment:
      text += L"<!--";
      text += value;
      text += L"-->";
      return;

   case XNodeType::CData:
      text += L"<![CDATA[";
      text += value;
      text += L"]]>";
      return;

   case XNodeType::ProcessingInstruction:
   case XNodeType::Element:
      break;
   }

   const bool instruction = type_ == XNodeType::ProcessingInstruction;

   text += instruction ? L"<?" : L"<";
   text += name;

   if (!attributes_.empty())
      text += L' ';

   for (const std::unique_ptr<XAttr> &attribute : attributes_)
   {
      text += attribute->name;
      text += L"=\"";
      AppendEscaped(text, attribute->value);
      text += L"\" ";
   }

   if (instruction)
   {
      text += L"?>";
      return;
   }

   if (children_.empty() && value.empty())
   {
      text += L"/>";
      return;
   }

   text += L'>';

   const size_t inner = children_.empty() ? depth : depth + 1;

   for (const std::unique_ptr<XNode> &child : children_)
      child->WriteTo_(text, inner);

   if (!value.empty())
   {
      if (!children_.empty())
         AppendLine(text, inner);

      AppendEscaped(text, value);
   }

   if (!children_.empty())
      AppendLine(text, depth);

   text += L"</";
   text += name;
   text += L'>';
}

// ---------------------------------------------------------------- XDoc

XDoc::XDoc() :
   XNode(XNodeType::Document),
   problem_(XmlProblem::None)
{
}

bool
XDoc::Parse(const HM::String &text)
{
   Clear_();
   problem_ = XmlProblem::None;
   problem_text_.clear();

   XmlTreeReader reader(*this, text);
   return reader.Read();
}

bool
XDoc::Load(const HM::String &text)
{
   if (Parse(text))
      return true;

   if (problem_ != XmlProblem::None)
   {
      const HM::String message = L"The XML data stream contains formatting errors. Code: " +
         HM::String(std::to_wstring(static_cast<int>(problem_))) + L", Error: " + problem_text_;

      HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Medium, 4229, "XDoc::Load", message);
   }

   return false;
}

XNode *
XDoc::GetRoot() const
{
   for (int index = 0; index < GetChildCount(); index++)
   {
      XNode *child = GetChild(static_cast<unsigned int>(index));
      if (child->GetType() == XNodeType::Element)
         return child;
   }

   return nullptr;
}
