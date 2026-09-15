// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "InlineImageParts.h"

#include "GUIDCreator.h"
#include "Utilities.h"
#include "Encoding/Base64.h"
#include "../BO/MessageData.h"
#include "../Mime/Mime.h"
#include "../Mime/MimeCode.h"

#include <cstring>
#include <map>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const wchar_t *DataImagePrefix = L"data:image/";
      const int DataImagePrefixLength = 11;

      // The same nesting limit MessageData::GetViewBodyPart_ keeps, so the part
      // found here is the part GetHTMLBody answers with.
      const int MaxDepth = 10;

      bool IsHtmlSpace(wchar_t c)
      {
         return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
      }

      // The bytes are the image the URL names: the few bytes every file of that
      // type starts with. A data: URL that calls a GIF a PNG, or calls a page a
      // picture, is left in the HTML as it was rather than given a Content-Type
      // it does not deserve.
      bool StartsAs(const AnsiString &media_type, const AnsiString &bytes)
      {
         const unsigned char *b = (const unsigned char *) bytes.c_str();
         const size_t n = (size_t) bytes.GetLength();

         if (media_type == "image/png")
            return n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0;
         if (media_type == "image/jpeg")
            return n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
         if (media_type == "image/gif")
            return n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0);
         if (media_type == "image/webp")
            return n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0;

         return false;
      }

      AnsiString ExtensionOf(const AnsiString &media_type)
      {
         if (media_type == "image/jpeg")
            return "jpg";

         return media_type.Mid(6);
      }

      void MakeRelated(std::shared_ptr<MimeBody> part)
      {
         part->SetContentType("multipart/related", "");
         // RFC 2387 section 3.1: type is required, and names the root part.
         part->SetParameter(CMimeConst::ContentType(), "type", "text/html");
         part->SetBoundary(NULL);
      }

      // For the tester: the trees a script's Attachments.Add is given.
      std::shared_ptr<MimeBody> NewPart(const char *type, const String &text)
      {
         std::shared_ptr<MimeBody> part = std::shared_ptr<MimeBody>(new MimeBody);
         part->SetContentType(type, "");
         part->SetUnicodeText(text);
         return part;
      }

      void MakeMultipart(std::shared_ptr<MimeBody> part, const char *type)
      {
         part->SetContentType(type, "");
         part->SetRawText("This is a multi-part message.\r\n\r\n");
         part->SetBoundary(NULL);
      }

      void MakeFile(std::shared_ptr<MimeBody> part, const char *file_name)
      {
         part->SetContentType("application/octet-stream", "");
         part->SetTransferEncoding("base64");
         part->SetRawText("QUJD\r\n");
         part->SetRawFieldValue(CMimeConst::ContentDisposition(), CMimeConst::Attachment(), "");
         part->SetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename(), file_name);
      }

      std::shared_ptr<MimeBody> FindRelatedPart(std::shared_ptr<MimeBody> node, int depth)
      {
         if (!node || depth > MaxDepth)
            return nullptr;

         if (node->GetCleanContentType().CompareNoCase(_T("multipart/related")) == 0)
            return node;

         std::vector<std::shared_ptr<MimeBody> > children;
         for (std::shared_ptr<MimeBody> child = node->FindFirstPart(); child; child = node->FindNextPart())
            children.push_back(child);

         for (std::shared_ptr<MimeBody> child : children)
         {
            std::shared_ptr<MimeBody> found = FindRelatedPart(child, depth + 1);
            if (found)
               return found;
         }

         return nullptr;
      }

      // After a file is added the way Attachments.Add adds one: the message is
      // mixed, the file is one of its own parts, the HTML is still the body and
      // still names its image, and the image is still beside the HTML in a
      // related part under the Content-ID the HTML names.
      void AssertKeptTogether(MessageData &data, std::shared_ptr<MimeBody> added, bool with_text, const std::string &shape)
      {
         std::shared_ptr<MimeBody> root = data.GetMimeMessage();
         if (root->GetCleanContentType().CompareNoCase(_T("multipart/mixed")) != 0)
            throw std::logic_error("Adding a file did not make the message multipart/mixed: " + shape);

         bool added_at_top = false;
         for (std::shared_ptr<MimeBody> child = root->FindFirstPart(); child; child = root->FindNextPart())
         {
            if (child == added)
               added_at_top = true;
         }

         if (!added_at_top)
            throw std::logic_error("The file added is not a part of the message: " + shape);

         const String html = data.GetHTMLBody();
         if (html.Find(_T("<p>Ada</p>")) < 0 || html.Find(_T("cid:img1.")) < 0)
            throw std::logic_error("The HTML was dropped when a file was added: " + shape);

         if (with_text && data.GetBody().Find(_T("Ada")) < 0)
            throw std::logic_error("The text was dropped when a file was added: " + shape);

         std::shared_ptr<MimeBody> related = FindRelatedPart(root, 0);
         if (!related)
            throw std::logic_error("The related part was dropped when a file was added: " + shape);

         bool named = false;
         for (std::shared_ptr<MimeBody> child = related->FindFirstPart(); child; child = related->FindNextPart())
         {
            const char *content_id = child->GetRawFieldValue(_T("Content-ID"));
            if (content_id == NULL || child->GetCleanContentType().CompareNoCase(_T("image/png")) != 0)
               continue;

            AnsiString id = content_id;
            if (id.StartsWith("<") && id.EndsWith(">") && html.Find(_T("cid:") + String(id.Mid(1, id.GetLength() - 2))) >= 0)
               named = true;
         }

         if (!named)
            throw std::logic_error("The image the HTML names is no longer beside it in the related part: " + shape);
      }
   }

   bool
   InlineImageParts::HasDataImages(const String &html)
   {
      String lower = html;
      lower.ToLower();
      return lower.Find(DataImagePrefix) >= 0;
   }

   int
   InlineImageParts::FromDataUrls(MessageData &message_data)
   {
      return FromDataUrls(message_data.GetMimeMessage());
   }

   int
   InlineImageParts::FromDataUrls(std::shared_ptr<MimeBody> root)
   {
      if (!root)
         return 0;

      std::shared_ptr<MimeBody> html_part;
      std::shared_ptr<MimeBody> html_parent;
      std::shared_ptr<MimeBody> related;
      if (!FindHtmlPart_(root, nullptr, nullptr, 0, html_part, html_parent, related))
         return 0;

      const String html = html_part->GetUnicodeText();
      if (!HasDataImages(html))
         return 0;

      std::vector<Image> images;
      const String rewritten = RewriteHtml(html, images, ContentIdDomain_());
      if (images.empty())
         return 0;

      if (!related)
      {
         if (!html_parent)
         {
            // The HTML is the whole message. The message itself becomes the
            // related part - its own headers, From and Subject among them, stay
            // where they are - and the HTML moves into a first part of it.
            std::shared_ptr<MimeBody> body = std::shared_ptr<MimeBody>(new MimeBody);
            body->SetContentType("text/html", "");
            const AnsiString charset = root->GetCharset();
            if (!charset.IsEmpty())
               body->SetCharset(charset.c_str());

            root->DeleteField(CMimeConst::TransferEncoding());
            MakeRelated(root);
            root->SetRawText("This is a multi-part message.\r\n\r\n");
            root->AddPart(body);

            html_part = body;
            related = root;
         }
         else
         {
            // In the HTML part's own place, so an alternative keeps its order:
            // the text first, then the formatted form, as RFC 2046 has it.
            related = html_parent->CreatePart("multipart/related", html_part);
            MakeRelated(related);

            html_parent->ErasePart(html_part);
            related->AddPart(html_part);
         }
      }

      html_part->SetUnicodeText(rewritten);

      for (const Image &image : images)
      {
         std::shared_ptr<MimeBody> part = std::shared_ptr<MimeBody>(new MimeBody);
         part->SetContentType(image.media_type, "");
         part->SetParameter(CMimeConst::ContentType(), CMimeConst::Name(), image.file_name);
         part->SetTransferEncoding("base64");

         std::unique_ptr<MimeCodeBase> coder(MimeEnvironment::CreateCoder("base64"));
         coder->SetInput(image.bytes.c_str(), (size_t) image.bytes.GetLength(), true);
         AnsiString encoded;
         coder->GetOutput(encoded);
         part->SetRawText(encoded);

         part->SetRawFieldValue(CMimeConst::ContentID(), "<" + image.content_id + ">", "");
         part->SetRawFieldValue(CMimeConst::ContentDisposition(), CMimeConst::Inline(), "");
         part->SetParameter(CMimeConst::ContentDisposition(), CMimeConst::Filename(), image.file_name);

         related->AddPart(part);
      }

      return (int) images.size();
   }

   bool
   InlineImageParts::FindHtmlPart_(std::shared_ptr<MimeBody> node, std::shared_ptr<MimeBody> parent, std::shared_ptr<MimeBody> related,
                                   int depth, std::shared_ptr<MimeBody> &html_part, std::shared_ptr<MimeBody> &html_parent,
                                   std::shared_ptr<MimeBody> &html_related)
   {
      if (!node || depth > MaxDepth)
         return false;

      String type = node->GetCleanContentType();
      type.ToLower();

      if (type == _T("text/html") && !node->IsAttachment())
      {
         html_part = node;
         html_parent = parent;
         html_related = related;
         return true;
      }

      if (type.Find(_T("multipart/")) != 0)
         return false;

      // What the sender signed or encrypted is not this server's to rearrange.
      if (type == _T("multipart/signed") || type == _T("multipart/encrypted"))
         return false;

      std::shared_ptr<MimeBody> nearest_related = type == _T("multipart/related") ? node : related;

      // Collected first: FindFirstPart and FindNextPart keep their place in the
      // part itself.
      std::vector<std::shared_ptr<MimeBody> > children;
      for (std::shared_ptr<MimeBody> child = node->FindFirstPart(); child; child = node->FindNextPart())
         children.push_back(child);

      for (std::shared_ptr<MimeBody> child : children)
      {
         if (FindHtmlPart_(child, node, nearest_related, depth + 1, html_part, html_parent, html_related))
            return true;
      }

      return false;
   }

   String
   InlineImageParts::RewriteHtml(const String &html, std::vector<Image> &images, const AnsiString &content_id_domain)
   {
      String lower = html;
      lower.ToLower();

      // One identifier for the message, and a number for each image in it.
      String guid = GUIDCreator::GetGUID();
      guid.Replace(_T("{"), _T(""));
      guid.Replace(_T("}"), _T(""));
      guid.Replace(_T("-"), _T(""));
      guid.ToLower();
      const AnsiString stamp = guid;

      std::map<AnsiString, size_t> known;
      String result;
      int copied = 0;
      int from = 0;

      for (;;)
      {
         const int at = lower.Find(DataImagePrefix, from);
         if (at < 0)
            break;

         from = at + DataImagePrefixLength;

         // The whole value of a src attribute, quoted: <img src="data:...">.
         if (at == 0)
            continue;

         const wchar_t quote = html[at - 1];
         if (quote != '"' && quote != '\'')
            continue;

         if (!IsSrcAttribute_(lower, at - 1))
            continue;

         const int close = html.Find(quote, at);
         if (close < 0)
            break;

         from = close + 1;

         AnsiString media_type;
         AnsiString bytes;
         if (!ParseDataUrl_(html.Mid(at, close - at), media_type, bytes))
            continue;

         size_t index = 0;
         std::map<AnsiString, size_t>::const_iterator seen = known.find(bytes);
         if (seen != known.end())
         {
            index = seen->second;
         }
         else if (images.size() >= (size_t) MostImages)
         {
            // Past what the webmail's view puts back in place: left as a data:
            // URL, which that view shows as it is (InlineImageParts.h).
            continue;
         }
         else
         {
            AnsiString number;
            number.Format("%d", (int) images.size() + 1);

            Image image;
            image.media_type = media_type;
            image.bytes = bytes;
            image.content_id = "img" + number + "." + stamp + "@" + content_id_domain;
            image.file_name = "image" + number + "." + ExtensionOf(media_type);

            index = images.size();
            images.push_back(image);
            known[bytes] = index;
         }

         result += html.Mid(copied, at - copied);
         result += _T("cid:");
         result += String(images[index].content_id);
         copied = close;
      }

      if (images.empty())
         return html;

      result += html.Mid(copied);
      return result;
   }

   bool
   InlineImageParts::IsSrcAttribute_(const String &lower_html, int quote_position)
   {
      int i = quote_position - 1;
      while (i >= 0 && IsHtmlSpace(lower_html[i]))
         i--;

      if (i < 0 || lower_html[i] != '=')
         return false;

      i--;
      while (i >= 0 && IsHtmlSpace(lower_html[i]))
         i--;

      // "src", and not the tail of another name - data-src is a page's lazy
      // loader, not an image the message shows.
      if (i < 3 || lower_html[i] != 'c' || lower_html[i - 1] != 'r' || lower_html[i - 2] != 's')
         return false;

      const wchar_t before = lower_html[i - 3];
      return IsHtmlSpace(before) || before == '/';
   }

   bool
   InlineImageParts::ParseDataUrl_(const String &value, AnsiString &media_type, AnsiString &bytes)
   {
      String lower = value;
      lower.ToLower();

      const int comma = lower.Find(_T(','));
      if (comma < DataImagePrefixLength)
         return false;

      // data:image/<subtype>[;parameter...];base64,<data>
      const String meta = lower.Mid(DataImagePrefixLength, comma - DataImagePrefixLength);
      const int semicolon = meta.Find(_T(';'));
      if (semicolon <= 0 || meta.Right(7) != _T(";base64"))
         return false;

      const String subtype = meta.Mid(0, semicolon);
      if (subtype == _T("png"))
         media_type = "image/png";
      else if (subtype == _T("jpeg") || subtype == _T("jpg") || subtype == _T("pjpeg"))
         media_type = "image/jpeg";
      else if (subtype == _T("gif"))
         media_type = "image/gif";
      else if (subtype == _T("webp"))
         media_type = "image/webp";
      else
         return false;

      AnsiString encoded;
      encoded.reserve((size_t) (value.GetLength() - comma));
      for (int i = comma + 1; i < value.GetLength(); i++)
      {
         const wchar_t c = value[i];
         if (IsHtmlSpace(c))
            continue;

         const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
         if (!alphabet)
            return false;

         encoded += (char) c;
      }

      if (encoded.IsEmpty() || encoded.GetLength() % 4 != 0)
         return false;

      // Padding only at the end, and at most two of it.
      const int padding = encoded.Find('=');
      if (padding >= 0)
      {
         if (padding < encoded.GetLength() - 2)
            return false;

         for (int i = padding; i < encoded.GetLength(); i++)
         {
            if (encoded[i] != '=')
               return false;
         }
      }

      bytes = Base64::Decode(encoded.c_str(), encoded.GetLength());
      return !bytes.IsEmpty() && StartsAs(media_type, bytes);
   }

   AnsiString
   InlineImageParts::ContentIdDomain_()
   {
      // The machine's name, as MessageData::GenerateMessageID puts it after the
      // @ of a Message-ID, reduced to what a cid: URL carries without escaping.
      String name = Utilities::ComputerName();
      name.ToLower();

      AnsiString domain;
      for (int i = 0; i < name.GetLength(); i++)
      {
         const wchar_t c = name[i];
         if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
            domain += (char) c;
      }

      if (domain.IsEmpty())
         domain = "localhost";

      return domain;
   }

   void
   InlineImagePartsTester::Test()
   {
      // The eight bytes every PNG starts with, which is all the check looks at.
      const String png = _T("iVBORw0KGgo=");

      // One image named twice, in both quotings and in capitals, is one part.
      {
         const String html = _T("<p>Ada</p><img src=\"data:image/png;base64,") + png + _T("\" alt=\"\"><br><img alt=\"\" SRC = 'DATA:IMAGE/PNG;BASE64,") + png + _T("'>");
         std::vector<InlineImageParts::Image> images;
         const String rewritten = InlineImageParts::RewriteHtml(html, images, "example.test");

         if (images.size() != 1)
            throw std::logic_error("The same image named twice did not become one part.");

         if (images[0].media_type != "image/png" || images[0].bytes.GetLength() != 8 || images[0].file_name != "image1.png")
            throw std::logic_error("The image was not described as the PNG it is.");

         if (!images[0].content_id.EndsWith("@example.test"))
            throw std::logic_error("The Content-ID does not end in the domain it was given.");

         String lower = rewritten;
         lower.ToLower();
         if (lower.Find(_T("data:")) >= 0)
            throw std::logic_error("A data: URL was left in the HTML.");

         const String cid = _T("cid:") + String(images[0].content_id);
         const int first = rewritten.Find(cid);
         if (first < 0 || rewritten.Find(cid, first + 1) < 0)
            throw std::logic_error("Both references were not rewritten to the one Content-ID.");

         if (rewritten.Find(_T("<p>Ada</p><img src=\"cid:")) != 0)
            throw std::logic_error("The HTML around the image was not kept as it was.");
      }

      // Left exactly as they were.
      {
         const String untouched[] =
         {
            _T("<img data-src=\"data:image/png;base64,") + png + _T("\">"),
            _T("<div style=\"background:url(data:image/png;base64,") + png + _T(")\"></div>"),
            _T("<img src=\"data:image/svg+xml;base64,PHN2Zz4=\">"),
            _T("<img src=\"data:image/png;base64,R0lGODlhAQABAAAAACw=\">"),
            _T("<img src=\"data:image/png,") + png + _T("\">"),
            _T("<img src=\"data:image/png;base64,") + png,
            _T("<img src=\"data:image/png;base64,iVBOR*w0KGgo=\">"),
            _T("<img src=\"data:image/png;base64,iVBORw0=KGgo\">"),
         };

         for (const String &html : untouched)
         {
            std::vector<InlineImageParts::Image> images;
            if (InlineImageParts::RewriteHtml(html, images, "example.test") != html || !images.empty())
               throw std::logic_error("A data: URL that is not an image in a src attribute was rewritten.");
         }
      }

      const String html = _T("<p>Ada</p><img src=\"data:image/png;base64,") + png + _T("\" alt=\"\">");

      // The webmail's shape: an alternative of text and HTML. The HTML goes
      // into a related part in its own place, beside the image.
      {
         std::shared_ptr<MimeBody> root = std::shared_ptr<MimeBody>(new MimeBody);
         root->SetContentType("multipart/alternative", "");
         root->SetRawText("This is a multi-part message.\r\n\r\n");
         root->SetBoundary(NULL);

         std::shared_ptr<MimeBody> text = std::shared_ptr<MimeBody>(new MimeBody);
         text->SetContentType("text/plain", "");
         text->SetUnicodeText(_T("Ada"));
         std::shared_ptr<MimeBody> body = std::shared_ptr<MimeBody>(new MimeBody);
         body->SetContentType("text/html", "");
         body->SetUnicodeText(html);
         root->AddPart(text);
         root->AddPart(body);

         if (InlineImageParts::FromDataUrls(root) != 1)
            throw std::logic_error("The image in an alternative's HTML was not moved into a part.");

         if (root->GetPartCount() != 2 || root->FindFirstPart() != text)
            throw std::logic_error("The alternative did not keep its text part first.");

         std::shared_ptr<MimeBody> related = root->FindNextPart();
         if (!related || related->GetCleanContentType().CompareNoCase(_T("multipart/related")) != 0 || related->GetPartCount() != 2)
            throw std::logic_error("The HTML was not put in a related part of two.");

         if (related->FindFirstPart() != body)
            throw std::logic_error("The HTML is not the first part of the related part.");

         std::shared_ptr<MimeBody> image = related->FindNextPart();
         const char *content_id = image ? image->GetRawFieldValue(_T("Content-ID")) : NULL;
         if (!image || image->GetCleanContentType().CompareNoCase(_T("image/png")) != 0 || content_id == NULL)
            throw std::logic_error("The image part is not a PNG with a Content-ID.");

         AnsiString id = content_id;
         if (!id.StartsWith("<") || !id.EndsWith(">"))
            throw std::logic_error("The Content-ID is not in angle brackets.");

         if (body->GetUnicodeText().Find(_T("cid:") + String(id.Mid(1, id.GetLength() - 2))) < 0)
            throw std::logic_error("The HTML does not name the image part's Content-ID.");

         // The reader lists it, and resolves the cid: against it.
         if (!image->IsAttachment())
            throw std::logic_error("The image part is not one the reader lists.");

         AnsiString stored;
         root->Store(stored);
         if (stored.Find("type=\"text/html\"") < 0 || stored.Find("data:image") >= 0)
            throw std::logic_error("The message as written is not multipart/related with no data: URL left.");

         if (InlineImageParts::FromDataUrls(root) != 0)
            throw std::logic_error("A second pass found something to move.");
      }

      // Outlook's shape: the related part is already there, around the
      // alternative. The image joins it; no second related part.
      {
         std::shared_ptr<MimeBody> root = std::shared_ptr<MimeBody>(new MimeBody);
         root->SetContentType("multipart/related", "");
         root->SetBoundary(NULL);
         std::shared_ptr<MimeBody> alternative = std::shared_ptr<MimeBody>(new MimeBody);
         alternative->SetContentType("multipart/alternative", "");
         alternative->SetBoundary(NULL);
         std::shared_ptr<MimeBody> body = std::shared_ptr<MimeBody>(new MimeBody);
         body->SetContentType("text/html", "");
         body->SetUnicodeText(html);
         alternative->AddPart(body);
         root->AddPart(alternative);

         if (InlineImageParts::FromDataUrls(root) != 1 || root->GetPartCount() != 2 || alternative->GetPartCount() != 1)
            throw std::logic_error("An image under an existing related part was not added to that part.");
      }

      // The HTML is the whole message: the message becomes the related part.
      {
         std::shared_ptr<MimeBody> root = std::shared_ptr<MimeBody>(new MimeBody);
         root->SetContentType("text/html", "");
         root->SetUnicodeText(html);

         if (InlineImageParts::FromDataUrls(root) != 1)
            throw std::logic_error("The image in an HTML-only message was not moved into a part.");

         std::shared_ptr<MimeBody> first = root->FindFirstPart();
         if (root->GetCleanContentType().CompareNoCase(_T("multipart/related")) != 0 || root->GetPartCount() != 2 ||
             !first || first->GetCleanContentType().CompareNoCase(_T("text/html")) != 0)
            throw std::logic_error("An HTML-only message did not become a related part with its HTML first.");

         String lower = first->GetUnicodeText();
         lower.ToLower();
         if (lower.Find(_T("data:")) >= 0 || lower.Find(_T("cid:img1.")) < 0)
            throw std::logic_error("The HTML of an HTML-only message was not rewritten.");
      }

      // Signed: not this server's to rearrange.
      {
         std::shared_ptr<MimeBody> root = std::shared_ptr<MimeBody>(new MimeBody);
         root->SetContentType("multipart/signed", "");
         root->SetBoundary(NULL);
         std::shared_ptr<MimeBody> body = std::shared_ptr<MimeBody>(new MimeBody);
         body->SetContentType("text/html", "");
         body->SetUnicodeText(html);
         root->AddPart(body);

         if (InlineImageParts::FromDataUrls(root) != 0 || root->GetPartCount() != 1)
            throw std::logic_error("A signed message was rearranged.");
      }

      // At most MostImages distinct images become parts; a new one past that
      // stays a data: URL, and the first image named again is still replaced.
      {
         String many;
         String first_url;
         for (int i = 0; i <= InlineImageParts::MostImages; i++)
         {
            AnsiString bytes;
            bytes.append("\x89PNG\r\n\x1a\n", 8);
            bytes += (char) ('A' + (i % 26));
            bytes += (char) ('a' + (i / 26));
            const String url = _T("data:image/png;base64,") + String(Base64::Encode(bytes.c_str(), bytes.GetLength()));
            if (i == 0)
               first_url = url;
            many += _T("<img src=\"") + url + _T("\" alt=\"\">");
         }
         many += _T("<img src=\"") + first_url + _T("\" alt=\"\">");

         std::vector<InlineImageParts::Image> images;
         const String rewritten = InlineImageParts::RewriteHtml(many, images, "example.test");
         if (images.size() != (size_t) InlineImageParts::MostImages)
            throw std::logic_error("More or fewer images than MostImages were taken from a message with one more than that.");

         String lower = rewritten;
         lower.ToLower();
         const int left = lower.Find(_T("data:image/png"));
         if (left < 0 || lower.Find(_T("data:image/png"), left + 1) >= 0)
            throw std::logic_error("The image past MostImages was not left as the one data: URL.");

         const String tail = _T("cid:") + String(images[0].content_id) + _T("\" alt=\"\">");
         if (rewritten.Right(tail.GetLength()) != tail)
            throw std::logic_error("The first image, named again after the limit, was not replaced.");
      }

      // A file added afterwards the way a script adds one - Message.Attachments.Add,
      // which rebuilds the message in MessageData::CreatePart - keeps the HTML and
      // its image together, in every shape the move leaves: the webmail's
      // alternative, the same with a file already on it, an HTML-only message
      // (which became the related part), and Outlook's related around the
      // alternative, with and without a file.
      {
         const char *shapes[] = { "alternative", "mixed around the alternative", "HTML only", "related around the alternative", "mixed around related" };
         for (int shape = 0; shape < 5; shape++)
         {
            MessageData data;
            std::shared_ptr<MimeBody> root = data.GetMimeMessage();
            std::shared_ptr<MimeBody> alternative = std::shared_ptr<MimeBody>(new MimeBody);
            MakeMultipart(alternative, "multipart/alternative");
            alternative->AddPart(NewPart("text/plain", _T("Ada")));
            alternative->AddPart(NewPart("text/html", html));

            if (shape == 0)
            {
               MakeMultipart(root, "multipart/alternative");
               root->AddPart(NewPart("text/plain", _T("Ada")));
               root->AddPart(NewPart("text/html", html));
            }
            else if (shape == 1)
            {
               MakeMultipart(root, "multipart/mixed");
               root->AddPart(alternative);
               std::shared_ptr<MimeBody> file = std::shared_ptr<MimeBody>(new MimeBody);
               MakeFile(file, "first.bin");
               root->AddPart(file);
            }
            else if (shape == 2)
            {
               root->SetContentType("text/html", "");
               root->SetUnicodeText(html);
            }
            else if (shape == 3)
            {
               MakeMultipart(root, "multipart/related");
               root->AddPart(alternative);
            }
            else
            {
               MakeMultipart(root, "multipart/mixed");
               std::shared_ptr<MimeBody> related = std::shared_ptr<MimeBody>(new MimeBody);
               MakeMultipart(related, "multipart/related");
               related->AddPart(alternative);
               root->AddPart(related);
               std::shared_ptr<MimeBody> file = std::shared_ptr<MimeBody>(new MimeBody);
               MakeFile(file, "first.bin");
               root->AddPart(file);
            }

            if (InlineImageParts::FromDataUrls(data) != 1)
               throw std::logic_error(std::string("The image was not moved into a part: ") + shapes[shape]);

            std::shared_ptr<MimeBody> added = data.CreatePart("application/octet-stream");
            MakeFile(added, "added.bin");
            AssertKeptTogether(data, added, shape != 2, shapes[shape]);

            // And a second file, the message now rebuilt from what the first left.
            added = data.CreatePart("application/octet-stream");
            MakeFile(added, "again.bin");
            AssertKeptTogether(data, added, shape != 2, std::string(shapes[shape]) + ", a second file");
         }
      }

      // A plain body set afterwards on an HTML-only message (Message.Body, which
      // creates the text part through CreatePart): an alternative of the text and
      // the related part, not the image turned into an attachment.
      {
         MessageData data;
         std::shared_ptr<MimeBody> root = data.GetMimeMessage();
         root->SetContentType("text/html", "");
         root->SetUnicodeText(html);

         if (InlineImageParts::FromDataUrls(data) != 1)
            throw std::logic_error("The image in an HTML-only message was not moved into a part.");

         std::shared_ptr<MimeBody> text = data.CreatePart("text/plain");
         std::shared_ptr<MimeBody> first = root->FindFirstPart();
         std::shared_ptr<MimeBody> second = root->FindNextPart();
         if (root->GetCleanContentType().CompareNoCase(_T("multipart/alternative")) != 0 || root->GetPartCount() != 2 || first != text ||
             !second || second->GetCleanContentType().CompareNoCase(_T("multipart/related")) != 0 || second->GetPartCount() != 2)
            throw std::logic_error("A text part added to an HTML-only message with an image did not make an alternative of the text and the related part.");

         std::shared_ptr<MimeBody> related_html = second->FindFirstPart();
         if (!related_html || related_html->GetCleanContentType().CompareNoCase(_T("text/html")) != 0 || related_html->GetUnicodeText().Find(_T("cid:img1.")) < 0)
            throw std::logic_error("The related part does not begin with the HTML naming its image.");
      }
   }
}
