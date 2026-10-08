# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The hMailServer side of a run, made through its REST API as the
administrator: the domain, the load accounts, the route to the sink, an IP
range for the generator's address and the message store's fsync. Postfix and
Dovecot are set up by their own configuration (build/lab/docker-compose.yml,
profile numbers, and build/lab/dovecot/numbers.conf); this is the same for the
server under test, so that each side is given only what the run needs.

Everything it makes is named so that --cleanup finds it again: the accounts
by their template, the route by its domain, the IP range by the name
"loadgen". Nothing else on the server is touched. MessageStoreFsync is a
setting that already exists (a row in the settings store, not a new key); the
run's fsync policy is part of the result and must be stated.

--cleanup removes the accounts, the route and the range. It leaves the domain,
which may have been there before the run, and MessageStoreFsync as the run set
it: hmailserver/docs/PublishedNumbers.md gives the two commands that put them
back.

The range's priority is one above every other range that holds an address of
it, and 20 at least. A connection takes the range of the highest priority that
holds its address (PersistentSecurityRange::ReadMatchingIP), and every
installation has "My computer", 127.0.0.1 at priority 30 with spam and virus
protection on: a range of 20, as this made until 8 October 2026, was passed
over for a generator on the server's own machine, which the Windows bench is.
"""

import base64
import http.client
import ipaddress
import json
import ssl
import urllib.parse

RANGE_NAME = "loadgen"
RANGE_PRIORITY = 20


class Rest(object):

    def __init__(self, base, password, insecure=False, timeout=60):
        parts = urllib.parse.urlsplit(base)
        self.https = parts.scheme == "https"
        self.host = parts.hostname
        self.port = parts.port or (443 if self.https else 80)
        self.timeout = timeout
        self.context = None
        if self.https:
            self.context = ssl.create_default_context()
            if insecure:
                self.context.check_hostname = False
                self.context.verify_mode = ssl.CERT_NONE
        self.auth = "Basic " + base64.b64encode(("Administrator:" + password).encode("utf-8")).decode("ascii")

    def call(self, method, path, body=None):
        if self.https:
            connection = http.client.HTTPSConnection(self.host, self.port, timeout=self.timeout, context=self.context)
        else:
            connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        headers = {"Authorization": self.auth, "Accept": "application/json", "X-Requested-With": "hMailServer"}
        payload = None
        if body is not None:
            payload = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        try:
            connection.request(method, path, body=payload, headers=headers)
            response = connection.getresponse()
            raw = response.read()
        finally:
            connection.close()
        try:
            data = json.loads(raw.decode("utf-8")) if raw else None
        except ValueError:
            data = raw.decode("utf-8", "replace")[:300]
        return response.status, data


def _objects(data):
    """Every JSON object in a response, however the list is wrapped."""
    if isinstance(data, dict):
        yield data
        for value in data.values():
            for item in _objects(value):
                yield item
    elif isinstance(data, list):
        for value in data:
            for item in _objects(value):
                yield item


def remove_routes(rest, domain, say):
    status, data = rest.call("GET", "/api/v1/routes")
    for item in _objects(data):
        if item.get("domain_name") == domain and item.get("id") is not None:
            say("route %s removed: HTTP %s" % (domain, rest.call("DELETE", "/api/v1/routes/%s" % item["id"])[0]))


def remove_range(rest, say):
    status, data = rest.call("GET", "/api/v1/ipranges")
    for item in _objects(data):
        if item.get("name") == RANGE_NAME and item.get("id") is not None:
            say("IP range %s removed: HTTP %s" % (RANGE_NAME, rest.call("DELETE", "/api/v1/ipranges/%s" % item["id"])[0]))


def range_priority(ranges, lower, upper):
    """(priority, the ranges it is above): one above the highest priority of
    every range but this tool's own that holds an address from lower to upper,
    and RANGE_PRIORITY at least. A range whose addresses do not parse, or are
    of the other IP version, holds none of them."""
    try:
        low, high = ipaddress.ip_address(lower), ipaddress.ip_address(upper)
    except ValueError:
        return RANGE_PRIORITY, []
    over = []
    for item in ranges:
        if item.get("name") == RANGE_NAME or not isinstance(item.get("priority"), int):
            continue
        try:
            first, last = ipaddress.ip_address(item.get("lower", "")), ipaddress.ip_address(item.get("upper", ""))
        except ValueError:
            continue
        if first.version != low.version or last.version != low.version:
            continue
        if first <= high and low <= last:
            over.append(item)
    if not over:
        return RANGE_PRIORITY, []
    return max(RANGE_PRIORITY, max(item["priority"] for item in over) + 1), over


def prepare(rest, args, password, say):
    """Idempotent: a route to the same domain and a range of the same name
    are removed before they are made again, and the accounts made afresh."""
    ok = True
    status, data = rest.call("POST", "/api/v1/domains", {"name": args.domain, "active": True})
    say("domain %s: %s" % (args.domain, "made" if status in (200, 201) else "HTTP %s (kept as it is)" % status))
    for n in range(1, args.accounts + 1):
        address = args.account_template.format(n=n)
        rest.call("DELETE", "/api/v1/accounts/" + urllib.parse.quote(address, safe="@"))
        status, data = rest.call("POST", "/api/v1/domains/%s/accounts" % urllib.parse.quote(args.domain),
                                 {"address": address, "password": password, "active": True, "max_size_mb": 0})
        if status not in (200, 201):
            # Older servers refuse a key they do not know; try once without the quota.
            status, data = rest.call("POST", "/api/v1/domains/%s/accounts" % urllib.parse.quote(args.domain),
                                     {"address": address, "password": password, "active": True})
        if status not in (200, 201):
            say("account %s: HTTP %s %s" % (address, status, data))
            ok = False
    say("accounts: %s to %s" % (args.account_template.format(n=1), args.account_template.format(n=args.accounts)))
    if args.route:
        domain, _, target = args.route.partition("=")
        host, _, port = target.rpartition(":")
        remove_routes(rest, domain, say)
        status, data = rest.call("POST", "/api/v1/routes", {
            "domain_name": domain, "description": "loadgen sink", "target_smtp_host": host,
            "target_smtp_port": int(port), "number_of_tries": 2, "minutes_between_try": 1, "all_addresses": True})
        say("route %s -> %s:%s: HTTP %s" % (domain, host, port, status))
        ok = ok and status in (200, 201)
    if args.client_range:
        lower, _, upper = args.client_range.partition("-")
        lower, upper = lower.strip(), (upper or lower).strip()
        remove_range(rest, say)
        status, data = rest.call("GET", "/api/v1/ipranges")
        priority, over = range_priority([item for item in _objects(data) if "priority" in item], lower, upper)
        if over:
            say("IP range %s: priority %d, above %s" % (RANGE_NAME, priority, ", ".join(
                "%s (%s)" % (item.get("name"), item.get("priority")) for item in over)))
        status, data = rest.call("POST", "/api/v1/ipranges", {
            "name": RANGE_NAME, "lower": lower, "upper": upper, "priority": priority,
            "allow_smtp": True, "allow_imap": True, "allow_pop3": False,
            "deliver_local_to_local": True, "deliver_local_to_remote": True,
            "deliver_remote_to_local": True, "deliver_remote_to_remote": False,
            "require_auth_local_to_local": False, "require_auth_local_to_remote": False,
            "require_auth_remote_to_local": False, "require_auth_remote_to_remote": True,
            "spam_protection": False, "virus_protection": False})
        say("IP range %s %s: HTTP %s" % (RANGE_NAME, args.client_range, status))
        ok = ok and status in (200, 201)
    if args.fsync:
        value = "1" if args.fsync == "on" else "0"
        status, data = rest.call("PUT", "/api/v1/settings/ini/MessageStoreFsync", {"value": value})
        say("MessageStoreFsync = %s: HTTP %s" % (value, status))
        ok = ok and status in (200, 204)
    status, data = rest.call("GET", "/api/v1/settings/ini/MessageStoreFsync")
    say("MessageStoreFsync is now %s" % (data.get("value") if isinstance(data, dict) else data))
    return ok


def cleanup(rest, args, say):
    for n in range(1, args.accounts + 1):
        address = args.account_template.format(n=n)
        rest.call("DELETE", "/api/v1/accounts/" + urllib.parse.quote(address, safe="@"))
    say("accounts deleted: %d" % args.accounts)
    if args.route:
        remove_routes(rest, args.route.partition("=")[0], say)
    remove_range(rest, say)
    return True
