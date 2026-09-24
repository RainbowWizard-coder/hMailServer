// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A strict, namespace-aware XML reader, and Exclusive XML Canonicalization 1.0
// over what it reads: the two things an XML signature cannot be verified
// without (XmlSignature.h), and so the two things SAML sign-in stands on
// (SamlSignIn.h).
//
// WHY NOT THE READERS THE TREE HAS
//
// XMLite knows nothing of namespaces. Dav::XmlReader (DavSupport.h) is bounded
// and namespace-aware, but it keeps what a WebDAV body needs and throws away
// what a canonicaliser needs: the prefixes, the namespace declarations and
// where they were written, attributes by their namespace, and an element's
// text node by node. It also lets an unknown &name; through as text and does
// not refuse an attribute written twice. A canonicaliser fed by either would
// compute a digest over something other than what was signed - which fails
// safe at best, and at worst verifies a document that means one thing to the
// signer and another to this server. So this reader keeps exactly what the
// XPath data model keeps, and refuses everything else.
//
// WHAT IT READS, AND WHAT IT REFUSES
//
//   * UTF-8 only (an XML declaration naming any other encoding is refused),
//     every byte checked: malformed and overlong sequences, surrogates and
//     characters XML 1.0 does not allow are refused. Line ends are normalised
//     as XML 1.0 section 2.11 says, before anything else reads the text.
//   * Elements, attributes and text, with namespaces resolved: every element
//     and attribute carries its prefix and its namespace URI, and every
//     element the declarations written on it, in the order written.
//   * The five predefined entities and character references. Any other
//     &name; is refused, and so is a document type declaration (<!DOCTYPE),
//     since that is where another entity would be defined - and an entity is
//     how a signed document is made to read one way to the signer and another
//     way to the verifier.
//   * Comments are read and dropped: exc-c14n without comments leaves them out
//     of every digest, so what a comment separates is one text node here, as
//     it is in the bytes the signature covers. (A NameID of
//     user@example.test<!---->.evil is user@example.test.evil, which is the
//     whole of the comment-injection defence - see SamlSignIn.h.)
//   * A processing instruction is refused; the XML declaration at the start is
//     not one, and is read for its encoding.
//   * An attribute written twice, or two attributes with one namespace and one
//     local name under two prefixes, is refused; so is a prefix nobody
//     declared, xmlns:p="" and a binding of the xml or xmlns namespaces.
//   * Ceilings: MaximumBytes of input, MaximumDepth of nesting,
//     MaximumElements elements, MaximumAttributes attributes on one element,
//     MaximumNamespaceDeclarations namespace declarations in the whole document
//     and MaximumNamespaceCharacters characters in a namespace name. The last
//     two are what bound the canonicaliser below: what it looks at for each
//     element grows with the namespaces in scope, and what it writes grows with
//     their names - exc-c14n writes a declaration again on every element that
//     uses a prefix no output ancestor wrote, so one name of 100 KB used by
//     twenty thousand elements would be two gigabytes written. With both
//     ceilings, what an element costs is its own declarations, each at most 512
//     characters. A SAML Response declares fewer than twenty namespaces, none
//     longer than about seventy characters.
//
// THE CANONICALISER
//
// Exclusive XML Canonicalization 1.0 (http://www.w3.org/2001/10/xml-exc-c14n#,
// the W3C recommendation of 18 July 2002), without comments, of one element
// and everything under it in the context of its document - which is what an
// XML signature's same-document reference with the enveloped-signature and
// exc-c14n transforms digests. The serialisation is Canonical XML 1.0's (empty
// elements as start and end tags, attributes in double quotes sorted by
// namespace URI and then local name, the characters that must be escaped
// escaped); the namespace declarations are exc-c14n's: a declaration is
// written on the element that visibly uses its prefix - its own name's, or one
// of its attributes' - where an output ancestor has not already written the
// same one, xmlns="" only to undo a default namespace an output ancestor
// wrote, and the xml: attributes of ancestors are not carried down. The
// InclusiveNamespaces PrefixList names prefixes handled as Canonical XML
// handles them instead, "#default" meaning the default namespace; the list is
// read without its repeats, and one of more than MaximumPrefixes prefixes is
// refused, since the canonicaliser looks at every one of them for every element.
// It is held to the W3C's own examples and to what .NET's
// XmlDsigExcC14NTransform writes for the same input, and a worst-case document
// of 256 KB to a time, in XmlCanonicalizationTester.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace HM
{
   struct XmlNamespaceDeclaration
   {
      // Empty for the default namespace.
      std::string prefix;

      // Empty for xmlns="", which takes the default namespace away.
      std::string uri;
   };

   struct XmlAttribute
   {
      // Empty for an attribute written without one; such an attribute is in no
      // namespace, whatever the default namespace is.
      std::string prefix;
      std::string local;
      std::string uri;

      // After the references are expanded and the value normalised as a CDATA
      // attribute's is (XML 1.0 section 3.3.3): with no document type, every
      // attribute is CDATA.
      std::string value;
   };

   class XmlNode
   {
   public:
      enum NodeKind
      {
         KindElement,
         KindText
      };

      XmlNode() : kind(KindElement), parent(nullptr) {}

      NodeKind kind;

      // A text node's characters, UTF-8. Adjacent text - including text on
      // either side of a comment, and a CDATA section - is one node.
      std::string text;

      // An element's name.
      std::string prefix;
      std::string local;
      std::string uri;

      // The namespace declarations written on this element, in the order
      // written. Not attributes: attributes below never include one.
      std::vector<XmlNamespaceDeclaration> namespaces;

      std::vector<XmlAttribute> attributes;
      std::vector<std::shared_ptr<XmlNode>> children;

      // Null for the document's root element.
      XmlNode *parent;

      bool IsElement() const { return kind == KindElement; }

      // Whether this is the element {namespaceUri}localName.
      bool Is(const char *namespaceUri, const char *localName) const;

      // An attribute written without a prefix - ID, Destination, Algorithm - and
      // whether it is there at all.
      bool GetAttribute(const char *localName, std::string &value) const;

      // The element children, in order.
      std::vector<const XmlNode *> ElementChildren() const;

      // The element children named {namespaceUri}localName, in order; and the
      // first of them, or null.
      std::vector<const XmlNode *> ChildElements(const char *namespaceUri, const char *localName) const;
      const XmlNode *ChildElement(const char *namespaceUri, const char *localName) const;

      // The element's own text children, joined; not its descendants'.
      std::string TextContent() const;
   };

   class XmlDocument
   {
   public:
      static const size_t MaximumBytes = 1024 * 1024;
      static const int MaximumDepth = 64;
      static const int MaximumElements = 20000;
      static const int MaximumAttributes = 256;
      static const int MaximumNamespaceDeclarations = 128;
      static const int MaximumNamespaceCharacters = 512;

      // False with a sentence when the text is not a document this reader
      // takes. Reads nothing from anywhere but text.
      bool Parse(const std::string &text, std::string &problem);

      // Null until a Parse succeeds.
      const XmlNode *Root() const { return root_.get(); }

   private:
      std::shared_ptr<XmlNode> root_;
   };

   class ExclusiveCanonicalizer
   {
   public:
      // The exc-c14n form of element and everything under it, UTF-8, in the
      // context of its document. excluded, when not null, is left out with
      // everything under it - the enveloped-signature transform. inclusive is
      // the InclusiveNamespaces PrefixList as PrefixList reads it.
      static std::string Canonicalize(const XmlNode &element, const std::vector<std::string> &inclusive,
                                      const XmlNode *excluded);

      // An InclusiveNamespaces PrefixList attribute's value: its white-space
      // separated prefixes, "#default" read as the default namespace (empty),
      // each once. False when it names more than MaximumPrefixes, counted as
      // written, repeats and all.
      static bool PrefixList(const std::string &value, std::vector<std::string> &prefixes);

      static const int MaximumPrefixes = 32;

      // The algorithm's identifier.
      static const char *Algorithm() { return "http://www.w3.org/2001/10/xml-exc-c14n#"; }

      // The namespace its InclusiveNamespaces element is in.
      static const char *Namespace() { return "http://www.w3.org/2001/10/xml-exc-c14n#"; }
   };

   // Run from ClassTester::DoTests: the reader's refusals and each of its
   // ceilings beside its negative control, the canonicaliser against the W3C's
   // examples - Exclusive XML Canonicalization section 2.2's two documents, and
   // Canonical XML 1.0 section 3's examples 3.2, 3.3, 3.4 and 3.6 (3.3 and 3.4
   // without their DTD, 3.6 without its ISO-8859-1 declaration, both of which
   // the reader refuses) - and the PrefixList, each byte for byte, a failure
   // naming the example; and a worst-case document of 256 KB - every ceiling
   // spent where it costs the canonicaliser most - read and canonicalised
   // within a time.
   class XmlCanonicalizationTester
   {
   public:
      void Test();
   };
}
