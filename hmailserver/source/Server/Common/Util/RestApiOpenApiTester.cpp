// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "RestApiOpenApiTester.h"
#include "RestApiServer.h"
#include "JsonDocument.h"

#include <cstring>
#include <stdexcept>

namespace HM
{
   namespace
   {
      void
      Expect_(bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      std::string
      Join_(const std::vector<std::string> &items)
      {
         std::string joined;
         for (const std::string &item : items)
            joined += (joined.empty() ? "" : "; ") + item;
         return joined;
      }

      // RFC 8259's grammar, read once and kept nowhere, for the one thing the
      // readers the document meets do not say: that a member is named twice
      // in one object. JsonValue keeps the first of two, a browser's or
      // Python's reader the last, and each is silent about the other.
      class StrictReader
      {
      public:
         explicit StrictReader(const std::string &text) : text_(text), at_(0), depth_(0) { }

         // Reads the text as one JSON value. error is empty when it is one;
         // duplicates names each member found twice in one object by the
         // keys from the root, as paths./api/v1/status.
         void
         Read(std::string &error, std::vector<std::string> &duplicates)
         {
            error.clear();
            duplicates_.clear();
            at_ = 0;
            depth_ = 0;

            try
            {
               Whitespace_();
               Value_("");
               Whitespace_();
               if (at_ != text_.size())
                  Fail_("text after the value");
            }
            catch (const NotJson_ &failure)
            {
               error = failure.what();
            }

            duplicates = duplicates_;
         }

      private:
         class NotJson_ : public std::runtime_error
         {
         public:
            explicit NotJson_(const std::string &what) : std::runtime_error(what) { }
         };

         [[noreturn]] void
         Fail_(const std::string &what) const
         {
            throw NotJson_("at byte " + std::to_string(at_) + ": " + what);
         }

         char
         Peek_() const
         {
            return at_ < text_.size() ? text_[at_] : '\0';
         }

         void
         Whitespace_()
         {
            while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n'))
               at_++;
         }

         void
         Value_(const std::string &where)
         {
            const char c = Peek_();
            if (c == '{')
               Object_(where);
            else if (c == '[')
               Array_(where);
            else if (c == '"')
               String_();
            else if (c == 't')
               Literal_("true");
            else if (c == 'f')
               Literal_("false");
            else if (c == 'n')
               Literal_("null");
            else if (c == '-' || (c >= '0' && c <= '9'))
               Number_();
            else
               Fail_("not a value");
         }

         void
         Object_(const std::string &where)
         {
            if (++depth_ > 64)
               Fail_("nested deeper than 64");
            at_++;
            Whitespace_();

            std::set<std::string> keys;
            if (Peek_() == '}')
            {
               at_++;
               depth_--;
               return;
            }

            for (;;)
            {
               if (Peek_() != '"')
                  Fail_("a member without a name");
               const std::string key = String_();
               const std::string here = where.empty() ? key : where + "." + key;
               if (!keys.insert(key).second)
                  duplicates_.push_back(here);

               Whitespace_();
               if (Peek_() != ':')
                  Fail_("no colon after the name of " + here);
               at_++;
               Whitespace_();
               Value_(here);
               Whitespace_();

               if (Peek_() == ',')
               {
                  at_++;
                  Whitespace_();
                  continue;
               }
               if (Peek_() == '}')
               {
                  at_++;
                  depth_--;
                  return;
               }
               Fail_("neither a comma nor the end of the object after " + here);
            }
         }

         void
         Array_(const std::string &where)
         {
            if (++depth_ > 64)
               Fail_("nested deeper than 64");
            at_++;
            Whitespace_();

            if (Peek_() == ']')
            {
               at_++;
               depth_--;
               return;
            }

            for (size_t index = 0;; index++)
            {
               Value_(where + "[" + std::to_string(index) + "]");
               Whitespace_();

               if (Peek_() == ',')
               {
                  at_++;
                  Whitespace_();
                  continue;
               }
               if (Peek_() == ']')
               {
                  at_++;
                  depth_--;
                  return;
               }
               Fail_("neither a comma nor the end of the array after " + where + "[" + std::to_string(index) + "]");
            }
         }

         static int
         Hex_(char c)
         {
            if (c >= '0' && c <= '9')
               return c - '0';
            if (c >= 'a' && c <= 'f')
               return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
               return c - 'A' + 10;
            return -1;
         }

         unsigned
         FourHex_()
         {
            unsigned value = 0;
            for (int i = 0; i < 4; i++)
            {
               const int digit = Hex_(Peek_());
               if (digit < 0)
                  Fail_("not four hex digits after \\u");
               value = value * 16 + (unsigned) digit;
               at_++;
            }
            return value;
         }

         static void
         AppendUtf8_(std::string &out, unsigned codePoint)
         {
            if (codePoint < 0x80)
               out += (char) codePoint;
            else if (codePoint < 0x800)
            {
               out += (char) (0xC0 | (codePoint >> 6));
               out += (char) (0x80 | (codePoint & 0x3F));
            }
            else if (codePoint < 0x10000)
            {
               out += (char) (0xE0 | (codePoint >> 12));
               out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
               out += (char) (0x80 | (codePoint & 0x3F));
            }
            else
            {
               out += (char) (0xF0 | (codePoint >> 18));
               out += (char) (0x80 | ((codePoint >> 12) & 0x3F));
               out += (char) (0x80 | ((codePoint >> 6) & 0x3F));
               out += (char) (0x80 | (codePoint & 0x3F));
            }
         }

         // The string decoded, so that a name written "\u002fa" and one
         // written "/a" are the same name.
         std::string
         String_()
         {
            at_++;
            std::string out;
            for (;;)
            {
               if (at_ >= text_.size())
                  Fail_("a string that never ends");
               const char c = text_[at_++];
               if (c == '"')
                  return out;
               if ((unsigned char) c < 0x20)
                  Fail_("a control character in a string");
               if (c != '\\')
               {
                  out += c;
                  continue;
               }

               const char escaped = Peek_();
               at_++;
               switch (escaped)
               {
               case '"': out += '"'; break;
               case '\\': out += '\\'; break;
               case '/': out += '/'; break;
               case 'b': out += '\b'; break;
               case 'f': out += '\f'; break;
               case 'n': out += '\n'; break;
               case 'r': out += '\r'; break;
               case 't': out += '\t'; break;
               case 'u':
                  {
                     unsigned codePoint = FourHex_();
                     if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                     {
                        if (Peek_() != '\\' || at_ + 1 >= text_.size() || text_[at_ + 1] != 'u')
                           Fail_("a high surrogate without its low one");
                        at_ += 2;
                        const unsigned low = FourHex_();
                        if (low < 0xDC00 || low > 0xDFFF)
                           Fail_("a high surrogate followed by something else");
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                     }
                     else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                        Fail_("a low surrogate on its own");
                     AppendUtf8_(out, codePoint);
                  }
                  break;
               default:
                  Fail_("an escape that is not one");
               }
            }
         }

         void
         Digits_(const char *what)
         {
            if (Peek_() < '0' || Peek_() > '9')
               Fail_(what);
            while (Peek_() >= '0' && Peek_() <= '9')
               at_++;
         }

         void
         Number_()
         {
            if (Peek_() == '-')
               at_++;
            if (Peek_() == '0')
               at_++;
            else
               Digits_("a number without digits");
            if (Peek_() == '.')
            {
               at_++;
               Digits_("a fraction without digits");
            }
            if (Peek_() == 'e' || Peek_() == 'E')
            {
               at_++;
               if (Peek_() == '+' || Peek_() == '-')
                  at_++;
               Digits_("an exponent without digits");
            }
         }

         void
         Literal_(const char *word)
         {
            const size_t length = strlen(word);
            if (text_.compare(at_, length, word) != 0)
               Fail_(std::string("not ") + word);
            at_ += length;
         }

         const std::string &text_;
         size_t at_;
         int depth_;
         std::vector<std::string> duplicates_;
      };

      // What OpenAPI 3.0 lets a path item carry (section 4.7.8.1).
      bool
      IsOperation_(const std::string &key)
      {
         return key == "get" || key == "post" || key == "put" || key == "patch" || key == "delete" ||
                key == "head" || key == "options" || key == "trace";
      }

      bool
      IsPathItemField_(const std::string &key)
      {
         return key == "$ref" || key == "summary" || key == "description" || key == "servers" || key == "parameters";
      }

      std::string
      Upper_(const std::string &text)
      {
         std::string upper = text;
         for (char &c : upper)
            if (c >= 'a' && c <= 'z')
               c = (char) (c - 'a' + 'A');
         return upper;
      }
   }

   void
   RestApiOpenApiTester::Test()
   {
      TestTheReaderRefusesWhatOtherReadersKeep_();
      TestTheDocumentIsOneJsonValueWithNoMemberTwice_();
      TestEveryPathItemIsAsOpenApiHasIt_();
      TestEveryOperationIsRouted_();
      TestEveryRouteIsDocumented_();
   }

   // The reader proved before it judges the document: a path written as two
   // entries, an operation written twice in one path, a name escaped two ways,
   // and the shapes that are not JSON at all - and that JsonValue, the
   // server's own reader, takes the first two without a word, which is why
   // this reader exists.
   void
   RestApiOpenApiTester::TestTheReaderRefusesWhatOtherReadersKeep_()
   {
      struct Case
      {
         const char *text;
         const char *error;       // a part of the error expected, or "" for JSON
         const char *duplicates;  // the duplicates expected, joined with "; "
      };

      const Case cases[] =
      {
         { "{\"paths\":{\"/a\":{\"get\":{}},\"/a\":{\"post\":{}}}}", "", "paths./a" },
         { "{\"paths\":{\"/a\":{\"get\":{},\"get\":{}}}}", "", "paths./a.get" },
         { "{\"paths\":{\"\\u002fa\":{},\"/a\":{}}}", "", "paths./a" },
         { "{\"a\":{\"b\":1,\"b\":2},\"c\":[{\"d\":1,\"d\":1}],\"a\":3}", "", "a.b; c[0].d; a" },
         { "{\"a\":1,\"b\":[1,2,{\"c\":\"x\\\"y\\n\\u00e9\\ud83d\\ude00\"}],\"d\":true,\"e\":null,\"f\":-1.5e+3}", "", "" },
         { "{\"a\":1,}", "a member without a name", "" },
         { "{\"a\":1} x", "text after the value", "" },
         { "{\"a\":01}", "neither a comma nor the end of the object", "" },
         { "{\"a\":\"\t\"}", "a control character", "" },
         { "{\"a\":\"\\x\"}", "an escape that is not one", "" },
         { "{\"a\":\"\\ud83d\"}", "a high surrogate without its low one", "" },
         { "[1,2,]", "not a value", "" },
         { "{\"a\" 1}", "no colon", "" },
         { "{\"a\":.5}", "not a value", "" },
         { "{\"a\":tru}", "not true", "" },
         { "", "not a value", "" },
         { "\"unterminated", "a string that never ends", "" },
      };

      std::vector<std::string> wrong;
      for (const Case &c : cases)
      {
         const std::string text = c.text;
         StrictReader reader(text);
         std::string error;
         std::vector<std::string> duplicates;
         reader.Read(error, duplicates);

         const bool errorAsExpected = *c.error == '\0' ? error.empty() : error.find(c.error) != std::string::npos;
         if (!errorAsExpected || Join_(duplicates) != c.duplicates)
            wrong.push_back(text + " read as " + (error.empty() ? "JSON" : "not JSON (" + error + ")") +
                            " with the duplicates [" + Join_(duplicates) + "], expected " +
                            (*c.error == '\0' ? "JSON" : std::string("not JSON (") + c.error + ")") + " with [" + c.duplicates + "]");
      }
      Expect_(wrong.empty(), "the strict reader judged these wrongly: " + Join_(wrong));

      // The server's reader takes a member named twice, keeping the first:
      // the document has to be judged by a reader that does not.
      JsonValue value;
      std::string error;
      Expect_(JsonValue::Parse("{\"paths\":{\"/a\":{\"get\":{}},\"/a\":{\"post\":{}}}}", value, error) && error.empty(),
              "JsonValue refused a path written twice, which it is documented to take: " + error);
      const JsonValue *a = value.Get("paths") ? value.Get("paths")->Get("/a") : nullptr;
      Expect_(a && a->Get("get") && !a->Get("post"),
              "JsonValue no longer keeps the first of two members of a name - the reason this tester reads the document itself has changed");
   }

   void
   RestApiOpenApiTester::TestTheDocumentIsOneJsonValueWithNoMemberTwice_()
   {
      document_ = std::string(RestApiServer::OpenApiDocument_().c_str());
      Expect_(document_.size() > 100000, "the OpenAPI document is " + std::to_string(document_.size()) + " bytes, which is not the document");

      StrictReader reader(document_);
      std::string error;
      std::vector<std::string> duplicates;
      reader.Read(error, duplicates);
      Expect_(error.empty(), "the OpenAPI document is not JSON: " + error);
      Expect_(duplicates.empty(), "the OpenAPI document names a member twice in one object, so a reader keeps one of the two and loses the other (a path written as two entries is one path item with both operations): " + Join_(duplicates));
   }

   // The structure the readers see, through the server's own reader now that
   // the document is known to have no member twice: the paths, each an object
   // of the fields OpenAPI gives a path item, each operation with its summary
   // and its responses. The operations are kept for the two tests after this.
   void
   RestApiOpenApiTester::TestEveryPathItemIsAsOpenApiHasIt_()
   {
      JsonValue root;
      std::string error;
      Expect_(JsonValue::Parse(document_, root, error), "JsonValue refuses the OpenAPI document the strict reader took: " + error);
      Expect_(root.GetString("openapi") == "3.0.3", "the document says it is OpenAPI " + root.GetString("openapi") + ", not 3.0.3");
      Expect_(root.Get("info") && root.Get("info")->IsObject() && !root.Get("info")->GetString("title").empty(), "the document has no info.title");
      Expect_(root.Get("components") && root.Get("components")->Get("securitySchemes"), "the document has no components.securitySchemes");

      const JsonValue *paths = root.Get("paths");
      Expect_(paths && paths->IsObject() && paths->Members().size() > 300,
              "the document's paths are not an object of more than 300 paths");

      operations_.clear();
      std::vector<std::string> wrong;
      for (const auto &path : paths->Members())
      {
         if (path.first.empty() || path.first[0] != '/')
         {
            wrong.push_back("the path " + path.first + " does not begin with /");
            continue;
         }
         if (!path.second.IsObject() || path.second.Members().empty())
         {
            wrong.push_back(path.first + " is not an object of operations");
            continue;
         }

         size_t operations = 0;
         for (const auto &member : path.second.Members())
         {
            if (IsOperation_(member.first))
            {
               operations++;
               const JsonValue &operation = member.second;
               if (!operation.IsObject())
                  wrong.push_back(Upper_(member.first) + " " + path.first + " is not an object");
               else if (operation.GetString("summary").empty())
                  wrong.push_back(Upper_(member.first) + " " + path.first + " has no summary");
               else if (!operation.Get("responses") || !operation.Get("responses")->IsObject() || operation.Get("responses")->Members().empty())
                  wrong.push_back(Upper_(member.first) + " " + path.first + " has no responses");

               Operation found;
               found.method = Upper_(member.first);
               found.path = path.first;
               found.concrete = Concrete_(path.first);
               operations_.push_back(found);
            }
            else if (!IsPathItemField_(member.first))
               wrong.push_back(path.first + " carries \"" + member.first + "\", which is neither an operation nor a field of an OpenAPI path item");
         }

         if (operations == 0)
            wrong.push_back(path.first + " has no operation");
      }
      Expect_(wrong.empty(), "the document's paths are not as OpenAPI has them: " + Join_(wrong));
      Expect_(operations_.size() > 400, "the document describes " + std::to_string(operations_.size()) + " operations, which is not the document");
   }

   // A documented path made concrete, the way RestApiRouteSweep asks it: an
   // address, a domain, a help page, a language, a token, and 1 for a number
   // - and in the shapes the router holds an identifier to, which the sweep
   // never meets on the account's own routes: a certificate's fingerprint is
   // its SHA-256, 64 hex characters; a browser session of the account's own is
   // named by the first twelve hex digits of its token's hash (a logged
   // session, under /api/v1/logs, by a number); a device by its key, 32 hex
   // characters.
   std::string
   RestApiOpenApiTester::Concrete_(const std::string &path)
   {
      std::string concrete;
      size_t at = 0;
      while (at < path.size())
      {
         const size_t open = path.find('{', at);
         if (open == std::string::npos)
         {
            concrete += path.substr(at);
            break;
         }
         const size_t close = path.find('}', open);
         if (close == std::string::npos)
         {
            concrete += path.substr(at);
            break;
         }

         concrete += path.substr(at, open - at);
         const std::string parameter = path.substr(open + 1, close - open - 1);

         // The segment before the template: what the identifier names.
         std::string names;
         if (open > 1 && path[open - 1] == '/')
         {
            const size_t previous = path.rfind('/', open - 2);
            if (previous != std::string::npos)
               names = path.substr(previous + 1, open - 2 - previous);
         }

         if (parameter == "fingerprint")
            concrete += "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
         else if (parameter == "id" && path.compare(0, 20, "/api/v1/me/sessions/") == 0)
            concrete += "0123456789ab";
         else if (parameter == "id" && names == "devices")
            concrete += "0123456789abcdef0123456789abcdef";
         else if (parameter == "address" || parameter == "target" || parameter == "member" || parameter == "destination")
            concrete += "someone@example.test";
         else if (parameter == "domain" || parameter == "name")
            concrete += "example.test";
         else if (parameter == "section")
            concrete += "delivery";
         else if (parameter == "page")
            concrete += "RestApi";
         else if (parameter == "code")
            concrete += "en";
         else if (parameter == "token")
            concrete += "sweep";
         else
            concrete += "1";
         at = close + 1;
      }
      return concrete;
   }

   // The operations ProcessRequest_ answers before it parses a route - the
   // pages, the links a message carries, the sign-ins that begin with no
   // credential - each with the method it takes, in the order it asks them
   // (RestApiServer.cpp, ProcessRequest_, before ParseRoute_). A documented
   // operation here is not asked of the router; the same path with another
   // method is (GET /api/v1/portal/branding is the page's, PUT the
   // administrator's).
   bool
   RestApiOpenApiTester::AnsweredBeforeTheRoute_(const std::string &method, const std::string &path)
   {
      struct Served
      {
         const char *methods;  // the methods, separated by spaces; "*" for any
         const char *path;     // the path, or the prefix of every path under it
         bool under;
      };

      static const Served served[] =
      {
         { "GET", "/", false }, { "GET", "/index.html", false },
         { "GET", "/deck-lang/", true },
         { "GET", "/api/v1/help", false }, { "GET", "/api/v1/help/", true },
         { "GET", "/portal", false }, { "GET", "/portal.js", false }, { "GET", "/portal-smime.js", false },
         { "GET", "/portal-markdown.js", false }, { "GET", "/portal-openpgp.js", false },
         { "GET", "/portal.webmanifest", false }, { "GET", "/api/v1/portal/branding", false },
         { "GET", "/portal-lang/", true },
         { "GET POST", "/files/", true }, { "GET POST", "/confidential/", true },
         { "GET", "/portal/protected/", true }, { "GET", "/portal-protected.js", false },
         { "GET POST", "/api/v1/protected/", true },
         { "GET", "/portal/oauth/callback", false }, { "GET", "/portal/oauth/migration", false },
         { "GET", "/portal/oidc/start", false }, { "GET", "/portal/oidc/callback", false },
         { "GET", "/portal/saml/start", false }, { "POST", "/portal/saml/acs", false },
         { "GET", "/portal/saml/finish", false }, { "GET", "/saml/metadata", false },
         { "GET", "/api/v1/signin-methods", false }, { "POST", "/api/v1/passkeys/challenge", false },
         { "GET POST", "/recovery/verify", false },
         { "POST", "/api/v1/password-reset", false }, { "POST", "/api/v1/password-reset/complete", false },
         { "GET POST", "/portal/reset", false },
         { "GET POST", "/held/release", false }, { "GET POST", "/held/stop", false },
         { "GET POST", "/lists/confirm", false }, { "GET POST", "/lists/unsubscribe", false },
         { "GET POST", "/lists/approve", false }, { "*", "/lists/", true },
         { "GET", "/portal-sw.js", false },
      };

      for (const Served &candidate : served)
      {
         const bool pathMatches = candidate.under
            ? path.compare(0, strlen(candidate.path), candidate.path) == 0 && path.size() > strlen(candidate.path)
            : path == candidate.path;
         if (!pathMatches)
            continue;
         const std::string methods = std::string(" ") + candidate.methods + " ";
         if (candidate.methods[0] == '*' || methods.find(" " + method + " ") != std::string::npos)
            return true;
      }
      return false;
   }

   // Every operation the document names, given to the router as a request
   // would be: the path with its templates made concrete and the method. One
   // the router does not know is answered 404 whoever asks, and the document
   // describes something the server does not do.
   void
   RestApiOpenApiTester::TestEveryOperationIsRouted_()
   {
      kinds_.assign(operations_.size(), -1);

      std::vector<std::string> unrouted;
      size_t asked = 0;
      for (size_t i = 0; i < operations_.size(); i++)
      {
         const Operation &operation = operations_[i];
         if (AnsweredBeforeTheRoute_(operation.method, operation.path))
            continue;

         asked++;
         RestApiServer::Route route;
         RestApiServer::ParseRoute_(AnsiString(operation.method.c_str()), AnsiString(operation.concrete.c_str()), route);
         if (route.kind == RestApiServer::RouteUnknown || route.kind == RestApiServer::RouteApiKeyUnsupported)
            unrouted.push_back(operation.method + " " + operation.path +
                               (operation.concrete == operation.path ? "" : " (asked as " + operation.concrete + ")"));
         else
            kinds_[i] = (int) route.kind;
      }

      Expect_(asked > 400, "only " + std::to_string(asked) + " documented operations were asked of the router, which is not the document");
      Expect_(unrouted.empty(), "the document describes these operations, which the router does not know - each is answered 404 whoever asks: " + Join_(unrouted));
   }

   // Every route the router can name, reached by some documented operation -
   // but those a request never carries a path for, listed here with the reason
   // each. A kind nothing reaches is named by its number, counted from
   // RouteUnknown in RestApiServer.h's RouteKind, between the two documented
   // operations nearest to it, which is where in the enum to look.
   void
   RestApiOpenApiTester::TestEveryRouteIsDocumented_()
   {
      // The kinds the document describes nothing for, each with its reason.
      static const int undocumentedByDesign[] =
      {
         // Under /api/v1/apikeys but none of the three supported calls: the
         // refusal's own kind, not an operation.
         RestApiServer::RouteApiKeyUnsupported,
         // Under /scim/v2 but none of SCIM's calls: every path there is a
         // SCIM route, this one included, so that the refusal is SCIM's own
         // (RestApiScim.cpp, ParseScimRoute_). Not an operation.
         RestApiServer::RouteScimUnknown,
      };

      std::map<int, std::string> reached;
      for (size_t i = 0; i < operations_.size(); i++)
         if (kinds_[i] > 0 && reached.find(kinds_[i]) == reached.end())
            reached[kinds_[i]] = operations_[i].method + " " + operations_[i].path;

      std::vector<std::string> undocumented;
      std::vector<std::string> stale;
      for (int kind = RestApiServer::RouteUnknown + 1; kind <= RestApiServer::RouteOpenApi; kind++)
      {
         bool listed = false;
         for (int candidate : undocumentedByDesign)
            if (candidate == kind)
               listed = true;

         const bool documented = reached.find(kind) != reached.end();
         if (documented && listed)
            stale.push_back(std::to_string(kind) + " (" + reached[kind] + ")");
         if (documented || listed)
            continue;

         std::string below = "the start";
         std::string above = "the end";
         auto next = reached.lower_bound(kind);
         if (next != reached.end())
            above = std::to_string(next->first) + " " + next->second;
         if (next != reached.begin())
         {
            auto previous = next;
            --previous;
            below = std::to_string(previous->first) + " " + previous->second;
         }
         undocumented.push_back("kind " + std::to_string(kind) + " (between " + below + " and " + above + ")");
      }

      Expect_(stale.empty(), "these route kinds are listed as undocumented by design and the document describes them - take them off the list: " + Join_(stale));
      Expect_(undocumented.empty(), "the router names these route kinds, which no documented operation reaches - each is a route the document does not describe (RestApiServer.h, RouteKind, counted from RouteUnknown): " + Join_(undocumented));
   }
}
