# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""SMTP on 127.0.0.1:25 as a client sees it: the 220 banner, EHLO, and STARTTLS
offered and taken - which the first start's new-installation defaults turn on
(hmailserver/docs/SecureDefaults.md). Prints what it found; exits 1 when any of it is
not so."""

import smtplib
import ssl
import sys


def main():
    # smtplib refuses, with SMTPConnectError, a banner that is not 220.
    try:
        client = smtplib.SMTP("127.0.0.1", 25, timeout=30)
    except smtplib.SMTPConnectError as error:
        print("banner: %d %s" % (error.smtp_code, error.smtp_error))
        return 1
    print("banner: 220")
    code, answer = client.ehlo("molecule.example.test")
    print("EHLO: %d %s" % (code, answer.decode("utf-8", "replace").replace("\n", " | ")))
    if code != 250:
        return 1
    if not client.has_extn("starttls"):
        print("STARTTLS is not offered")
        return 1
    # The certificate is the self-signed one the first start made: what is checked is
    # that TLS is offered and taken, not who signed it.
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    code, answer = client.starttls(context=context)
    print("STARTTLS: %d %s, %s" % (code, answer.decode("utf-8", "replace"), client.sock.version()))
    if code != 220:
        return 1
    code, answer = client.ehlo("molecule.example.test")
    print("EHLO over TLS: %d" % code)
    client.quit()
    return 0 if code == 250 else 1


if __name__ == "__main__":
    sys.exit(main())
