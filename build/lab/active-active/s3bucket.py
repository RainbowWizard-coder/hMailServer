#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Makes a bucket on the lab's MinIO (aa-lab.sh store): PUT /<bucket>, signed with
AWS Signature Version 4, then HEAD /<bucket> to see that it is there.

  s3bucket.py https://host:9000 BUCKET CA.pem

The access key and the secret come from the environment, AA_MINIO_ACCESS and
AA_MINIO_SECRET. Python's standard library only. A bucket that is there already
(409 BucketAlreadyOwnedByYou) is fine.
"""

import datetime
import hashlib
import hmac
import os
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request


def sign(key, message):
    return hmac.new(key, message.encode(), hashlib.sha256).digest()


def request(method, endpoint, bucket, context):
    access = os.environ["AA_MINIO_ACCESS"]
    secret = os.environ["AA_MINIO_SECRET"]
    region = "us-east-1"
    url = urllib.parse.urlparse(endpoint)
    host = url.netloc
    when = datetime.datetime.now(datetime.timezone.utc)
    amz_date = when.strftime("%Y%m%dT%H%M%SZ")
    day = when.strftime("%Y%m%d")
    payload = hashlib.sha256(b"").hexdigest()
    path = "/" + bucket
    canonical_headers = "host:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n" % (host, payload, amz_date)
    signed_headers = "host;x-amz-content-sha256;x-amz-date"
    canonical = "\n".join([method, path, "", canonical_headers, signed_headers, payload])
    scope = "%s/%s/s3/aws4_request" % (day, region)
    to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canonical.encode()).hexdigest()])
    key = sign(sign(sign(sign(("AWS4" + secret).encode(), day), region), "s3"), "aws4_request")
    signature = hmac.new(key, to_sign.encode(), hashlib.sha256).hexdigest()
    headers = {
        "x-amz-date": amz_date,
        "x-amz-content-sha256": payload,
        "Authorization": "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (access, scope, signed_headers, signature),
    }
    req = urllib.request.Request(endpoint.rstrip("/") + path, method=method, headers=headers, data=b"" if method == "PUT" else None)
    try:
        with urllib.request.urlopen(req, context=context, timeout=30) as response:
            return response.status
    except urllib.error.HTTPError as e:
        return e.code


def main():
    endpoint, bucket, ca = sys.argv[1], sys.argv[2], sys.argv[3]
    context = ssl.create_default_context(cafile=ca)
    for _ in range(30):
        try:
            made = request("PUT", endpoint, bucket, context)
            break
        except (urllib.error.URLError, ConnectionError, OSError):
            import time
            time.sleep(2)
    else:
        print("REFUSED: MinIO at %s did not answer" % endpoint)
        return 2
    there = request("HEAD", endpoint, bucket, context)
    print("bucket %s: PUT %s, HEAD %s" % (bucket, made, there))
    return 0 if there == 200 else 1


if __name__ == "__main__":
    sys.exit(main())
