// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// GET /api/v1/store and POST /api/v1/store/measure: the message store's totals
// (docs/MessageStore.md).
//
// What the delivered messages are - the size IMAP's RFC822.SIZE and POP3's
// octet counts promise, which is all hmailserver_store_bytes has ever counted -
// beside what their files take on disk and what the parts held once take, so
// that a saving can be read rather than claimed. Walking the store is not
// something to do on the REST worker, so the answer is always the last
// measurement, made on the asynchronous work queue. GET starts the next one when
// the last is a quarter of an hour old and the store is in use - compression
// on, a part held once, or a container seen last time - so that the Deck's
// dashboard, which asks on every refresh, walks nothing on a server that never
// turned compression on; POST starts one now.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "../Persistence/MessageStoreMaintenance.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   HttpResponse
   RestApiServer::HandleStoreStatus_()
   {
      return BuildResponse_(200, MessageStoreMaintenance::TotalsJson(MessageStoreMaintenance::MeasureIfDueWhileInUse));
   }

   HttpResponse
   RestApiServer::HandleStoreMeasure_()
   {
      // 202: the measurement runs on the server's own time; the answer is the
      // last one, with measuring true, and GET says when the new one is in.
      return BuildResponse_(202, MessageStoreMaintenance::TotalsJson(MessageStoreMaintenance::MeasureImmediately));
   }
}
