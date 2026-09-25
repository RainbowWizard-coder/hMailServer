#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The test lab's Keycloak: what the realm file cannot carry, and a check of the realm.

keycloak/hmailserver-realm.json defines realm "hmailserver" - an OpenID Connect
client for the server's single sign-on, a confidential client for tokens, a SAML
client, and the users alice and bob - and holds no secret. This sets the two
secrets from ~/hmlab/secrets.env through Keycloak's admin API: the test users'
password (HMLAB_IDP_USER_PASSWORD) and the token client's secret
(HMLAB_IDP_CLIENT_SECRET). It prints no secret and no token.

  python3 build/lab/keycloak-setup.py setup   (lab.sh up idp runs it)
  python3 build/lab/keycloak-setup.py check   (lab.sh idp-check runs it)

The check reads the realm as a relying party does, over https with the lab CA:
the discovery document and its issuer, the key set, the SAML descriptor and its
signing certificate, a password grant for alice and a client-credentials grant, and
token introspection - and prints the claims, never the tokens.

Runs on linix with Python's standard library; the admin API is reached at
http://127.0.0.1:10320 (the loopback, which Keycloak takes without TLS), the public
side at https://<lab address>:10321.
"""

import base64
import hashlib
import json
import os
import re
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HOME = os.environ.get("HMLAB_HOME") or os.path.expanduser("~/hmlab")
LAB_IP = os.environ.get("HMLAB_HOST_IP", "192.168.11.154")
ADMIN = "http://127.0.0.1:10320"
PUBLIC = "https://%s:10321" % LAB_IP
REALM = "hmailserver"
ISSUER = "%s/realms/%s" % (PUBLIC, REALM)
USERS = ("alice", "bob")
TOKEN_CLIENT = "hmailserver-token"


def secrets():
    path = os.path.join(HOME, "secrets.env")
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
    return values


def tls():
    return ssl.create_default_context(cafile=os.path.join(HOME, "ca", "lab-ca.pem"))


def call(method, url, body=None, token=None, form=None, basic=None, context=None, accept="application/json"):
    headers = {"Accept": accept}
    data = None
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    elif body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = "Bearer " + token
    if basic:
        headers["Authorization"] = "Basic " + base64.b64encode(("%s:%s" % basic).encode()).decode()
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=30, context=context) as response:
            raw = response.read()
            return response.status, raw
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def admin_token(s):
    status, raw = call("POST", ADMIN + "/realms/master/protocol/openid-connect/token",
                       form={"grant_type": "password", "client_id": "admin-cli", "username": "admin",
                             "password": s["HMLAB_KEYCLOAK_ADMIN_PASSWORD"]})
    if status != 200:
        raise SystemExit("the admin token was refused (%d); is HMLAB_KEYCLOAK_ADMIN_PASSWORD the one Keycloak started with?" % status)
    return json.loads(raw)["access_token"]


def jwt_part(jwt, index):
    part = jwt.split(".")[index]
    part += "=" * (-len(part) % 4)
    return json.loads(base64.urlsafe_b64decode(part))


def claims_of(jwt):
    return jwt_part(jwt, 1)


def setup():
    s = secrets()
    for _ in range(60):
        status, _raw = call("GET", ADMIN + "/realms/%s/.well-known/openid-configuration" % REALM)
        if status == 200:
            break
        time.sleep(2)
    else:
        raise SystemExit("realm %s did not appear: was keycloak/hmailserver-realm.json imported?" % REALM)
    token = admin_token(s)
    base = ADMIN + "/admin/realms/" + REALM
    for user in USERS:
        status, raw = call("GET", base + "/users?exact=true&username=" + user, token=token)
        found = json.loads(raw) if status == 200 else []
        if not found:
            raise SystemExit("user %s is not in realm %s" % (user, REALM))
        status, raw = call("PUT", base + "/users/%s/reset-password" % found[0]["id"], token=token,
                           body={"type": "password", "value": s["HMLAB_IDP_USER_PASSWORD"], "temporary": False})
        if status not in (200, 204):
            raise SystemExit("setting %s's password failed (%d): %s" % (user, status, raw[:200]))
        print("user %-6s %s: password set from HMLAB_IDP_USER_PASSWORD" % (user, found[0].get("email")))
    status, raw = call("GET", base + "/clients?clientId=" + TOKEN_CLIENT, token=token)
    clients = json.loads(raw) if status == 200 else []
    if not clients:
        raise SystemExit("client %s is not in realm %s" % (TOKEN_CLIENT, REALM))
    client = clients[0]
    client["secret"] = s["HMLAB_IDP_CLIENT_SECRET"]
    status, raw = call("PUT", base + "/clients/" + client["id"], token=token, body=client)
    if status not in (200, 204):
        raise SystemExit("setting the secret of %s failed (%d): %s" % (TOKEN_CLIENT, status, raw[:200]))
    print("client %s: secret set from HMLAB_IDP_CLIENT_SECRET" % TOKEN_CLIENT)
    status, raw = call("GET", base + "/clients", token=token)
    for c in json.loads(raw):
        if not c["clientId"].startswith(("account", "admin-cli", "broker", "realm-management", "security-admin-console")):
            print("client %-40s %s" % (c["clientId"], c.get("protocol", "openid-connect")))


def check():
    s = secrets()
    context = tls()
    ok = True

    def verdict(passed, text):
        nonlocal ok
        ok = ok and passed
        print("%s  %s" % ("PASS" if passed else "FAIL", text))

    status, raw = call("GET", ISSUER + "/.well-known/openid-configuration", context=context)
    discovery = json.loads(raw) if status == 200 else {}
    verdict(discovery.get("issuer") == ISSUER,
            "discovery over https with the lab CA: issuer %s" % discovery.get("issuer"))
    for key in ("authorization_endpoint", "token_endpoint", "jwks_uri", "introspection_endpoint"):
        print("      %-24s %s" % (key, discovery.get(key)))
    verdict("S256" in discovery.get("code_challenge_methods_supported", []), "PKCE S256 offered")

    status, raw = call("GET", discovery.get("jwks_uri", ISSUER + "/protocol/openid-connect/certs"), context=context)
    keys = json.loads(raw).get("keys", []) if status == 200 else []
    signing = [k for k in keys if k.get("use") == "sig"]
    verdict(any(k.get("alg") == "RS256" for k in signing),
            "key set: %s" % ", ".join("%s %s" % (k.get("kty"), k.get("alg")) for k in signing))

    status, raw = call("GET", ISSUER + "/protocol/saml/descriptor", context=context, accept="*/*")
    text = raw.decode("utf-8", "replace") if status == 200 else ""
    entity = re.search(r'entityID="([^"]+)"', text)
    certificate = re.search(r"<(?:ds:)?X509Certificate>([^<]+)</", text)
    sso = re.search(r'SingleSignOnService[^>]*Binding="urn:oasis:names:tc:SAML:2.0:bindings:HTTP-Redirect"[^>]*Location="([^"]+)"', text)
    fingerprint = hashlib.sha256(base64.b64decode(certificate.group(1))).hexdigest() if certificate else ""
    verdict(bool(entity and certificate and sso),
            "SAML descriptor: entityID %s" % (entity.group(1) if entity else None))
    print("      HTTP-Redirect SSO URL    %s" % (sso.group(1) if sso else None))
    print("      signing certificate      SHA-256 %s" % fingerprint)

    token_endpoint = discovery.get("token_endpoint")
    status, raw = call("POST", token_endpoint, context=context,
                       basic=(TOKEN_CLIENT, s["HMLAB_IDP_CLIENT_SECRET"]),
                       form={"grant_type": "password", "username": "alice",
                             "password": s["HMLAB_IDP_USER_PASSWORD"], "scope": "openid email"})
    answer = json.loads(raw) if status == 200 else {}
    access = answer.get("access_token", "")
    if access:
        c = claims_of(access)
        verdict(c.get("email") == "alice@example.test" and c.get("email_verified") is True,
                "password grant for alice: iss %s, aud %s, azp %s, email %s, email_verified %s, lifetime %ss, alg %s"
                % (c.get("iss"), c.get("aud"), c.get("azp"), c.get("email"), c.get("email_verified"),
                   c.get("exp", 0) - c.get("iat", 0), jwt_part(access, 0).get("alg")))
        idt = claims_of(answer.get("id_token", access))
        print("      id_token                 aud %s, email %s" % (idt.get("aud"), idt.get("email")))
    else:
        verdict(False, "password grant for alice refused (%d): %s" % (status, raw[:200]))

    status, raw = call("POST", token_endpoint, context=context,
                       basic=(TOKEN_CLIENT, s["HMLAB_IDP_CLIENT_SECRET"]),
                       form={"grant_type": "client_credentials"})
    service = json.loads(raw).get("access_token", "") if status == 200 else ""
    verdict(bool(service), "client-credentials grant for %s (%d)" % (TOKEN_CLIENT, status))

    if access:
        status, raw = call("POST", discovery.get("introspection_endpoint"), context=context,
                           basic=(TOKEN_CLIENT, s["HMLAB_IDP_CLIENT_SECRET"]), form={"token": access})
        inspected = json.loads(raw) if status == 200 else {}
        verdict(inspected.get("active") is True,
                "introspection of alice's token: active %s, username %s" % (inspected.get("active"), inspected.get("username")))

    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("setup", "check"):
        sys.stderr.write(__doc__)
        sys.exit(64)
    sys.exit(setup() or 0 if sys.argv[1] == "setup" else check())
