#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Screenshots of the AIGW LuCI pages, from a VM started with
`KEEP=1 platform/openwrt/qemu-test.sh` (LuCI on http://127.0.0.1:8080).

    pip install playwright && python -m playwright install chromium
    python3 platform/openwrt/luci_screenshots.py [base URL] [out dir]

Logs in as root (no password in the test VM), opens every AIGW page, fails
if a page shows a JavaScript error or LuCI's error box, and saves one PNG
per page.
"""
import os
import sys

from playwright.sync_api import sync_playwright

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
OUT = sys.argv[2] if len(sys.argv) > 2 else "dist/luci"
PAGES = ["overview", "devices", "decisions", "access", "plugins"]


def main():
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": 1280, "height": 900})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        # a script in a host name or alert must never run (the KEEP=1 VM seeds one)
        page.on("dialog", lambda d: (errors.append(f"a dialog opened: {d.message}"), d.dismiss()))
        page.goto(f"{BASE}/cgi-bin/luci/", wait_until="networkidle")
        page.fill("input[name=luci_username]", "root")
        page.fill("input[name=luci_password]", "")
        page.click("button.btn.cbi-button-positive, input[type=submit], button[type=submit]")
        page.wait_for_load_state("networkidle")
        for name in PAGES:
            errors.clear()
            page.goto(f"{BASE}/cgi-bin/luci/admin/aigw/{name}", wait_until="networkidle")
            page.wait_for_selector("#view .cbi-map", timeout=20000)
            page.wait_for_timeout(800)
            alert = page.query_selector(".alert-message.error, .alert-message.danger")
            injected = page.evaluate("document.querySelectorAll('#view svg[onload], #view [onerror], #view script').length")
            if injected:
                errors.append(f"{injected} element(s) from data became markup")
            path = os.path.join(OUT, f"{name}.png")
            page.screenshot(path=path, full_page=True)
            problem = errors or (alert and alert.inner_text())
            print(f"{'FAIL' if problem else 'PASS'} {name}: {path}{'  ' + str(problem) if problem else ''}")
            bad += 1 if problem else 0
        # the seeded hostile host name must be on the devices page, as text
        page.goto(f"{BASE}/cgi-bin/luci/admin/aigw/devices", wait_until="networkidle")
        page.wait_for_selector("#view .cbi-map", timeout=20000)
        shown = "<svg/onload=alert(1)>" in page.inner_text("#view")
        print(f"{'PASS' if shown else 'FAIL'} escaping: the seeded host name with markup "
              f"{'is shown as text' if shown else 'is not shown as text'}")
        bad += 0 if shown else 1
        browser.close()
    sys.exit(bad)


if __name__ == "__main__":
    main()
