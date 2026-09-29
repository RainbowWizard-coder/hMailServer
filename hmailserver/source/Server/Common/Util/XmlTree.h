// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The tree the server writes its backup index in, and reads it back from: the
// XML of hMailServerBackup.xml (BackupExecuter, BackupRestorer and the XMLStore
// and XMLLoad of every object a backup carries), of the account.xml a person's
// export holds (PersonExport), and of the class tests' test_config.xml.
//
// It is the project's own, written in September 2026 to replace XMLite (Cho Kyung
// Min, The Code Project, 2002-2004), which carried only its author's notice - one
// a distribution's legal review would have had to settle, and the project could
// not relicense. It was written anew from a description of what XMLite did - what
// it wrote, byte for byte, and what it read, quirk for quirk - by the same hand
// that read XMLite to write that description, so it is not a clean-room
// reimplementation; none of XMLite's code is in it. It keeps the names the
// callers use: XNode, XAttr, LPXAttr, XDoc. A differential harness ran the two
// side by side over the shapes the server writes, every character, the old
// tests' cases and a fuzz corpus - some 17 million cases on Linux and 7 million
// on Windows - comparing what each read (the tree, the outcome, the problem) and
// what each wrote, byte for byte, and found no difference.
//
// WHY IT READS WHAT A STRICT PARSER REFUSES
//
// An archive 6.3.5 restored must restore, whichever version wrote it. So the
// reader accepts exactly what XMLite's did as 6.3.5 had it - no more and no
// less - rather than what XML 1.0 allows: an archive that restored yesterday
// restores today, and one it refused is refused. Older copies of XMLite read
// some malformed texts otherwise - before 6.2.19 it had no depth limit, and
// before 6.3.4 it took "&amp" for "&amp;", refused a processing instruction
// inside an element and read past the end of a truncated tag - and an index
// the server wrote holds none of those. (For XML off the wire the tree
// has a strict reader, XmlDocument.h, and a WebDAV one, DavSupport.h.)
//
// WHAT IT WRITES (XDoc::GetXML)
//
//   * Every node starts on a line of its own: CR LF, then a tab for each element
//     it is inside. The document's own line is empty, so an index begins with two
//     CR LFs.
//   * An element is <name; then, if it has attributes, a space and each one as
//     name="value" and a space, in the order added - so <Store Name="x" > and
//     <Row a="1" b="2" />. With no children and no text it ends />; otherwise >,
//     the children, the text (on a line of its own when there are children), and
//     </name> (on a line of its own when there are children).
//   * In attribute values and element text, & " ' < > are written &amp; &quot;
//     &apos; &lt; &gt;. Nothing else is escaped: tabs, line breaks, control
//     characters and every other character go out as they are, one wchar_t for
//     one, and FileUtilities::WriteToFile decides the file's encoding. A value is
//     written up to its first NUL, if it has one (the setters take C strings, so
//     only a value assigned to the field directly can).
//   * A processing instruction is <?name, its attributes as an element's, and ?>;
//     a comment <!--text-->; a CDATA section <![CDATA[text]]>, the text as it is.
//   * Names are written as they are, which is why a name that comes from anywhere
//     but the program goes into an attribute (IniSettingStore, AccountStores).
//
// WHAT IT READS (XDoc::Load)
//
// The text up to its first NUL. Whitespace below is what iswspace says it is; a
// "blank" text is one in which std::isspace, in the global locale, finds nothing
// but space.
//
//   * Anything before the first '<' is skipped. Before the root element, only
//     whitespace, processing instructions, comments and CDATA sections; after it,
//     the same are read and anything else is ignored. A close tag where the root
//     should start is read as a root with no name and nothing in it.
//   * A tag name runs to the first space, '/', '>', tab, CR or LF. An attribute
//     name runs to the first space or '=' - wherever that is. A value is quoted
//     with " or ', or runs unquoted to the first space or '>' (over any '/'); a
//     name with no '=' after it is an attribute with an empty value. Attributes
//     are kept in the order written, repeats and all.
//   * An element's text is its first run of characters between its markup that is
//     not blank, or its last run if all are blank; the other runs are dropped.
//     (So an element with children, read from an indented file, has as its text
//     the line break and tabs before its close tag.)
//   * A close tag is </, optional whitespace, the name up to a space or '>', which
//     must be the element's own; with a space before its '>', the '>' is left in
//     the text that follows.
//   * An element the text ends inside is closed where the text ends - so an index
//     cut short reads as far as it goes - unless the text ends inside a tag, or in
//     a run of text after the element already has its text.
//   * The five references &amp; &quot; &apos; &lt; &gt; are decoded in values and
//     text; any other '&' - a numeric reference, "&amp" without its ';' - is kept
//     as written.
//   * <? ?>, <!-- --> and <![CDATA[ ]]> (the last in any case) are nodes of their
//     own: a comment or CDATA section where it is written, and every processing
//     instruction in the document's own list, in the order read, wherever it was
//     written. Each ends at the first closer searched from its own '<', so <!-->
//     is an empty comment.
//   * An element whose name is empty is read and dropped with everything in it,
//     unless it is the root.
//   * It refuses, and Load reports HM4229: an unterminated name, instruction,
//     comment or CDATA section; anything but whitespace and those nodes between
//     them and the root; a close tag that is not the element's; "/" without ">";
//     a text that ends in an attribute's name or where one would start; and
//     elements nested deeper than MaximumDepth, which is why a hostile index
//     cannot exhaust the stack. It refuses with nothing to report a text with no
//     '<', and one that ends inside a tag anywhere else or in the run of text
//     above.
//
// OWNERSHIP
//
// A node owns its children and its attributes and deletes them with itself;
// RemoveChild and RemoveAttr delete what they remove. Every pointer handed out
// lives as long as the node it came from. Nodes cannot be copied.
//
// These are at global scope, as XMLite's were: headers that only pass an XNode *
// declare `class XNode;` rather than include this.
#pragma once

#include <memory>
#include <vector>

class XNode;
class XmlTreeReader;

enum class XNodeType
{
   Element,
   ProcessingInstruction,
   Comment,
   CData,
   Document
};

// Why the last XDoc::Parse or Load failed. The numbers are the ones HM4229 has
// always reported as its "Code".
enum class XmlProblem
{
   None = 0,
   EmptyTagNotClosed = 1,
   NotClosed = 2,
   Nesting = 3,
   Attribute = 4
};

struct XAttr
{
   HM::String name;
   HM::String value;

   // The node this is an attribute of.
   XNode *parent = nullptr;
};

// The name the callers have always used for a pointer to one.
typedef XAttr *LPXAttr;

class XNode
{
public:
   explicit XNode(XNodeType type = XNodeType::Element);
   virtual ~XNode();

   XNode(const XNode &) = delete;
   XNode &operator=(const XNode &) = delete;

   // A comment is named #COMMENT and a CDATA section #CDATA; their text is value.
   HM::String name;
   HM::String value;

   // The node whose list holds this one; null for a document.
   XNode *parent;

   XNodeType GetType() const { return type_; }

   // Children, in order. By name: the first of that name, of any kind.
   int GetChildCount() const;
   XNode *GetChild(unsigned int index) const;
   XNode *GetChild(const wchar_t *childName) const;

   // The text of the first child of that name, or null when there is none.
   const wchar_t *GetChildValue(const wchar_t *childName) const;

   // The first attribute of that name on the first child of that name, or null.
   XAttr *GetChildAttr(const wchar_t *childName, const wchar_t *attrName) const;

   // A new element at the end of the children. A null name or value is empty.
   XNode *AppendChild(const wchar_t *childName, const wchar_t *childValue = nullptr);

   // Deletes the child. False, and nothing done, when it is not one of these.
   bool RemoveChild(XNode *child);

   // Attributes, in order. By name: the first of that name.
   int GetAttrCount() const;
   XAttr *GetAttr(unsigned int index) const;
   XAttr *GetAttr(const wchar_t *attrName) const;

   // Its value, or empty when there is no such attribute.
   HM::String GetAttrValue(const wchar_t *attrName) const;

   // A new attribute at the end. A null name or value is empty.
   XAttr *AppendAttr(const wchar_t *attrName, const wchar_t *attrValue);

   // Deletes the attribute. False, and nothing done, when it is not one of these.
   bool RemoveAttr(XAttr *attr);

   // This node and everything under it as text; see the top of the file.
   HM::String GetXML() const;

protected:
   void Clear_();

private:
   friend class XmlTreeReader;

   XNode *Adopt_(std::unique_ptr<XNode> child);
   void WriteTo_(HM::String &text, size_t depth) const;

   XNodeType type_;
   std::vector<std::unique_ptr<XNode>> children_;
   std::vector<std::unique_ptr<XAttr>> attributes_;
};

class XDoc : public XNode
{
public:
   // Deeper than this, a document is refused: the reader recurses once for each
   // level. The root element is level 1, and an element that holds anything,
   // even only its close tag, needs one level more for it.
   static const int MaximumDepth = 100;

   XDoc();

   // Replaces what the document held with what the text holds, up to its first
   // NUL if it has one. False when the text is not a document the reader takes -
   // see the top of the file - and then the document keeps what it read before
   // the failure: the processing instructions read anywhere, and the comments and
   // CDATA sections before the root. Parse says nothing; Load reports a failure
   // it has a reason for as HM4229. The text is read where it is, not copied, so
   // it must not be a string this document holds.
   bool Parse(const HM::String &text);
   bool Load(const HM::String &text);

   // The first element among the document's nodes, or null.
   XNode *GetRoot() const;

   // What the last Parse or Load found wrong first (a nesting too deep is
   // recorded over anything found before it). A Parse can succeed with a problem
   // recorded: one in a processing instruction's attributes, or in what follows
   // the root element, does not stop the reading.
   XmlProblem GetProblem() const { return problem_; }
   const HM::String &GetProblemText() const { return problem_text_; }

private:
   friend class XmlTreeReader;

   XmlProblem problem_;
   HM::String problem_text_;
};
