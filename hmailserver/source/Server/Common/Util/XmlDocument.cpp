// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "XmlDocument.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *XmlNamespaceUri = "http://www.w3.org/XML/1998/namespace";
      const char *XmlnsNamespaceUri = "http://www.w3.org/2000/xmlns/";

      // XML 1.0's Char production.
      bool IsXmlCharacter(unsigned long code)
      {
         return code == 0x9 || code == 0xA || code == 0xD ||
                (code >= 0x20 && code <= 0xD7FF) ||
                (code >= 0xE000 && code <= 0xFFFD) ||
                (code >= 0x10000 && code <= 0x10FFFF);
      }

      void AppendUtf8(std::string &out, unsigned long code)
      {
         if (code < 0x80)
         {
            out += (char) code;
         }
         else if (code < 0x800)
         {
            out += (char) (0xC0 | (code >> 6));
            out += (char) (0x80 | (code & 0x3F));
         }
         else if (code < 0x10000)
         {
            out += (char) (0xE0 | (code >> 12));
            out += (char) (0x80 | ((code >> 6) & 0x3F));
            out += (char) (0x80 | (code & 0x3F));
         }
         else
         {
            out += (char) (0xF0 | (code >> 18));
            out += (char) (0x80 | ((code >> 12) & 0x3F));
            out += (char) (0x80 | ((code >> 6) & 0x3F));
            out += (char) (0x80 | (code & 0x3F));
         }
      }

      // Every byte checked as UTF-8 and every character as one XML allows, and
      // the line ends normalised (XML 1.0 section 2.11): CR LF, and a CR on its
      // own, are LF before anything else reads the text. A byte order mark at
      // the very start is not markup and is dropped.
      bool Normalise(const std::string &text, std::string &out, std::string &problem)
      {
         out.clear();
         out.reserve(text.size());

         size_t at = 0;
         if (text.size() >= 3 && (unsigned char) text[0] == 0xEF && (unsigned char) text[1] == 0xBB &&
             (unsigned char) text[2] == 0xBF)
            at = 3;

         while (at < text.size())
         {
            const unsigned char first = (unsigned char) text[at];
            unsigned long code = 0;
            size_t length = 0;

            if (first < 0x80)
            {
               code = first;
               length = 1;
            }
            else if ((first & 0xE0) == 0xC0)
            {
               code = first & 0x1F;
               length = 2;
            }
            else if ((first & 0xF0) == 0xE0)
            {
               code = first & 0x0F;
               length = 3;
            }
            else if ((first & 0xF8) == 0xF0)
            {
               code = first & 0x07;
               length = 4;
            }
            else
            {
               problem = "the document is not UTF-8";
               return false;
            }

            if (at + length > text.size())
            {
               problem = "the document is not UTF-8: it ends inside a character";
               return false;
            }

            for (size_t k = 1; k < length; k++)
            {
               const unsigned char next = (unsigned char) text[at + k];
               if ((next & 0xC0) != 0x80)
               {
                  problem = "the document is not UTF-8";
                  return false;
               }
               code = (code << 6) | (next & 0x3F);
            }

            // An overlong form is a second spelling of a character, which is
            // exactly what a canonical form must not have; a surrogate is not
            // a character at all.
            if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
                (code >= 0xD800 && code <= 0xDFFF))
            {
               problem = "the document is not UTF-8: it holds an overlong form or a surrogate";
               return false;
            }

            if (!IsXmlCharacter(code))
            {
               problem = "the document holds a character XML 1.0 does not allow";
               return false;
            }

            if (code == 0xD)
            {
               out += '\n';
               at++;
               if (at < text.size() && text[at] == '\n')
                  at++;
               continue;
            }

            out.append(text, at, length);
            at += length;
         }

         return true;
      }

      bool IsNameStart(unsigned char c)
      {
         return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80;
      }

      bool IsNameCharacter(unsigned char c)
      {
         return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
      }

      // A QName: an NCName, or two joined by one colon.
      bool SplitQName(const std::string &name, std::string &prefix, std::string &local)
      {
         const size_t colon = name.find(':');
         if (colon == std::string::npos)
         {
            prefix.clear();
            local = name;
            return !local.empty();
         }

         if (colon == 0 || colon + 1 == name.size() || name.find(':', colon + 1) != std::string::npos)
            return false;

         prefix = name.substr(0, colon);
         local = name.substr(colon + 1);
         return IsNameStart((unsigned char) local[0]);
      }

      // Namespace bindings in scope, the nearest last.
      typedef std::vector<std::pair<std::string, std::string>> Scope;

      // The URI a prefix is bound to. The default namespace ("") may be bound
      // to nothing, which is its URI being empty; any other prefix must have
      // been declared.
      bool Resolve(const Scope &scope, const std::string &prefix, std::string &uri)
      {
         if (prefix == "xml")
         {
            uri = XmlNamespaceUri;
            return true;
         }

         for (auto it = scope.rbegin(); it != scope.rend(); ++it)
         {
            if (it->first == prefix)
            {
               uri = it->second;
               return true;
            }
         }

         uri.clear();
         return prefix.empty();
      }

      // A name from the document, for a sentence: cut short, and a control
      // character shown as a question mark, since the sentence is logged.
      std::string Shown(const std::string &text)
      {
         std::string shown = text.size() > 60 ? text.substr(0, 60) + "..." : text;
         for (char &c : shown)
         {
            if ((unsigned char) c < 0x20 || c == 0x7F)
               c = '?';
         }
         return shown;
      }

      class Reader
      {
      public:
         explicit Reader(const std::string &text) : text_(text), at_(0), elements_(0), declarations_(0) { }

         bool Read(std::shared_ptr<XmlNode> &root, std::string &problem)
         {
            // The XML declaration, when there is one, is the very first thing.
            if (Looking("<?xml") && at_ + 5 < text_.size() && IsSpace(text_[at_ + 5]))
            {
               const size_t end = text_.find("?>", at_);
               if (end == std::string::npos)
               {
                  problem = "the XML declaration does not end";
                  return false;
               }

               if (!DeclarationAcceptable(text_.substr(at_ + 5, end - at_ - 5), problem))
                  return false;

               at_ = end + 2;
            }

            if (!SkipMisc(problem))
               return false;

            if (AtEnd() || text_[at_] != '<')
            {
               problem = "the document has no root element";
               return false;
            }

            root.reset(new XmlNode());
            Scope none;
            if (!ReadElement(*root, none, 1, problem))
               return false;

            if (!SkipMisc(problem))
               return false;

            if (!AtEnd())
            {
               problem = "the document has content after its root element";
               return false;
            }

            return true;
         }

      private:
         bool AtEnd() const
         {
            return at_ >= text_.size();
         }

         bool Looking(const char *literal) const
         {
            const size_t length = strlen(literal);
            return at_ + length <= text_.size() && text_.compare(at_, length, literal) == 0;
         }

         static bool IsSpace(char c)
         {
            // CR is gone: Normalise turned every one into LF.
            return c == ' ' || c == '\t' || c == '\n';
         }

         bool SkipSpace()
         {
            const size_t start = at_;
            while (!AtEnd() && IsSpace(text_[at_]))
               at_++;
            return at_ > start;
         }

         bool ReadName(std::string &name)
         {
            if (AtEnd() || !IsNameStart((unsigned char) text_[at_]))
               return false;

            const size_t start = at_;
            while (!AtEnd() && IsNameCharacter((unsigned char) text_[at_]))
               at_++;

            name = text_.substr(start, at_ - start);
            return true;
         }

         // The encoding the declaration names, if it names one, must be UTF-8:
         // the bytes were read as UTF-8 and a document that says otherwise is
         // either wrong about itself or meant to be read two ways.
         static bool DeclarationAcceptable(const std::string &declaration, std::string &problem)
         {
            std::string lower = declaration;
            for (char &c : lower)
               c = (char) tolower((unsigned char) c);

            const size_t name = lower.find("encoding");
            if (name == std::string::npos)
               return true;

            const size_t quote = lower.find_first_of("\"'", name);
            if (quote == std::string::npos)
            {
               problem = "the XML declaration's encoding is not quoted";
               return false;
            }

            const size_t end = lower.find(lower[quote], quote + 1);
            if (end == std::string::npos || lower.substr(quote + 1, end - quote - 1) != "utf-8")
            {
               problem = "the XML declaration names an encoding other than UTF-8";
               return false;
            }

            return true;
         }

         bool SkipComment(std::string &problem)
         {
            at_ += 4;

            // "--" may not occur inside a comment (XML 1.0 section 2.5), so the
            // first one found has to be the one that ends it.
            const size_t end = text_.find("--", at_);
            if (end == std::string::npos || text_.compare(end, 3, "-->") != 0)
            {
               problem = "a comment holds -- or does not end";
               return false;
            }

            at_ = end + 3;
            return true;
         }

         // White space and comments outside the root element; a processing
         // instruction or a document type declaration is refused.
         bool SkipMisc(std::string &problem)
         {
            while (true)
            {
               SkipSpace();

               if (Looking("<!--"))
               {
                  if (!SkipComment(problem))
                     return false;
                  continue;
               }

               if (Looking("<!DOCTYPE"))
               {
                  problem = "the document has a document type declaration (<!DOCTYPE), which is refused: it is where "
                            "an entity would be defined, and an entity is how a signed document is made to say two things";
                  return false;
               }

               if (Looking("<?"))
               {
                  problem = "the document has a processing instruction, which is refused";
                  return false;
               }

               if (Looking("<!"))
               {
                  problem = "the document has a markup declaration, which is refused";
                  return false;
               }

               return true;
            }
         }

         // After '&': a character reference, or one of the five entities XML
         // predefines. Nothing else: there is no document type to define more.
         bool ReadReference(std::string &out, std::string &problem)
         {
            const size_t semicolon = text_.find(';', at_);
            if (semicolon == std::string::npos || semicolon - at_ > 12 || semicolon == at_)
            {
               problem = "an & begins no reference";
               return false;
            }

            const std::string name = text_.substr(at_, semicolon - at_);
            at_ = semicolon + 1;

            if (name == "amp")
               out += '&';
            else if (name == "lt")
               out += '<';
            else if (name == "gt")
               out += '>';
            else if (name == "quot")
               out += '"';
            else if (name == "apos")
               out += '\'';
            else if (name[0] == '#')
            {
               const bool hex = name.size() > 1 && name[1] == 'x';
               const size_t first = hex ? 2 : 1;
               if (first >= name.size())
               {
                  problem = "a character reference with no number";
                  return false;
               }

               unsigned long code = 0;
               for (size_t k = first; k < name.size(); k++)
               {
                  const char c = name[k];
                  unsigned long digit = 0;
                  if (c >= '0' && c <= '9')
                     digit = (unsigned long) (c - '0');
                  else if (hex && c >= 'a' && c <= 'f')
                     digit = (unsigned long) (c - 'a' + 10);
                  else if (hex && c >= 'A' && c <= 'F')
                     digit = (unsigned long) (c - 'A' + 10);
                  else
                  {
                     problem = "a character reference that is not a number";
                     return false;
                  }

                  code = code * (hex ? 16 : 10) + digit;
                  if (code > 0x10FFFF)
                  {
                     problem = "a character reference beyond Unicode";
                     return false;
                  }
               }

               if (!IsXmlCharacter(code))
               {
                  problem = "a character reference to a character XML 1.0 does not allow";
                  return false;
               }

               AppendUtf8(out, code);
            }
            else
            {
               problem = "the document uses an entity this server does not define (&" + Shown(name) +
                         ";): only the five XML predefines are read";
               return false;
            }

            return true;
         }

         bool ReadAttributeValue(std::string &value, std::string &problem)
         {
            if (AtEnd() || (text_[at_] != '"' && text_[at_] != '\''))
            {
               problem = "an attribute value is not quoted";
               return false;
            }

            const char quote = text_[at_++];
            value.clear();

            while (true)
            {
               if (AtEnd())
               {
                  problem = "an attribute value does not end";
                  return false;
               }

               const char c = text_[at_];
               if (c == quote)
               {
                  at_++;
                  return true;
               }

               if (c == '<')
               {
                  problem = "an attribute value holds a <";
                  return false;
               }

               if (c == '&')
               {
                  at_++;
                  if (!ReadReference(value, problem))
                     return false;
                  continue;
               }

               // Attribute-value normalisation for a CDATA attribute: a white
               // space character written as itself is a space. One written as
               // a character reference is kept as the character it names.
               value += (c == '\n' || c == '\t') ? ' ' : c;
               at_++;
            }
         }

         static void AppendText(XmlNode &element, const std::string &text)
         {
            if (text.empty())
               return;

            if (!element.children.empty() && element.children.back()->kind == XmlNode::KindText)
            {
               element.children.back()->text += text;
               return;
            }

            std::shared_ptr<XmlNode> node(new XmlNode());
            node->kind = XmlNode::KindText;
            node->text = text;
            node->parent = &element;
            element.children.push_back(node);
         }

         struct RawAttribute
         {
            std::string name;
            std::string value;
         };

         bool ReadElement(XmlNode &element, const Scope &outer, int depth, std::string &problem)
         {
            if (depth > XmlDocument::MaximumDepth)
            {
               problem = "the document nests its elements more than 64 deep";
               return false;
            }

            if (++elements_ > XmlDocument::MaximumElements)
            {
               problem = "the document has more than 20000 elements";
               return false;
            }

            at_++;   // '<'

            std::string qname;
            if (!ReadName(qname) || !SplitQName(qname, element.prefix, element.local))
            {
               problem = "an element's name is not one XML allows";
               return false;
            }

            std::vector<RawAttribute> raw;
            bool empty = false;

            while (true)
            {
               const bool spaced = SkipSpace();
               if (AtEnd())
               {
                  problem = "the start tag of " + Shown(qname) + " does not end";
                  return false;
               }

               if (Looking("/>"))
               {
                  at_ += 2;
                  empty = true;
                  break;
               }

               if (text_[at_] == '>')
               {
                  at_++;
                  break;
               }

               if (!spaced)
               {
                  problem = "the attributes of " + Shown(qname) + " are not separated by white space";
                  return false;
               }

               RawAttribute attribute;
               if (!ReadName(attribute.name))
               {
                  problem = "an attribute of " + Shown(qname) + " has a name XML does not allow";
                  return false;
               }

               SkipSpace();
               if (AtEnd() || text_[at_] != '=')
               {
                  problem = "the attribute " + Shown(attribute.name) + " has no value";
                  return false;
               }

               at_++;
               SkipSpace();

               if (!ReadAttributeValue(attribute.value, problem))
                  return false;

               for (const RawAttribute &seen : raw)
               {
                  if (seen.name == attribute.name)
                  {
                     problem = "the attribute " + Shown(attribute.name) + " is written twice on one element";
                     return false;
                  }
               }

               if ((int) raw.size() >= XmlDocument::MaximumAttributes)
               {
                  problem = "an element has more than 256 attributes";
                  return false;
               }

               raw.push_back(attribute);
            }

            // The declarations first: they apply to the element's own name and
            // to every attribute on it, wherever they are written among them.
            Scope extended;
            const Scope *scope = &outer;

            for (const RawAttribute &attribute : raw)
            {
               const bool isDefault = attribute.name == "xmlns";
               const bool isPrefixed = attribute.name.size() > 6 && attribute.name.compare(0, 6, "xmlns:") == 0;
               if (!isDefault && !isPrefixed)
               {
                  if (attribute.name == "xmlns:")
                  {
                     problem = "a namespace declaration with no prefix after its colon";
                     return false;
                  }
                  continue;
               }

               // Every declaration counts, whatever it declares: the canonicaliser's
               // work for each element grows with what is in scope.
               if (++declarations_ > XmlDocument::MaximumNamespaceDeclarations)
               {
                  problem = "the document declares more than 128 namespaces";
                  return false;
               }

               // And what it writes grows with their names: exc-c14n writes a
               // declaration again on every element that uses a prefix no
               // output ancestor wrote.
               if (attribute.value.size() > (size_t) XmlDocument::MaximumNamespaceCharacters)
               {
                  problem = "the document declares a namespace name longer than 512 characters";
                  return false;
               }

               XmlNamespaceDeclaration declaration;
               declaration.prefix = isDefault ? std::string() : attribute.name.substr(6);
               declaration.uri = attribute.value;

               if (isPrefixed)
               {
                  if (declaration.prefix.find(':') != std::string::npos || !IsNameStart((unsigned char) declaration.prefix[0]))
                  {
                     problem = "a namespace prefix XML does not allow";
                     return false;
                  }

                  if (declaration.prefix == "xmlns")
                  {
                     problem = "the xmlns prefix is declared, which XML namespaces do not allow";
                     return false;
                  }

                  if (declaration.uri.empty())
                  {
                     problem = "the prefix " + Shown(declaration.prefix) + " is declared empty, which XML namespaces 1.0 do not allow";
                     return false;
                  }
               }

               // xml is bound to its namespace for ever, and nothing else may be;
               // xmlns's namespace is nobody's.
               if (declaration.prefix == "xml")
               {
                  if (declaration.uri != XmlNamespaceUri)
                  {
                     problem = "the xml prefix is bound to another namespace";
                     return false;
                  }
                  continue;
               }

               if (declaration.uri == XmlNamespaceUri || declaration.uri == XmlnsNamespaceUri)
               {
                  problem = "a prefix is bound to the xml or xmlns namespace";
                  return false;
               }

               if (scope != &extended)
               {
                  extended = outer;
                  scope = &extended;
               }

               extended.push_back(std::make_pair(declaration.prefix, declaration.uri));
               element.namespaces.push_back(declaration);
            }

            if (element.prefix == "xmlns")
            {
               problem = "an element is named in the xmlns prefix";
               return false;
            }

            if (!Resolve(*scope, element.prefix, element.uri))
            {
               problem = "the prefix " + Shown(element.prefix) + " of " + Shown(qname) + " is not declared";
               return false;
            }

            for (const RawAttribute &attribute : raw)
            {
               if (attribute.name == "xmlns" || attribute.name.compare(0, 6, "xmlns:") == 0)
                  continue;

               XmlAttribute resolved;
               if (!SplitQName(attribute.name, resolved.prefix, resolved.local))
               {
                  problem = "the attribute name " + Shown(attribute.name) + " is not one XML namespaces allow";
                  return false;
               }

               if (!resolved.prefix.empty() && !Resolve(*scope, resolved.prefix, resolved.uri))
               {
                  problem = "the prefix of the attribute " + Shown(attribute.name) + " is not declared";
                  return false;
               }

               resolved.value = attribute.value;

               for (const XmlAttribute &seen : element.attributes)
               {
                  if (seen.uri == resolved.uri && seen.local == resolved.local)
                  {
                     problem = "the attribute " + Shown(resolved.local) + " is written twice on one element under two prefixes";
                     return false;
                  }
               }

               element.attributes.push_back(resolved);
            }

            if (empty)
               return true;

            while (true)
            {
               if (AtEnd())
               {
                  problem = "the element " + Shown(qname) + " is not closed";
                  return false;
               }

               if (text_[at_] == '<')
               {
                  if (Looking("</"))
                  {
                     at_ += 2;

                     std::string closing;
                     if (!ReadName(closing) || closing != qname)
                     {
                        problem = "an end tag does not match the start tag of " + Shown(qname);
                        return false;
                     }

                     SkipSpace();
                     if (AtEnd() || text_[at_] != '>')
                     {
                        problem = "the end tag of " + Shown(qname) + " does not end";
                        return false;
                     }

                     at_++;
                     return true;
                  }

                  if (Looking("<!--"))
                  {
                     if (!SkipComment(problem))
                        return false;
                     continue;
                  }

                  if (Looking("<![CDATA["))
                  {
                     at_ += 9;
                     const size_t end = text_.find("]]>", at_);
                     if (end == std::string::npos)
                     {
                        problem = "a CDATA section does not end";
                        return false;
                     }

                     AppendText(element, text_.substr(at_, end - at_));
                     at_ = end + 3;
                     continue;
                  }

                  if (Looking("<?"))
                  {
                     problem = "the document has a processing instruction, which is refused";
                     return false;
                  }

                  if (Looking("<!"))
                  {
                     problem = "the document has a markup declaration inside an element, which is refused";
                     return false;
                  }

                  std::shared_ptr<XmlNode> child(new XmlNode());
                  child->parent = &element;
                  if (!ReadElement(*child, *scope, depth + 1, problem))
                     return false;

                  element.children.push_back(child);
                  continue;
               }

               std::string chunk;
               while (!AtEnd() && text_[at_] != '<')
               {
                  const char c = text_[at_];
                  if (c == '&')
                  {
                     at_++;
                     if (!ReadReference(chunk, problem))
                        return false;
                     continue;
                  }

                  if (c == '>' && at_ >= 2 && text_[at_ - 1] == ']' && text_[at_ - 2] == ']')
                  {
                     problem = "the text holds ]]>, which XML does not allow";
                     return false;
                  }

                  chunk += c;
                  at_++;
               }

               AppendText(element, chunk);
            }
         }

         const std::string &text_;
         size_t at_;
         int elements_;
         int declarations_;
      };

      // ------------------------------------------------------------ the canonicaliser

      typedef std::map<std::string, std::string> Bindings;

      void EscapeText(const std::string &text, std::string &out)
      {
         for (char c : text)
         {
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\r': out += "&#xD;"; break;
            default: out += c; break;
            }
         }
      }

      void EscapeAttribute(const std::string &value, std::string &out)
      {
         for (char c : value)
         {
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '"': out += "&quot;"; break;
            case '\t': out += "&#x9;"; break;
            case '\n': out += "&#xA;"; break;
            case '\r': out += "&#xD;"; break;
            default: out += c; break;
            }
         }
      }

      bool AttributeBefore(const XmlAttribute *a, const XmlAttribute *b)
      {
         // Namespace URI first, an empty one least; then the local name. Both
         // compared as UTF-8 bytes, which is code point order.
         if (a->uri != b->uri)
            return a->uri < b->uri;
         return a->local < b->local;
      }

      struct Canonicalization
      {
         std::vector<std::string> inclusive;
         bool inclusiveDefault = false;
         const XmlNode *excluded = nullptr;
      };

      // What an element wrote over what was rendered before it, so that it can
      // be put back when the element is closed.
      struct RenderedBefore
      {
         std::string prefix;
         bool had = false;
         std::string uri;
      };

      // One element. scope is what is bound where the element's parent is;
      // rendered is what the output ancestors wrote, prefix by prefix, the
      // nearest winning - one map for the whole walk, which each element adds
      // its own declarations to for its children and takes them back from.
      void WriteElement(const XmlNode &element, const Bindings &outerScope, Bindings &rendered,
                        const Canonicalization &how, std::string &out)
      {
         // Nothing is copied per element but what the element itself declares
         // and writes: the scope is copied only where the element declares
         // something - at most MaximumNamespaceDeclarations times in a
         // document - and what is rendered is never copied at all. So an
         // element costs its own declarations and the PrefixList's lookups, and
         // not a copy of everything in scope or everything written above it.
         Bindings ownScope;
         const Bindings *scopePointer = &outerScope;
         if (!element.namespaces.empty())
         {
            ownScope = outerScope;
            for (const XmlNamespaceDeclaration &declaration : element.namespaces)
               ownScope[declaration.prefix] = declaration.uri;
            scopePointer = &ownScope;
         }
         const Bindings &scope = *scopePointer;

         Bindings output;

         auto consider = [&](const std::string &prefix)
         {
            if (prefix == "xml")
               return;

            const Bindings::const_iterator bound = scope.find(prefix);
            const bool hasUri = bound != scope.end() && !bound->second.empty();
            const Bindings::const_iterator before = rendered.find(prefix);

            if (!hasUri)
            {
               // xmlns="", only to undo a default namespace an output ancestor
               // wrote (exc-c14n section 3, the fourth rule). A prefix other
               // than the default is never bound to nothing: the reader refuses
               // that.
               if (prefix.empty() && before != rendered.end() && !before->second.empty())
                  output[prefix] = std::string();
               return;
            }

            if (before == rendered.end() || before->second != bound->second)
               output[prefix] = bound->second;
         };

         // What the element visibly uses: its own name's prefix (the default
         // namespace when it has none) and its prefixed attributes' prefixes.
         // An attribute without a prefix is in no namespace and uses nothing.
         consider(element.prefix);
         for (const XmlAttribute &attribute : element.attributes)
         {
            if (!attribute.prefix.empty())
               consider(attribute.prefix);
         }

         // And the prefixes the PrefixList names, as Canonical XML handles them:
         // whatever is in scope, where the output ancestors have not written it.
         for (const std::string &prefix : how.inclusive)
         {
            if (scope.find(prefix) != scope.end())
               consider(prefix);
         }
         if (how.inclusiveDefault)
            consider(std::string());

         const std::string qname = element.prefix.empty() ? element.local : element.prefix + ":" + element.local;

         out += '<';
         out += qname;

         // The default namespace first, then by prefix: a std::map's order.
         for (const Bindings::value_type &entry : output)
         {
            if (entry.first.empty())
               out += " xmlns=\"";
            else
            {
               out += " xmlns:";
               out += entry.first;
               out += "=\"";
            }
            EscapeAttribute(entry.second, out);
            out += '"';
         }

         std::vector<const XmlAttribute *> sorted;
         for (const XmlAttribute &attribute : element.attributes)
            sorted.push_back(&attribute);
         std::sort(sorted.begin(), sorted.end(), AttributeBefore);

         for (const XmlAttribute *attribute : sorted)
         {
            out += ' ';
            if (!attribute->prefix.empty())
            {
               out += attribute->prefix;
               out += ':';
            }
            out += attribute->local;
            out += "=\"";
            EscapeAttribute(attribute->value, out);
            out += '"';
         }

         out += '>';

         // What this element wrote is rendered for its children, and put back
         // as it was when they are done.
         std::vector<RenderedBefore> before;
         before.reserve(output.size());
         for (const Bindings::value_type &entry : output)
         {
            RenderedBefore step;
            step.prefix = entry.first;

            const Bindings::iterator found = rendered.find(entry.first);
            step.had = found != rendered.end();
            if (step.had)
            {
               step.uri = found->second;
               found->second = entry.second;
            }
            else
               rendered[entry.first] = entry.second;

            before.push_back(step);
         }

         for (const std::shared_ptr<XmlNode> &child : element.children)
         {
            if (child.get() == how.excluded)
               continue;

            if (child->kind == XmlNode::KindText)
               EscapeText(child->text, out);
            else
               WriteElement(*child, scope, rendered, how, out);
         }

         for (auto step = before.rbegin(); step != before.rend(); ++step)
         {
            if (step->had)
               rendered[step->prefix] = step->uri;
            else
               rendered.erase(step->prefix);
         }

         out += "</";
         out += qname;
         out += '>';
      }
   }

   // ------------------------------------------------------------ XmlNode

   bool
   XmlNode::Is(const char *namespaceUri, const char *localName) const
   {
      return kind == KindElement && local == localName && uri == namespaceUri;
   }

   bool
   XmlNode::GetAttribute(const char *localName, std::string &value) const
   {
      for (const XmlAttribute &attribute : attributes)
      {
         if (attribute.uri.empty() && attribute.local == localName)
         {
            value = attribute.value;
            return true;
         }
      }

      value.clear();
      return false;
   }

   std::vector<const XmlNode *>
   XmlNode::ElementChildren() const
   {
      std::vector<const XmlNode *> found;
      for (const std::shared_ptr<XmlNode> &child : children)
      {
         if (child->kind == KindElement)
            found.push_back(child.get());
      }
      return found;
   }

   std::vector<const XmlNode *>
   XmlNode::ChildElements(const char *namespaceUri, const char *localName) const
   {
      std::vector<const XmlNode *> found;
      for (const std::shared_ptr<XmlNode> &child : children)
      {
         if (child->Is(namespaceUri, localName))
            found.push_back(child.get());
      }
      return found;
   }

   const XmlNode *
   XmlNode::ChildElement(const char *namespaceUri, const char *localName) const
   {
      for (const std::shared_ptr<XmlNode> &child : children)
      {
         if (child->Is(namespaceUri, localName))
            return child.get();
      }
      return nullptr;
   }

   std::string
   XmlNode::TextContent() const
   {
      std::string joined;
      for (const std::shared_ptr<XmlNode> &child : children)
      {
         if (child->kind == KindText)
            joined += child->text;
      }
      return joined;
   }

   // ------------------------------------------------------------ XmlDocument

   bool
   XmlDocument::Parse(const std::string &text, std::string &problem)
   {
      root_.reset();

      if (text.size() > MaximumBytes)
      {
         problem = "the document is larger than 1 MB";
         return false;
      }

      std::string normalised;
      if (!Normalise(text, normalised, problem))
         return false;

      std::shared_ptr<XmlNode> root;
      Reader reader(normalised);
      if (!reader.Read(root, problem))
         return false;

      root_ = root;
      return true;
   }

   // ------------------------------------------------------------ ExclusiveCanonicalizer

   bool
   ExclusiveCanonicalizer::PrefixList(const std::string &value, std::vector<std::string> &prefixes)
   {
      prefixes.clear();
      std::string current;
      int written = 0;

      for (size_t i = 0; i <= value.size(); i++)
      {
         const char c = i < value.size() ? value[i] : ' ';
         if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
         {
            if (!current.empty())
            {
               // Counted as written, so that a list of thousands of one prefix
               // is refused as a list of thousands, not read as one.
               if (++written > MaximumPrefixes)
               {
                  prefixes.clear();
                  return false;
               }

               const std::string prefix = current == "#default" ? std::string() : current;
               if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end())
                  prefixes.push_back(prefix);
               current.clear();
            }
            continue;
         }
         current += c;
      }

      return true;
   }

   std::string
   ExclusiveCanonicalizer::Canonicalize(const XmlNode &element, const std::vector<std::string> &inclusive,
                                        const XmlNode *excluded)
   {
      // What is bound where the element's parent is: every ancestor's
      // declarations, the outermost first so that a nearer one wins.
      std::vector<const XmlNode *> ancestors;
      for (const XmlNode *at = element.parent; at != nullptr; at = at->parent)
         ancestors.push_back(at);

      Bindings scope;
      for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
      {
         for (const XmlNamespaceDeclaration &declaration : (*it)->namespaces)
            scope[declaration.prefix] = declaration.uri;
      }

      // Each prefix once, whatever the caller handed in: the list is looked at
      // for every element.
      Canonicalization how;
      how.excluded = excluded;
      for (const std::string &prefix : inclusive)
      {
         if (prefix.empty())
            how.inclusiveDefault = true;
         else if (prefix != "xml" && prefix != "xmlns" &&
                  std::find(how.inclusive.begin(), how.inclusive.end(), prefix) == how.inclusive.end())
            how.inclusive.push_back(prefix);
      }

      std::string out;
      Bindings rendered;
      WriteElement(element, scope, rendered, how, out);
      return out;
   }

   // ------------------------------------------------------------ the tests

   namespace
   {
      void Require(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      const XmlNode *FindElement(const XmlNode *node, const char *localName)
      {
         if (node == nullptr)
            return nullptr;
         if (node->IsElement() && node->local == localName)
            return node;
         for (const std::shared_ptr<XmlNode> &child : node->children)
         {
            const XmlNode *found = FindElement(child.get(), localName);
            if (found != nullptr)
               return found;
         }
         return nullptr;
      }

      std::vector<std::string> Prefixes(const char *example, const char *list)
      {
         std::vector<std::string> prefixes;
         Require(ExclusiveCanonicalizer::PrefixList(list == nullptr ? "" : list, prefixes),
                 std::string(example) + ": the PrefixList was refused");
         return prefixes;
      }

      // The canonical form of the named element of a document, or of its root.
      void Expect(const char *example, const std::string &input, const char *elementName, const char *prefixes,
                  const std::string &expected)
      {
         XmlDocument document;
         std::string problem;
         Require(document.Parse(input, problem), std::string(example) + ": the reader refused the example: " + problem);

         const XmlNode *element = elementName == nullptr ? document.Root() : FindElement(document.Root(), elementName);
         Require(element != nullptr, std::string(example) + ": the element is not in the document");

         const std::string canonical = ExclusiveCanonicalizer::Canonicalize(*element, Prefixes(example, prefixes), nullptr);

         if (canonical != expected)
         {
            size_t at = 0;
            while (at < canonical.size() && at < expected.size() && canonical[at] == expected[at])
               at++;
            throw std::logic_error(std::string(example) + ": the canonical form differs from the expected one at byte " +
                                   std::to_string(at) + ": expected ..." + expected.substr(at, 40) + "... and got ..." +
                                   canonical.substr(at, 40) + "...");
         }

         // And the canonical form is a document this reader takes, whose
         // canonical form is itself - which is what re-reading the signed bytes
         // (XmlSignature.h) depends on.
         XmlDocument again;
         Require(again.Parse(canonical, problem), std::string(example) + ": the canonical form could not be read again: " + problem);
         Require(ExclusiveCanonicalizer::Canonicalize(*again.Root(), Prefixes(example, prefixes), nullptr) == canonical,
                 std::string(example) + ": the canonical form is not its own canonical form");
      }

      void Refused(const char *what, const std::string &input, const char *sentence)
      {
         XmlDocument document;
         std::string problem;
         Require(!document.Parse(input, problem), std::string("The reader took ") + what + ".");
         Require(problem.find(sentence) != std::string::npos,
                 std::string("The reader refused ") + what + " with another sentence: " + problem);
      }

      void Taken(const char *what, const std::string &input)
      {
         XmlDocument document;
         std::string problem;
         Require(document.Parse(input, problem), std::string("The reader refused ") + what + ": " + problem);
      }
   }

   void
   XmlCanonicalizationTester::Test()
   {
      // ---- the W3C's examples, byte for byte

      // Exclusive XML Canonicalization 1.0, section 2.2: the same n1:elem2 in
      // two documents. Inclusive canonicalisation writes each with its
      // ancestors' namespaces and xml: attributes and so writes them
      // differently; exclusive canonicalisation writes both the same - the
      // namespace each element visibly uses and nothing it inherited.
      const std::string elem2 =
         "<n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\">\n"
         "      <n3:stuff xmlns:n3=\"ftp://example.org\"></n3:stuff>\n"
         "   </n1:elem2>";

      Expect("exc-c14n 2.2, the first document",
         "<n0:local xmlns:n0=\"foo:bar\" xmlns:n3=\"ftp://example.org\">\n"
         "   <n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\">\n"
         "      <n3:stuff xmlns:n3=\"ftp://example.org\"/>\n"
         "   </n1:elem2>\n"
         "</n0:local>",
         "elem2", nullptr, elem2);

      Expect("exc-c14n 2.2, the second document",
         "<n2:pdu xmlns:n1=\"http://example.com\"\n"
         "           xmlns:n2=\"http://foo.example\"\n"
         "           xml:lang=\"fr\"\n"
         "           xml:space=\"retain\">\n"
         "   <n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\">\n"
         "      <n3:stuff xmlns:n3=\"ftp://example.org\"/>\n"
         "   </n1:elem2>\n"
         "</n2:pdu>",
         "elem2", nullptr, elem2);

      // The first document again with n0 and n3 in the PrefixList: those two
      // are written as Canonical XML writes them - on the apex, inherited or
      // not - and n3 is then not written again on n3:stuff. (.NET's
      // XmlDsigExcC14NTransform with "n0 n3" writes these bytes.)
      Expect("exc-c14n 2.2, the first document, PrefixList n0 n3",
         "<n0:local xmlns:n0=\"foo:bar\" xmlns:n3=\"ftp://example.org\">\n"
         "   <n1:elem2 xmlns:n1=\"http://example.net\" xml:lang=\"en\">\n"
         "      <n3:stuff xmlns:n3=\"ftp://example.org\"/>\n"
         "   </n1:elem2>\n"
         "</n0:local>",
         "elem2", "n0 n3",
         "<n1:elem2 xmlns:n0=\"foo:bar\" xmlns:n1=\"http://example.net\" xmlns:n3=\"ftp://example.org\" xml:lang=\"en\">\n"
         "      <n3:stuff></n3:stuff>\n"
         "   </n1:elem2>");

      // Canonical XML 1.0, section 3.2: white space in content is kept, all of
      // it; the canonical form is the input.
      const std::string whitespace =
         "<doc>\n"
         "   <clean>   </clean>\n"
         "   <dirty>   A   B   </dirty>\n"
         "   <mixed>\n"
         "      A\n"
         "      <clean>   </clean>\n"
         "      B\n"
         "      <dirty>   A   B   </dirty>\n"
         "      C\n"
         "   </mixed>\n"
         "</doc>";
      Expect("c14n 3.2, white space in document content", whitespace, nullptr, nullptr, whitespace);

      // Canonical XML 1.0, section 3.3: start and end tags - empty elements
      // written out, attributes in double quotes and sorted by namespace URI
      // and then local name, the declarations sorted with the default first.
      // Without the example's document type, which this reader refuses (so e9
      // has no defaulted attr), and with exc-c14n's namespace rules, under
      // which e6 and e9 write nothing and e8 writes xmlns="" to undo e7's
      // default. .NET's XmlDsigExcC14NTransform writes these bytes.
      Expect("c14n 3.3, start and end tags",
         "<doc>\n"
         "   <e1   />\n"
         "   <e2   ></e2>\n"
         "   <e3   name = \"elem3\"   id=\"elem3\"   />\n"
         "   <e4   name=\"elem4\"   id=\"elem4\"   ></e4>\n"
         "   <e5 a:attr=\"out\" b:attr=\"sorted\" attr2=\"all\" attr=\"I'm\"\n"
         "      xmlns:b=\"http://www.ietf.org\"\n"
         "      xmlns:a=\"http://www.w3.org\"\n"
         "      xmlns=\"http://example.org\"/>\n"
         "   <e6 xmlns=\"\" xmlns:a=\"http://www.w3.org\">\n"
         "      <e7 xmlns=\"http://www.ietf.org\">\n"
         "         <e8 xmlns=\"\" xmlns:a=\"http://www.w3.org\">\n"
         "            <e9 xmlns=\"\" xmlns:a=\"http://www.ietf.org\"/>\n"
         "         </e8>\n"
         "      </e7>\n"
         "   </e6>\n"
         "</doc>",
         nullptr, nullptr,
         "<doc>\n"
         "   <e1></e1>\n"
         "   <e2></e2>\n"
         "   <e3 id=\"elem3\" name=\"elem3\"></e3>\n"
         "   <e4 id=\"elem4\" name=\"elem4\"></e4>\n"
         "   <e5 xmlns=\"http://example.org\" xmlns:a=\"http://www.w3.org\" xmlns:b=\"http://www.ietf.org\" attr=\"I'm\" attr2=\"all\" b:attr=\"sorted\" a:attr=\"out\"></e5>\n"
         "   <e6>\n"
         "      <e7 xmlns=\"http://www.ietf.org\">\n"
         "         <e8 xmlns=\"\">\n"
         "            <e9></e9>\n"
         "         </e8>\n"
         "      </e7>\n"
         "   </e6>\n"
         "</doc>");

      // Canonical XML 1.0, section 3.4: character modifications and
      // character references. CR written as a reference survives as &#xD;, a
      // CDATA section becomes escaped text, an attribute's literal white space
      // is normalised and its referenced white space kept. Without the
      // example's document type, normNames and normId are CDATA attributes
      // like norm and are normalised as norm is. .NET writes these bytes.
      Expect("c14n 3.4, character modifications and character references",
         "<doc>\n"
         "   <text>First line&#x0d;&#10;Second line</text>\n"
         "   <value>&#x32;</value>\n"
         "   <compute><![CDATA[value>\"0\" && value<\"10\" ?\"valid\":\"error\"]]></compute>\n"
         "   <compute expr='value>\"0\" &amp;&amp; value&lt;\"10\" ?\"valid\":\"error\"'>valid</compute>\n"
         "   <norm attr=' &apos;   &#x20;&#13;&#xa;&#9;   &apos; '/>\n"
         "   <normNames attr='   A   &#x20;&#13;&#xa;&#9;   B   '/>\n"
         "   <normId id=' &apos;   &#x20;&#13;&#xa;&#9;   &apos; '/>\n"
         "</doc>",
         nullptr, nullptr,
         "<doc>\n"
         "   <text>First line&#xD;\n"
         "Second line</text>\n"
         "   <value>2</value>\n"
         "   <compute>value&gt;\"0\" &amp;&amp; value&lt;\"10\" ?\"valid\":\"error\"</compute>\n"
         "   <compute expr=\"value>&quot;0&quot; &amp;&amp; value&lt;&quot;10&quot; ?&quot;valid&quot;:&quot;error&quot;\">valid</compute>\n"
         "   <norm attr=\" '    &#xD;&#xA;&#x9;   ' \"></norm>\n"
         "   <normNames attr=\"   A    &#xD;&#xA;&#x9;   B   \"></normNames>\n"
         "   <normId id=\" '    &#xD;&#xA;&#x9;   ' \"></normId>\n"
         "</doc>");

      // Canonical XML 1.0, section 3.6: the output is UTF-8 - the copyright
      // sign referenced as &#169; is written as its two bytes.
      Expect("c14n 3.6, UTF-8 encoding", "<doc>&#169;</doc>", nullptr, nullptr, "<doc>\xC2\xA9</doc>");

      // Line ends: CR LF in the input is LF in the canonical form, and a CR on
      // its own is too (XML 1.0 section 2.11, before canonicalisation sees it).
      Expect("line ends normalised", "<doc>a\r\nb\rc</doc>", nullptr, nullptr, "<doc>a\nb\nc</doc>");

      // ---- the PrefixList

      // A prefix used only inside an attribute's value (xsi:type="xs:string",
      // which SAML attribute values carry) is not visibly used, so exclusive
      // canonicalisation drops its declaration unless the PrefixList names it.
      const char *valueOnly =
         "<a:Root xmlns:a=\"urn:a\" xmlns:b=\"urn:b\" xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
         "<a:Value xsi:type=\"xs:string\">text</a:Value><b:Other/></a:Root>";
      Expect("PrefixList empty", valueOnly, nullptr, nullptr,
         "<a:Root xmlns:a=\"urn:a\"><a:Value xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"xs:string\">text</a:Value>"
         "<b:Other xmlns:b=\"urn:b\"></b:Other></a:Root>");
      Expect("PrefixList xs", valueOnly, nullptr, "xs",
         "<a:Root xmlns:a=\"urn:a\" xmlns:xs=\"http://www.w3.org/2001/XMLSchema\"><a:Value xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"xs:string\">text</a:Value>"
         "<b:Other xmlns:b=\"urn:b\"></b:Other></a:Root>");

      // The default namespace: written where an element without a prefix first
      // uses it, or on the apex when the PrefixList says #default.
      const char *defaulted = "<a:Root xmlns=\"urn:d\" xmlns:a=\"urn:a\"><a:Child/><Plain/></a:Root>";
      Expect("PrefixList empty, default namespace", defaulted, nullptr, nullptr,
         "<a:Root xmlns:a=\"urn:a\"><a:Child></a:Child><Plain xmlns=\"urn:d\"></Plain></a:Root>");
      Expect("PrefixList #default", defaulted, nullptr, "#default",
         "<a:Root xmlns=\"urn:d\" xmlns:a=\"urn:a\"><a:Child></a:Child><Plain></Plain></a:Root>");

      // ---- the enveloped-signature transform: the excluded element and
      // everything under it are left out, the text around it kept.
      {
         XmlDocument document;
         std::string problem;
         Require(document.Parse("<r xmlns:ds=\"urn:ds\"><a>1</a> <ds:Signature><ds:x/></ds:Signature> <b>2</b></r>", problem),
                 "The enveloped example was refused: " + problem);
         const XmlNode *signature = FindElement(document.Root(), "Signature");
         Require(ExclusiveCanonicalizer::Canonicalize(*document.Root(), std::vector<std::string>(), signature) ==
                 "<r><a>1</a>  <b>2</b></r>",
                 "The enveloped-signature transform did not leave out exactly the signature.");
      }

      // ---- comments are not in the canonical form, and what they separated is
      // one text node: the comment-injection defence.
      {
         XmlDocument document;
         std::string problem;
         Require(document.Parse("<n>user@example.test<!---->.evil</n>", problem), "A comment in text was refused: " + problem);
         Require(document.Root()->TextContent() == "user@example.test.evil" && document.Root()->children.size() == 1,
                 "Text on either side of a comment is not one text node.");
         Require(ExclusiveCanonicalizer::Canonicalize(*document.Root(), std::vector<std::string>(), nullptr) ==
                 "<n>user@example.test.evil</n>", "A comment reached the canonical form.");
      }

      // ---- what the reader refuses, and its ceilings with their negative controls

      Refused("a document type declaration",
         "<!DOCTYPE r [<!ENTITY e \"x\">]><r>&e;</r>", "document type declaration");
      Refused("an entity no document defines", "<r>&e;</r>", "entity this server does not define");
      Refused("an external entity reference", "<r>&xxe;</r>", "entity this server does not define");
      Refused("a processing instruction", "<r><?pi data?></r>", "processing instruction");
      Refused("a processing instruction before the root", "<?pi data?><r/>", "processing instruction");
      Refused("an attribute written twice", "<r a=\"1\" a=\"2\"/>", "written twice");
      Refused("one attribute under two prefixes",
         "<r xmlns:p=\"urn:x\" xmlns:q=\"urn:x\" p:a=\"1\" q:a=\"2\"/>", "under two prefixes");
      Refused("an undeclared prefix", "<p:r/>", "is not declared");
      Refused("an undeclared attribute prefix", "<r p:a=\"1\"/>", "is not declared");
      Refused("xmlns:p=\"\"", "<r xmlns:p=\"\"/>", "declared empty");
      Refused("the xml prefix bound elsewhere", "<r xmlns:xml=\"urn:x\"/>", "xml prefix");
      Refused("a comment holding --", "<r><!-- a -- b --></r>", "comment");
      Refused("an element that is not closed", "<r><a></r>", "does not match");
      Refused("content after the root", "<r/><s/>", "after its root element");
      Refused("an encoding other than UTF-8", "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><r/>", "other than UTF-8");
      Refused("a byte that is not UTF-8", std::string("<r>\xC3</r>"), "not UTF-8");
      Refused("an overlong form", std::string("<r>\xC0\xAF</r>"), "overlong");
      Refused("a control character", std::string("<r>\x01</r>"), "does not allow");
      Refused("a reference to a control character", "<r>&#1;</r>", "does not allow");
      Refused("]]> in text", "<r>a]]>b</r>", "]]>");
      Refused("an unquoted attribute", "<r a=1/>", "not quoted");
      Refused("a < in an attribute", "<r a=\"<\"/>", "holds a <");

      std::string deep;
      for (int i = 0; i < 64; i++)
         deep += "<e>";
      for (int i = 0; i < 64; i++)
         deep += "</e>";
      Taken("elements nested 64 deep", deep);
      Refused("elements nested 65 deep", "<e>" + deep + "</e>", "64 deep");

      // The 1 MB ceiling: a document of exactly MaximumBytes is read, and one
      // byte more is not.
      {
         const std::string head = "<r>";
         const std::string tail = "</r>";
         std::string exact = head + std::string(XmlDocument::MaximumBytes - head.size() - tail.size(), 'a') + tail;
         Require(exact.size() == XmlDocument::MaximumBytes, "The ceiling example is not the ceiling's size.");
         Taken("a document of exactly 1 MB", exact);

         std::string over = head + std::string(XmlDocument::MaximumBytes - head.size() - tail.size() + 1, 'a') + tail;
         Refused("a document one byte over 1 MB", over, "larger than 1 MB");
      }

      // A byte order mark is not markup; an XML declaration naming UTF-8 is read.
      Taken("a byte order mark", std::string("\xEF\xBB\xBF<r/>"));
      Taken("an XML declaration naming UTF-8", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<r/>");

      // A surrogate written in UTF-8 is not a character, and the xmlns
      // namespace is nobody's to bind a prefix to.
      Refused("a surrogate written as UTF-8", std::string("<r>\xED\xA0\x80</r>"), "surrogate");
      Refused("a prefix bound to the xmlns namespace", "<r xmlns:p=\"http://www.w3.org/2000/xmlns/\"/>", "xmlns namespace");

      // The element ceiling: 20,000 elements, the root among them, are read,
      // and one more is not.
      {
         std::string many = "<r>";
         for (int i = 1; i < XmlDocument::MaximumElements; i++)
            many += "<e/>";
         many += "</r>";
         Taken("20000 elements", many);
         Refused("20001 elements", "<r><e/>" + many.substr(3), "more than 20000 elements");
      }

      // The attribute ceiling: 256 on one element are read, 257 are not.
      {
         std::string attributes;
         for (int i = 0; i < XmlDocument::MaximumAttributes; i++)
            attributes += " a" + std::to_string(i) + "=\"\"";
         Taken("256 attributes on one element", "<r" + attributes + "/>");
         Refused("257 attributes on one element", "<r" + attributes + " z=\"\"/>", "more than 256 attributes");
      }

      // The declaration ceiling: 128 in the whole document are read, and one
      // more anywhere is not.
      {
         std::string declarations;
         for (int i = 0; i < XmlDocument::MaximumNamespaceDeclarations; i++)
            declarations += " xmlns:p" + std::to_string(i) + "=\"urn:p:" + std::to_string(i) + "\"";
         Taken("128 namespace declarations", "<r" + declarations + "/>");
         Refused("129 namespace declarations", "<r" + declarations + "><e xmlns:q=\"urn:q\"/></r>", "more than 128 namespaces");
      }

      // The name ceiling: a namespace name of 512 characters is read, and one
      // of 513 is not - the default namespace's as a prefix's.
      {
         const std::string name = "urn:" + std::string(XmlDocument::MaximumNamespaceCharacters - 4, 'n');
         Taken("a namespace name of 512 characters", "<p:r xmlns:p=\"" + name + "\"/>");
         Refused("a namespace name of 513 characters", "<p:r xmlns:p=\"" + name + "n\"/>", "longer than 512 characters");
         Refused("a default namespace name of 513 characters", "<r xmlns=\"" + name + "n\"/>", "longer than 512 characters");
      }

      // The PrefixList: read once per prefix, counted as written, and refused
      // past 32.
      {
         std::vector<std::string> prefixes;
         std::string same = "a";
         for (int i = 1; i < ExclusiveCanonicalizer::MaximumPrefixes; i++)
            same += " a";
         Require(ExclusiveCanonicalizer::PrefixList(same, prefixes) && prefixes.size() == 1,
                 "A PrefixList of one prefix written 32 times was not read as that one prefix.");
         Require(!ExclusiveCanonicalizer::PrefixList(same + " a", prefixes),
                 "A PrefixList of 33 prefixes, one prefix repeated, was taken.");
         Require(ExclusiveCanonicalizer::PrefixList("#default xs #default", prefixes) && prefixes.size() == 2 &&
                 prefixes[0].empty() && prefixes[1] == "xs",
                 "A PrefixList with #default twice was not read as the default namespace and xs.");
      }

      // The worst a document inside every ceiling can ask of the canonicaliser,
      // at the ACS's 256 KB: 128 declarations on the root, each name 512
      // characters; a PrefixList of 32 of them, written on the root; 60
      // elements nested inside each other, each using one more of the rest, so
      // that what is rendered grows as deep as it can; and inside the deepest,
      // as many elements as the rest of the 256 KB holds, each using one of the
      // prefixes nothing above it wrote - so that each writes a declaration of
      // 512 characters of its own, and, before what is rendered stopped being
      // copied, copied the 92 written above it - half of them with an
      // attribute under another such prefix as well. Read and canonicalised in
      // well under the time a request has: before these ceilings, documents of
      // the same size took four to forty seconds of a worker's time, and this
      // one would have copied the better part of a gigabyte.
      {
         const int nested = 60;
         const int firstNested = ExclusiveCanonicalizer::MaximumPrefixes;
         const int firstUnwritten = firstNested + nested;
         const int unwritten = XmlDocument::MaximumNamespaceDeclarations - firstUnwritten;

         std::string worst = "<r";
         std::string list;
         for (int i = 0; i < XmlDocument::MaximumNamespaceDeclarations; i++)
         {
            const std::string number = std::to_string(i);
            const std::string name = "urn:worst-case:" + number + ":";
            worst += " xmlns:p" + number + "=\"" + name + std::string(XmlDocument::MaximumNamespaceCharacters - name.size(), 'u') + "\"";
            if (i < ExclusiveCanonicalizer::MaximumPrefixes)
               list += " p" + number;
         }
         worst += ">";

         std::string closing;
         for (int i = 0; i < nested; i++)
         {
            const std::string prefix = "p" + std::to_string(firstNested + i);
            worst += "<" + prefix + ":n>";
            closing = "</" + prefix + ":n>" + closing;
         }
         closing += "</r>";

         const size_t ceiling = 256 * 1024;
         for (int i = 0; i + 1 + nested < XmlDocument::MaximumElements; i++)
         {
            const std::string prefix = "p" + std::to_string(firstUnwritten + i % unwritten);
            const std::string element = (i % 2 == 0)
               ? "<" + prefix + ":e/>"
               : "<" + prefix + ":e p" + std::to_string(firstUnwritten + (i + 1) % unwritten) + ":a=\"\"/>";

            if (worst.size() + element.size() + closing.size() > ceiling)
               break;
            worst += element;
         }

         Require(worst.size() + closing.size() <= ceiling, "The worst-case document is over 256 KB before its text.");
         worst += std::string(ceiling - worst.size() - closing.size(), 'x');
         worst += closing;
         Require(worst.size() == ceiling, "The worst-case document is not 256 KB.");

         const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

         XmlDocument parsed;
         std::string problem;
         Require(parsed.Parse(worst, problem), "The worst-case document was refused: " + problem);

         std::vector<std::string> inclusive;
         Require(ExclusiveCanonicalizer::PrefixList(list, inclusive) && (int) inclusive.size() == ExclusiveCanonicalizer::MaximumPrefixes,
                 "The worst-case PrefixList was not read.");

         const std::string canonical = ExclusiveCanonicalizer::Canonicalize(*parsed.Root(), inclusive, nullptr);

         const long long took = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();

         Require(canonical.size() > worst.size(), "The worst-case document's canonical form is not all of it.");
         Require(took < 3000, "The worst-case document of 256 KB took " + std::to_string(took) +
                 " ms to read and canonicalise, and the bound is 3000 ms.");
      }
   }
}
