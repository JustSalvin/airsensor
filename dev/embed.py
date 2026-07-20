#!/usr/bin/env python3
"""Wraps dev/*.html into PROGMEM headers for the sketch."""
import pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
for src, dst, sym in [("dev/dashboard.html", "dashboard_html.h", "DASHBOARD_HTML"),
                      ("dev/admin.html", "admin_html.h", "ADMIN_HTML")]:
    p = ROOT / src
    if not p.exists():
        continue
    html = p.read_text()
    assert ")HTMLDOC" not in html, f"{src}: raw-literal delimiter collision"
    (ROOT / dst).write_text(
        "// AUTO-GENERATED from " + src + " by dev/embed.py - DO NOT EDIT\n"
        "#pragma once\n"
        f'static const char {sym}[] PROGMEM = R"HTMLDOC({html})HTMLDOC";\n')
    print(f"{dst}: {len(html)} bytes")
