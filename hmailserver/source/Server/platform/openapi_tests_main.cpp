// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's OpenAPI document as a program of its own, for the POSIX build
// - the counterpart of spf-tests, which linux-build.sh runs with every other
// *-tests program.
//
// On Windows RestApiOpenApiTester runs inside the service with the other class
// tests: ClassTester::DoTests calls it and the regression suite reaches it
// through Utilities.RunTestSuite. That route does not exist here - it is a COM
// method, and the Linux suite's shim skips it - so the CMake build makes this.
//
// What it holds (Common/Util/RestApiOpenApiTester.h says each): the document
// the server serves as /api/v1/openapi.json, built from the same static
// functions, is one JSON value with no member named twice in one object - a
// path written as two entries, one a method, is that, and every reader keeps
// one of the two and loses the other, which is what the folder listing's GET
// and the import's POST of /api/v1/me/folders/{id}/messages were from
// 11 September to 8 October 2026; every path item carries what OpenAPI lets
// it; every operation the document names is one the router (ParseRoute_)
// knows; and every route the router can name is one the document describes.
//
// No database, no configuration, no file, no network, no root. Milliseconds.
// It prints one line either way and exits non-zero on a failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/RestApiOpenApiTester.h"

int
main()
{
   try
   {
      HM::RestApiOpenApiTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the REST API's OpenAPI document: %s\n", failure.what());
      return 1;
   }

   printf("ok    the REST API's OpenAPI document: one JSON value with no member twice, every path item as OpenAPI has it, every documented operation known to the router, every route the router names described\n");
   return 0;
}
