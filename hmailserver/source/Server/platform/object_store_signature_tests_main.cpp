// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The object store's request signatures as a program of their own, for the
// POSIX build (Common/Util/ObjectStoreSignature.h, docs/ObjectStorage.md).
//
// On Windows the same tester runs inside the shipping server: ClassTester::
// DoTests calls ObjectStoreSignatureTester::Test, and the regression suite
// reaches it through Utilities.RunTestSuite. That route does not exist here, so
// the CMake build makes this instead and .github/workflows/linux-build.yml runs
// it on both architectures and under both compilers.
//
// What it holds the signer to: AWS Signature Version 4 against AWS's own test
// suite (the get-vanilla family and get-unreserved, the service "service" in
// us-east-1) and the four S3 examples of the SigV4 documentation, and Azure's
// Shared Key against a string to sign written out from the specification. Nothing
// here touches the network, the configuration or a database.
//
// One line per failure, and a non-zero exit when there is any.

#include <cstdio>

#include "../Common/Util/ObjectStoreSignature.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   HM::ObjectStoreSignatureTester tester;
   const std::vector<HM::AnsiString> failures = tester.Run();

   if (failures.empty())
   {
      printf("ok    AWS Signature Version 4 (the test suite and the S3 examples) and Azure Shared Key\n");
      return 0;
   }

   printf("FAIL  the object store's request signatures: %d\n", (int) failures.size());
   for (size_t i = 0; i < failures.size(); i++)
      printf("        %s\n", failures[i].c_str());

   return 1;
}
