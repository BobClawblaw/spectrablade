#!/usr/bin/env python3
"""Converts the Universal Category System's category list (the "UCS v8.2.1 Full List.xlsx" in the
UCS release, from universalcategorysystem.com) into the tab-separated list SpectraBlade bundles:

    Category, SubCategory, CatID, CatShort, Explanation, Synonyms

one row per subcategory, in the list's order, after comment lines naming the source and its
SHA-256. Only the standard library is used (an .xlsx file is a zip of XML).

    python3 Scripts/ucs_convert.py "UCS v8.2.1 Full List.xlsx" ThirdParty/UCS/ucs_v8_2_1.tsv
"""

import hashlib
import re
import sys
import xml.etree.ElementTree as ET
import zipfile

MAIN = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
EXPECTED_HEADER = ["Category", "SubCategory", "CatID", "CatShort", "Explanations", "Synonyms - Comma Separated"]


def clean(text):
    return re.sub(r"\s+", " ", text or "").strip()


def cell_text(cell, shared):
    value = cell.find(MAIN + "v")

    if value is None:
        inline = cell.find(MAIN + "is")
        return "".join(t.text or "" for t in inline.iter(MAIN + "t")) if inline is not None else ""

    return shared[int(value.text)] if cell.get("t") == "s" else (value.text or "")


def main(source, target):
    digest = hashlib.sha256(open(source, "rb").read()).hexdigest()
    book = zipfile.ZipFile(source)
    shared = ["".join(t.text or "" for t in si.iter(MAIN + "t"))
              for si in ET.fromstring(book.read("xl/sharedStrings.xml")).findall(MAIN + "si")]
    rows = ET.fromstring(book.read("xl/worksheets/sheet1.xml")).find(MAIN + "sheetData")
    table = []

    for row in rows:
        cells = {re.sub(r"\d", "", c.get("r")): clean(cell_text(c, shared)) for c in row}
        table.append([cells.get(column, "") for column in "ABCDEF"])

    start = next(i for i, r in enumerate(table) if r[:2] == EXPECTED_HEADER[:2])

    if table[start] != EXPECTED_HEADER:
        sys.exit("unexpected header: %r" % table[start])

    entries = [r for r in table[start + 1:] if r[2]]
    ids = [r[2] for r in entries]

    if len(set(ids)) != len(ids) or any(not (r[0] and r[1] and r[3]) for r in entries):
        sys.exit("the list has a repeated CatID or a row missing its category, subcategory or CatShort")

    with open(target, "w", encoding="utf-8", newline="\n") as out:
        out.write("# The Universal Category System's category list, v8.2.1 (universalcategorysystem.com),\n")
        out.write("# a public domain initiative. Converted by Scripts/ucs_convert.py from \"UCS v8.2.1 Full List.xlsx\",\n")
        out.write("# SHA-256 %s.\n" % digest)
        out.write("# Category\tSubCategory\tCatID\tCatShort\tExplanation\tSynonyms\n")

        for r in entries:
            out.write("\t".join(r) + "\n")

    print("%d subcategories in %d categories" % (len(entries), len({r[0] for r in entries})))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    main(sys.argv[1], sys.argv[2])
