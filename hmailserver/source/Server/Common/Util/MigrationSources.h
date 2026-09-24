// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a migration job reads beside the mail (Application/MigrationManager.h):
// the contacts and the calendars of the source mailbox, and the table of the
// three kinds of source with the endpoints and OAuth scopes each needs.
//
// An IMAP server (kind 0) is asked over CardDAV and CalDAV with the same
// credentials the mail was read with, found as RFC 6764 has a client find them:
// /.well-known/carddav and /.well-known/caldav on the host (or on the DAV URL the
// migration names), then current-user-principal, then the home set, then every
// collection under it. Google Workspace (kind 1) is CardDAV and CalDAV too, at
// Google's own endpoints, with the OAuth token - its People API is not CardDAV,
// its CardDAV endpoint is. Microsoft 365 (kind 2) has neither: its contacts and
// events are read from Microsoft Graph, a page at a time, and written as vCard
// 3.0 and iCalendar here, so that what reaches the address book and the calendar
// is what a CardDAV or CalDAV client would have put there and is checked by the
// same parsers.
//
// What each reader answers: the collections, each with its items as text; or a
// reason the phase cannot run (no DAV on the host, a token without the scope,
// credentials the DAV server refuses, an answer too large or not DAV at all) -
// which skips the phase and is written in the report, never failing the job; or
// an error that another attempt might not meet (a lost connection, a timeout, a
// 429, a 5xx), with transient set. The first request tells a host with no DAV
// (nothing listening, a name that does not resolve, a 404) from one that is
// there and busy (a 429, a 5xx, a timeout): the first skips, the second retries.
//
// The credentials go only to the origin - scheme, host and port - the reader
// began at: a redirect, an href or a next page on another origin is followed
// without them, a redirect from https to plain http is refused, and Graph's
// next page is taken only from graph.microsoft.com. A DAV server that sends a
// client to another host therefore gets no password there, and its migration
// names that host as its dav_url instead.
//
// A reader asks stopRequested before every request and gives up, transient,
// when it answers true, so a pause, a cancel or a service stop is not held for
// a whole address book.

#pragma once

#include <functional>
#include <vector>

namespace HM
{
   class JsonValue;

   // One kind of source. A ClassTester fact holds the table (MigrationSourcesTester).
   struct MigrationProvider
   {
      int kind;
      const char *word;            // "imap", "google", "microsoft365" - the REST spelling
      const char *label;           // what a person reads
      const char *oauthProvider;   // ReaderOAuth's word, or "" for a password
      const char *imapHost;        // the default host, "" when the migration names one
      int imapPort;
      const char *mailScope;       // the OAuth scope the IMAP login needs, "" for a password
      const char *contactsScope;   // the scope the contacts need, "" when the credentials serve
      const char *calendarsScope;
      const char *consentScope;    // what a job's sign-in link asks consent for
      bool graph;                  // contacts and calendars from Microsoft Graph rather than DAV
      const char *contactsUrl;     // with {user}; "" to discover from the host
      const char *calendarsUrl;
   };

   class MigrationSources
   {
   public:
      // The provider of a kind, or of its REST word; null when there is none.
      static const MigrationProvider *ByKind(int kind);
      static const MigrationProvider *ByWord(const AnsiString &word);

      // How the reader proves itself: Basic with the user and the password, or
      // a bearer token.
      struct Credentials
      {
         AnsiString user;
         AnsiString password;
         AnsiString bearer;
         AnsiString origin;   // where they may go, "https://host:443"; the readers set it from where they begin
      };

      struct Item
      {
         AnsiString href;
         AnsiString data;          // one vCard or one iCalendar object, UTF-8
      };

      struct Collection
      {
         AnsiString href;
         AnsiString segment;       // the last path segment, "contacts", "calendar", "work"
         AnsiString displayName;
         std::vector<Item> items;
      };

      // The address books (calendar false) or the calendars (true) of a DAV
      // source. start is where to begin: a /.well-known/ URI, which is
      // discovered from, or a collection's own URL, which is read as it is.
      static bool ReadDav(const AnsiString &start, bool calendar, const Credentials &credentials,
                          std::vector<Collection> &collections, String &skipReason, String &error, bool &transient,
                          const std::function<bool()> &stopRequested = std::function<bool()>());

      // Microsoft Graph's contacts, as vCards, and events, as iCalendar objects,
      // in one collection each.
      static bool ReadGraphContacts(const AnsiString &token, std::vector<Collection> &collections,
                                    String &skipReason, String &error, bool &transient,
                                    const std::function<bool()> &stopRequested = std::function<bool()>());
      static bool ReadGraphEvents(const AnsiString &token, std::vector<Collection> &collections,
                                  String &skipReason, String &error, bool &transient,
                                  const std::function<bool()> &stopRequested = std::function<bool()>());

      // Where DAV begins for a source: the migration's DAV URL, else https://<host>/,
      // with the well-known path; or the provider's own URL with {user} filled in.
      static AnsiString DavStart(const MigrationProvider &provider, const AnsiString &host, const AnsiString &davUrl,
                                 const AnsiString &user, bool calendar);

      // Public for the tester.
      static AnsiString GraphContactToVCard(const JsonValue &contact);
      static AnsiString GraphEventToICalendar(const JsonValue &event);
      static bool ExtractItems(const AnsiString &multistatus, const char *dataElement, std::vector<Item> &items);
      static AnsiString ResolveUrl(const AnsiString &base, const AnsiString &reference);
      static AnsiString OriginOf(const AnsiString &url);
      static AnsiString DecodeXmlText(const AnsiString &text);
      static AnsiString GraphTimeToUtc(const AnsiString &graphDateTime);
      static AnsiString RecurrenceRule(const JsonValue &recurrence);
   };

   class MigrationSourcesTester
   {
   public:
      void Test();
   };
}
