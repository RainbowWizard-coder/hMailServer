// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace HM
{
   // The REST API's OpenAPI document (GET /api/v1/openapi.json), built by
   // RestApiServer::OpenApiDocument_ from the literals in RestApiServer.cpp and
   // the OpenApi*Paths_ function of every REST translation unit, held to the
   // router that answers it:
   //
   //   - the document is one JSON value, under a reader of this tester's own
   //     that refuses a member named twice in one object. The server's own
   //     reader (JsonValue) keeps the first of two, a browser's or Python's
   //     keeps the last, and neither says anything: a path written as two
   //     entries - which is what the folder listing's GET and the import's
   //     POST of /api/v1/me/folders/{id}/messages were from 11 September to
   //     8 October 2026, and /api/v1/status's two GETs from 24 September -
   //     publishes one of them and loses the other. The reader is first proved
   //     on documents written to fool it;
   //   - every path item carries only what OpenAPI 3.0 lets it (the operations,
   //     parameters, summary, description, servers), and every operation its
   //     summary and responses, as the generator writes them;
   //   - every operation the document names is one the router knows: the
   //     path, its templates made concrete, and the method, given to
   //     ParseRoute_, name a route other than unknown - unless the operation
   //     is one ProcessRequest_ answers before it parses a route, which are
   //     listed, method and path;
   //   - every route the router can name is one the document describes: each
   //     RouteKind but those listed, with the reason each, is reached by some
   //     documented operation.
   //
   // Static functions of RestApiServer only: no listener, database,
   // configuration or network. Run by ClassTester on Windows and by
   // openapi-tests on Linux; throws std::logic_error with the sentence.
   class RestApiOpenApiTester
   {
   public:
      void Test();

   private:
      struct Operation
      {
         std::string method;   // GET, POST, PUT, PATCH or DELETE
         std::string path;     // as the document writes it, templates and all
         std::string concrete; // the path with its templates made concrete
      };

      void TestTheReaderRefusesWhatOtherReadersKeep_();
      void TestTheDocumentIsOneJsonValueWithNoMemberTwice_();
      void TestEveryPathItemIsAsOpenApiHasIt_();
      void TestEveryOperationIsRouted_();
      void TestEveryRouteIsDocumented_();

      static std::string Concrete_(const std::string &path);
      static bool AnsweredBeforeTheRoute_(const std::string &method, const std::string &path);

      std::string document_;
      std::vector<Operation> operations_;
      // The route kind each operation reaches, by its index in operations_.
      std::vector<int> kinds_;
   };
}
