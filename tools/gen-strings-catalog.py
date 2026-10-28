#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds the Windows client's string catalog (resources/strings.json).

Translations are NOT authored here. The product's single source of
translations is the Mac catalog, Resources/Localizable.xcstrings, which the
dedicated translation repository maintains (see the Mac repo's
docs/i18n/localization-guidelines.md). This script joins the two:

  * `client/resources/strings-source.json` owns the KEYS the Windows client
    asks for, each with its English value and a translator comment. It is the
    Windows counterpart of the NSLocalizedString call sites on the Mac.

  * For every key, translations come from the Mac catalog — by key when the
    Mac carries that key, otherwise by exact English match, which is how the
    two clients share wording that was deliberately ported (a Windows "Pin"
    and a Mac "Pin" must never drift apart).

  * A Windows-only string with no Mac counterpart may carry its own
    `translations` block in the source file. Anything still untranslated is
    reported and falls back to English at runtime, which is the same
    degradation the Mac has for a new key awaiting the translation repo.

  * PLURALS. A string that counts something is not one key but a FAMILY:
    `<key>.one` and `<key>.other` (the CLDR category name is the suffix, so
    `few`/`many`/`zero`/`two` need no format change when a locale that has
    them ships). PhiStrN picks the branch at runtime. The Mac spells the same
    thing as an xcstrings `variations.plural` block on ONE key, so this
    script splits that block: `downloads.….daysAgo.other` is joined against
    the Mac's `downloads.….daysAgo` variation for `other`, by key or by that
    branch's English. A family missing a branch is an ERROR, not a warning —
    a missing `.other` reads as a singular in the UI and nobody notices.

Usage:
    tools/gen-strings-catalog.py            # write resources/strings.json
    tools/gen-strings-catalog.py --check    # report only, touch nothing
"""

from __future__ import annotations

import argparse
import re
import json
import os
import sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "client", "resources", "strings-source.json")
OUTPUT = os.path.join(REPO, "client", "resources", "strings.json")
MAC_CATALOG = os.path.join(
    os.path.dirname(os.path.dirname(REPO)), "Phi", "phibrowser-mac",
    "Resources", "Localizable.xcstrings")

# The locales the product ships. "en" leads: it is the source language and the
# runtime fallback.
LOCALES = ["en", "de", "es", "fr", "ja", "ko", "nl", "pt-BR", "zh-Hans",
           "zh-Hant"]

# CLDR's plural categories. A source key ending in one of these is a branch of
# a plural family, not a key of its own (see the module docstring).
PLURAL_CATEGORIES = ("zero", "one", "two", "few", "many", "other")


# The Mac's values carry printf placeholders ("%d", "%@", "%1$@"); PhiStrF
# substitutes positional "{0}" tokens. Convert on the way in so a key shared
# with the Mac formats on Windows too.
_PRINTF = re.compile(
    r"%(\d+\$)?[-+ 0#]*\d*(?:\.\d+)?(?:hh|h|ll|l|q|z|t|j)?[@dsifuxXc]")


def positionalize(text):
    if not text or "%" not in text:
        return text
    counter = [0]

    def repl(match):
        if match.group(1):
            index = int(match.group(1)[:-1]) - 1
        else:
            index = counter[0]
            counter[0] += 1
        return "{%d}" % index

    return _PRINTF.sub(repl, text.replace("%%", "%"))


def load_mac(path):
    """(by_key, by_english, by_plural, by_plural_english) views of the Mac."""
    if not os.path.exists(path):
        sys.exit(f"Mac catalog not found: {path}\n"
                 "It is the source of translations; clone phibrowser-mac beside "
                 "this repo or pass --mac-catalog.")
    with open(path, encoding="utf-8") as f:
        strings = json.load(f)["strings"]
    by_key, by_english = {}, {}
    # (mac key, category) -> {locale: text}, and the same tables keyed by the
    # branch's own English so a Windows plural family can join a Mac one whose
    # key it does not share.
    by_plural, by_plural_english = {}, defaultdict(dict)
    for key, entry in strings.items():
        table = {}
        for locale, value in (entry.get("localizations") or {}).items():
            unit = value.get("stringUnit") or {}
            text = positionalize(unit.get("value"))
            if text is not None:
                table[locale] = text
            # A "variations.plural" block is the Mac's plural family. Each
            # category becomes its own table here, which is what lets a
            # "<key>.other" on this side find "<key>"'s "other" branch.
            plural = (value.get("variations") or {}).get("plural") or {}
            for category, branch in plural.items():
                branch_text = positionalize(
                    (branch.get("stringUnit") or {}).get("value"))
                if branch_text is not None:
                    by_plural.setdefault((key, category), {})[locale] = (
                        branch_text)
        for (plural_key, category), branch_table in by_plural.items():
            if plural_key != key:
                continue
            english = branch_table.get("en")
            if english and english not in by_plural_english[category]:
                by_plural_english[category][english] = branch_table
        if not table:
            continue
        by_key[key] = table
        english = table.get("en")
        # First writer wins: identical English under several keys means the
        # Mac gave them separate identities for context, and the Windows
        # source file names the key it wants explicitly in that case.
        if english and english not in by_english:
            by_english[english] = table
    return by_key, by_english, by_plural, by_plural_english


def normalize(text):
    """English as the Mac spells it: menus there carry no accelerators."""
    return text.split("\t")[0].replace("&", "").replace("…", "...").strip()


def plural_families(source):
    """{family: {category, ...}} for every plural key in the source file."""
    families = defaultdict(set)
    for key in source["strings"]:
        family, _, category = key.rpartition(".")
        if family and category in PLURAL_CATEGORIES:
            families[family].add(category)
    return families


def check_plural_families(families):
    """A family without an "other" branch is an error, not a warning."""
    broken = sorted(f for f, cats in families.items() if "other" not in cats)
    if not broken:
        return 0
    print("\nPLURAL FAMILIES MISSING AN \"other\" BRANCH:")
    for family in broken:
        print(f"    {family}.{{{','.join(sorted(families[family]))}}}")
    print("  PhiStrN falls back to the KEY when the branch it picks is "
          "absent; \"other\" is the branch every locale can reach.")
    return len(broken)


def build(source, by_key, by_english, by_plural, by_plural_english):
    out, report = {}, defaultdict(list)
    for key, entry in source["strings"].items():
        english = entry["en"]
        table = {"en": english}

        mac = by_key.get(key)
        origin = "mac-key"
        if mac is None:
            # A plural branch joins the Mac's variation for the same
            # category, by key first and then by that branch's English.
            family, _, category = key.rpartition(".")
            if family and category in PLURAL_CATEGORIES:
                mac = by_plural.get((family, category))
                if mac is None:
                    mac = by_plural_english[category].get(normalize(english))
                if mac is not None:
                    origin = "mac-plural"
        if mac is None:
            mac = by_english.get(normalize(english))
            origin = "mac-english"
        if mac is None:
            mac = by_english.get(normalize(english).replace("...", "…"))
            origin = "mac-english"

        # An explicit `translations` block is a deliberate Windows override and
        # WINS over the Mac match: it exists precisely where the shared wording
        # is wrong here — a menu title whose "&" mnemonic differs per language
        # ("&Datei", "\u6587\u4ef6(&F)"), which no amount of re-decoration can derive.
        # Such a value is taken VERBATIM.
        #
        # A Mac value, by contrast, is the translation of the BARE label: the
        # "&" mnemonic and the "\tCtrl+N" accelerator are Windows layout, not
        # language, so they are re-applied around it.
        own = entry.get("translations") or {}
        translations = own or mac or {}
        # Decoration: translations are authored and reused BARE, and the
        # Windows "&" mnemonic plus "\tCtrl+N" accelerator are re-applied
        # around them. An entry marked "verbatim" opts out because its
        # decoration is language-specific and cannot be derived — a menu
        # title picks its own mnemonic letter per language ("&Datei",
        # "\u6587\u4ef6(&F)"), so those values already carry it.
        if entry.get("verbatim"):
            prefix = accelerator = ""
        else:
            prefix = "&" if english.startswith("&") else ""
            accelerator = ("\t" + english.split("\t", 1)[1]) if "\t" in english else ""
        for locale, text in translations.items():
            if locale in LOCALES and locale != "en":
                table[locale] = prefix + text + accelerator
        report["own" if own else (origin if mac else "untranslated")].append(key)

        missing = [l for l in LOCALES if l not in table]
        if missing and key not in report["untranslated"]:
            report["partial"].append(key)
        out[key] = table
    return out, report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="report coverage without writing the catalog")
    parser.add_argument("--mac-catalog", default=MAC_CATALOG)
    args = parser.parse_args()

    with open(SOURCE, encoding="utf-8") as f:
        source = json.load(f)
    by_key, by_english, by_plural, by_plural_english = load_mac(
        args.mac_catalog)
    strings, report = build(source, by_key, by_english, by_plural,
                            by_plural_english)
    broken_families = check_plural_families(plural_families(source))

    total = len(strings)
    print(f"keys                    : {total}")
    print(f"  translated by Mac key : {len(report['mac-key'])}")
    print(f"  plural branches joined: {len(report['mac-plural'])}")
    print(f"  translated by English : {len(report['mac-english'])}")
    print(f"  translated in source  : {len(report['own'])}")
    print(f"  ENGLISH ONLY          : {len(report['untranslated'])}")
    if report["untranslated"]:
        print("\nawaiting translation (falls back to English at runtime):")
        for key in sorted(report["untranslated"])[:40]:
            print("   ", key)
        if len(report["untranslated"]) > 40:
            print(f"    … and {len(report['untranslated']) - 40} more")

    if broken_families:
        return 1
    if args.check:
        return 0

    payload = {"locales": LOCALES, "strings": dict(sorted(strings.items()))}
    with open(OUTPUT, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    print(f"\nwrote {OUTPUT} ({os.path.getsize(OUTPUT):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
