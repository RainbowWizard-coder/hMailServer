// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Images an HTML body carries inside itself, as data: URLs, moved into parts of
// their own that the HTML then names by Content-ID.
//
// The webmail keeps an image in a formatted signature - and one pasted into a
// formatted message - as <img src="data:image/png;base64,...">, which is the
// only form a page can hold an image in without a server to put it on. That
// is fine for the page and wrong for the recipient: Gmail and Outlook do not
// display a data: image in a message at all, so a signature logo sent that way
// arrives as an empty box. What every mail client that embeds an image sends
// instead is RFC 2387 multipart/related - the HTML part, and beside it each
// image as a part with a Content-ID that the HTML points at with a cid: URL
// (RFC 2392). This turns the first form into the second, once, just before the
// message is written.
//
// The shape it writes is the one Thunderbird writes: the related part wraps
// the HTML alone, inside the alternative, so the text part is still the first
// alternative and the top of the message is still multipart/alternative (or
// mixed, with attachments). A message whose HTML already sits under a
// multipart/related part - Outlook's shape, related around the alternative -
// has the images added to that part instead of a second one inside it.
//
// Who calls it: the webmail's send and draft routes (RestApiServer.cpp), once
// each, as the last change to the MIME tree before the file is written, after
// the attachments the request carried; and the two places this server appends
// HTML it holds to a message submitted over SMTP, SignatureAdder and
// DisclaimerAdder, when what they appended carried a data: image. An attachment
// added later - Message.Attachments.Add in an event script, which rebuilds the
// message in MessageData::CreatePart - carries a related part holding the HTML
// across whole (MessageData::FindFormattedBodyNoRecurse_), so the HTML and its
// images are not dropped by it.
//
// How many: at most MostImages distinct images are moved, which is what the
// webmail's own view of a message puts back in place (below).
//
// What it leaves alone: a data: URL that is not the whole value of a src
// attribute (a CSS url(), a data-src), a type other than PNG, JPEG, GIF and
// WebP (an SVG is a document, not a picture, and is not displayed in mail
// anyway), bytes that are not the image the URL says they are, and anything
// under multipart/signed or multipart/encrypted, where a change breaks what the
// sender signed.

#pragma once

namespace HM
{
   class MessageData;
   class MimeBody;

   class InlineImageParts
   {
   public:
      // One image found in the HTML: its media type, its bytes, and the
      // Content-ID it is given (without the angle brackets - the form a cid:
      // URL uses).
      struct Image
      {
         AnsiString media_type;
         AnsiString bytes;
         AnsiString content_id;
         AnsiString file_name;
      };

      // The most distinct images one message has moved into parts; an image
      // past it stays the data: URL it was. GET /api/v1/me/messages/{id}/html
      // puts back in place as many as this (RestApiServer.cpp,
      // HandleMeMessageHtml_), so the webmail's own view of a Sent copy or a
      // draft shows every image either way: a part put back, or a data: URL its
      // frame shows as it is. The route's other bounds - 4 MB an image, 12 MB
      // together - never bind there, because the page frames the HTML only of a
      // message of up to 1 MB. Sixty-four is more than a message written by hand
      // carries - a logo and a few screenshots - and few enough that the route,
      // which makes a pass over the HTML for each image it puts back, stays quick.
      static const int MostImages = 64;

      // Moves every data: image in the message's HTML body into an inline
      // part. Returns how many distinct images the message now carries that
      // way (the same image twice is one part named twice), 0 when there was
      // nothing to move.
      static int FromDataUrls(MessageData &message_data);
      static int FromDataUrls(std::shared_ptr<MimeBody> root);

      // Whether the HTML names at least one data: image - a cheap test for a
      // caller that has only just appended HTML and wants to know whether the
      // tree needs walking.
      static bool HasDataImages(const String &html);

      // The HTML with each data: image it names replaced by cid:<content id>,
      // and the images, in the order they first appear - the first MostImages
      // distinct ones; a later new image is left as it was, and a later mention
      // of one already taken is still replaced. content_id_domain is the part
      // after the @. Public for the tester.
      static String RewriteHtml(const String &html, std::vector<Image> &images, const AnsiString &content_id_domain);

   private:
      static bool FindHtmlPart_(std::shared_ptr<MimeBody> node, std::shared_ptr<MimeBody> parent, std::shared_ptr<MimeBody> related,
                                int depth, std::shared_ptr<MimeBody> &html_part, std::shared_ptr<MimeBody> &html_parent,
                                std::shared_ptr<MimeBody> &html_related);
      static bool ParseDataUrl_(const String &value, AnsiString &media_type, AnsiString &bytes);
      static bool IsSrcAttribute_(const String &lower_html, int quote_position);
      static AnsiString ContentIdDomain_();
   };

   class InlineImagePartsTester
   {
   public:
      void Test();
   };
}
