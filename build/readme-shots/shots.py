#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The README's two webmail screenshots, taken against the real bench: the
inbox with its tabs and a conversation open in the reading pane, and a message
opened on its own. Run after bench.ps1 -Up and seed.ps1, with the bench service
running the build to be shown; the Control Panel's Welcome shot is
build/capture-cp.ps1 -Launch -Nav Welcome with the window sized to 1920x1200.
Writes into hmailserver/docs/images/ of this checkout unless told otherwise.
Run it as `python -P` if anything in the script's directory shadows a module.

Usage: python build/readme-shots/shots.py [out-dir]
"""
import os
import sys

from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:9530"
ACCOUNT = "self@uicheck.test"
PASSWORD = "Original-Passw0rd!"
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hmailserver", "docs", "images")


def sign_in(pg):
    pg.goto(BASE + "/portal", timeout=30000)
    pg.wait_for_selector("#signin:not([hidden])", timeout=15000)
    pg.fill("#address", ACCOUNT)
    pg.fill("#password", PASSWORD)
    pg.click("#signin-form button[type=submit]")
    pg.wait_for_selector("#signin", state="hidden", timeout=15000)
    pg.wait_for_selector("#message-list .msg", timeout=15000)
    pg.wait_for_timeout(600)


with sync_playwright() as p:
    browser = p.chromium.launch(headless=True)
    context = browser.new_context(viewport={"width": 1440, "height": 900}, device_scale_factor=1, color_scheme="light")
    pg = context.new_page()
    sign_in(pg)

    # The inbox: the conversation of three opened beside the list.
    pg.locator("#message-list .msg", has_text="Thread root").first.click()
    pg.wait_for_selector("#message-view:not([hidden])", timeout=10000)
    pg.wait_for_timeout(900)
    pg.screenshot(path=os.path.join(OUT, "webmail-inbox.png"))
    print("webmail-inbox.png")

    # A message on its own: the reading pane off, the message in the list's place.
    # One the tabs leave in Primary (the invoice with its List-Unsubscribe is filed elsewhere), with an attachment.
    # The reading pane is a preference now, on the settings page.
    def set_pane(mode):
        pg.evaluate("location.hash = '#/settings'")
        pg.wait_for_selector("#pref-pane", state="visible", timeout=10000)
        pg.select_option("#pref-pane", mode)
        pg.click("#prefs-form button[type=submit]")
        pg.wait_for_timeout(800)
        pg.evaluate("location.hash = ''")
        pg.wait_for_selector("#message-list .msg", timeout=15000)
        pg.wait_for_timeout(600)
    set_pane("off")
    pg.locator("#message-list .msg", has_text="Photos from the weekend").first.click()
    pg.wait_for_selector("#message-view:not([hidden])", timeout=10000)
    pg.wait_for_timeout(900)
    pg.screenshot(path=os.path.join(OUT, "webmail-message.png"))
    print("webmail-message.png")

    # Back to the reading pane on the right, so the account is left as found.
    pg.click("#message-back")
    pg.wait_for_selector("#message-list:not([hidden])", timeout=10000)
    set_pane("right")
    browser.close()
print("done")
