// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// GET /api/v1/logs/sessions and GET /api/v1/logs/sessions/{id}: the sessions
// the server has open, and one of them followed - every line the log carries
// for it, handed over as it is written.
//
// WHY A CURSOR AND NOT A STREAM. The HTTP listener (HttpServer.cpp) writes a
// response whole: there is no way, on it, to hold a response open and append
// to it, which is what text/event-stream needs. So the follow is a long poll:
// the client asks for the lines after the sequence it last saw, and when
// there are none yet the handler waits - on SessionFollow's condition
// variable, for at most `wait` seconds - for the next one to arrive, then
// answers with it. The lines still appear the moment they are logged; what
// the client pays is one request per batch of lines rather than one
// connection. When the listener grows a streaming response, this route can
// grow an event-stream form beside the cursor one; the ring and the wait do
// not change.
//
// The wait is bounded twice, and both bounds are SessionFollow's: twenty
// seconds, under the listener's thirty-second request deadline; and one
// waiting follow at a time, because a waiting follow holds one of the
// listener's four worker threads for the length of its wait - and those four
// threads are not the Deck's own. The same handler answers /portal and every
// /api/v1/me route, so they are the webmail of every mailbox on this server
// as well; a follow budget of half the pool would let a page somebody left
// open, under nothing stronger than view_logs, hold half of it. A second
// follow is answered at once with `waited` false, and the Deck then pauses a
// second before asking again, which is what a plain poll looks like.
//
// Who may read: whoever may read the log files (view_logs), server-wide,
// since a session's lines are whichever domain's traffic it carried.

#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "SessionFollow.h"

#include <cstdlib>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The JSON escaper and the narrowing, handed in by the member functions
      // that own them - the arrangement RestApiReports.cpp makes, and for the
      // same reason: a second escaper here would be a second opinion about
      // what a quotation mark is.
      typedef AnsiString (*Escaper)(const AnsiString &);
      typedef AnsiString (*Narrower)(const String &);

      AnsiString SessionJson(const SessionFollow::Session &session, Escaper escape, Narrower utf8)
      {
         AnsiString head;
         head.Format("{\"id\":%d,\"port\":%d,\"client\":%hs,\"open\":%hs,",
            session.id, session.local_port, session.client ? "true" : "false", session.open ? "true" : "false");

         AnsiString body = head;
         body += "\"protocol\":\"" + escape(session.protocol) + "\"";
         body += ",\"peer\":\"" + escape(session.peer) + "\"";
         body += ",\"account\":\"" + escape(utf8(session.account)) + "\"";
         body += ",\"started\":\"" + escape(utf8(session.started)) + "\"";
         body += ",\"ended\":\"" + escape(utf8(session.ended)) + "\"}";
         return body;
      }

      // Whether the log mask lets the lines of this protocol through at all -
      // the one thing a follower of an empty session most wants to know. The
      // conversation category each class logs under: the server side of a
      // protocol under its own bit, the fetcher clients under the protocol
      // they speak, and the classes that log through LOG_DEBUG alone under
      // debug.
      bool ProtocolLogged(const AnsiString &protocol)
      {
         Logger *logger = Logger::Instance();
         if (!logger->GetLoggingEnabled())
            return false;

         int mask = logger->GetLogMask();

         if (protocol == "SMTP" || protocol == "SMTP client")
            return (mask & Logger::LSSMTP) != 0;
         if (protocol == "POP3" || protocol == "POP3 client")
            return (mask & Logger::LSPOP3) != 0;
         if (protocol == "IMAP")
            return (mask & Logger::LSIMAP) != 0;

         return (mask & Logger::LSDebug) != 0;
      }
   }

   AnsiString
   RestApiServer::OpenApiSessionFollowPaths_()
   {
      static const char *paths =
         ",\"/api/v1/logs/sessions\":{\"get\":{\"summary\":\"The sessions the server has open, and the last ones that ended\",\"description\":\"Every connection this server has open now, newest first, then the last 200 that ended, newest first: {sessions:[{id,protocol,peer,port,client,open,account,started,ended}],open,ended,truncated,held_lines,logging_enabled}. id is the session id the log lines carry; protocol is SMTP, POP3 or IMAP for a session this server answers and SMTP client, POP3 client, IMAP client, SpamAssassin or Diagnostics for one it opened (client true); account is set once the session has signed in; ended is empty while the session is open. Only connections the mail server makes through its own TCP layer are here: the HTTP listener's requests (the REST API, the webmail, the DAV services), ManageSieve and the anti-virus scanner's socket are not sessions and carry no session id. truncated is true when more than 1000 sessions are open and only the newest 1000 are listed. held_lines is how many log entries the server holds in memory for following, and logging_enabled whether the log mask lets any line through. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"The sessions\"}}}},"
         "\"/api/v1/logs/sessions/{id}\":{\"get\":{\"summary\":\"Follow one session's log lines as they happen\",\"description\":\"The lines the log carries for session {id} after the sequence number `after` (0, the default, for everything the server still holds), as {session,known,open,protocol,peer,account,started,ended,logged,cursor,dropped,waited,lines:[{seq,time,category,message}]}. Ask again with after=cursor for the lines since. When there is none yet and `wait` (seconds, at most 20, default 0) is given, the request waits up to that long for the next line to arrive and answers with it - which is how the lines appear as they happen; waited says whether it did, and is false when another follow is already waiting (one at a time, since a waiting follow holds one of the listener's four worker threads and those also serve the webmail), in which case ask again after a pause. A session that has ended is never waited on, since its last line was written before it was listed as ended. dropped is true when after pointed before the oldest entry the server still holds (it holds the last 4096 across every session), so lines between were written and are gone. A line is the session's when it carries the id in its session column (the conversation lines) or names it in its text as \\\"session 41\\\" or \\\"Session Id: 41\\\" (the DEBUG and TCPIP lines). A line matched by its text is an inference, not a fact the logger recorded: the number has to follow a space, which is what keeps a token like session41 quoted out of a request target or an address from putting a stranger's sentence in front of a follower. logged says whether the log mask lets this protocol's lines through at all. known is false for a session neither open nor among the last 200 ended; 404 when it is unknown and no line is held for it either. At most 500 lines per answer. Server-wide; refused for domain-restricted keys.\",\"parameters\":[{\"name\":\"after\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"wait\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}],\"responses\":{\"200\":{\"description\":\"The lines, oldest first, and the cursor to ask after\"},\"404\":{\"description\":\"No such session and no line held for it\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleLogSessions_()
   {
      bool truncated = false;
      std::vector<SessionFollow::Session> sessions = SessionFollow::Instance()->ListSessions(truncated);

      Narrower utf8 = static_cast<Narrower>(&RestApiServer::Utf8_);

      int open = 0;
      int ended = 0;
      AnsiString body = "{\"sessions\":[";
      for (size_t i = 0; i < sessions.size(); i++)
      {
         if (sessions[i].open)
            open++;
         else
            ended++;

         if (i > 0)
            body += ",";
         body += SessionJson(sessions[i], &RestApiServer::JsonEscape_, utf8);
      }

      AnsiString tail;
      tail.Format("],\"open\":%d,\"ended\":%d,\"truncated\":%hs,\"held_lines\":%I64d,\"logging_enabled\":%hs}",
         open, ended, truncated ? "true" : "false", SessionFollow::Instance()->HeldEntries(),
         Logger::Instance()->GetLoggingEnabled() ? "true" : "false");
      body += tail;

      // Live state, never a cached copy: what is open now is the whole point.
      return BuildResponse_(200, body, "Cache-Control: no-store\r\n");
   }

   HttpResponse
   RestApiServer::HandleLogSessionFollow_(const Route &route)
   {
      if (route.record_id <= 0 || route.record_id > 0x7fffffff)
         return BuildResponse_(404, "{\"error\":\"no such session\"}");

      const int sessionId = (int) route.record_id;

      long long after = 0;
      AnsiString afterText = QueryParameter_(route.query, "after");
      if (!afterText.IsEmpty())
      {
         if (afterText.find_first_not_of("0123456789") != std::string::npos || afterText.size() > 18)
            return BuildResponse_(400, "{\"error\":\"after must be a sequence number\"}");
         after = strtoll(afterText.c_str(), nullptr, 10);
      }

      int wait = 0;
      AnsiString waitText = QueryParameter_(route.query, "wait");
      if (!waitText.IsEmpty())
      {
         // Nine digits at most, so atoi cannot overflow; anything that is a
         // number is then clamped rather than refused, because a client asking
         // to wait longer than the ceiling means the ceiling, not a mistake.
         if (waitText.find_first_not_of("0123456789") != std::string::npos || waitText.size() > 9)
            return BuildResponse_(400, "{\"error\":\"wait must be a number of seconds\"}");
         wait = atoi(waitText.c_str());
         if (wait > SessionFollow::MaxWaitSeconds)
            wait = SessionFollow::MaxWaitSeconds;
      }

      SessionFollow::Session session;
      const bool known_before = SessionFollow::Instance()->FindSession(sessionId, session);

      // Only a session that is still open is worth waiting on. A session
      // nobody has heard of will never have a line, and one that has ended
      // wrote its last before it was listed as ended - so a wait on either
      // would hold one of the four worker threads for nothing. Both are
      // answered at once with whatever the ring still holds.
      const bool waitable = known_before && session.open;
      SessionFollow::FollowResult result = SessionFollow::Instance()->Follow(sessionId, after, waitable ? wait : 0);

      // The session is read again AFTER the wait. A wait lasts up to twenty
      // seconds, and a session can sign in or end inside them: an answer
      // built from the copy taken before the wait would tell the page that a
      // session was open when it had gone, or give an empty account for one
      // that had signed in - and with debug logging off nothing wakes the
      // waiter when a session ends, so that stale answer would be exactly
      // the one that came back at the deadline, carried by the page until
      // its next request.
      SessionFollow::Session fresh;
      const bool still_known = SessionFollow::Instance()->FindSession(sessionId, fresh);
      if (still_known)
         session = fresh;
      else if (known_before)
      {
         // It ended during the wait and has already fallen off the end of
         // the last two hundred ended, which a busy server can do in twenty
         // seconds. It was known, and it is certainly not open.
         session.open = false;
      }

      const bool known = known_before || still_known;

      if (!known && result.lines.empty())
         return BuildResponse_(404, "{\"error\":\"no such session\"}");

      Narrower utf8 = static_cast<Narrower>(&RestApiServer::Utf8_);

      AnsiString head;
      head.Format("{\"session\":%d,\"known\":%hs,\"open\":%hs,\"logged\":%hs,\"cursor\":%I64d,\"dropped\":%hs,\"waited\":%hs,",
         sessionId, known ? "true" : "false", (known && session.open) ? "true" : "false",
         ProtocolLogged(session.protocol) ? "true" : "false",
         result.cursor, result.dropped ? "true" : "false", result.waited ? "true" : "false");

      AnsiString body = head;
      body += "\"protocol\":\"" + JsonEscape_(session.protocol) + "\"";
      body += ",\"peer\":\"" + JsonEscape_(session.peer) + "\"";
      body += ",\"account\":\"" + JsonEscape_(utf8(session.account)) + "\"";
      body += ",\"started\":\"" + JsonEscape_(utf8(session.started)) + "\"";
      body += ",\"ended\":\"" + JsonEscape_(utf8(session.ended)) + "\"";
      body += ",\"lines\":[";

      for (size_t i = 0; i < result.lines.size(); i++)
      {
         const SessionFollow::Line &line = result.lines[i];

         AnsiString entry;
         entry.Format("%hs{\"seq\":%I64d,", i > 0 ? "," : "", line.sequence);
         body += entry;
         body += "\"time\":\"" + JsonEscape_(utf8(line.time)) + "\"";
         body += ",\"category\":\"" + JsonEscape_(utf8(line.category)) + "\"";
         body += ",\"message\":\"" + JsonEscape_(utf8(line.message)) + "\"}";
      }

      body += "]}";

      // Never cached and never compressed into a proxy's buffer: the point is
      // that the reader sees the line now.
      return BuildResponse_(200, body, "Cache-Control: no-store\r\n");
   }
}
