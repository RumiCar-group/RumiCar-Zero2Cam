#!/usr/bin/env python3
"""Keep the figures in README.md / README.en.md identical to their sources.

The figures are Mermaid diagrams kept in docs/figures/<lang>/NN-name.mmd (the
single source).  GitHub renders a ```mermaid block in Markdown, so the READMEs
carry a copy of each source right after a marker line:

    <!-- figure: ja/01-system.mmd -->
    ```mermaid
    ...the contents of docs/figures/ja/01-system.mmd...
    ```

    python3 docs/figures/sync_readme.py --check   # exit 1 if a copy differs (CI)
    python3 docs/figures/sync_readme.py --write   # rewrite the copies from the sources

Every source file must be used by its README exactly once (ja -> README.md,
en -> README.en.md).  Python 3 standard library only.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
READMES = {"ja": "README.md", "en": "README.en.md"}
BLOCK = re.compile(r"(<!-- figure: (?P<ref>(?P<lang>[a-z]{2})/[0-9A-Za-z_.-]+\.mmd) -->\n```mermaid\n)(?P<body>.*?)(```)",
                   re.DOTALL)


def source(ref):
    path = os.path.join(HERE, ref)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return text if text.endswith("\n") else text + "\n"


def process(lang, readme, write):
    path = os.path.join(ROOT, readme)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    problems = []
    used = []

    def repl(m):
        ref = m.group("ref")
        used.append(ref)
        if m.group("lang") != lang:
            problems.append(f"{readme}: {ref} is not a {lang} figure")
            return m.group(0)
        want = source(ref)
        if m.group("body") != want:
            problems.append(f"{readme}: the copy of {ref} differs from the source")
        return m.group(1) + want + m.group(5)

    new = BLOCK.sub(repl, text)
    expected = sorted(f"{lang}/{n}" for n in os.listdir(os.path.join(HERE, lang)) if n.endswith(".mmd"))
    for ref in expected:
        if used.count(ref) != 1:
            problems.append(f"{readme}: {ref} is used {used.count(ref)} times (must be exactly once)")
    for ref in used:
        if ref not in expected:
            problems.append(f"{readme}: {ref} has no source file")
    if write and new != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args()
    problems = []
    for lang, readme in READMES.items():
        problems += process(lang, readme, args.write)
    if args.write:
        problems = [p for p in problems if "differs" not in p]
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        sys.exit(1)
    print("figures: OK")


if __name__ == "__main__":
    main()
