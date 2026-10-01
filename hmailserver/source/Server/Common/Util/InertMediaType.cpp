// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The media type of a response whose type somebody other than the server
// chose. See InertMediaType.h.

#include "StdAfx.h"
#include "InertMediaType.h"

#include <cctype>
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
      const char *const OctetStream = "application/octet-stream";

      // A declared type longer than this is not read. The longest on the list
      // is 73 characters; the column a file's type is kept in holds 100.
      const size_t LongestDeclared = 255;

      // The list. declared is a type as a sender may write it, lower-cased;
      // served is what goes out for it: the same, except for the few names
      // some systems give a type whose name is another (image/jpg).
      struct Listed
      {
         const char *declared;
         const char *served;
         bool picture;
      };

      const Listed List[] =
      {
         // Pictures - never image/svg+xml, which is a document that runs
         // script.
         { "image/png", "image/png", true },
         { "image/x-png", "image/png", true },
         { "image/jpeg", "image/jpeg", true },
         { "image/jpg", "image/jpeg", true },
         { "image/pjpeg", "image/jpeg", true },
         { "image/gif", "image/gif", true },
         { "image/webp", "image/webp", true },
         { "image/avif", "image/avif", true },
         { "image/bmp", "image/bmp", true },
         { "image/tiff", "image/tiff", true },
         { "image/heic", "image/heic", true },
         { "image/heif", "image/heif", true },
         { "image/x-icon", "image/x-icon", true },
         { "image/vnd.microsoft.icon", "image/vnd.microsoft.icon", true },

         // Sound.
         { "audio/mpeg", "audio/mpeg", false },
         { "audio/mp3", "audio/mpeg", false },
         { "audio/mp4", "audio/mp4", false },
         { "audio/x-m4a", "audio/x-m4a", false },
         { "audio/aac", "audio/aac", false },
         { "audio/ogg", "audio/ogg", false },
         { "audio/opus", "audio/opus", false },
         { "audio/flac", "audio/flac", false },
         { "audio/x-flac", "audio/x-flac", false },
         { "audio/wav", "audio/wav", false },
         { "audio/wave", "audio/wave", false },
         { "audio/x-wav", "audio/x-wav", false },
         { "audio/webm", "audio/webm", false },
         { "audio/midi", "audio/midi", false },
         { "audio/x-ms-wma", "audio/x-ms-wma", false },
         { "audio/amr", "audio/amr", false },
         { "audio/3gpp", "audio/3gpp", false },

         // Video.
         { "video/mp4", "video/mp4", false },
         { "video/x-m4v", "video/x-m4v", false },
         { "video/webm", "video/webm", false },
         { "video/ogg", "video/ogg", false },
         { "video/quicktime", "video/quicktime", false },
         { "video/mpeg", "video/mpeg", false },
         { "video/mp2t", "video/mp2t", false },
         { "video/x-msvideo", "video/x-msvideo", false },
         { "video/x-matroska", "video/x-matroska", false },
         { "video/x-ms-wmv", "video/x-ms-wmv", false },
         { "video/3gpp", "video/3gpp", false },
         { "video/3gpp2", "video/3gpp2", false },

         // Documents: PDF, plain text and CSV, and the office formats.
         { "application/pdf", "application/pdf", false },
         { "text/plain", "text/plain", false },
         { "text/csv", "text/csv", false },
         { "application/rtf", "application/rtf", false },
         { "application/msword", "application/msword", false },
         { "application/vnd.ms-excel", "application/vnd.ms-excel", false },
         { "application/vnd.ms-powerpoint", "application/vnd.ms-powerpoint", false },
         { "application/vnd.ms-outlook", "application/vnd.ms-outlook", false },
         { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", false },
         { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", false },
         { "application/vnd.openxmlformats-officedocument.presentationml.presentation", "application/vnd.openxmlformats-officedocument.presentationml.presentation", false },
         { "application/vnd.oasis.opendocument.text", "application/vnd.oasis.opendocument.text", false },
         { "application/vnd.oasis.opendocument.spreadsheet", "application/vnd.oasis.opendocument.spreadsheet", false },
         { "application/vnd.oasis.opendocument.presentation", "application/vnd.oasis.opendocument.presentation", false },
         { "application/vnd.oasis.opendocument.graphics", "application/vnd.oasis.opendocument.graphics", false },

         // Archives.
         { "application/zip", "application/zip", false },
         { "application/x-zip-compressed", "application/x-zip-compressed", false },
         { "application/gzip", "application/gzip", false },
         { "application/x-gzip", "application/x-gzip", false },
         { "application/x-tar", "application/x-tar", false },
         { "application/x-compressed", "application/x-compressed", false },
         { "application/x-7z-compressed", "application/x-7z-compressed", false },
         { "application/vnd.rar", "application/vnd.rar", false },
         { "application/x-rar-compressed", "application/x-rar-compressed", false },
         { "application/x-bzip2", "application/x-bzip2", false },
         { "application/x-xz", "application/x-xz", false },
         { "application/zstd", "application/zstd", false },

         // Bytes, as such.
         { "application/octet-stream", "application/octet-stream", false },
      };

      bool IsBlank(char c)
      {
         return c == ' ' || c == '\t';
      }

      char Lower(char c)
      {
         return c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c;
      }

      std::string Lowered(const std::string &text)
      {
         std::string lowered;
         lowered.reserve(text.size());
         for (size_t i = 0; i < text.size(); i++)
            lowered += Lower(text[i]);
         return lowered;
      }

      // A charset's name as IANA's registry writes them: letters, digits and
      // - _ . : +, forty characters at most.
      bool IsCharsetName(const std::string &value)
      {
         if (value.empty() || value.size() > 40)
            return false;
         for (size_t i = 0; i < value.size(); i++)
         {
            const char c = value[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               c == '-' || c == '_' || c == '.' || c == ':' || c == '+';
            if (!plain)
               return false;
         }
         return true;
      }

      // The type and subtype of a declared type, lower-cased, when the
      // declared type is one plain media type - optional blanks around it, and
      // at most one parameter, a charset - and empty when it is anything else.
      // Whether the type and subtype are a type at all is the list's to say.
      std::string Essence(const std::string &text)
      {
         if (text.size() > LongestDeclared)
            return std::string();

         for (size_t i = 0; i < text.size(); i++)
         {
            const unsigned char c = (unsigned char) text[i];
            if (c >= 0x7F || (c < 0x20 && c != '\t') || c == ',')
               return std::string();
         }

         size_t begin = 0;
         size_t end = text.size();
         while (begin < end && IsBlank(text[begin]))
            begin++;
         while (end > begin && IsBlank(text[end - 1]))
            end--;

         const size_t semicolon = text.find(';', begin);
         const bool parameter = semicolon != std::string::npos && semicolon < end;

         size_t essenceEnd = parameter ? semicolon : end;
         while (essenceEnd > begin && IsBlank(text[essenceEnd - 1]))
            essenceEnd--;

         const std::string essence = text.substr(begin, essenceEnd - begin);
         if (essence.empty())
            return std::string();
         for (size_t i = 0; i < essence.size(); i++)
         {
            if (IsBlank(essence[i]))
               return std::string();
         }

         if (parameter)
         {
            size_t start = semicolon + 1;
            while (start < end && IsBlank(text[start]))
               start++;

            // One parameter, so no second ";" - nor the empty parameter a
            // trailing one would make.
            const std::string given = text.substr(start, end - start);
            if (given.find(';') != std::string::npos)
               return std::string();

            const size_t equals = given.find('=');
            if (equals == std::string::npos || Lowered(given.substr(0, equals)) != "charset")
               return std::string();

            std::string value = given.substr(equals + 1);
            if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"')
               value = value.substr(1, value.size() - 2);
            if (!IsCharsetName(value))
               return std::string();
         }

         return Lowered(essence);
      }

      const Listed *Find(const std::string &declared)
      {
         const std::string essence = Essence(declared);
         if (essence.empty())
            return nullptr;

         for (size_t i = 0; i < sizeof(List) / sizeof(List[0]); i++)
         {
            if (essence == List[i].declared)
               return &List[i];
         }

         return nullptr;
      }
   }

   AnsiString
   InertMediaType::ForDownload(const AnsiString &declared)
   {
      const Listed *listed = Find(declared);
      return AnsiString(listed ? listed->served : OctetStream);
   }

   AnsiString
   InertMediaType::ForPicture(const AnsiString &declared)
   {
      const Listed *listed = Find(declared);
      return AnsiString(listed && listed->picture ? listed->served : OctetStream);
   }

   namespace
   {
      void Expect(bool condition, const std::string &message)
      {
         if (!condition)
            throw std::logic_error("InertMediaTypeTester: " + message);
      }

      // What a declared type goes out as, as a download and as a picture.
      void ExpectServed(const std::string &declared, const char *download, const char *picture)
      {
         const AnsiString given(declared.data(), declared.size());
         const AnsiString asDownload = InertMediaType::ForDownload(given);
         const AnsiString asPicture = InertMediaType::ForPicture(given);
         Expect(asDownload == download, "\"" + declared + "\" as a download went out as \"" + std::string(asDownload) + "\", not \"" + download + "\"");
         Expect(asPicture == picture, "\"" + declared + "\" as a picture went out as \"" + std::string(asPicture) + "\", not \"" + picture + "\"");
      }

      // A declared type that must go out as application/octet-stream either way.
      void ExpectRefused(const std::string &declared)
      {
         ExpectServed(declared, OctetStream, OctetStream);
      }

      // Nothing on the list is a type a browser runs, applies or renders as a
      // page: none of the names a page could be told it is.
      bool LooksActive(const std::string &type)
      {
         const char *const words[] = { "script", "html", "/xml", "+xml", "css", "json", "svg", "xsl", "wasm", "mjs", "rfc822", "multipart/", "message/", "component" };
         for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
         {
            if (type.find(words[i]) != std::string::npos)
               return true;
         }
         return false;
      }
   }

   void
   InertMediaTypeTester::Test()
   {
      // Every type on the list goes out as the list writes it, whatever its
      // case and the blanks around it; what goes out is on the list as itself;
      // and nothing on it is a type a page could run or render.
      for (size_t i = 0; i < sizeof(List) / sizeof(List[0]); i++)
      {
         const Listed &listed = List[i];
         const std::string declared = listed.declared;
         Expect(Lowered(declared) == declared, "\"" + declared + "\" is on the list in capitals");
         Expect(!LooksActive(declared) && !LooksActive(listed.served), "\"" + declared + "\" is on the list, and names a type a page could run or render");
         const char *picture = listed.picture ? listed.served : OctetStream;
         ExpectServed(declared, listed.served, picture);
         ExpectServed(declared + "; charset=utf-8", listed.served, picture);
         ExpectServed(" \t" + declared + "\t ", listed.served, picture);
         std::string shouted = declared;
         for (size_t j = 0; j < shouted.size(); j++)
            shouted[j] = (char) std::toupper((unsigned char) shouted[j]);
         ExpectServed(shouted, listed.served, picture);
         ExpectServed(listed.served, listed.served, picture);
         Expect(listed.picture == (declared.compare(0, 6, "image/") == 0), "\"" + declared + "\" is a picture on the list exactly when it is an image type");
      }

      // What a browser sends for the files people send.
      ExpectServed("image/png", "image/png", "image/png");
      ExpectServed("image/jpg", "image/jpeg", "image/jpeg");
      ExpectServed("application/pdf", "application/pdf", OctetStream);
      ExpectServed("video/mp4", "video/mp4", OctetStream);
      ExpectServed("audio/mp3", "audio/mpeg", OctetStream);
      ExpectServed("application/vnd.openxmlformats-officedocument.wordprocessingml.document", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", OctetStream);
      ExpectServed("application/x-zip-compressed", "application/x-zip-compressed", OctetStream);
      ExpectServed("application/octet-stream", OctetStream, OctetStream);

      // A charset: accepted, as written or quoted, in any case, and not kept.
      ExpectServed("text/plain; charset=utf-8", "text/plain", OctetStream);
      ExpectServed("text/plain;charset=UTF-8", "text/plain", OctetStream);
      ExpectServed("Text/Plain ; Charset=\"us-ascii\"", "text/plain", OctetStream);
      ExpectServed("text/csv; charset=ISO-8859-1", "text/csv", OctetStream);

      // Every JavaScript name a browser runs, in any case and with a charset.
      const char *const scripts[] =
      {
         "text/javascript", "application/javascript", "application/x-javascript", "text/x-javascript",
         "application/ecmascript", "application/x-ecmascript", "text/ecmascript", "text/x-ecmascript",
         "text/jscript", "text/livescript", "application/mjs",
         "text/javascript1.0", "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
         "text/javascript1.4", "text/javascript1.5",
      };
      for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); i++)
      {
         const std::string script = scripts[i];
         ExpectRefused(script);
         ExpectRefused(script + "; charset=utf-8");
         std::string shouted = script;
         for (size_t j = 0; j < shouted.size(); j++)
            shouted[j] = (char) std::toupper((unsigned char) shouted[j]);
         ExpectRefused(shouted);
      }

      // What a browser applies or renders as a page, or treats as more than
      // bytes: style sheets, HTML, XML and SVG, JSON, WebAssembly, archives
      // of a page, and the types nothing on the list resembles.
      const char *const active[] =
      {
         "text/css", "text/html", "application/xhtml+xml", "image/svg+xml", "text/xml", "application/xml",
         "application/rss+xml", "application/atom+xml", "text/xsl", "application/xslt+xml",
         "application/json", "application/manifest+json", "application/wasm", "message/rfc822",
         "multipart/related", "multipart/x-mixed-replace", "text/vtt", "text/cache-manifest",
         "text/x-component", "application/hta", "application/x-shockwave-flash", "application/x-msdownload",
         "text/markdown", "image/x-portable-pixmap", "application/vnd.ms-word.document.macroenabled.12",
      };
      for (size_t i = 0; i < sizeof(active) / sizeof(active[0]); i++)
         ExpectRefused(active[i]);

      // A list of types, in any order, and a comma wherever it is.
      ExpectRefused("image/png,text/javascript");
      ExpectRefused("text/javascript,image/png");
      ExpectRefused("image/png, text/javascript");
      ExpectRefused("image/png,");
      ExpectRefused(",image/png");
      ExpectRefused("text/plain; charset=utf-8, text/javascript");
      ExpectRefused("text/plain; charset=\"utf-8,text/javascript\"");

      // Whitespace inside the type.
      ExpectRefused("image/png text/javascript");
      ExpectRefused("image /png");
      ExpectRefused("image/ png");
      ExpectRefused("image/\tpng");

      // A parameter other than one charset, or a charset that is not a name.
      ExpectRefused("image/png; name=x");
      ExpectRefused("text/plain; charset=utf-8; format=flowed");
      ExpectRefused("text/plain; charset=utf-8; charset=utf-8");
      ExpectRefused("text/plain;");
      ExpectRefused("text/plain; ");
      ExpectRefused("text/plain;;charset=utf-8");
      ExpectRefused("text/plain; charset");
      ExpectRefused("text/plain; charset=");
      ExpectRefused("text/plain; charset=\"\"");
      ExpectRefused("text/plain; charset = utf-8");
      ExpectRefused("text/plain; charset= utf-8");
      ExpectRefused("text/plain; charset=utf 8");
      ExpectRefused("text/plain; charset=\"utf-8");
      ExpectRefused("text/plain; charset=utf-8\"");
      ExpectRefused("text/plain; charset=\"utf\\-8\"");
      ExpectRefused("text/plain; charset=" + std::string(41, 'a'));
      ExpectRefused("; charset=utf-8");

      // Control characters and bytes outside ASCII, anywhere.
      ExpectRefused("text/plain\r\nX-Injected: 1");
      ExpectRefused("text/plain\n");
      ExpectRefused("text/plain\r");
      ExpectRefused(std::string("image/png\0text/javascript", 25));
      ExpectRefused(std::string("image/png\0", 10));
      ExpectRefused("image/png\x01");
      ExpectRefused("image/png\x7F");
      ExpectRefused("image/png\xC3\xA9");
      ExpectRefused("\xEF\xBB\xBFimage/png");

      // Not a type at all, or too long to read.
      ExpectRefused("");
      ExpectRefused(" \t ");
      ExpectRefused("png");
      ExpectRefused("image");
      ExpectRefused("/");
      ExpectRefused("image/");
      ExpectRefused("/png");
      ExpectRefused("image//png");
      ExpectRefused("image/png/x");
      ExpectRefused(std::string(LongestDeclared - 9, ' ') + "image/png" + std::string(1, ' '));
      ExpectServed(std::string(LongestDeclared - 9, ' ') + "image/png", "image/png", "image/png");
   }
}
