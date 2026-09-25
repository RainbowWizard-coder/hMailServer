#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Single sign-on into the served lab server through the lab's Keycloak, both kinds.

Runs INSIDE the bench container (lab.sh serve, then lab.sh idp-check), where the
server's REST listener is http://127.0.0.1:8045 and the lab CA is in the system
store, and plays the browser - with Python's standard library, following every
redirect by hand - through what docs/PasskeysAndSingleSignOn.md describes:

  OpenID Connect, as alice: /portal/oidc/start -> Keycloak's authorize endpoint
  (PKCE S256) -> its login form -> /portal/oidc/callback, where the server
  exchanges the code and verifies the id_token -> /portal#sso=<ticket> ->
  POST /api/v1/session {"sso_ticket"}.

  SAML, as bob: /portal/saml/start (the hmsaml cookie, an AuthnRequest by
  HTTP-Redirect) -> Keycloak's login form -> the auto-posted Response ->
  /portal/saml/acs, where the server verifies the signatures -> 303
  /portal/saml/finish -> /portal#sso=<ticket> -> POST /api/v1/session.

What it sets on the server first, over REST as the administrator: the two
accounts (alice@ and bob@example.test), and the settings - the issuer, the client
hmailserver-oidc, the base URL http://127.0.0.1:8045, and the SAML identity
provider's entity ID, sign-in URL and signing certificate from Keycloak's own
descriptor. It prints statuses and claims; never a ticket, a token, a session or
an assertion.
"""

import base64
import html
import http.cookiejar
import json
import os
import re
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

REST = "http://127.0.0.1:8045"
ADMIN = ("Administrator", os.environ.get("HMTEST_ADMIN_PASSWORD", "testar"))
ISSUER = os.environ["HMLAB_IDP_ISSUER"]
IDP_PASSWORD = os.environ["HMLAB_IDP_USER_PASSWORD"]
ACCOUNT_PASSWORD = os.environ["HMLAB_BENCH_ACCOUNT_PASSWORD"]
failed = []


class Keep(urllib.request.HTTPRedirectHandler):
    """No redirect is followed by itself: each is a step to see."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def browser():
    jar = http.cookiejar.CookieJar()
    return urllib.request.build_opener(Keep, urllib.request.HTTPCookieProcessor(jar),
                                       urllib.request.HTTPSHandler(context=ssl.create_default_context()))


def go(opener, method, url, form=None, body=None, basic=None, extra=None):
    data = None
    headers = dict(extra or {})
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if basic:
        headers["Authorization"] = "Basic " + base64.b64encode(("%s:%s" % basic).encode()).decode()
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with opener.open(request, timeout=30) as response:
            return response.status, response.headers, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read().decode("utf-8", "replace")


def absolute(base, location):
    return urllib.parse.urljoin(base, location)


def verdict(passed, text):
    print("%s  %s" % ("PASS" if passed else "FAIL", text))
    if not passed:
        failed.append(text)
    return passed


def admin(method, path, body=None):
    return go(urllib.request.build_opener(), method, REST + path, body=body, basic=ADMIN)


def login_form(page):
    form = re.search(r'<form[^>]*id="kc-form-login"[^>]*action="([^"]+)"', page)
    return html.unescape(form.group(1)) if form else None


def hidden(page, name):
    found = re.search(r'name="%s"\s+value="([^"]*)"' % name, page) or re.search(r'value="([^"]*)"\s+name="%s"' % name, page)
    return html.unescape(found.group(1)) if found else None


def session(opener, url_with_fragment, who):
    fragment = urllib.parse.urlparse(url_with_fragment).fragment
    values = urllib.parse.parse_qs(fragment)
    if "sso_error" in values:
        return verdict(False, "%s: the server refused the sign-in: %s" % (who, values["sso_error"][0]))
    ticket = values.get("sso", [""])[0]
    if not verdict(bool(ticket), "%s: back at %s with a one-use ticket in the fragment" % (who, url_with_fragment.split("#")[0])):
        return False
    # X-Requested-With, as the webmail and the Deck send it: the server takes a
    # credential from a request body only with it (login forgery, RestApiServer.cpp).
    status, _headers, text = go(opener, "POST", REST + "/api/v1/session", body={"sso_ticket": ticket},
                                extra={"X-Requested-With": "hMailServer"})
    try:
        answer = json.loads(text)
    except ValueError:
        answer = {}
    shown = {k: v for k, v in answer.items() if not re.search(r"token|session|secret|csrf", k, re.I)}
    return verdict(status in (200, 201), "%s: POST /api/v1/session with the ticket -> %d %s" % (who, status, json.dumps(shown)[:200]))


def setup():
    for local in ("alice", "bob"):
        address = local + "@example.test"
        admin("DELETE", "/api/v1/accounts/" + address)
        status, _h, text = admin("POST", "/api/v1/domains/example.test/accounts",
                                 {"address": address, "password": ACCOUNT_PASSWORD, "active": True})
        verdict(status in (200, 201), "account %s on the served server (%d)" % (address, status))
    status, _h, descriptor = go(browser(), "GET", ISSUER + "/protocol/saml/descriptor")
    certificate = re.search(r"<(?:ds:)?X509Certificate>([^<]+)</", descriptor)
    pem = "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n" % "\n".join(
        re.findall(".{1,64}", certificate.group(1).strip())) if certificate else ""
    settings = {
        "oidc_issuer": ISSUER,
        "oidc_client_id": "hmailserver-oidc",
        "oidc_redirect_base_url": REST,
        "saml_idp_entity_id": ISSUER,
        "saml_idp_sso_url": ISSUER + "/protocol/saml",
        "saml_idp_certificate": pem,
    }
    status, _h, text = admin("PUT", "/api/v1/settings", settings)
    verdict(status in (200, 204), "settings: issuer %s, client hmailserver-oidc, base URL %s, the SAML identity provider from its descriptor (%d%s)"
            % (ISSUER, REST, status, "" if status in (200, 204) else ": " + text[:300]))
    status, _h, text = go(browser(), "GET", REST + "/api/v1/signin-methods")
    print("      /api/v1/signin-methods: %s" % text[:200])


def oidc():
    print("")
    print("== OpenID Connect, as alice")
    b = browser()
    status, headers, _t = go(b, "GET", REST + "/portal/oidc/start")
    authorize = headers.get("Location", "")
    query = urllib.parse.parse_qs(urllib.parse.urlparse(authorize).query)
    if not verdict(status == 302 and authorize.startswith(ISSUER + "/protocol/openid-connect/auth"),
                   "/portal/oidc/start -> %d to Keycloak's authorize endpoint, client %s, PKCE %s, scope %s"
                   % (status, query.get("client_id", ["?"])[0], query.get("code_challenge_method", ["none"])[0], query.get("scope", ["?"])[0])):
        return
    status, _h, page = go(b, "GET", authorize)
    action = login_form(page)
    if not verdict(status == 200 and action, "Keycloak's login page (%d)" % status):
        return
    status, headers, page = go(b, "POST", action, form={"username": "alice", "password": IDP_PASSWORD, "credentialId": ""})
    callback = headers.get("Location", "")
    if not verdict(status == 302 and callback.startswith(REST + "/portal/oidc/callback"),
                   "alice signs in at Keycloak -> %d back to /portal/oidc/callback with a code" % status):
        return
    status, headers, _t = go(b, "GET", callback)
    back = absolute(REST, headers.get("Location", ""))
    verdict(status == 302, "/portal/oidc/callback: the server exchanged the code and verified the id_token -> %d" % status)
    session(b, back, "OIDC")


def saml():
    print("")
    print("== SAML, as bob")
    b = browser()
    status, headers, _t = go(b, "GET", REST + "/portal/saml/start")
    sso = headers.get("Location", "")
    if not verdict(status == 302 and sso.startswith(ISSUER + "/protocol/saml") and "SAMLRequest=" in sso,
                   "/portal/saml/start -> %d to Keycloak with an AuthnRequest (HTTP-Redirect)" % status):
        return
    status, _h, page = go(b, "GET", sso)
    action = login_form(page)
    if not verdict(status == 200 and action, "Keycloak's login page (%d)" % status):
        return
    status, headers, page = go(b, "POST", action, form={"username": "bob", "password": IDP_PASSWORD, "credentialId": ""})
    response = hidden(page, "SAMLResponse")
    relay = hidden(page, "RelayState")
    acs = re.search(r'<form[^>]*method="post"[^>]*action="([^"]+)"', page, re.I) or re.search(r'<form[^>]*action="([^"]+)"', page)
    acs = html.unescape(acs.group(1)) if acs else ""
    if not verdict(status == 200 and response and acs == REST + "/portal/saml/acs",
                   "bob signs in at Keycloak -> the Response, auto-posted to %s" % (acs or "?")):
        return
    xml = base64.b64decode(response).decode("utf-8", "replace")
    issuer = re.search(r"<(?:\w+:)?Issuer[^>]*>([^<]+)<", xml)
    name_id = re.search(r"<(?:\w+:)?NameID[^>]*Format=\"([^\"]+)\"[^>]*>([^<]+)<", xml)
    print("      Response: Issuer %s, NameID %s (%s), %d signature(s), %s"
          % (issuer.group(1) if issuer else "?", name_id.group(2) if name_id else "?",
             name_id.group(1).rsplit(":", 1)[-1] if name_id else "?", len(re.findall(r"<(?:\w+:)?Signature[ >]", xml)),
             "encrypted" if "EncryptedAssertion" in xml else "not encrypted"))
    status, headers, text = go(b, "POST", acs, form={"SAMLResponse": response, "RelayState": relay or ""})
    finish = absolute(REST, headers.get("Location", ""))
    if "#sso_error=" in finish:
        verdict(False, "/portal/saml/acs refused the Response: %s" % urllib.parse.unquote_plus(finish.split("#sso_error=", 1)[1]))
        return
    if not verdict(status == 303 and "/portal/saml/finish?token=" in finish,
                   "/portal/saml/acs: the server verified the signed Response -> %d to /portal/saml/finish" % status):
        return
    status, headers, _t = go(b, "GET", finish)
    back = absolute(REST, headers.get("Location", ""))
    verdict(status == 303, "/portal/saml/finish with the hmsaml cookie -> %d" % status)
    session(b, back, "SAML")


setup()
oidc()
saml()
print("")
if failed:
    print("SINGLE SIGN-ON FAILED: %d step(s)" % len(failed))
    sys.exit(1)
print("SINGLE SIGN-ON PASSED: OpenID Connect and SAML, through Keycloak, into the served server")
