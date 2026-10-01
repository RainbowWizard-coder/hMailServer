// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The media type of a response whose type somebody other than the server
// chose: a file sent as a link (RestApiFiles.cpp) and a contact's photo
// (RestApiContacts.cpp). Those responses carry nosniff, and under nosniff the
// media type decides what a browser may do with the bytes: a JavaScript type
// is code, text/css a style sheet, HTML, XML and SVG a page. A
// Content-Disposition of attachment and a sandbox policy govern what happens
// when the address is opened, not every use a page of this origin can make of
// it.
//
// So the declared type is kept only when it is on a list of types no browser
// runs, applies or renders as a page - pictures other than SVG, sound, video,
// PDF, plain text and CSV, the office documents and the archives - and is
// written as the list writes it; any other type goes out as
// application/octet-stream, and so does a declared type that is not one plain
// media type:
//
//   * a comma anywhere: a list of types, which browsers read differently from
//     one another;
//   * whitespace inside the type itself, or anywhere a control character or a
//     byte outside ASCII;
//   * a parameter other than one charset, or a charset that is not a plain
//     name. A charset is accepted and not kept: every response this serves is
//     a download or a picture, whose charset nothing reads.
//
// A list of what is allowed, where there was a list of what is not: that one
// named text/javascript and application/javascript and missed the other names
// browsers run as script.
#pragma once

namespace HM
{
   class InertMediaType
   {
   public:
      // The type a download goes out under: the declared one when it is on the
      // list, application/octet-stream otherwise.
      static AnsiString ForDownload(const AnsiString &declared);

      // The type a picture goes out under: the declared one when it is one of
      // the list's pictures - never SVG - and application/octet-stream
      // otherwise.
      static AnsiString ForPicture(const AnsiString &declared);
   };

   // InertMediaType held to the list: every type on it kept as the list writes
   // it, and application/octet-stream for every JavaScript name browsers run,
   // the types they apply or render as a page, every list of types, a type
   // with whitespace, a control character or a byte outside ASCII in it, and a
   // parameter other than one plainly named charset. It runs inside the
   // service with the other class tests (ClassTester, which
   // Utilities.RunTestSuite reaches), and on Linux as media-type-tests. A
   // function of values only: no database, setting, file or network. It throws
   // std::logic_error naming the first thing that is wrong.
   class InertMediaTypeTester
   {
   public:
      void Test();
   };
}
